#include "book.h"

#include "epubguard.h"
#include "fsutil.h"
#include "log.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include <crengine.h>
#include <crhyphman.h>
#include <crlog.h>
#include <ldomdoccache.h>
#include <lvcolordrawbuf.h>
#include <lvcontainer.h>
#include <lvcontaineriteminfo.h>
#include <lvdocview.h>
#include <lvdocviewprops.h>
#include <lvfntman.h>
#include <lvimg.h>
#include <lvstreamutils.h>
#include <lvtocitem.h>

namespace dr {

static std::string utf8(const lString32& s) {
    return UnicodeToUtf8(s).c_str();
}

static lString32 u32(const std::string& s) {
    return Utf8ToUnicode(s.c_str());
}

// crengine's stock EPUB stylesheet (its user-agent CSS); our overrides are
// appended to it per settings.
static std::string g_base_css;

// crengine's LoadDocument(path) treats "@/" in a path as "file inside an
// archive"; opening the stream ourselves avoids that (D22).
static bool load_document(LVDocView& view, const std::string& path, bool metadata_only) {
    LVStreamRef stream = LVOpenFileStream(path.c_str(), LVOM_READ);
    if (stream.isNull())
        return false;
    return view.LoadDocument(stream, u32(path).c_str(), metadata_only);
}

namespace engine {

bool init(const std::string& fonts_dir, const std::string& crengine_data_dir, const std::string& cache_dir) {
    CRLog::setStderrLogger();
    CRLog::setLogLevel(log_level() >= LogLevel::Debug ? CRLog::LL_ERROR : CRLog::LL_FATAL);

    // Only the bundled fonts: system fonts would make startup depend on what
    // the host has installed, and fontconfig scans are slow (D9).
    InitFontManager(lString8::empty_str, false);
    std::vector<DirEntry> entries;
    list_dir(fonts_dir, entries);
    for (const DirEntry& e : entries) {
        if (e.is_dir || !(ends_with_ci(e.name, ".ttf") || ends_with_ci(e.name, ".otf")))
            continue;
        std::string p = path_join(fonts_dir, e.name);
        if (!fontMan->RegisterFont(lString8(p.c_str())))
            LOG_W("font not loaded: %s", p.c_str());
    }
    if (fontMan->GetFontCount() == 0) {
        LOG_E("no fonts found in %s", fonts_dir.c_str());
        return false;
    }
    fontMan->SetFallbackFontFaces(lString8("Literata;Inter"));

    std::string hyph = path_join(crengine_data_dir, "hyph") + "/";
    if (!HyphMan::initDictionaries(u32(hyph), true))
        LOG_W("hyphenation patterns not found in %s", hyph.c_str());

    std::string css_path = path_join(crengine_data_dir, "epub.css");
    if (!read_file(css_path, g_base_css))
        LOG_W("stylesheet not found: %s", css_path.c_str());

    // crengine keeps parsed and laid-out books here, so reopening a book is
    // fast (requirements 7.1).
    std::string cr_cache = path_join(cache_dir, "layout");
    make_dirs(cr_cache);
    ldomDocCache::init(u32(cr_cache), 128u << 20);
    return true;
}

void shutdown() {
    ldomDocCache::close();
    HyphMan::uninit();
    ShutdownFontManager();
}

std::vector<std::string> font_faces() {
    std::vector<std::string> out;
    lString32Collection list;
    fontMan->getFaceList(list);
    for (int i = 0; i < list.length(); i++)
        out.push_back(utf8(list[i]));
    return out;
}

std::string check_book_file(const std::string& path) {
    LVStreamRef stream = LVOpenFileStream(path.c_str(), LVOM_READ);
    if (stream.isNull())
        return "The file could not be opened.";
    LVContainerRef arc = LVOpenArchieve(stream);
    if (arc.isNull())
        return "The file is not a valid EPUB (not a ZIP archive).";
    std::vector<ArchiveEntry> entries;
    for (int i = 0; i < arc->GetObjectCount(); i++) {
        const LVContainerItemInfo* item = arc->GetObjectInfo(i);
        if (!item || item->IsContainer())
            continue;
        entries.push_back({utf8(lString32(item->GetName())), static_cast<uint64_t>(item->GetSize()),
                           static_cast<uint64_t>(item->GetPackSize())});
    }
    return check_archive(entries);
}

bool read_metadata(const std::string& path, BookInfo& info, Image* cover, int cover_h) {
    if (!check_book_file(path).empty())
        return false;
    LVDocView view(32, true);
    if (!load_document(view, path, true))
        return false;
    info.title = trim(utf8(view.getTitle()));
    info.author = trim(utf8(view.getAuthors()));
    info.language = trim(utf8(view.getLanguage()));
    if (!cover)
        return true;
    LVStreamRef stream = view.getBookCoverImageStream();
    if (stream.isNull())
        return true;
    LVImageSourceRef img = LVCreateStreamImageSource(stream);
    if (img.isNull() || img->GetWidth() <= 0 || img->GetHeight() <= 0)
        return true;
    // A tiny file can declare an enormous image; covers are never this big.
    if (static_cast<uint64_t>(img->GetWidth()) * static_cast<uint64_t>(img->GetHeight()) > (40ull << 20)) {
        LOG_W("cover of %s ignored: %dx%d", path_basename(path).c_str(), img->GetWidth(), img->GetHeight());
        return true;
    }
    // Decode at twice the size, then shrink with an area filter: sharper than
    // a single scaling step for the usual 1600x2400 covers.
    int h = cover_h * 2;
    int w = static_cast<int>(std::lround(static_cast<double>(h) * img->GetWidth() / img->GetHeight()));
    w = std::clamp(w, h / 3, h * 2);
    LVColorDrawBuf buf(w, h, 32);
    buf.FillRect(0, 0, w, h, 0x808080);
    buf.Draw(img, 0, 0, w, h, false);
    Image big;
    big.w = w;
    big.h = h;
    big.px.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; y++) {
        const lUInt32* row = reinterpret_cast<const lUInt32*>(buf.GetScanLine(y));
        for (int x = 0; x < w; x++)
            big.px[static_cast<size_t>(y) * w + x] = 0xFF000000u | (row[x] & 0xFFFFFF);
    }
    *cover = scale_image(big, std::max(1, w / 2), cover_h);
    return true;
}

} // namespace engine

Book::Book() = default;

Book::~Book() {
    close();
}

bool Book::open(const std::string& path, std::string* error) {
    close();
    std::string problem = engine::check_book_file(path);
    if (!problem.empty()) {
        if (error)
            *error = problem;
        LOG_W("refusing %s: %s", path.c_str(), problem.c_str());
        return false;
    }
    view_ = std::make_unique<LVDocView>(32, true);
    view_->setPageHeaderInfo(0); // our own HUD replaces crengine's status line
    view_->setMinFontSize(6);
    view_->setMaxFontSize(400);
    if (!load_document(*view_, path, false)) {
        view_.reset();
        if (error)
            *error = "The book could not be read.";
        return false;
    }
    info_.title = trim(utf8(view_->getTitle()));
    info_.author = trim(utf8(view_->getAuthors()));
    info_.language = trim(utf8(view_->getLanguage()));
    if (info_.title.empty())
        info_.title = strip_extension(path_basename(path));
    path_ = path;
    open_ = true;
    configured_ = false;
    return true;
}

void Book::close() {
    if (view_) {
        // Keeps the laid-out book in crengine's cache for a fast reopen.
        view_->updateCache();
        view_->close();
    }
    view_.reset();
    open_ = false;
    anchor_.clear();
    toc_.clear();
    path_.clear();
    info_ = {};
    configured_ = false;
}

std::string Book::make_css(const ReaderSettings& s) const {
    std::string css = g_base_css;
    css += "\n/* DeckReader settings */\n";
    // Text always takes the theme's ink: publishers' black text vanishes on
    // the OLED themes and their blue links are unreadable there. Images
    // keep their colours; links stay underlined (D14).
    css += "* { color: inherit !important; background-color: transparent !important; }\n";
    if (s.line_spacing > 0)
        css += strf("body, p, li, blockquote, dd, dt { line-height: %d.%d !important; }\n", s.line_spacing / 10,
                    s.line_spacing % 10);
    static const char* para[] = {"", "0", "0.5em", "1em"};
    if (s.para_spacing > 0 && s.para_spacing <= 3)
        css += strf("p { margin-top: 0 !important; margin-bottom: %s !important; }\n", para[s.para_spacing]);
    if (s.align == 1)
        css += "p, li { text-align: justify !important; }\n";
    else if (s.align == 2)
        css += "p, li { text-align: left !important; }\n";
    return css;
}

bool Book::configure(const ReaderSettings& s, int w, int h, float unit) {
    if (!open_ || w <= 0 || h <= 0)
        return false;
    bool changed = !configured_ || s.layout_differs(applied_) || w != width_ || h != height_ ||
                   std::fabs(unit - unit_) > 1e-4f;
    if (!changed)
        return false;
    std::string pos = configured_ ? position() : std::string();
    const int page_before = configured_ ? current_page() : -1;
    const uint64_t t0 = mono_ms();

    const FontDef& font = FONTS[std::max(0, s.font_index())];
    const bool book_fonts = font.face[0] == '\0';
    const std::string face = book_fonts ? "Literata" : font.face;

    // Books often name fonts we don't have ("Garamond", with no generic
    // family after it). crengine then scores every font by name (none
    // match) and by family type, so Literata, a serif, would win over the
    // chosen sans font. A bias above the family-type difference (192 x 100)
    // and below a name match (256 x 1000) makes the chosen font win unless
    // the book really provides the named font (D14).
    // With "Book's fonts", no font is preferred: the book's families decide.
    fontMan->SetAsPreferredFontWithBias(lString8(book_fonts ? "" : face.c_str()), book_fonts ? 0 : 25600, true);

    CRPropRef p = LVCreatePropsContainer();
    p->setString(PROP_FONT_FACE, face.c_str());
    // The chosen font wins over the book's generic families (serif,
    // sans-serif...) unless the reader asked for the book's own fonts.
    p->setString(PROP_GENERIC_SERIF_FONT_FACE, face.c_str());
    p->setString(PROP_GENERIC_SANS_SERIF_FONT_FACE, book_fonts ? "Inter" : face.c_str());
    p->setString(PROP_GENERIC_CURSIVE_FONT_FACE, face.c_str());
    p->setString(PROP_GENERIC_FANTASY_FONT_FACE, face.c_str());
    p->setBool(PROP_EMBEDDED_FONTS, book_fonts);
    p->setBool(PROP_EMBEDDED_STYLES, s.book_styles);
    p->setInt(PROP_FONT_SIZE, static_cast<int>(std::lround(s.font_size * unit)));
    // Grayscale by default: both Deck panels are rotated, so horizontal RGB
    // subpixel rendering only adds fringes there (requirements 4.2). The
    // vertical modes are there to try on the Deck's rotated panels (D21).
    static const int aa[] = {font_aa_all, font_aa_lcd_rgb, font_aa_lcd_bgr, font_aa_lcd_v_rgb, font_aa_lcd_v_bgr};
    p->setInt(PROP_FONT_ANTIALIASING, aa[std::clamp(s.text_aa, 0, TEXT_AA_COUNT - 1)]);
    p->setInt(PROP_FONT_HINTING, HINTING_MODE_AUTOHINT); // light autohinting
    p->setInt(PROP_FONT_SHAPING, SHAPING_MODE_HARFBUZZ);
    p->setBool(PROP_FONT_KERNING_ENABLED, true);
    // Justified lines may shrink spaces to 75% (crengine's default, 50%,
    // glues words together; 75% is what KOReader settled on).
    p->setInt(PROP_FORMAT_MIN_SPACE_CONDENSING_PERCENT, 75);
    p->setInt(PROP_LANDSCAPE_PAGES, s.pages);
    p->setBool(PROP_FOOTNOTES, true);
    p->setInt(PROP_INTERLINE_SPACE, 100);
    // CSS absolute units (pt, px, in) scale with the screen like everything else.
    p->setInt(PROP_RENDER_DPI, static_cast<int>(std::lround(96 * unit)));
    p->setBool(PROP_RENDER_SCALE_FONT_WITH_DPI, false);
    p->setBool(PROP_TEXTLANG_EMBEDDED_LANGS_ENABLED, true);
    p->setBool(PROP_TEXTLANG_HYPHENATION_ENABLED, s.hyphenation);
    p->setString(PROP_TEXTLANG_MAIN_LANG, info_.language.empty() ? "en" : info_.language.c_str());
    p->setString(PROP_HYPHENATION_DICT, s.hyphenation ? PROP_HYPHENATION_DICT_VALUE_ALGORITHM
                                                      : PROP_HYPHENATION_DICT_VALUE_NONE);
    int side = static_cast<int>(std::lround(s.side_margin * unit));
    int vert = static_cast<int>(std::lround(s.vert_margin * unit));
    p->setInt(PROP_PAGE_MARGIN_LEFT, side);
    p->setInt(PROP_PAGE_MARGIN_RIGHT, side);
    p->setInt(PROP_PAGE_MARGIN_TOP, vert);
    p->setInt(PROP_PAGE_MARGIN_BOTTOM, vert);

    const std::string css = make_css(s);
    if (!configured_ || css != make_css(applied_))
        view_->setStyleSheet(lString8(css.c_str()));
    view_->propsApply(p);
    view_->setPageHeaderInfo(0);
    view_->Resize(w, h);
    view_->checkRender();

    if (!pos.empty())
        go_to_position(pos);
    rebuild_toc();
    applied_ = s;
    width_ = w;
    height_ = h;
    unit_ = unit;
    configured_ = true;
    LOG_I("laid out \"%s\" in %llu ms: %d pages, %d per screen, font %s %dpx, page %d -> %d", info_.title.c_str(),
          static_cast<unsigned long long>(mono_ms() - t0), page_count(), pages_per_screen(), face.c_str(),
          static_cast<int>(std::lround(s.font_size * unit)), page_before, current_page());
    return true;
}

void Book::set_colors(uint32_t bg, uint32_t text) {
    if (!view_)
        return;
    view_->setBackgroundColor(bg);
    view_->setTextColor(text);
}

int Book::page_count() const {
    return view_ ? view_->getPageCount() : 0;
}

int Book::current_page() const {
    return view_ ? view_->getCurPage() : 0;
}

int Book::pages_per_screen() const {
    return view_ ? view_->getVisiblePageCount() : 1;
}

void Book::go_to_page(int page) {
    if (!view_)
        return;
    page = std::clamp(page, 0, std::max(0, page_count() - 1));
    view_->goToPage(page);
    ldomXPointer bm = view_->getBookmark();
    anchor_ = bm.isNull() ? std::string() : utf8(bm.toString());
}

bool Book::render(int page, uint32_t* px, int w, int h) {
    if (!view_ || !configured_ || w != width_ || h != height_)
        return false;
    LVColorDrawBuf buf(w, h, reinterpret_cast<lUInt8*>(px), 32);
    view_->Draw(buf, -1, page, false, false);
    return true;
}

std::string Book::position() {
    if (!view_)
        return {};
    if (!anchor_.empty())
        return anchor_;
    ldomXPointer bm = view_->getBookmark();
    return bm.isNull() ? std::string() : utf8(bm.toString());
}

bool Book::go_to_position(const std::string& xpointer) {
    if (!view_ || xpointer.empty() || !view_->getDocument())
        return false;
    ldomXPointer xp = view_->getDocument()->createXPointer(u32(xpointer));
    if (xp.isNull())
        return false;
    view_->goToBookmark(xp);
    anchor_ = xpointer;
    return true;
}

int Book::page_of_position(const std::string& xpointer) {
    if (!view_ || xpointer.empty() || !view_->getDocument())
        return -1;
    ldomXPointer xp = view_->getDocument()->createXPointer(u32(xpointer));
    return xp.isNull() ? -1 : view_->getBookmarkPage(xp);
}

int Book::percent() {
    return view_ ? std::clamp(view_->getPosPercent(), 0, 10000) : 0;
}

void Book::rebuild_toc() {
    toc_.clear();
    if (!view_)
        return;
    LVTocItem* root = view_->getToc();
    if (!root)
        return;
    view_->updatePageNumbers(root);
    std::function<void(LVTocItem*, int)> walk = [&](LVTocItem* item, int level) {
        for (int i = 0; i < item->getChildCount(); i++) {
            LVTocItem* c = item->getChild(i);
            std::string title = trim(utf8(c->getName()));
            if (!title.empty() && c->getPage() >= 0)
                toc_.push_back({title, level, c->getPage()});
            walk(c, level + 1);
        }
    };
    walk(root, 0);
}

int toc_chapter_at(const std::vector<TocEntry>& toc, int page) {
    // Every entry is checked: TOCs are not always in page order (notes and
    // appendices listed out of order). Same page: the later (deeper) entry.
    int found = -1;
    for (size_t i = 0; i < toc.size(); i++)
        if (toc[i].page <= page && (found < 0 || toc[i].page >= toc[static_cast<size_t>(found)].page))
            found = static_cast<int>(i);
    return found;
}

int toc_next_page(const std::vector<TocEntry>& toc, int last_visible) {
    int best = -1;
    for (const TocEntry& e : toc)
        if (e.page > last_visible && (best < 0 || e.page < best))
            best = e.page;
    return best;
}

int toc_prev_page(const std::vector<TocEntry>& toc, int page) {
    int best = -1;
    for (const TocEntry& e : toc)
        if (e.page < page)
            best = std::max(best, e.page);
    return best;
}

int Book::chapter_at(int page) const {
    return toc_chapter_at(toc_, page);
}

std::string Book::chapter_title_at(int page) const {
    int c = chapter_at(page);
    return c >= 0 ? toc_[static_cast<size_t>(c)].title : std::string();
}

int Book::next_chapter_page(int page) const {
    return toc_next_page(toc_, page + pages_per_screen() - 1);
}

int Book::prev_chapter_page(int page) const {
    return toc_prev_page(toc_, page);
}

} // namespace dr
