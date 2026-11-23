#include "wikipedia.h"

#include "fsutil.h"
#include "http.h"
#include "log.h"
#include "util.h"
#include "wallabag.h" // parse_iso8601

#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <set>

#include "nlohmann/json.hpp"

namespace dr {

using json = nlohmann::json;

namespace {
constexpr size_t MAX_JSON = 16u << 20;
constexpr size_t MAX_PAGE = 8u << 20;   // Parsoid's HTML of one article (long ones: 1–1.5 MB)
constexpr int MAX_LIST_PAGES = 100;     // 10 lists a page
constexpr int MAX_ENTRY_PAGES = 200;    // of one list (the API allows 5000 entries, 100 a page)
constexpr long STALL_S = 30;            // below 1 KB/s this long: given up (V4-SEC-1, D38)
constexpr size_t MAX_ENTRIES = 20000;   // in all the lists read
constexpr int ENTRY_PAGE = 100;         // the API's most

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}
} // namespace

// --- the lists ------------------------------------------------------------------

bool parse_wiki_lists(const std::string& body, std::vector<WikiList>& out, std::string* next) {
    out.clear();
    if (next)
        next->clear();
    json j = json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("lists") || !j["lists"].is_array())
        return false;
    for (const json& l : j["lists"]) {
        if (!l.is_object() || !l.contains("id") || !l["id"].is_number_integer() || l["id"].get<int64_t>() <= 0)
            continue;
        WikiList w;
        w.id = l["id"].get<int64_t>();
        if (l.contains("name") && l["name"].is_string())
            w.name = trim(l["name"].get<std::string>());
        w.is_default = l.contains("default") && l["default"].is_boolean() && l["default"].get<bool>();
        out.push_back(w);
    }
    if (next && j.contains("next") && j["next"].is_string())
        *next = j["next"].get<std::string>();
    return true;
}

bool parse_wiki_entries(const std::string& body, std::vector<WikiEntry>& out, std::string* next) {
    out.clear();
    if (next)
        next->clear();
    json j = json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("entries") || !j["entries"].is_array())
        return false;
    for (const json& e : j["entries"]) {
        if (!e.is_object() || !e.contains("id") || !e["id"].is_number_integer())
            continue;
        if (e.contains("deleted") && e["deleted"].is_boolean() && e["deleted"].get<bool>())
            continue;
        WikiEntry w;
        w.id = e["id"].get<int64_t>();
        if (e.contains("listId") && e["listId"].is_number_integer())
            w.list = e["listId"].get<int64_t>();
        if (e.contains("project") && e["project"].is_string())
            w.project = trim(e["project"].get<std::string>());
        if (e.contains("title") && e["title"].is_string())
            w.title = trim(e["title"].get<std::string>());
        if (e.contains("created") && e["created"].is_string())
            w.created = parse_iso8601(e["created"].get<std::string>());
        if (w.id > 0 && !w.title.empty())
            out.push_back(w);
    }
    if (next && j.contains("next") && j["next"].is_string())
        *next = j["next"].get<std::string>();
    return true;
}

std::string wiki_error(long status, const std::string& body) {
    json j = json::parse(body, nullptr, false);
    std::string key;
    if (j.is_object())
        for (const char* k : {"errorKey", "error"})
            if (j.contains(k) && j[k].is_string()) {
                key = j[k].get<std::string>();
                break;
            }
    if (key == "readinglists-db-error-not-set-up")
        return "No reading lists in this Wikipedia account yet: turn on Sync reading lists in the Wikipedia app.";
    if (status == 401 || status == 403 || key.find("permission") != std::string::npos ||
        key.find("oauth") != std::string::npos)
        return strf("Wikipedia refused the token (HTTP %ld). Check token in [wikipedia] in config.ini.", status);
    if (status == 200)
        return "Wikipedia's reading lists could not be read.";
    return strf("Wikipedia answered HTTP %ld to the reading lists.", status);
}

std::string wiki_host(const std::string& project) {
    std::string p = to_lower(trim(project));
    if (!starts_with(p, "https://"))
        return "";
    p = p.substr(8);
    if (!p.empty() && p.back() == '/')
        p.pop_back();
    const std::string tail = ".wikipedia.org";
    if (p.size() <= tail.size() || p.compare(p.size() - tail.size(), tail.size(), tail) != 0)
        return "";
    const std::string lang = p.substr(0, p.size() - tail.size());
    // "en", "simple", "zh-min-nan", "be-tarask": letters, digits, hyphens.
    if (lang.size() > 20 || lang.front() == '-' || lang.back() == '-' || !std::all_of(lang.begin(), lang.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        }))
        return "";
    return p;
}

bool wiki_list_wanted(const WikiList& list, const std::vector<std::string>& names) {
    if (names.empty())
        return true;
    const std::string name = to_lower(list.name);
    for (const std::string& n : names) {
        const std::string want = to_lower(trim(n));
        if (want == name || (list.is_default && (want == "saved" || want == "default")))
            return true;
    }
    return false;
}

int64_t wiki_article_id(const std::string& host, const std::string& title) {
    std::string t = title;
    std::replace(t.begin(), t.end(), '_', ' ');
    const int64_t id = static_cast<int64_t>(fnv1a64(to_lower(host) + "\n" + t) & ((uint64_t(1) << 53) - 1));
    return id ? id : 1;
}

namespace {
// Each page once, dated when it was first saved, newest first.
std::vector<WikiArticle> newest_pages(const std::vector<WikiEntry>& entries, int* skipped) {
    std::map<int64_t, WikiArticle> pages;
    int bad = 0;
    for (const WikiEntry& e : entries) {
        const std::string host = wiki_host(e.project);
        if (host.empty()) {
            bad++;
            continue;
        }
        const int64_t id = wiki_article_id(host, e.title);
        auto it = pages.find(id);
        if (it == pages.end()) {
            WikiArticle a;
            a.id = id;
            a.title = e.title;
            a.created = e.created;
            a.host = host;
            pages.emplace(id, a);
        } else if (e.created > 0 && (it->second.created == 0 || e.created < it->second.created)) {
            it->second.created = e.created; // in several lists: when it was first saved
        }
    }
    std::vector<WikiArticle> out;
    out.reserve(pages.size());
    for (auto& [id, a] : pages)
        out.push_back(std::move(a));
    std::stable_sort(out.begin(), out.end(), [](const WikiArticle& a, const WikiArticle& b) {
        return a.created != b.created ? a.created > b.created : a.id < b.id;
    });
    *skipped += bad;
    return out;
}
} // namespace

WikiShelf wiki_choose(const std::vector<WikiList>& lists, const std::vector<WikiEntry>& entries, int max_per_list,
                      int* skipped) {
    WikiShelf shelf;
    int bad = 0;
    std::map<int64_t, WikiArticle> chosen;
    for (const WikiList& l : lists) {
        std::vector<WikiEntry> mine;
        for (const WikiEntry& e : entries)
            if (e.list == l.id)
                mine.push_back(e);
        std::vector<WikiArticle> pages = newest_pages(mine, &bad);
        if (max_per_list >= 0 && pages.size() > static_cast<size_t>(max_per_list))
            pages.resize(static_cast<size_t>(max_per_list));
        if (pages.empty())
            continue;
        ArticleList folder;
        folder.id = l.id;
        // The name is shown as it is: valid UTF-8, not too long.
        folder.name = l.is_default ? std::string("Saved") : utf8_sanitize(trim(l.name).substr(0, 120));
        if (folder.name.empty())
            folder.name = strf("List %lld", static_cast<long long>(l.id));
        for (const WikiArticle& a : pages) {
            folder.articles.push_back(a.id);
            auto it = chosen.find(a.id);
            if (it == chosen.end())
                chosen.emplace(a.id, a);
            else if (a.created > 0 && (it->second.created == 0 || a.created < it->second.created))
                it->second.created = a.created;
        }
        shelf.lists.push_back(std::move(folder));
    }
    for (auto& [id, a] : chosen)
        shelf.articles.push_back(std::move(a));
    std::stable_sort(shelf.articles.begin(), shelf.articles.end(), [](const WikiArticle& a, const WikiArticle& b) {
        return a.created != b.created ? a.created > b.created : a.id < b.id;
    });
    if (skipped)
        *skipped = bad;
    return shelf;
}

std::string wiki_title_path(const std::string& title) {
    std::string t = trim(title);
    std::replace(t.begin(), t.end(), ' ', '_');
    return url_encode(t);
}

// --- the page ---------------------------------------------------------------------

namespace {

struct Attr {
    std::string name, value; // value as written (entities and all)
};

struct StartTag {
    std::string name;
    std::vector<Attr> attrs;
    bool self_closing = false;
    size_t end = 0;
    const std::string* get(const char* n) const {
        for (const Attr& a : attrs)
            if (a.name == n)
                return &a.value;
        return nullptr;
    }
};

// The start tag at h[pos] == '<'. Quotes are respected: Parsoid's data-mw
// attributes hold JSON, with '>' in it. False if it isn't a start tag.
bool read_start_tag(const std::string& h, size_t pos, size_t end, StartTag& t) {
    size_t i = pos + 1;
    if (i >= end || !std::isalpha(static_cast<unsigned char>(h[i])))
        return false;
    const size_t s = i;
    while (i < end && (std::isalnum(static_cast<unsigned char>(h[i])) || h[i] == '-' || h[i] == ':' || h[i] == '_'))
        i++;
    t.name = to_lower(h.substr(s, i - s));
    t.attrs.clear();
    t.self_closing = false;
    for (;;) {
        while (i < end && is_space(h[i]))
            i++;
        if (i >= end)
            return false;
        if (h[i] == '>') {
            t.end = i + 1;
            return true;
        }
        if (h[i] == '/') {
            if (i + 1 < end && h[i + 1] == '>') {
                t.self_closing = true;
                t.end = i + 2;
                return true;
            }
            i++;
            continue;
        }
        const size_t as = i;
        while (i < end && !is_space(h[i]) && h[i] != '=' && h[i] != '>' && h[i] != '/')
            i++;
        Attr a;
        a.name = to_lower(h.substr(as, i - as));
        while (i < end && is_space(h[i]))
            i++;
        if (i < end && h[i] == '=') {
            i++;
            while (i < end && is_space(h[i]))
                i++;
            if (i < end && (h[i] == '"' || h[i] == '\'')) {
                const size_t e = h.find(h[i], i + 1);
                if (e == std::string::npos || e >= end)
                    return false;
                a.value = h.substr(i + 1, e - i - 1);
                i = e + 1;
            } else {
                const size_t vs = i;
                while (i < end && !is_space(h[i]) && h[i] != '>')
                    i++;
                a.value = h.substr(vs, i - vs);
            }
        }
        if (!a.name.empty())
            t.attrs.push_back(std::move(a));
        else if (i == as)
            i++; // a stray character
    }
}

bool is_void(const std::string& n) {
    static const std::set<std::string> v = {"area", "base", "br",   "col",   "embed", "hr",    "img",
                                            "input", "link", "meta", "param", "source", "track", "wbr"};
    return v.count(n) != 0;
}

// Whole elements left out: scripts and styles, what needs a browser, and
// what only the editor or the site's pages use.
bool drop_element(const std::string& n) {
    static const std::set<std::string> d = {
        "script", "style",  "link",   "meta",   "noscript", "template", "video",  "audio",    "iframe", "object",
        "embed",  "canvas", "map",    "form",   "input",    "button",   "select", "textarea", "svg",    "base",
        "title",  "head",   "source", "track"};
    return d.count(n) != 0;
}

// Boxes for navigating the site, notices for editors, and what Wikipedia
// itself hides on phones or in print.
bool drop_class(const std::string& classes) {
    static const std::set<std::string> d = {
        "navbox",        "navbox-styles", "vertical-navbox", "navbar",          "sidebar",
        "sistersitebox", "side-box",      "metadata",        "ambox",           "ombox",
        "tmbox",         "cmbox",         "fmbox",           "imbox",           "mbox-small",
        "hatnote",       "dablink",       "shortdescription", "noprint",        "nomobile",
        "mw-empty-elt",  "mw-editsection", "authority-control", "portalbox",    "portal",
        "mw-cite-backlink", "Z3988",      "geo-default",     "geo-nondefault",  "geo-multi-punct",
        "mw-kartographer-maplink", "navigation-not-searchable", "succession-box", "printfooter", "catlinks"};
    size_t i = 0;
    while (i < classes.size()) {
        while (i < classes.size() && is_space(classes[i]))
            i++;
        size_t e = i;
        while (e < classes.size() && !is_space(classes[e]))
            e++;
        if (e > i && d.count(classes.substr(i, e - i)))
            return true;
        i = e;
    }
    return false;
}

// Sections dropped whole, by their heading's id: the links to other sites.
bool external_links_heading(const std::string& id) {
    static const std::set<std::string> ids = {"External_links",   "Enlaces_externos", "Liens_externes",
                                              "Weblinks",         "Collegamenti_esterni", "Ligações_externas",
                                              "Links_externos",   "Externe_links"};
    return ids.count(id) != 0;
}

// An attribute value for the output: quotes and angle brackets as entities,
// a bare '&' as &amp; (Parsoid writes '>' unescaped in attributes).
std::string attr_value(const std::string& v) {
    std::string o;
    o.reserve(v.size());
    for (size_t i = 0; i < v.size(); i++) {
        const char c = v[i];
        if (c == '"')
            o += "&quot;";
        else if (c == '<')
            o += "&lt;";
        else if (c == '>')
            o += "&gt;";
        else if (c == '&') {
            size_t j = i + 1;
            if (j < v.size() && v[j] == '#')
                j++;
            while (j < v.size() && j - i < 32 && std::isalnum(static_cast<unsigned char>(v[j])))
                j++;
            o += j < v.size() && v[j] == ';' && j > i + 1 ? "&" : "&amp;";
        } else
            o += c;
    }
    return o;
}

// The sharpest candidate of a srcset ("url 1.5x, url 2x", or "url 400w,
// url 800w": the widest): the address runs to white space, its descriptor
// to the comma.
std::string densest(const std::string& srcset) {
    std::string best, best_w;
    double best_x = 0, widest = 0;
    size_t i = 0;
    while (i < srcset.size()) {
        while (i < srcset.size() && (is_space(srcset[i]) || srcset[i] == ','))
            i++;
        size_t u = i;
        while (i < srcset.size() && !is_space(srcset[i]))
            i++;
        std::string url = srcset.substr(u, i - u);
        while (!url.empty() && url.back() == ',')
            url.pop_back();
        size_t d = i;
        while (i < srcset.size() && srcset[i] != ',')
            i++;
        const std::string desc = trim(srcset.substr(d, i - d));
        if (url.empty())
            continue;
        if (!desc.empty() && (desc.back() == 'w' || desc.back() == 'W')) {
            const double w = std::atof(desc.c_str());
            if (w > widest) {
                widest = w;
                best_w = url;
            }
            continue;
        }
        double x = 1;
        if (!desc.empty() && (desc.back() == 'x' || desc.back() == 'X'))
            x = std::atof(desc.c_str());
        if (x > best_x) {
            best_x = x;
            best = url;
        }
    }
    return !best_w.empty() ? best_w : best;
}

int int_attr(const StartTag& t, const char* n) {
    const std::string* v = t.get(n);
    return v ? std::atoi(v->c_str()) : 0;
}

// The first "</name>" from `from` on, in any case (not "</names>");
// npos if none before `end`.
size_t find_end_tag(const std::string& h, const std::string& name, size_t from, size_t end) {
    for (size_t p = h.find("</", from); p != std::string::npos && p + 2 + name.size() < end; p = h.find("</", p + 2)) {
        const char after = h[p + 2 + name.size()];
        if (to_lower(h.substr(p + 2, name.size())) == name && (after == '>' || after == '/' || is_space(after)))
            return p;
    }
    return std::string::npos;
}

// Text of some HTML: the tags removed (entities kept).
std::string strip_tags(const std::string& h) {
    std::string o;
    bool in_tag = false;
    for (char c : h) {
        if (c == '<')
            in_tag = true;
        else if (c == '>' && in_tag)
            in_tag = false;
        else if (!in_tag)
            o += c;
    }
    return trim(o);
}

bool has_class(const std::string& classes, const char* name) {
    const std::string n = name;
    for (size_t i = classes.find(n); i != std::string::npos; i = classes.find(n, i + 1))
        if ((i == 0 || is_space(classes[i - 1])) &&
            (i + n.size() == classes.size() || is_space(classes[i + n.size()])))
            return true;
    return false;
}

// The article's first paragraph goes before its infobox, as on Wikipedia's
// mobile pages: the book starts with text, not with a screen of table.
// Only a paragraph beside the infobox moves, never one from a caption, a
// table or a quote; and only in the lead, before the first h2 (D38).
void lead_first(std::string& h) {
    const size_t lead_end = std::min({h.find("</section>"), h.find("<h2"), h.size()});
    // Walks the tags in [from, to) from `depth`, calling fn(tag start, tag,
    // depth before it) for each start tag; fn returns false to stop. The
    // depth is the walker's: the output is well formed and has no comments.
    auto walk = [&](size_t from, size_t to, int depth, auto fn) {
        for (size_t i = h.find('<', from); i != std::string::npos && i < to; i = h.find('<', i + 1)) {
            if (h.compare(i, 2, "</") == 0) {
                depth--;
                continue;
            }
            StartTag st;
            if (!read_start_tag(h, i, h.size(), st))
                continue;
            if (!fn(i, st, depth))
                return depth;
            if (!st.self_closing && !is_void(st.name))
                depth++;
            i = st.end - 1;
        }
        return depth;
    };
    size_t box = std::string::npos;
    int box_depth = 0;
    walk(0, lead_end, 0, [&](size_t at, const StartTag& st, int d) {
        if (st.name == "table" && st.get("class") && has_class(*st.get("class"), "infobox")) {
            box = at;
            box_depth = d;
            return false;
        }
        return true;
    });
    if (box == std::string::npos)
        return;
    // A paragraph with text, beside the infobox (at its depth).
    auto paragraph = [&](size_t from, size_t to, int depth, size_t* end) {
        size_t found = std::string::npos;
        walk(from, to, depth, [&](size_t at, const StartTag& st, int d) {
            if (d < box_depth)
                return false; // left the infobox's parent
            if (st.name != "p" || d != box_depth || st.self_closing)
                return true;
            const size_t e = h.find("</p>", st.end); // paragraphs don't nest
            if (e == std::string::npos)
                return false;
            if (strip_tags(h.substr(st.end, e - st.end)).empty())
                return true;
            found = at;
            *end = e + 4;
            return false;
        });
        return found;
    };
    size_t end = 0;
    if (paragraph(0, box, 0, &end) != std::string::npos)
        return; // the text comes first already
    // The end of the infobox: tables nest.
    int depth = 1;
    size_t pos = box + 6;
    while (depth > 0) {
        const size_t open = h.find("<table", pos), close = h.find("</table>", pos);
        if (close == std::string::npos)
            return;
        if (open != std::string::npos && open < close) {
            depth++;
            pos = open + 6;
        } else {
            depth--;
            pos = close + 8;
        }
    }
    const size_t p = paragraph(pos, lead_end, box_depth, &end);
    if (p == std::string::npos)
        return;
    h = h.substr(0, box) + h.substr(p, end - p) + h.substr(box, p - box) + h.substr(end);
}

} // namespace

bool wiki_clean_page(const std::string& h, WikiPage& page) {
    page = WikiPage{};
    const size_t body = h.find("<body");
    if (body == std::string::npos)
        return false;
    StartTag bt;
    if (!read_start_tag(h, body, h.size(), bt))
        return false;
    size_t end = h.rfind("</body>");
    if (end == std::string::npos || end < bt.end)
        end = h.size();
    if (const std::string* l = bt.get("lang"))
        page.language = *l;

    // From the head: the title and the revision's date.
    const size_t ts = h.find("<title>");
    if (ts != std::string::npos && ts < body) {
        const size_t te = h.find("</title>", ts);
        if (te != std::string::npos && te < body)
            page.title = trim(decode_entities(strip_tags(h.substr(ts + 7, te - ts - 7))));
    }
    for (size_t p = h.find("<meta"); p != std::string::npos && p < body; p = h.find("<meta", p + 5)) {
        StartTag m;
        if (read_start_tag(h, p, body, m) && m.get("property") && *m.get("property") == "dc:modified" &&
            m.get("content"))
            page.modified = parse_iso8601(*m.get("content"));
    }

    struct Open {
        std::string name;
        bool keep = false;
        size_t out_pos = 0;  // where its start tag went into the output
        size_t toc_size = 0; // headings found before it
    };
    std::string& out = page.html;
    out.reserve((end - bt.end) / 3);
    std::vector<Open> stack;
    // Inside a dropped element: the elements open in it, by name, so a stray
    // end tag in it doesn't end it early (V4-BUG-5, D38).
    std::vector<std::string> dropped;
    int math = 0; // open <math> elements
    size_t heading_pos = std::string::npos;
    ArticleSection heading;

    // Drops what was written since an open element started, and the rest of
    // it: its end tag (and those of the elements open inside it) end the drop.
    auto drop_from = [&](size_t k, const std::string& open_now) {
        out.resize(stack[k].out_pos);
        page.toc.resize(stack[k].toc_size); // no contents entries for what went
        dropped.clear();
        for (size_t j = k; j < stack.size(); j++)
            dropped.push_back(stack[j].name);
        while (stack.size() > k) {
            if (stack.back().name == "math")
                math--;
            stack.pop_back();
        }
        if (!open_now.empty())
            dropped.push_back(open_now);
        heading_pos = std::string::npos;
    };

    size_t i = bt.end;
    while (i < end) {
        const size_t lt = h.find('<', i);
        const size_t stop = lt == std::string::npos || lt >= end ? end : lt;
        if (dropped.empty())
            out.append(h, i, stop - i);
        if (stop >= end)
            break;
        i = lt;
        if (h.compare(i, 4, "<!--") == 0) {
            const size_t e = h.find("-->", i + 4);
            i = e == std::string::npos ? end : e + 3;
            continue;
        }
        if (h.compare(i, 2, "</") == 0) {
            const size_t gt = h.find('>', i);
            if (gt == std::string::npos)
                break;
            std::string name = to_lower(trim(h.substr(i + 2, gt - i - 2)));
            i = gt + 1;
            if (is_void(name))
                continue;
            if (!dropped.empty()) {
                size_t k = dropped.size();
                while (k > 0 && dropped[k - 1] != name)
                    k--;
                if (k > 0) // a stray end tag inside is ignored
                    dropped.resize(k - 1);
                continue;
            }
            // Close up to the matching element (Parsoid's HTML is well formed).
            size_t k = stack.size();
            while (k > 0 && stack[k - 1].name != name)
                k--;
            if (k == 0)
                continue; // nothing open by that name
            while (stack.size() >= k) {
                const Open o = stack.back();
                stack.pop_back();
                if (o.name == "math")
                    math--;
                if (!o.keep)
                    continue;
                out += "</" + o.name + ">";
                if (heading_pos != std::string::npos && (o.name == "h2" || o.name == "h3")) {
                    heading.text = strip_tags(out.substr(heading_pos));
                    if (!heading.text.empty())
                        page.toc.push_back(heading);
                    heading_pos = std::string::npos;
                }
            }
            continue;
        }
        if (h[i] == '<' && (h.compare(i, 2, "<!") == 0 || h.compare(i, 2, "<?") == 0)) {
            const size_t gt = h.find('>', i);
            i = gt == std::string::npos ? end : gt + 1;
            continue;
        }
        StartTag t;
        if (!read_start_tag(h, i, end, t)) {
            if (dropped.empty())
                out += "&lt;";
            i++;
            continue;
        }
        i = t.end;
        const bool empty = t.self_closing || is_void(t.name);
        // Scripts and styles: straight to their end tag.
        if (!empty && (t.name == "script" || t.name == "style" || t.name == "textarea")) {
            const size_t e = find_end_tag(h, t.name, i, end);
            const size_t gt = e == std::string::npos ? e : h.find('>', e);
            i = gt == std::string::npos ? end : gt + 1;
            continue;
        }
        if (!dropped.empty()) {
            if (!empty)
                dropped.push_back(t.name);
            continue;
        }

        const std::string* cls = t.get("class");
        const std::string* id = t.get("id");
        std::string style = t.get("style") ? to_lower(*t.get("style")) : std::string();
        style.erase(std::remove_if(style.begin(), style.end(), is_space), style.end());
        bool drop = drop_element(t.name) || (cls && drop_class(*cls)) || style.find("display:none") != std::string::npos ||
                    (id && *id == "coordinates") || (t.get("role") && *t.get("role") == "navigation");
        // The external links section goes whole, from its <section>, when
        // the heading is that section's own (the first heading in it);
        // otherwise only the heading goes (V4-BUG-4, D38).
        if (!drop && (t.name == "h2" || t.name == "h3") && id && external_links_heading(*id)) {
            size_t k = stack.size();
            while (k > 0 && stack[k - 1].name != "section")
                k--;
            if (k > 0 && stack[k - 1].toc_size == page.toc.size()) {
                drop_from(k - 1, empty ? std::string() : t.name);
                continue;
            }
            drop = true;
        }
        // Small pictures: flags and icons.
        if (!drop && t.name == "img") {
            const int w = int_attr(t, "width"), ht = int_attr(t, "height");
            drop = w > 0 && ht > 0 && std::max(w, ht) < 48;
        }
        if (drop) {
            if (!empty)
                dropped = {t.name};
            continue;
        }

        // Links: notes stay, inside the book; the rest become their text
        // (other in-page links too: their targets' ids go, V4-BUG-9).
        std::string href;
        bool note = false;
        if (t.name == "a") {
            const std::string* hr = t.get("href");
            const size_t hash = hr ? hr->find("#cite_note-") : std::string::npos;
            if (hash != std::string::npos) {
                href = hr->substr(hash);
                note = true;
            } else {
                if (!empty)
                    stack.push_back({t.name, false, out.size(), page.toc.size()});
                continue;
            }
        }

        const size_t pos = out.size();
        out += "<" + t.name;
        std::set<std::string> written; // XML allows each attribute once (V4-BUG-5)
        if (t.name == "math" || math > 0) {
            // MathML: its own attributes, without the editor's.
            for (const Attr& a : t.attrs)
                if (!starts_with(a.name, "data-") && a.name != "id" && a.name != "about" && a.name != "typeof" &&
                    a.name != "class" && a.name != "style" && written.insert(a.name).second)
                    out += " " + a.name + "=\"" + attr_value(a.value) + "\"";
        } else {
            for (const Attr& a : t.attrs) {
                const std::string& n = a.name;
                if (written.count(n))
                    continue;
                bool keep = n == "class" || n == "lang" || n == "dir";
                if (n == "id")
                    keep = starts_with(a.value, "cite_note-") || starts_with(a.value, "cite_ref-") ||
                           t.name == "h2" || t.name == "h3" || t.name == "h4";
                if ((t.name == "td" || t.name == "th") && (n == "colspan" || n == "rowspan"))
                    keep = true;
                if ((t.name == "ol" && n == "start") || (t.name == "li" && n == "value") ||
                    (t.name == "abbr" && n == "title") || (t.name == "img" && n == "alt"))
                    keep = true;
                if (keep) {
                    out += " " + n + "=\"" + attr_value(a.value) + "\"";
                    written.insert(n);
                }
            }
            if (t.name == "a") {
                out += " href=\"" + attr_value(href) + "\"";
                if (note)
                    out += " role=\"doc-noteref\""; // D35 shows these in its panel
            }
            if (t.name == "li" && id && starts_with(*id, "cite_note-"))
                out += " role=\"doc-endnote\"";
            if (t.name == "img") {
                // The sharper picture of srcset (2x: 500 px for a 250 px figure).
                std::string src = t.get("srcset") ? densest(*t.get("srcset")) : std::string();
                if (src.empty() && t.get("src"))
                    src = *t.get("src");
                out += " src=\"" + attr_value(src) + "\"";
            }
        }
        out += empty ? "/>" : ">";
        if (!empty) {
            stack.push_back({t.name, true, pos, page.toc.size()});
            if (t.name == "math")
                math++;
            if ((t.name == "h2" || t.name == "h3") && id && !id->empty()) {
                heading = ArticleSection{*id, "", t.name == "h2" ? 2 : 3};
                heading_pos = out.size();
            }
        }
    }
    // Whatever is still open (a page cut short) is closed.
    while (!stack.empty()) {
        if (stack.back().keep)
            out += "</" + stack.back().name + ">";
        stack.pop_back();
    }
    lead_first(out);
    return true;
}

ArticleBook wiki_book(const WikiArticle& a, const WikiPage& p) {
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    auto date = [](int64_t s) {
        time_t t = static_cast<time_t>(s);
        std::tm tm{};
        gmtime_r(&t, &tm);
        return strf("%d %s %d", tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900);
    };
    ArticleBook b;
    b.id = a.id;
    b.source = "wikipedia";
    b.title = !p.title.empty() ? p.title : a.title;
    b.author = "Wikipedia";
    const std::string lang = p.language.empty() ? a.host.substr(0, a.host.find('.')) : p.language;
    if (lang.size() <= 20 && std::all_of(lang.begin(), lang.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '-';
        }))
        b.language = lang;
    b.url = "https://" + a.host + "/wiki/" + wiki_title_path(b.title);
    b.byline = a.host + (a.created > 0 ? " \xC2\xB7 saved " + date(a.created) : std::string());
    b.html = p.html;
    // The licence asks for the source and the licence (CC BY-SA 4.0).
    b.html += "\n<p class=\"licence\">From Wikipedia, the free encyclopedia (CC BY-SA 4.0)" +
              (p.modified > 0 ? ", the version of " + date(p.modified) : std::string()) + ".</p>";
    b.toc = p.toc;
    b.css = "table { border-collapse: collapse; margin: 0.8em 0; font-size: 0.85em; }\n"
            "th, td { border: 1px solid; padding: 0.15em 0.4em; vertical-align: top; text-indent: 0; }\n"
            "figure { margin: 1em 0; text-align: center; }\n"
            "figcaption { font-size: 0.85em; text-indent: 0; margin-top: 0.3em; }\n"
            "ol.references, ol.mw-references { font-size: 0.85em; }\n"
            "p.licence { font-size: 0.75em; text-indent: 0; margin-top: 2em; }\n";
    return b;
}

// --- a sync -----------------------------------------------------------------------

SyncResult run_wiki_sync(const WikiSyncRequest& req, const std::atomic<bool>& cancel, const SyncProgressFn& progress) {
    SyncResult r;
    SyncProgress p;
    auto report = [&] {
        if (progress)
            progress(p);
    };
    const WikipediaSettings& s = req.settings;
    report();
    HttpClient http; // one set of connections for the whole sync

    // The reading lists: only to this address, with the token.
    auto api = [&](const std::string& path) {
        HttpRequest q;
        q.url = req.lists_api + path;
        q.headers = {"Authorization: Bearer " + s.token, "Accept: application/json"};
        q.max_bytes = MAX_JSON;
        q.timeout_s = 60;
        q.low_speed_s = STALL_S;
        return http.request(q, &cancel);
    };
    auto failed = [&](const HttpResponse& res) {
        if (res.cancelled) {
            r.cancelled = true;
            return;
        }
        r.error = !res.error.empty() ? res.error : wiki_error(res.status, res.body);
        LOG_W("wikipedia: reading lists: HTTP %ld", res.status);
    };

    p.phase = SyncProgress::Listing;
    report();
    // Pages are followed to the last ("next" empty), not to an empty page
    // (one may hold only entries left out); the same "next" twice, or too
    // many pages, end it. Then the lists weren't all read: nothing is
    // removed this time (V4-BUG-1, D38).
    bool complete = true;
    std::vector<WikiList> lists;
    std::string next;
    std::set<std::string> seen;
    for (int page = 0;; page++) {
        if (page == MAX_LIST_PAGES) {
            complete = false;
            break;
        }
        HttpResponse res = api("/lists" + (next.empty() ? std::string() : "?next=" + url_encode(next)));
        std::vector<WikiList> got;
        if (!res.error.empty() || res.cancelled || res.status != 200 || !parse_wiki_lists(res.body, got, &next)) {
            failed(res);
            return r;
        }
        lists.insert(lists.end(), got.begin(), got.end());
        if (next.empty())
            break;
        if (!seen.insert(next).second) {
            complete = false;
            break;
        }
    }
    std::vector<WikiEntry> entries;
    std::vector<WikiList> read;
    for (const WikiList& l : lists) {
        if (!wiki_list_wanted(l, s.lists))
            continue;
        if (entries.size() >= MAX_ENTRIES) {
            complete = false;
            break;
        }
        read.push_back(l);
        next.clear();
        seen.clear();
        for (int page = 0;; page++) {
            if (page == MAX_ENTRY_PAGES) {
                complete = false;
                break;
            }
            HttpResponse res = api(strf("/lists/%lld/entries?limit=%d&sort=updated&dir=descending",
                                        static_cast<long long>(l.id), ENTRY_PAGE) +
                                   (next.empty() ? std::string() : "&next=" + url_encode(next)));
            if (res.status == 404 && !res.cancelled && res.error.empty())
                break; // deleted meanwhile: its articles go
            std::vector<WikiEntry> got;
            if (!res.error.empty() || res.cancelled || res.status != 200 ||
                !parse_wiki_entries(res.body, got, &next)) {
                failed(res);
                return r;
            }
            entries.insert(entries.end(), got.begin(), got.end());
            if (next.empty())
                break;
            if (!seen.insert(next).second || entries.size() >= MAX_ENTRIES) {
                complete = false;
                break;
            }
        }
    }
    int skipped = 0;
    const WikiShelf shelf = wiki_choose(read, entries, s.max_articles, &skipped);
    const std::vector<WikiArticle>& chosen = shelf.articles;
    LOG_I("wikipedia: %zu lists, %zu read, %zu entries, %zu articles kept in %zu lists%s", lists.size(), read.size(),
          entries.size(), chosen.size(), shelf.lists.size(),
          skipped ? strf(" (%d not on Wikipedia left out)", skipped).c_str() : "");
    // Which list has which article, for the library's folders; written
    // now, so the folders fill as the articles arrive.
    std::string lists_err;
    if (!write_file_atomic(path_join(req.dir, ARTICLE_LISTS_FILE), article_lists_json(shelf.lists), &lists_err))
        LOG_W("wikipedia: the lists weren't saved: %s", lists_err.c_str());
    std::map<int64_t, const WikiArticle*> by_id;
    std::vector<ArticleEntry> wanted;
    for (const WikiArticle& a : chosen) {
        by_id[a.id] = &a;
        wanted.push_back(a);
    }
    r.listed = true;
    r.unread = static_cast<int>(chosen.size());
    const SyncPlan plan = plan_sync(wanted, req.local, {}, req.rebuild);
    r.remove = plan.remove;
    if (!complete) {
        LOG_W("wikipedia: the lists weren't all read (too many pages or entries): nothing removed");
        r.remove.clear();
    }
    const std::set<int64_t> local(req.local.begin(), req.local.end());

    p = SyncProgress{};
    p.phase = SyncProgress::Downloading;
    p.total = static_cast<int>(plan.download.size());
    report();
    for (const ArticleEntry& e : plan.download) {
        const WikiArticle& a = *by_id[e.id];
        const uint64_t t0 = mono_ms();
        const std::string server = req.pages_server.empty() ? "https://" + a.host : req.pages_server;
        // The page from its wiki, without the token; a redirect leads to
        // the page it names.
        HttpRequest q;
        q.url = server + "/w/rest.php/v1/page/" + wiki_title_path(a.title) + "/html";
        q.headers = {"Accept: text/html"};
        q.max_bytes = MAX_PAGE;
        q.timeout_s = 120;
        q.low_speed_s = STALL_S;
        q.follow_redirects = true;
        q.public_only = req.pages_server.empty();
        q.https_only = req.pages_server.empty(); // no redirect to http (V4-SEC-5)
        HttpResponse res = http.request(q, &cancel);
        if (res.cancelled || (!res.error.empty() && !res.too_large)) {
            if (res.cancelled)
                r.cancelled = true;
            else
                r.error = res.error;
            return r;
        }
        WikiPage page;
        const bool ok = !res.too_large && res.status == 200 && wiki_clean_page(res.body, page);
        const size_t html_size = res.body.size();
        std::string().swap(res.body);
        if (!ok) {
            LOG_W("wikipedia: %s \"%s\" not downloaded (HTTP %ld, %zu bytes)", a.host.c_str(), a.title.c_str(),
                  res.status, html_size);
            r.failed++;
            if (req.rebuild && local.count(a.id))
                r.redo = true; // made by an older version, and still: again next time
        } else {
            ArticleBook book = wiki_book(a, page);
            const std::string base = server + "/wiki/" + wiki_title_path(a.title);
            const std::string trusted = req.pages_server.empty() ? "" : host_name(url_host(req.pages_server));
            int left_out = 0;
            const std::string html = std::move(book.html);
            if (s.images) {
                // The cover: the article's first picture.
                const std::vector<std::string> pics = article_images(html, base);
                if (!add_pictures(http, html, base, pics.empty() ? "" : pics.front(), trusted, book, cancel,
                                  &left_out)) {
                    r.cancelled = true;
                    return r;
                }
            } else {
                book.html = rewrite_html(html, base, {}); // no broken squares
            }
            std::string err;
            size_t bytes = 0;
            const size_t pictures = book.images.size();
            if (!save_article(req.dir, a.id, a.created, std::move(book), &bytes, &err)) {
                r.error = "Couldn't save the articles: " + err;
                return r;
            }
            LOG_I("wikipedia: %s \"%s\": %zu KB of HTML, %zu pictures (%d left out), %zu KB in %llu ms",
                  a.host.c_str(), a.title.c_str(), html_size >> 10, pictures, left_out, bytes >> 10,
                  static_cast<unsigned long long>(mono_ms() - t0));
            (local.count(a.id) ? r.rebuilt : r.downloaded)++;
        }
        p.done++;
        report();
    }
    return r;
}

} // namespace dr
