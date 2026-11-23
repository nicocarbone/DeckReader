#include "articlesync.h"

#include "fsutil.h"
#include "http.h"
#include "log.h"
#include "util.h"

#include <algorithm>
#include <fcntl.h>
#include <map>
#include <set>
#include <sys/stat.h>

#include "nlohmann/json.hpp"

namespace dr {

namespace {
// Pictures: per article at most this many, this big each, this much in all
// (the archive checks, D5, allow 256 MB per file and 1 GB in all).
constexpr size_t MAX_PICTURES = 60;
constexpr size_t MAX_PICTURE = 10u << 20;
constexpr size_t MAX_PICTURES_TOTAL = 32u << 20; // in memory while the book is made
} // namespace

std::string article_file_name(int64_t id) {
    return std::to_string(id) + ".epub";
}

int64_t article_id(const std::string& name) {
    if (name.size() < 6 || name.size() > 23 || name.compare(name.size() - 5, 5, ".epub") != 0)
        return 0;
    std::string digits = name.substr(0, name.size() - 5);
    if (digits[0] == '0' || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return 0;
    return std::stoll(digits);
}

std::string article_lists_json(const std::vector<ArticleList>& lists) {
    nlohmann::json out = nlohmann::json::array();
    for (const ArticleList& l : lists)
        out.push_back({{"id", l.id}, {"name", l.name}, {"articles", l.articles}});
    return nlohmann::json{{"lists", out}}.dump(1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

bool parse_article_lists(const std::string& text, std::vector<ArticleList>& out) {
    out.clear();
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("lists") || !j["lists"].is_array())
        return false;
    for (const nlohmann::json& l : j["lists"]) {
        if (!l.is_object() || !l.contains("id") || !l["id"].is_number_integer())
            continue;
        ArticleList a;
        a.id = l["id"].get<int64_t>();
        if (l.contains("name") && l["name"].is_string())
            a.name = l["name"].get<std::string>();
        if (l.contains("articles") && l["articles"].is_array())
            for (const nlohmann::json& id : l["articles"])
                if (id.is_number_integer() && id.get<int64_t>() > 0)
                    a.articles.push_back(id.get<int64_t>());
        out.push_back(std::move(a));
    }
    return true;
}

SyncPlan plan_sync(const std::vector<ArticleEntry>& wanted, const std::vector<int64_t>& local,
                   const std::vector<int64_t>& skip_ids, bool rebuild) {
    SyncPlan plan;
    std::set<int64_t> have(local.begin(), local.end()), skip(skip_ids.begin(), skip_ids.end()), want;
    for (const ArticleEntry& e : wanted) {
        if (skip.count(e.id) || !want.insert(e.id).second)
            continue;
        if (rebuild || !have.count(e.id))
            plan.download.push_back(e);
    }
    for (int64_t id : have)
        if (!want.count(id))
            plan.remove.push_back(id);
    return plan;
}

bool add_pictures(HttpClient& http, const std::string& html, const std::string& base_url,
                  const std::string& cover_url, const std::string& trusted_host, ArticleBook& book,
                  const std::atomic<bool>& cancel, int* left_out) {
    // The cover first: it is never the one left out.
    std::vector<std::string> urls;
    if (!cover_url.empty())
        urls.push_back(cover_url);
    for (std::string& u : article_images(html, base_url))
        if (u != cover_url)
            urls.push_back(std::move(u));
    if (urls.size() > MAX_PICTURES) {
        *left_out += static_cast<int>(urls.size() - MAX_PICTURES);
        urls.resize(MAX_PICTURES);
    }
    std::map<std::string, std::string> local;
    // Don't wait for an unreachable host twice, nor a third time for one
    // whose transfers stall (Wikipedia's are all on one host, D38).
    std::set<std::string> dead_hosts;
    std::map<std::string, int> stalls;
    size_t total = 0;
    int n = 0;
    for (const std::string& url : urls) {
        const std::string host = url_host(url);
        if (dead_hosts.count(host)) {
            (*left_out)++;
            continue;
        }
        // A plain GET: no token, no cookies, redirects followed.
        HttpRequest q;
        q.url = url;
        q.max_bytes = MAX_PICTURE;
        q.timeout_s = 60;
        q.connect_timeout_s = 10;
        q.low_speed_s = 15;
        q.follow_redirects = true;
        q.public_only = trusted_host.empty() || host_name(host) != trusted_host;
        q.headers = {"Accept: image/webp,image/png,image/jpeg,image/gif,image/svg+xml;q=0.9,*/*;q=0.5"};
        HttpResponse res = http.request(q, &cancel);
        if (res.cancelled)
            return false;
        if (res.unreachable || (res.stalled && ++stalls[host] >= 2)) // not for one bad address or a picture too big
            dead_hosts.insert(host);
        std::string media, ext;
        if (!res.error.empty() || res.status != 200 || !image_type(res.body, &media, &ext) ||
            total + res.body.size() > MAX_PICTURES_TOTAL) {
            LOG_D("articles: picture %s left out (%s)", url.c_str(),
                  !res.error.empty() ? res.error.c_str() : strf("HTTP %ld, %zu bytes", res.status, res.body.size()).c_str());
            (*left_out)++;
            continue;
        }
        total += res.body.size();
        std::string href = strf("images/%d.%s", ++n, ext.c_str());
        local[url] = href;
        if (url == cover_url && media != "image/svg+xml")
            book.cover = href;
        book.images.push_back({href, media, std::move(res.body)});
    }
    // The cover asked for didn't come, or is an SVG (no thumbnail): the
    // first picture that can be one (V4-BUG-8, D38).
    if (!cover_url.empty() && book.cover.empty())
        for (const ArticleImage& im : book.images)
            if (im.media_type != "image/svg+xml") {
                book.cover = im.href;
                break;
            }
    book.html = rewrite_html(html, base_url, local);
    return true;
}

bool save_article(const std::string& dir, int64_t id, int64_t created, ArticleBook book, size_t* bytes,
                  std::string* err) {
    const std::string path = path_join(dir, article_file_name(id));
    const std::string epub = make_article_epub(std::move(book));
    if (bytes)
        *bytes = epub.size();
    if (!write_file_atomic(path, epub, err))
        return false;
    if (created > 0) {
        timespec times[2] = {{static_cast<time_t>(created), 0}, {static_cast<time_t>(created), 0}};
        utimensat(AT_FDCWD, path.c_str(), times, 0);
    }
    return true;
}

// --- the thread ------------------------------------------------------------------

SyncWorker::~SyncWorker() {
    cancel_ = true;
    if (thread_.joinable())
        thread_.join();
}

bool SyncWorker::start(SyncJob job, std::function<void()> wake) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (running_)
            return false;
    }
    if (thread_.joinable())
        thread_.join(); // the previous one is done (its result was taken)
    std::lock_guard<std::mutex> lock(mu_);
    running_ = true;
    finished_ = false;
    progress_ = SyncProgress{};
    result_ = SyncResult{};
    cancel_ = false;
    thread_ = std::thread([this, job = std::move(job), wake = std::move(wake)] {
        SyncResult r = job(cancel_, [&](const SyncProgress& p) {
            {
                std::lock_guard<std::mutex> l(mu_);
                progress_ = p;
            }
            if (wake)
                wake();
        });
        {
            std::lock_guard<std::mutex> l(mu_);
            result_ = std::move(r);
            finished_ = true;
        }
        if (wake)
            wake();
    });
    return true;
}

bool SyncWorker::busy() const {
    std::lock_guard<std::mutex> lock(mu_);
    return running_;
}

SyncProgress SyncWorker::progress() const {
    std::lock_guard<std::mutex> lock(mu_);
    return progress_;
}

bool SyncWorker::take_result(SyncResult& out) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!running_ || !finished_)
            return false;
        out = std::move(result_);
        running_ = false;
    }
    if (thread_.joinable())
        thread_.join();
    return true;
}

} // namespace dr
