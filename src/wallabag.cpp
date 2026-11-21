#include "wallabag.h"

#include "fsutil.h"
#include "http.h"
#include "log.h"
#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <map>
#include <set>
#include <sys/stat.h>

#include "nlohmann/json.hpp"

namespace dr {

using json = nlohmann::json;

namespace {
constexpr size_t MAX_JSON = 16u << 20;
constexpr size_t MAX_ARTICLE_JSON = 32u << 20;
// Pictures: per article at most this many, this big each, this much in all
// (the archive checks, D5, allow 256 MB per file and 1 GB in all).
constexpr size_t MAX_PICTURES = 60;
constexpr size_t MAX_PICTURE = 10u << 20;
constexpr size_t MAX_PICTURES_TOTAL = 32u << 20; // in memory while the book is made
// The unread list is asked for in pages of at most this many (servers may
// limit pages; small ones may not manage hundreds at once).
constexpr int LIST_PAGE = 100;

std::string json_string(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? trim(it->get<std::string>()) : std::string();
}
} // namespace

std::string wallabag_base(const std::string& server) {
    std::string s = trim(server);
    std::string lower = to_lower(s);
    if (!starts_with(lower, "https://") && !starts_with(lower, "http://"))
        return "";
    if (url_host(s).empty())
        return "";
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}

int64_t parse_iso8601(const std::string& s) {
    // Digits only where digits go (sscanf alone would take "-1" for an hour).
    static const char* shape = "dddd-dd-ddTdd:dd:dd";
    if (s.size() < 19)
        return 0;
    for (size_t k = 0; k < 19; k++)
        if (shape[k] == 'd' ? !std::isdigit(static_cast<unsigned char>(s[k]))
                            : !(s[k] == shape[k] || (k == 10 && (s[k] == 't' || s[k] == ' '))))
            return 0;
    int y, mo, d, h, mi, sec;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2d%*c%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &sec) != 6)
        return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60 || y < 1970)
        return 0;
    std::string rest = s.substr(19);
    // Fractions of a second, if any.
    if (!rest.empty() && rest[0] == '.') {
        size_t i = 1;
        while (i < rest.size() && std::isdigit(static_cast<unsigned char>(rest[i])))
            i++;
        rest = rest.substr(i);
    }
    int64_t offset = 0;
    if (rest.empty() || rest == "Z" || rest == "z") {
        offset = 0;
    } else if (rest[0] == '+' || rest[0] == '-') {
        std::string digits;
        for (char c : rest.substr(1))
            if (c != ':')
                digits += c;
        if (digits.size() != 4 || !std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
            return 0;
        const int oh = std::stoi(digits.substr(0, 2)), om = std::stoi(digits.substr(2));
        if (oh > 23 || om > 59)
            return 0;
        offset = (oh * 60 + om) * 60;
        if (rest[0] == '-')
            offset = -offset;
    } else {
        return 0;
    }
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    const time_t t = timegm(&tm);
    return t == static_cast<time_t>(-1) ? 0 : static_cast<int64_t>(t) - offset;
}

std::string parse_token(const std::string& body) {
    json j = json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("access_token") || !j["access_token"].is_string())
        return "";
    // It goes into a header: printable ASCII only, no spaces or line breaks.
    std::string t = j["access_token"].get<std::string>();
    if (t.size() > 4096 || !std::all_of(t.begin(), t.end(), [](char c) { return c > 0x20 && c < 0x7F; }))
        return "";
    return t;
}

std::string token_error(long status, const std::string& body, const std::string& location) {
    if (status >= 300 && status < 400) {
        std::string to = location;
        const std::string tail = "/oauth/v2/token";
        if (to.size() > tail.size() && to.compare(to.size() - tail.size(), tail.size(), tail) == 0)
            to = to.substr(0, to.size() - tail.size());
        if (to.empty())
            return strf("The server redirected the login (HTTP %ld). Check the server address.", status);
        return "The server sent the login to " + to + ". Use that as the server address.";
    }
    json j = json::parse(body, nullptr, false);
    std::string err = j.is_object() && j.contains("error") && j["error"].is_string() ? j["error"].get<std::string>() : "";
    if (err == "invalid_grant")
        return "wallabag refused the username or password.";
    if (err == "invalid_client")
        return "wallabag refused the client ID or secret.";
    if (!err.empty())
        return "wallabag refused the login (" + err + ").";
    if (status == 404)
        return "No wallabag at this server address (HTTP 404).";
    if (status == 200)
        return "wallabag's answer to the login could not be read.";
    return strf("wallabag answered HTTP %ld to the login.", status);
}

bool parse_entries(const std::string& body, std::vector<WallabagEntry>& out, int* pages) {
    out.clear();
    json j = json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("_embedded") || !j["_embedded"].is_object())
        return false;
    if (pages)
        *pages = j.contains("pages") && j["pages"].is_number_integer() ? std::max(1, j["pages"].get<int>()) : 1;
    const json& emb = j["_embedded"];
    if (!emb.contains("items") || !emb["items"].is_array())
        return false;
    for (const json& it : emb["items"]) {
        if (!it.is_object() || !it.contains("id") || !it["id"].is_number_integer())
            continue;
        WallabagEntry e;
        e.id = it["id"].get<int64_t>();
        if (e.id <= 0)
            continue;
        if (it.contains("title") && it["title"].is_string())
            e.title = it["title"].get<std::string>();
        if (it.contains("created_at") && it["created_at"].is_string())
            e.created = parse_iso8601(it["created_at"].get<std::string>());
        out.push_back(e);
    }
    return true;
}

bool parse_article(const std::string& body, WallabagArticle& a) {
    a = WallabagArticle{};
    json j = json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("id") || !j["id"].is_number_integer())
        return false;
    a.id = j["id"].get<int64_t>();
    a.title = json_string(j, "title");
    a.url = json_string(j, "url");
    a.domain = json_string(j, "domain_name");
    a.language = json_string(j, "language");
    a.content = json_string(j, "content");
    a.preview_picture = json_string(j, "preview_picture");
    a.published = parse_iso8601(json_string(j, "published_at"));
    a.created = parse_iso8601(json_string(j, "created_at"));
    if (j.contains("reading_time") && j["reading_time"].is_number_integer())
        a.reading_time = std::max(0, j["reading_time"].get<int>());
    if (j.contains("published_by") && j["published_by"].is_array())
        for (const json& p : j["published_by"])
            if (p.is_string() && !trim(p.get<std::string>()).empty())
                a.authors.push_back(trim(p.get<std::string>()));
    return a.id > 0;
}

ArticleBook article_book(const WallabagArticle& a) {
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    ArticleBook b;
    b.id = a.id;
    b.url = a.url;
    std::string site = a.domain.empty() ? url_host(a.url) : a.domain;
    if (starts_with(to_lower(site), "www."))
        site = site.substr(4);
    b.title = !a.title.empty() ? a.title : !site.empty() ? site : std::string("Untitled article");
    std::string authors;
    for (const std::string& p : a.authors)
        authors += (authors.empty() ? "" : ", ") + p;
    b.author = authors.empty() ? site : authors;
    // "en_US" -> "en-US"; anything odd is left out.
    std::string lang = a.language;
    std::replace(lang.begin(), lang.end(), '_', '-');
    if (lang.size() <= 20 && std::all_of(lang.begin(), lang.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '-';
        }))
        b.language = lang;
    std::vector<std::string> parts;
    if (!authors.empty())
        parts.push_back(authors);
    if (!site.empty())
        parts.push_back(site);
    if (a.published > 0) {
        time_t t = static_cast<time_t>(a.published);
        std::tm tm{};
        gmtime_r(&t, &tm);
        parts.push_back(strf("%d %s %d", tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900));
    }
    if (a.reading_time > 0)
        parts.push_back(strf("%d min read", a.reading_time));
    for (const std::string& p : parts)
        b.byline += (b.byline.empty() ? "" : " \xC2\xB7 ") + p;
    b.html = a.content;
    return b;
}

namespace {

// Downloads the article's pictures (and its preview picture, the cover)
// into the book, and points the HTML at them. A picture that can't be had
// is left out. False only when cancelled.
// `server` is the wallabag server's host name: pictures wallabag stored
// itself come from there, wherever it is (a server at home too); others
// only from the internet (D31).
bool add_pictures(HttpClient& http, const WallabagArticle& a, const std::string& server, ArticleBook& book,
                  const std::atomic<bool>& cancel, int* left_out) {
    // The preview picture (the cover) first: it is never the one left out.
    std::vector<std::string> urls;
    const std::string preview = resolve_url(a.url, a.preview_picture);
    if (!preview.empty())
        urls.push_back(preview);
    for (std::string& u : article_images(a.content, a.url))
        if (u != preview)
            urls.push_back(std::move(u));
    if (urls.size() > MAX_PICTURES) {
        *left_out += static_cast<int>(urls.size() - MAX_PICTURES);
        urls.resize(MAX_PICTURES);
    }
    std::map<std::string, std::string> local;
    std::set<std::string> dead_hosts; // don't wait for an unreachable host twice
    size_t total = 0;
    int n = 0;
    for (const std::string& url : urls) {
        const std::string host = url_host(url);
        if (dead_hosts.count(host)) {
            (*left_out)++;
            continue;
        }
        // A plain GET: no wallabag token, no cookies, redirects followed.
        HttpRequest q;
        q.url = url;
        q.max_bytes = MAX_PICTURE;
        q.timeout_s = 60;
        q.connect_timeout_s = 10;
        q.low_speed_s = 15;
        q.follow_redirects = true;
        q.public_only = host_name(host) != server;
        q.headers = {"Accept: image/webp,image/png,image/jpeg,image/gif,image/svg+xml;q=0.9,*/*;q=0.5"};
        HttpResponse res = http.request(q, &cancel);
        if (res.cancelled)
            return false;
        if (res.unreachable) // not for one bad address or a picture too big
            dead_hosts.insert(host);
        std::string media, ext;
        if (!res.error.empty() || res.status != 200 || !image_type(res.body, &media, &ext) ||
            total + res.body.size() > MAX_PICTURES_TOTAL) {
            LOG_D("wallabag: picture %s left out (%s)", url.c_str(),
                  !res.error.empty() ? res.error.c_str() : strf("HTTP %ld, %zu bytes", res.status, res.body.size()).c_str());
            (*left_out)++;
            continue;
        }
        total += res.body.size();
        std::string href = strf("images/%d.%s", ++n, ext.c_str());
        local[url] = href;
        if (url == preview && media != "image/svg+xml")
            book.cover = href;
        book.images.push_back({href, media, std::move(res.body)});
    }
    book.html = rewrite_html(a.content, a.url, local);
    return true;
}

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

SyncPlan plan_sync(const std::vector<WallabagEntry>& unread, const std::vector<int64_t>& local,
                   const std::vector<int64_t>& archive) {
    SyncPlan plan;
    std::set<int64_t> have(local.begin(), local.end()), skip(archive.begin(), archive.end()), want;
    for (const WallabagEntry& e : unread) {
        if (skip.count(e.id) || !want.insert(e.id).second)
            continue;
        if (!have.count(e.id))
            plan.download.push_back(e);
    }
    for (int64_t id : have)
        if (!want.count(id))
            plan.remove.push_back(id);
    return plan;
}

// --- a sync ---------------------------------------------------------------------

SyncResult run_sync(const SyncRequest& req, const std::atomic<bool>& cancel,
                    const std::function<void(const SyncProgress&)>& progress) {
    SyncResult r;
    r.update = req.update;
    SyncProgress p;
    auto report = [&] {
        if (progress)
            progress(p);
    };
    const WallabagSettings& s = req.settings;
    const std::string base = wallabag_base(s.server);
    if (base.empty()) {
        r.error = "The wallabag server address must start with https:// or http://.";
        return r;
    }
    const std::string server = host_name(url_host(base));
    report();
    HttpClient http; // one set of connections for the whole sync

    // A new token each time: nothing to store, and the password is needed
    // anyway once the refresh token expires (two weeks by default).
    HttpRequest tq;
    tq.method = "POST";
    tq.url = base + "/oauth/v2/token";
    tq.content_type = "application/x-www-form-urlencoded";
    tq.headers = {"Accept: application/json"};
    tq.body = form_encode({{"grant_type", "password"},
                           {"client_id", s.client_id},
                           {"client_secret", s.client_secret},
                           {"username", s.username},
                           {"password", s.password}});
    tq.max_bytes = MAX_JSON;
    HttpResponse tr = http.request(tq, &cancel);
    if (tr.cancelled) {
        r.cancelled = true;
        return r;
    }
    if (!tr.error.empty()) {
        r.error = tr.error;
        return r;
    }
    const std::string token = tr.status == 200 ? parse_token(tr.body) : "";
    if (token.empty()) {
        r.error = token_error(tr.status, tr.body, tr.location);
        LOG_W("wallabag login: HTTP %ld", tr.status);
        return r;
    }
    auto api = [&](const char* method, const std::string& path, size_t max_bytes, long timeout_s) {
        HttpRequest q;
        q.method = method;
        q.url = base + path;
        q.headers = {"Authorization: Bearer " + token};
        q.max_bytes = max_bytes;
        q.timeout_s = timeout_s;
        if (q.method == "PATCH") {
            q.content_type = "application/x-www-form-urlencoded";
            q.body = "archive=1";
        }
        return http.request(q, &cancel);
    };
    // A transport problem or a refusal ends the sync; the reason is kept.
    auto failed = [&](const HttpResponse& res, const char* what) {
        if (res.cancelled) {
            r.cancelled = true;
        } else if (!res.error.empty()) {
            r.error = res.error;
        } else if (res.status == 401 || res.status == 403) {
            r.error = strf("wallabag refused %s (HTTP %ld).", what, res.status);
        } else {
            r.error = strf("%s failed: wallabag answered HTTP %ld.", what, res.status);
            r.error[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(r.error[0])));
        }
    };

    // Articles marked as read on the Deck, first: they must not come back.
    if (!req.archive.empty()) {
        p.phase = SyncProgress::Archiving;
        p.total = static_cast<int>(req.archive.size());
        report();
        for (int64_t id : req.archive) {
            HttpResponse res = api("PATCH", "/api/entries/" + std::to_string(id) + ".json", MAX_JSON, 60);
            if (res.error.empty() && !res.cancelled && (res.status == 200 || res.status == 404)) {
                r.archived.push_back(id); // 404: deleted in wallabag, nothing left to do
            } else {
                failed(res, "marking an article as read");
                return r;
            }
            p.done++;
            report();
        }
    }
    if (!req.update)
        return r;

    p = SyncProgress{};
    p.phase = SyncProgress::Listing;
    report();
    // Page after page until max_articles: the whole list must come, or
    // articles on later pages would look read and be removed.
    std::vector<WallabagEntry> unread;
    const int per_page = std::min(s.max_articles, LIST_PAGE);
    for (int page = 1;; page++) {
        HttpResponse lr = api("GET",
                              strf("/api/entries.json?archive=0&sort=created&order=desc&detail=metadata&page=%d&perPage=%d",
                                   page, per_page),
                              MAX_JSON, 60);
        if (!lr.error.empty() || lr.cancelled || lr.status != 200) {
            failed(lr, "getting the list of articles");
            return r;
        }
        std::vector<WallabagEntry> items;
        int pages = 1;
        if (!parse_entries(lr.body, items, &pages)) {
            r.error = "wallabag's list of articles could not be read.";
            return r;
        }
        for (WallabagEntry& e : items)
            if (static_cast<int>(unread.size()) < s.max_articles)
                unread.push_back(std::move(e));
        if (items.empty() || page >= pages || static_cast<int>(unread.size()) >= s.max_articles)
            break;
    }
    r.listed = true;
    r.unread = static_cast<int>(unread.size());
    SyncPlan plan = plan_sync(unread, req.local, req.archive);
    r.remove = plan.remove;

    p.phase = SyncProgress::Downloading;
    p.total = static_cast<int>(plan.download.size());
    report();
    for (const WallabagEntry& e : plan.download) {
        const uint64_t t0 = mono_ms();
        HttpResponse res = api("GET", "/api/entries/" + std::to_string(e.id) + ".json", MAX_ARTICLE_JSON, 120);
        if (res.cancelled || (!res.error.empty() && !res.too_large)) {
            failed(res, "downloading an article");
            return r;
        }
        WallabagArticle article;
        const bool ok = !res.too_large && res.status == 200 && parse_article(res.body, article);
        const size_t json_size = res.body.size();
        std::string().swap(res.body); // the article is in `article` now
        if (!ok) {
            LOG_W("wallabag: article %lld not downloaded (HTTP %ld, %zu bytes)", static_cast<long long>(e.id),
                  res.status, json_size);
            r.failed++;
        } else {
            ArticleBook book = article_book(article);
            int left_out = 0;
            if (s.images && !add_pictures(http, article, server, book, cancel, &left_out)) {
                r.cancelled = true;
                return r;
            }
            if (!s.images)
                book.html = rewrite_html(article.content, article.url, {}); // no broken squares
            const std::string path = path_join(req.dir, article_file_name(e.id));
            std::string err;
            const size_t pictures = book.images.size();
            const std::string epub = make_article_epub(std::move(book));
            if (!write_file_atomic(path, epub, &err)) {
                r.error = "Couldn't save the articles: " + err;
                return r;
            }
            LOG_I("wallabag: article %lld: %zu pictures (%d left out), %zu KB in %llu ms", static_cast<long long>(e.id),
                  pictures, left_out, epub.size() >> 10, static_cast<unsigned long long>(mono_ms() - t0));
            // Dated when it was saved in wallabag: "Sorted by newest" uses it.
            if (e.created > 0) {
                timespec times[2] = {{static_cast<time_t>(e.created), 0}, {static_cast<time_t>(e.created), 0}};
                utimensat(AT_FDCWD, path.c_str(), times, 0);
            }
            r.downloaded++;
        }
        p.done++;
        report();
    }
    return r;
}

SyncWorker::~SyncWorker() {
    cancel_ = true;
    if (thread_.joinable())
        thread_.join();
}

bool SyncWorker::start(SyncRequest req, std::function<void()> wake) {
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
    thread_ = std::thread([this, req = std::move(req), wake = std::move(wake)] {
        SyncResult r = run_sync(req, cancel_, [&](const SyncProgress& p) {
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
