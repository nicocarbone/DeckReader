#include "epubwrite.h"

#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>
#include <zlib.h>

namespace dr {

// --- ZIP ---------------------------------------------------------------------------

namespace {

void put16(std::string& o, uint32_t v) {
    o += static_cast<char>(v & 0xff);
    o += static_cast<char>((v >> 8) & 0xff);
}

void put32(std::string& o, uint32_t v) {
    put16(o, v & 0xffff);
    put16(o, v >> 16);
}

// Raw deflate (no zlib header), as ZIP wants it.
bool deflate_raw(const std::string& in, std::string& out) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return false;
    out.resize(deflateBound(&zs, static_cast<uLong>(in.size())));
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
    zs.avail_out = static_cast<uInt>(out.size());
    int rc = deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return rc == Z_STREAM_END;
}

} // namespace

std::string zip_build(std::vector<ZipItem> items) {
    // A fixed date (1 Jan 2020): the books don't need one, and the same
    // article gives the same file.
    const uint32_t dos_time = 0, dos_date = ((2020 - 1980) << 9) | (1 << 5) | 1;
    size_t total = 22;
    for (const ZipItem& it : items)
        total += 30 + 46 + 2 * it.name.size() + it.data.size();
    std::string out, central;
    out.reserve(total); // an upper bound: no copy as it grows
    for (ZipItem& it : items) {
        const uint32_t crc = static_cast<uint32_t>(
            crc32(0, reinterpret_cast<const Bytef*>(it.data.data()), static_cast<uInt>(it.data.size())));
        std::string packed;
        bool deflated = it.compress && deflate_raw(it.data, packed) && packed.size() < it.data.size();
        const std::string& body = deflated ? packed : it.data;
        const uint32_t method = deflated ? 8 : 0, flags = 0x0800; // names are UTF-8
        const uint32_t offset = static_cast<uint32_t>(out.size());

        put32(out, 0x04034b50);
        put16(out, 20);
        put16(out, flags);
        put16(out, method);
        put16(out, dos_time);
        put16(out, dos_date);
        put32(out, crc);
        put32(out, static_cast<uint32_t>(body.size()));
        put32(out, static_cast<uint32_t>(it.data.size()));
        put16(out, static_cast<uint32_t>(it.name.size()));
        put16(out, 0);
        out += it.name;
        out += body;

        put32(central, 0x02014b50);
        put16(central, 20);
        put16(central, 20);
        put16(central, flags);
        put16(central, method);
        put16(central, dos_time);
        put16(central, dos_date);
        put32(central, crc);
        put32(central, static_cast<uint32_t>(body.size()));
        put32(central, static_cast<uint32_t>(it.data.size()));
        put16(central, static_cast<uint32_t>(it.name.size()));
        put16(central, 0); // extra
        put16(central, 0); // comment
        put16(central, 0); // disk
        put16(central, 0); // internal attributes
        put32(central, 0); // external attributes
        put32(central, offset);
        central += it.name;
        // Each picture is freed once it is in the archive: the article's
        // pictures are never in memory twice.
        std::string().swap(it.data);
    }
    const uint32_t cd_offset = static_cast<uint32_t>(out.size());
    out += central;
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<uint32_t>(items.size()));
    put16(out, static_cast<uint32_t>(items.size()));
    put32(out, static_cast<uint32_t>(central.size()));
    put32(out, cd_offset);
    put16(out, 0);
    return out;
}

// --- the article's HTML ------------------------------------------------------------

namespace {

// NUL and the control characters XML 1.0 doesn't allow (tab, line feed and
// carriage return it does).
bool xml_illegal(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return u < 0x20 && u != '\t' && u != '\n' && u != '\r';
}

bool xml_char(long cp) {
    return cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp <= 0xD7FF) || (cp >= 0xE000 && cp <= 0xFFFD) ||
           (cp >= 0x10000 && cp <= 0x10FFFF);
}

void append_utf8(std::string& o, long cp) {
    if (cp < 0x80) {
        o += static_cast<char>(cp);
    } else if (cp < 0x800) {
        o += static_cast<char>(0xC0 | (cp >> 6));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += static_cast<char>(0xE0 | (cp >> 12));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        o += static_cast<char>(0xF0 | (cp >> 18));
        o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_alnum(char c) {
    return is_alpha(c) || (c >= '0' && c <= '9');
}

// At s[i] == '&': true if a named reference ("&mdash;") starts there.
bool named_ref_at(const std::string& s, size_t i) {
    size_t k = i + 1;
    if (k >= s.size() || !is_alpha(s[k]))
        return false;
    while (k < s.size() && k - i <= 32 && is_alnum(s[k]))
        k++;
    return k < s.size() && s[k] == ';';
}

void escape_char(std::string& o, char c) {
    switch (c) {
    case '&': o += "&amp;"; break;
    case '<': o += "&lt;"; break;
    case '>': o += "&gt;"; break;
    case '"': o += "&quot;"; break;
    default:
        if (!xml_illegal(c))
            o += c;
    }
}

} // namespace

std::string xml_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + s.size() / 8);
    for (char c : s)
        escape_char(o, c);
    return o;
}

namespace {

// Character references in attribute values (addresses, alt texts): the
// numeric ones and &amp; &lt; &gt; &quot; &apos; &nbsp;. Other named ones
// ("&mdash;") are left as they are, for the engine, which knows HTML's
// names (attr_escape keeps them); references to characters XML can't hold
// are dropped.
std::string decode_entities(const std::string& s) {
    static const std::pair<const char*, long> named[] = {{"amp", '&'},   {"lt", '<'},     {"gt", '>'},
                                                         {"quot", '"'},  {"apos", '\''}, {"nbsp", 0xA0}};
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') {
            o += s[i];
            continue;
        }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 12) {
            o += '&';
            continue;
        }
        const std::string name = s.substr(i + 1, semi - i - 1);
        long cp = -1;
        for (const auto& [n, c] : named)
            if (name == n)
                cp = c;
        if (name.size() > 1 && name[0] == '#') {
            const bool hex = name[1] == 'x' || name[1] == 'X';
            const std::string digits = name.substr(hex ? 2 : 1);
            if (!digits.empty() && digits.size() <= 8 && std::all_of(digits.begin(), digits.end(), [&](char c) {
                    return hex ? std::isxdigit(static_cast<unsigned char>(c)) != 0
                               : std::isdigit(static_cast<unsigned char>(c)) != 0;
                })) {
                cp = std::strtol(digits.c_str(), nullptr, hex ? 16 : 10);
                if (!xml_char(cp)) {
                    i = semi; // "&#0;", "&#xD800;": dropped
                    continue;
                }
            }
        }
        if (cp >= 0) {
            append_utf8(o, cp);
            i = semi;
        } else {
            o += '&';
        }
    }
    return o;
}

// xml_escape for a decoded attribute value: named references it still
// holds ("&mdash;") are kept, not turned into "&amp;mdash;".
std::string attr_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '&' && named_ref_at(s, i))
            o += '&';
        else
            escape_char(o, s[i]);
    }
    return o;
}

struct Tag {
    size_t begin = 0, end = 0; // [begin, end): "<img ... >"
    std::map<std::string, std::string> attrs;
};

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// At s[pos] == '<': a tag named `name` (lower case), with its attributes.
// Unquoted values end at white space or '>' (a '/' belongs to the value, as
// in HTML).
bool read_tag(const std::string& s, size_t pos, const char* name, Tag& tag) {
    const size_t n = std::strlen(name);
    if (pos + 1 + n > s.size())
        return false;
    for (size_t k = 0; k < n; k++)
        if (std::tolower(static_cast<unsigned char>(s[pos + 1 + k])) != name[k])
            return false;
    size_t i = pos + 1 + n;
    if (i >= s.size() || !(is_space(s[i]) || s[i] == '/' || s[i] == '>'))
        return false;
    tag = Tag{};
    tag.begin = pos;
    while (i < s.size()) {
        while (i < s.size() && (is_space(s[i]) || s[i] == '/'))
            i++;
        if (i >= s.size())
            return false;
        if (s[i] == '>') {
            tag.end = i + 1;
            return true;
        }
        size_t k = i;
        while (i < s.size() && !is_space(s[i]) && s[i] != '=' && s[i] != '>' && s[i] != '/')
            i++;
        std::string key = to_lower(s.substr(k, i - k));
        while (i < s.size() && is_space(s[i]))
            i++;
        std::string value;
        if (i < s.size() && s[i] == '=') {
            i++;
            while (i < s.size() && is_space(s[i]))
                i++;
            if (i < s.size() && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i++];
                size_t e = s.find(q, i);
                if (e == std::string::npos)
                    return false;
                value = s.substr(i, e - i);
                i = e + 1;
            } else {
                k = i;
                while (i < s.size() && !is_space(s[i]) && s[i] != '>')
                    i++;
                value = s.substr(k, i - k);
            }
        }
        if (!key.empty() && !tag.attrs.count(key))
            tag.attrs[key] = decode_entities(value);
    }
    return false;
}

// A srcset's candidates, split as HTML does: the address runs to white
// space (image CDNs put commas in it: ".../w_848,c_limit,f_auto/..."), then
// an optional descriptor ("1200w", "2x") runs to the next comma.
std::vector<std::pair<std::string, std::string>> srcset_candidates(const std::string& s) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        while (i < n && (is_space(s[i]) || s[i] == ','))
            i++;
        if (i >= n)
            break;
        size_t start = i;
        while (i < n && !is_space(s[i]))
            i++;
        std::string url = s.substr(start, i - start), descriptor;
        if (url.back() == ',') {
            while (!url.empty() && url.back() == ',')
                url.pop_back(); // "a.png,b.png 2x": no descriptor
        } else {
            size_t d = i;
            int depth = 0;
            while (i < n && !(s[i] == ',' && depth == 0)) {
                if (s[i] == '(')
                    depth++;
                else if (s[i] == ')' && depth > 0)
                    depth--;
                i++;
            }
            descriptor = trim(s.substr(d, i - d));
        }
        if (!url.empty())
            out.emplace_back(url, descriptor);
    }
    return out;
}

// The biggest candidate of a srcset up to 2000 px wide, else the first.
std::string from_srcset(const std::string& srcset) {
    std::string best, first;
    long best_w = 0;
    for (const auto& [url, d] : srcset_candidates(srcset)) {
        if (first.empty())
            first = url;
        if (!d.empty() && d.back() == 'w') {
            long w = std::strtol(d.c_str(), nullptr, 10);
            if (w > best_w && w <= 2000) {
                best_w = w;
                best = url;
            }
        }
    }
    return best.empty() ? first : best;
}

std::string picture_url(const Tag& tag, const std::string& base) {
    auto get = [&](const char* k) {
        auto it = tag.attrs.find(k);
        return it == tag.attrs.end() ? std::string() : trim(it->second);
    };
    // Lazy-loading pages put the picture in data-* and a placeholder in src,
    // often a real address (a blank or blurred picture): data-* first.
    static const std::pair<const char*, bool> keys[] = {
        {"data-src", false}, {"data-lazy-src", false}, {"data-original", false}, {"data-srcset", true},
        {"src", false},      {"srcset", true},         {"data-url", false}};
    for (const auto& [k, set] : keys) {
        std::string v = get(k);
        if (v.empty())
            continue;
        if (std::string url = resolve_url(base, set ? from_srcset(v) : v); !url.empty())
            return url;
    }
    return "";
}

// Finds `what` (lower case) in s from pos, ignoring case.
size_t find_ci(const std::string& s, const char* what, size_t pos) {
    const size_t n = std::strlen(what);
    for (size_t i = pos; i + n <= s.size(); i++) {
        size_t k = 0;
        while (k < n && std::tolower(static_cast<unsigned char>(s[i + k])) == what[k])
            k++;
        if (k == n)
            return i;
    }
    return std::string::npos;
}

enum class Part { Img, Source, Drop };

// Calls fn(tag, part) in order for every <img> and <source> tag, and for
// every comment, <script> and <style> element (Drop: [begin, end) covers
// all of it). Pictures inside those three aren't pictures. Only complete
// ones count: an unclosed comment or element is left alone.
template <typename Fn> void each_part(const std::string& html, Fn fn) {
    size_t pos = 0;
    while ((pos = html.find('<', pos)) != std::string::npos) {
        Tag tag;
        if (html.compare(pos, 4, "<!--") == 0) {
            size_t e = html.find("-->", pos + 4);
            if (e == std::string::npos) {
                pos += 4;
                continue;
            }
            tag.begin = pos;
            tag.end = e + 3;
            fn(tag, Part::Drop);
            pos = tag.end;
            continue;
        }
        if (read_tag(html, pos, "img", tag)) {
            fn(tag, Part::Img);
            pos = tag.end;
            continue;
        }
        if (read_tag(html, pos, "source", tag)) {
            fn(tag, Part::Source);
            pos = tag.end;
            continue;
        }
        bool dropped = false;
        for (const char* el : {"script", "style"}) {
            if (!read_tag(html, pos, el, tag))
                continue;
            if (html[tag.end - 2] != '/') { // not <script/>: to its end tag
                size_t e = find_ci(html, el[1] == 'c' ? "</script" : "</style", tag.end);
                size_t gt = e == std::string::npos ? e : html.find('>', e);
                if (gt == std::string::npos)
                    break; // unclosed: left alone
                tag.end = gt + 1;
            }
            fn(tag, Part::Drop);
            pos = tag.end;
            dropped = true;
            break;
        }
        if (!dropped)
            pos++;
    }
}

// "/a/./b/../c" -> "/a/c" (RFC 3986, 5.2.4). `path` starts with '/'.
std::string remove_dots(const std::string& path) {
    std::vector<std::string> segs;
    size_t i = 1;
    for (;;) {
        size_t e = path.find('/', i);
        const bool last = e == std::string::npos;
        std::string seg = path.substr(i, last ? std::string::npos : e - i);
        if (seg == "..") {
            if (!segs.empty())
                segs.pop_back();
            if (last)
                segs.emplace_back();
        } else if (seg == ".") {
            if (last)
                segs.emplace_back();
        } else {
            segs.push_back(seg);
        }
        if (last)
            break;
        i = e + 1;
    }
    std::string out;
    for (const std::string& s : segs)
        out += "/" + s;
    return out.empty() ? "/" : out;
}

// Spaces, control characters, non-ASCII bytes and a few others in a path or
// query as %XX, as browsers send them (curl refuses some of them).
std::string encode_url_chars(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c >= 0x7F || c == '"' || c == '<' || c == '>' || c == '`') {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        } else {
            o += ch;
        }
    }
    return o;
}

struct UrlParts {
    std::string scheme, authority, path, query;
};

// An http(s) address in parts; false for anything else.
bool split_url(const std::string& u, UrlParts& p) {
    size_t sep = u.find("://");
    if (sep == std::string::npos)
        return false;
    p.scheme = to_lower(u.substr(0, sep));
    if (p.scheme != "http" && p.scheme != "https")
        return false;
    size_t a = u.find_first_of("/?", sep + 3);
    p.authority = u.substr(sep + 3, a == std::string::npos ? std::string::npos : a - sep - 3);
    std::string rest = a == std::string::npos ? std::string() : u.substr(a);
    size_t q = rest.find('?');
    p.path = rest.substr(0, q);
    p.query = q == std::string::npos ? std::string() : rest.substr(q);
    if (p.path.empty())
        p.path = "/";
    return !p.authority.empty();
}

std::string join_url(const UrlParts& p) {
    return p.scheme + "://" + p.authority + encode_url_chars(remove_dots(p.path)) + encode_url_chars(p.query);
}

} // namespace

std::string resolve_url(const std::string& base, const std::string& ref) {
    std::string r;
    for (char c : trim(ref))
        if (c != '\t' && c != '\n' && c != '\r') // as browsers do
            r += c;
    r = r.substr(0, r.find('#'));
    if (r.empty())
        return "";
    UrlParts p;
    const size_t colon = r.find(':'), stop = r.find_first_of("/?");
    if (colon != std::string::npos && (stop == std::string::npos || colon < stop))
        return split_url(r, p) ? join_url(p) : ""; // or another scheme: data:, javascript:...
    UrlParts b;
    if (!split_url(base.substr(0, base.find('#')), b))
        return "";
    if (starts_with(r, "//"))
        return split_url(b.scheme + ":" + r, p) ? join_url(p) : "";
    p = b;
    size_t q = r.find('?');
    std::string rpath = r.substr(0, q);
    p.query = q == std::string::npos ? std::string() : r.substr(q);
    if (rpath.empty())
        p.path = b.path; // "?w=800": the page's own path
    else if (rpath[0] == '/')
        p.path = rpath;
    else
        p.path = b.path.substr(0, b.path.rfind('/') + 1) + rpath;
    return join_url(p);
}

std::vector<std::string> article_images(const std::string& html, const std::string& base_url) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    each_part(html, [&](const Tag& tag, Part part) {
        if (part != Part::Img)
            return;
        std::string url = picture_url(tag, base_url);
        if (!url.empty() && seen.insert(url).second)
            out.push_back(url);
    });
    return out;
}

std::string rewrite_html(const std::string& html, const std::string& base_url,
                         const std::map<std::string, std::string>& local) {
    std::string out;
    out.reserve(html.size());
    size_t copied = 0;
    auto copy_to = [&](size_t end) {
        for (size_t i = copied; i < end; i++)
            if (!xml_illegal(html[i]))
                out += html[i];
    };
    each_part(html, [&](const Tag& tag, Part part) {
        copy_to(tag.begin);
        copied = tag.end;
        if (part != Part::Img)
            return; // <source>: the <img> next to it is what counts
        auto it = local.find(picture_url(tag, base_url));
        if (it == local.end())
            return;
        auto alt = tag.attrs.find("alt");
        out += "<img src=\"" + xml_escape(it->second) + "\" alt=\"" +
               attr_escape(alt == tag.attrs.end() ? std::string() : alt->second) + "\"/>";
    });
    copy_to(html.size());
    return out;
}

bool image_type(const std::string& d, std::string* media_type, std::string* ext) {
    auto is = [&](const char* m, const char* e) {
        *media_type = m;
        *ext = e;
        return true;
    };
    if (d.size() >= 3 && static_cast<unsigned char>(d[0]) == 0xFF && static_cast<unsigned char>(d[1]) == 0xD8 &&
        static_cast<unsigned char>(d[2]) == 0xFF)
        return is("image/jpeg", "jpg");
    if (d.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0)
        return is("image/png", "png");
    if (d.compare(0, 4, "GIF8") == 0)
        return is("image/gif", "gif");
    if (d.size() >= 12 && d.compare(0, 4, "RIFF") == 0 && d.compare(8, 4, "WEBP") == 0)
        return is("image/webp", "webp");
    // SVG: text whose first element is <svg>, after a byte order mark, an
    // XML declaration, comments or an SVG doctype (not an HTML page that
    // happens to hold one).
    const std::string head = to_lower(d.substr(0, 4096));
    size_t i = head.compare(0, 3, "\xef\xbb\xbf") == 0 ? 3 : 0;
    for (;;) {
        while (i < head.size() && is_space(head[i]))
            i++;
        size_t e = std::string::npos;
        if (head.compare(i, 2, "<?") == 0) {
            e = head.find("?>", i);
            if (e != std::string::npos)
                e += 2;
        } else if (head.compare(i, 4, "<!--") == 0) {
            e = head.find("-->", i);
            if (e != std::string::npos)
                e += 3;
        } else if (head.compare(i, 9, "<!doctype") == 0) {
            size_t k = i + 9;
            while (k < head.size() && is_space(head[k]))
                k++;
            if (head.compare(k, 3, "svg") != 0)
                return false;
            size_t gt = head.find('>', k), bracket = head.find('[', k);
            e = bracket < gt ? head.find("]>", bracket) : gt; // an internal subset holds '>'
            if (e != std::string::npos)
                e += bracket < gt ? 2 : 1;
        } else {
            break;
        }
        if (e == std::string::npos)
            return false;
        i = e;
    }
    if (head.compare(i, 4, "<svg") == 0 && i + 4 < head.size() &&
        (is_space(head[i + 4]) || head[i + 4] == '>' || head[i + 4] == '/'))
        return is("image/svg+xml", "svg");
    return false;
}

// --- the book ------------------------------------------------------------------------

std::string make_article_epub(ArticleBook b) {
    const std::string title = xml_escape(b.title);
    const std::string lang = xml_escape(b.language);
    const std::string lang_attrs = b.language.empty() ? "" : " lang=\"" + lang + "\" xml:lang=\"" + lang + "\"";

    std::string manifest;
    int n = 0;
    std::string cover_id;
    for (const ArticleImage& im : b.images) {
        std::string id = strf("img%d", ++n);
        manifest += "  <item id=\"" + id + "\" href=\"" + xml_escape(im.href) + "\" media-type=\"" + im.media_type + "\"";
        if (im.href == b.cover) {
            manifest += " properties=\"cover-image\"";
            cover_id = id;
        }
        manifest += "/>\n";
    }
    std::string opf =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"uid\">\n"
        " <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
        "  <dc:identifier id=\"uid\">urn:wallabag:" +
        std::to_string(b.id) + "</dc:identifier>\n  <dc:title>" + title + "</dc:title>\n";
    if (!b.author.empty())
        opf += "  <dc:creator>" + xml_escape(b.author) + "</dc:creator>\n";
    if (!b.language.empty())
        opf += "  <dc:language>" + lang + "</dc:language>\n";
    if (!b.url.empty())
        opf += "  <dc:source>" + xml_escape(b.url) + "</dc:source>\n";
    if (!cover_id.empty())
        opf += "  <meta name=\"cover\" content=\"" + cover_id + "\"/>\n";
    opf += " </metadata>\n <manifest>\n"
           "  <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
           "  <item id=\"article\" href=\"article.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
           "  <item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>\n" +
           manifest + " </manifest>\n <spine>\n  <itemref idref=\"article\"/>\n </spine>\n</package>\n";

    const std::string head = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                             "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\"" +
                             lang_attrs + ">\n<head>\n<meta charset=\"utf-8\"/>\n<title>" + title + "</title>\n";
    std::string nav = head + "</head>\n<body>\n<nav epub:type=\"toc\">\n<ol><li><a href=\"article.xhtml\">" + title +
                      "</a></li></ol>\n</nav>\n</body>\n</html>\n";
    std::string page = head + "<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/>\n</head>\n<body>\n<h1>" +
                       title + "</h1>\n";
    if (!b.byline.empty())
        page += "<p class=\"byline\">" + xml_escape(b.byline) + "</p>\n";
    page += "<div class=\"article\">\n";
    for (char c : b.html) // rewrite_html's already; a caller's own HTML too
        if (!xml_illegal(c))
            page += c;
    page += "\n</div>\n";
    if (!b.url.empty())
        page += "<p class=\"source\"><a href=\"" + xml_escape(b.url) + "\">" + xml_escape(b.url) + "</a></p>\n";
    page += "</body>\n</html>\n";
    const std::string css = "h1 { margin: 0 0 0.4em 0; }\n"
                            "p.byline { font-size: 0.85em; font-style: italic; text-indent: 0; margin: 0 0 1.6em 0; }\n"
                            "p.source { font-size: 0.75em; text-indent: 0; margin-top: 2em; }\n"
                            "img { max-width: 100%; }\n";

    std::vector<ZipItem> items;
    items.push_back({"mimetype", "application/epub+zip", false});
    items.push_back({"META-INF/container.xml",
                     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                     "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
                     " <rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
                     "media-type=\"application/oebps-package+xml\"/></rootfiles>\n"
                     "</container>\n",
                     true});
    items.push_back({"OEBPS/content.opf", opf, true});
    items.push_back({"OEBPS/nav.xhtml", nav, true});
    items.push_back({"OEBPS/style.css", css, true});
    items.push_back({"OEBPS/article.xhtml", std::move(page), true});
    for (ArticleImage& im : b.images)
        items.push_back({"OEBPS/" + im.href, std::move(im.data), im.media_type == "image/svg+xml"});
    return zip_build(std::move(items));
}

} // namespace dr
