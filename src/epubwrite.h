// Writing EPUBs for web articles (wallabag D31, Wikipedia D37): a ZIP
// writer (zlib) and the article book itself, one page with a title line,
// the article's text and its pictures, and the preview picture as the
// cover. Pure functions: the articles thread uses them, the unit tests check
// them.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dr {

struct ZipItem {
    std::string name;
    std::string data;
    bool compress = true; // deflate; false = stored (mimetype, pictures)
};
// A ZIP archive in memory (no ZIP64: under 4 GB, as books are). Each
// item's data is freed once it is in the archive.
std::string zip_build(std::vector<ZipItem> items);

struct ArticleImage {
    std::string href;       // "images/3.png", relative to the article page
    std::string media_type; // "image/png"
    std::string data;
};

// A section in the book's table of contents.
struct ArticleSection {
    std::string id;   // the heading's id in the article
    std::string text; // as XML text (already escaped)
    int level = 2;    // 2 or 3 (h3s go under the h2 before them)
};

struct ArticleBook {
    int64_t id = 0;
    std::string source = "wallabag"; // the identifier: urn:<source>:<id>
    std::string title;
    std::string author;   // the article's authors, else its site
    std::string language; // BCP 47 ("en-US"); "" = not given
    std::string url;      // the original page
    std::string byline;   // "Ana Pérez · example.com · 27 Mar 2009 · 2 min read"
    std::string html;     // the article, pictures already pointing at images/
    std::vector<ArticleImage> images;
    std::string cover;    // href of the cover picture, "" = none
    std::string css;      // added to the book's style
    std::vector<ArticleSection> toc; // "" = one entry, the title
};
// Takes the book (its pictures move into the archive: pass it with
// std::move when they are big).
std::string make_article_epub(ArticleBook book);

// --- the article's HTML ---

// &, <, > and " as entities; characters XML can't hold (NUL, most control
// characters) left out.
std::string xml_escape(const std::string& s);
// Character references in attribute values (addresses, alt texts) and TeX
// (texmath.h): the numeric ones and &amp; &lt; &gt; &quot; &apos; &nbsp;.
// Other named ones ("&mdash;") are left as they are, for the engine, which
// knows HTML's names; references to characters XML can't hold are dropped.
std::string decode_entities(const std::string& s);
// A link or picture address in a page at `base`, made absolute as browsers
// do ("../", "?query", spaces and non-ASCII as %XX); "" for what isn't
// http(s) (data:, javascript:, ...). The #fragment is dropped.
std::string resolve_url(const std::string& base, const std::string& ref);
// The pictures of an article: absolute addresses, in order, without
// repeats. Lazy-loading attributes (data-src...) win over src; srcset is
// understood (commas inside addresses too). Comments, scripts and styles
// hold no pictures.
std::vector<std::string> article_images(const std::string& html, const std::string& base_url);
// The article's HTML for the book. Each picture whose address is in
// `local` becomes <img src="images/N.ext" alt="..."/>; the others (not
// downloaded, or not a picture the engine can show) are removed, as are
// <source> tags, instead of showing as a broken square. Comments, <script>
// and <style> elements (the book has its own style) and characters XML
// can't hold are removed too.
std::string rewrite_html(const std::string& html, const std::string& base_url,
                         const std::map<std::string, std::string>& local);
// The picture's type from its first bytes: JPEG, PNG, GIF, WebP or SVG
// (what the engine shows; SVG when its first element is <svg>). False for
// anything else.
bool image_type(const std::string& data, std::string* media_type, std::string* ext);

} // namespace dr
