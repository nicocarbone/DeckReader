// Unit tests for DeckReader's pure logic. Run by tests/run.sh (inside the
// SDK). With DECKREADER_TEST_BOOK set (and the fonts/engine folders), an
// integration test also opens, lays out and renders a real book.
#include "book.h"
#include "canvas.h"
#include "config.h"
#include "epubguard.h"
#include "fsutil.h"
#include "input.h"
#include "library.h"
#include "metacache.h"
#include "settings.h"
#include "state.h"
#include "util.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <unistd.h>
#include <vector>

#include <lvcontainer.h>
#include <lvstream.h>
#include <lvstreamutils.h>

using namespace dr;

static int g_failed = 0, g_checks = 0;
static const char* g_test = "";

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failed++;                                                                \
            std::fprintf(stderr, "FAIL %s (%s:%d): %s\n", g_test, __FILE__, __LINE__, #cond); \
        }                                                                              \
    } while (0)
#define CHECK_EQ(a, b) CHECK((a) == (b))

static std::vector<std::pair<const char*, std::function<void()>>>& tests() {
    static std::vector<std::pair<const char*, std::function<void()>>> t;
    return t;
}
struct Reg {
    Reg(const char* name, std::function<void()> fn) { tests().emplace_back(name, std::move(fn)); }
};
#define TEST(name) \
    static void name(); \
    static Reg reg_##name(#name, name); \
    static void name()

static std::string temp_dir() {
    char tmpl[] = "/tmp/deckreader-test-XXXXXX";
    char* d = mkdtemp(tmpl);
    return d ? d : "/tmp";
}

// --- util ----------------------------------------------------------------

TEST(natural_sort) {
    CHECK(natural_less("Book 2", "Book 10"));
    CHECK(!natural_less("Book 10", "Book 2"));
    CHECK(natural_less("apple", "Banana"));
    CHECK(natural_less("\xC3\x81ngel", "Bosque"));       // Ángel < Bosque
    CHECK(natural_less("nube", "\xC3\xB1" "and\xC3\xBA")); // nube < ñandú
    CHECK(natural_less("Nzz", "\xC3\x91" "a"));           // Ñ after every N
    CHECK(natural_less("ficciones", "Ficciones") != natural_less("Ficciones", "ficciones")); // total order
}

TEST(paths) {
    CHECK_EQ(path_basename("/a/b/c.epub"), "c.epub");
    CHECK_EQ(path_basename("/a/b/"), "b");
    CHECK_EQ(path_dirname("/a/b/c.epub"), "/a/b");
    CHECK_EQ(path_dirname("/c"), "/");
    CHECK_EQ(path_join("/a/", "b"), "/a/b");
    CHECK_EQ(strip_extension("x.y.epub"), "x.y");
    CHECK_EQ(expand_user("~/Books", "/home/deck"), "/home/deck/Books");
    CHECK_EQ(expand_user("/mnt/x", "/home/deck"), "/mnt/x");
    CHECK_EQ(utf8_sanitize("ok\xff"), "ok\xEF\xBF\xBD");
    CHECK_EQ(utf8_sanitize("\xC3\xB1"), "\xC3\xB1");
}

// --- config --------------------------------------------------------------

TEST(config_defaults) {
    Config c = parse_config("", "/home/deck");
    CHECK_EQ(c.folders.size(), 1u);
    CHECK_EQ(c.folders[0], "/home/deck/Books");
    CHECK(c.removable_media);
    CHECK(c.clock_24h);
    CHECK(c.warnings.empty());
}

TEST(config_values) {
    Config c = parse_config("# comment\n[Library]\nfolder = ~/Books\nfolder=\"/run/media/deck/SD/Books\"\n"
                            "Removable-Media = no\n[interface]\nui_scale = 1.5\nclock = 12h\naccent_color = #ff8800\n"
                            "[reader]\ntap_zones = off\n",
                            "/home/deck");
    CHECK_EQ(c.folders.size(), 2u);
    CHECK_EQ(c.folders[1], "/run/media/deck/SD/Books");
    CHECK(!c.removable_media);
    CHECK(c.ui_scale > 1.49 && c.ui_scale < 1.51);
    CHECK(!c.clock_24h);
    CHECK_EQ(c.accent, 0xFF8800u);
    CHECK(!c.tap_zones);
    CHECK(c.warnings.empty());
}

TEST(config_warnings) {
    Config c = parse_config("[interface]\nui_scale = 9\nbogus = 1\nnot a line\n", "/h");
    CHECK_EQ(c.warnings.size(), 3u);
    CHECK(c.ui_scale > 0.99 && c.ui_scale < 1.01); // bad value keeps the default
}

// --- settings and state ----------------------------------------------------

TEST(settings_clamp) {
    ReaderSettings s;
    s.font_size = 99;
    s.line_spacing = 5;
    s.theme = 12;
    s.font = "comic";
    s.pages = 7;
    s.clamp();
    CHECK_EQ(s.font_size, 36);
    CHECK_EQ(s.line_spacing, 11);
    CHECK_EQ(s.theme, THEME_COUNT - 1);
    CHECK_EQ(s.font, std::string(FONTS[0].id));
    CHECK_EQ(s.pages, 2);
    s.text_aa = 9;
    s.clamp();
    CHECK_EQ(s.text_aa, TEXT_AA_COUNT - 1);
    ReaderSettings a, b;
    b.theme = 2;
    CHECK(!a.layout_differs(b)); // a theme is just colours
    b.font_size = 30;
    CHECK(a.layout_differs(b));
}

TEST(state_roundtrip) {
    StateStore s;
    s.settings.theme = 2;
    s.settings.font_size = 28;
    BookState& b = s.at("/books/a.epub");
    b.xpointer = "/body/DocFragment[3]/body/p[12]/text().5";
    b.percent = 4321;
    b.last_read = 100;
    b.title = "T\xC3\xADtulo";
    b.bookmarks.push_back({"/body/p[1]", 100, "Cap\xC3\xADtulo 1", 5});
    s.at("/books/b.epub").last_read = 200;
    StateStore t;
    CHECK(t.from_json(s.to_json()));
    CHECK_EQ(t.settings.theme, 2);
    CHECK_EQ(t.settings.font_size, 28);
    BookState* a = t.find("/books/a.epub");
    CHECK(a != nullptr);
    if (a) {
        CHECK_EQ(a->xpointer, b.xpointer);
        CHECK_EQ(a->percent, 4321);
        CHECK_EQ(a->title, "T\xC3\xADtulo");
        CHECK_EQ(a->bookmarks.size(), 1u);
    }
    std::vector<std::string> r = t.recent(10);
    CHECK_EQ(r.size(), 2u);
    CHECK_EQ(r[0], "/books/b.epub");
    CHECK(!t.from_json("{not json"));
}

TEST(state_bad_values) {
    StateStore t;
    CHECK(t.from_json("{\"settings\":{\"font_size\":\"big\",\"theme\":\"nope\"},\"books\":{\"/x\":{\"percent\":99999}}}"));
    CHECK_EQ(t.settings.font_size, 24);
    CHECK_EQ(t.settings.theme, 0);
    CHECK_EQ(t.find("/x")->percent, 10000);
}

TEST(state_save_rate_limit) {
    StateStore s(temp_dir() + "/state.json");
    CHECK_EQ(s.save_due_in(1000), -1);
    s.mark_dirty();
    CHECK_EQ(s.save_due_in(1000), 0); // first change: at once
    s.save_if_due(1000);
    CHECK(!s.dirty());
    s.mark_dirty();
    CHECK_EQ(s.save_due_in(1500), 1500); // then at most every 2 s
    CHECK_EQ(s.save_due_in(3000), 0);
}

TEST(atomic_write_and_corrupt_state) {
    std::string dir = temp_dir();
    std::string p = dir + "/state.json";
    CHECK(write_file_atomic(p, "{\"version\":1}"));
    std::string back;
    CHECK(read_file(p, back));
    CHECK_EQ(back, "{\"version\":1}");
    CHECK(write_file_atomic(p, "garbage"));
    StateStore s(p);
    CHECK(!s.load());                // unreadable...
    CHECK(is_file(p + ".corrupt")); // ...kept aside, not overwritten
}

// --- archive checks ----------------------------------------------------------

TEST(archive_names) {
    CHECK(unsafe_entry_name("../evil"));
    CHECK(unsafe_entry_name("OEBPS/../../evil"));
    CHECK(unsafe_entry_name("/etc/passwd"));
    CHECK(unsafe_entry_name("C:\\x"));
    CHECK(unsafe_entry_name("a\\..\\b"));
    CHECK(unsafe_entry_name(""));
    CHECK(!unsafe_entry_name("OEBPS/Text/ch1.xhtml"));
    CHECK(!unsafe_entry_name("OEBPS/..hidden/x")); // not a ".." component
}

TEST(archive_limits) {
    std::vector<ArchiveEntry> ok = {{"mimetype", 20, 20}, {"OEBPS/a.xhtml", 50000, 12000}};
    CHECK(check_archive(ok).empty());
    CHECK(!check_archive({}).empty());
    std::vector<ArchiveEntry> bomb = {{"x.xhtml", 900ull << 20, 1 << 20}};
    CHECK(!check_archive(bomb).empty());
    std::vector<ArchiveEntry> ratio = {{"x.xhtml", 50ull << 20, 10 << 10}}; // 5000:1
    CHECK(!check_archive(ratio).empty());
    std::vector<ArchiveEntry> many(30000, ArchiveEntry{"a", 1, 1});
    CHECK(!check_archive(many).empty());
    std::vector<ArchiveEntry> total(8, ArchiveEntry{"img.png", 200ull << 20, 190ull << 20});
    CHECK(!check_archive(total).empty());
}

// --- removable media --------------------------------------------------------------

TEST(mountinfo) {
    const std::string mi =
        "22 1 259:2 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p2 rw\n"
        "80 22 0:44 / /run/media rw,nosuid - tmpfs tmpfs rw\n"
        "81 80 179:1 / /run/media/deck/My\\040Card rw,nosuid,nodev shared:5 - ext4 /dev/mmcblk0p1 rw\n"
        "82 80 0:50 / /run/media/deck rw - tmpfs tmpfs rw\n"
        "83 80 8:1 / /run/media/deck/USB rw - vfat /dev/sda1 rw\n";
    std::vector<Mount> all = parse_mountinfo(mi);
    CHECK_EQ(all.size(), 5u);
    std::vector<Mount> rem = removable_mounts(mi);
    CHECK_EQ(rem.size(), 2u);
    if (rem.size() == 2) {
        CHECK_EQ(rem[0].mount_point, "/run/media/deck/My Card");
        CHECK_EQ(removable_label(rem[0]), "SD Card");
        CHECK_EQ(removable_label(rem[1]), "USB drive");
    }
}

// --- library -------------------------------------------------------------------------

TEST(library_sort_and_titles) {
    MetaCache meta; // empty: titles come from file names
    StateStore state;
    std::vector<LibEntry> v;
    LibEntry a;
    a.kind = LibEntry::Book;
    a.path = "/b/Zorro.epub";
    a.name = "Zorro.epub";
    LibEntry f;
    f.kind = LibEntry::Folder;
    f.name = "Series";
    LibEntry c = a;
    c.path = "/b/El_nombre_del_mundo.epub";
    c.name = "El_nombre_del_mundo.epub";
    v = {a, f, c};
    sort_entries(v, SortMode::Name, meta, state);
    CHECK_EQ(v[0].name, "Series"); // folders first
    CHECK_EQ(v[1].name, "El_nombre_del_mundo.epub");
    CHECK_EQ(display_title(v[1], nullptr), "El nombre del mundo");
    state.at("/b/Zorro.epub").last_read = 50;
    sort_entries(v, SortMode::Recent, meta, state);
    CHECK_EQ(v[1].name, "Zorro.epub");
    CHECK(is_book_file("A.EPUB"));
    CHECK(!is_book_file("a.pdf"));
}

TEST(library_folder_listing) {
    std::string d = temp_dir();
    make_dirs(d + "/Sub");
    write_file_atomic(d + "/b.epub", "x");
    write_file_atomic(d + "/notes.txt", "x");
    write_file_atomic(d + "/.hidden.epub", "x");
    std::vector<LibEntry> v = list_folder(d);
    CHECK_EQ(v.size(), 2u); // Sub and b.epub only
}

// --- input ------------------------------------------------------------------------------

TEST(input_mapping) {
    CHECK(Input::button_action(SDL_GAMEPAD_BUTTON_SOUTH) == Action::Confirm);
    CHECK(Input::button_action(SDL_GAMEPAD_BUTTON_EAST) == Action::Back);
    CHECK(Input::button_action(SDL_GAMEPAD_BUTTON_START) == Action::Menu);
    CHECK(Input::button_action(SDL_GAMEPAD_BUTTON_BACK) == Action::View);
    CHECK(Input::button_action(SDL_GAMEPAD_BUTTON_GUIDE) == Action::None); // Steam's
    CHECK(Input::key_action(SDLK_RIGHT, SDL_KMOD_NONE) == Action::Right);
    CHECK(Input::key_action(SDLK_Q, SDL_KMOD_NONE) == Action::None);
    CHECK(Input::key_action(SDLK_Q, SDL_KMOD_LCTRL) == Action::Quit);
    CHECK(Input::repeatable(Action::Right));
    CHECK(!Input::repeatable(Action::R2));
    CHECK(action_from_name("confirm") == Action::Confirm);
    CHECK(std::strcmp(action_name(Action::View), "view") == 0);
}

TEST(input_repeat_and_triggers) {
    Input in;
    std::vector<InputEvent> out;
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    e.gbutton.button = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    in.handle(e, 1000, out);
    CHECK_EQ(out.size(), 1u);
    in.tick(1000 + Input::REPEAT_DELAY_MS - 1, out);
    CHECK_EQ(out.size(), 1u);
    in.tick(1000 + Input::REPEAT_DELAY_MS, out);
    CHECK_EQ(out.size(), 2u);
    CHECK(out.back().repeat);
    e.type = SDL_EVENT_GAMEPAD_BUTTON_UP;
    in.handle(e, 1400, out);
    CHECK_EQ(in.next_due(1400), -1);

    out.clear();
    SDL_Event t{};
    t.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    t.gaxis.axis = SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    t.gaxis.value = 30000;
    in.handle(t, 2000, out);
    t.gaxis.value = 31000; // still pressed: no second action
    in.handle(t, 2010, out);
    CHECK_EQ(out.size(), 1u);
    CHECK(out[0].action == Action::R2);
    in.tick(5000, out);
    CHECK_EQ(out.size(), 1u); // triggers never repeat
}

TEST(input_stick_scrub) {
    Input in;
    std::vector<InputEvent> out;
    in.inject_stick_x(0.9f, 0, out);
    CHECK_EQ(out.size(), 1u); // acts as D-pad right in lists
    CHECK(out[0].action == Action::Right);
    CHECK(in.scrub_x() == 0.0f);
    in.inject_stick_x(0.0f, 10, out);
    out.clear();
    in.set_stick_scrub(true); // in the reader it scrubs instead
    in.inject_stick_x(0.9f, 20, out);
    CHECK(out.empty());
    CHECK(in.scrub_x() > 0.8f);
    in.inject_stick_x(-0.2f, 30, out);
    CHECK(in.scrub_x() == 0.0f); // dead zone
}

// --- canvas ---------------------------------------------------------------------------------

TEST(canvas_blend) {
    Canvas c;
    c.resize(10, 10);
    c.fill_rect(0, 0, 10, 10, Color::hex(0x204060));
    CHECK_EQ(c.data()[55], 0xFF204060u);
    c.fill_rect(0, 0, 10, 10, Color::hex(0xFFFFFF, 0.5f));
    uint32_t p = c.data()[55];
    CHECK_EQ(p >> 24, 0xFFu);
    CHECK(((p >> 16) & 0xFF) > 0x8C && ((p >> 16) & 0xFF) < 0x94); // ~0x8F
    Canvas t;
    t.resize(4, 4);
    t.fill_rrect(0, 0, 4, 4, 2, Color::hex(0xFFFFFF));
    CHECK((t.data()[0] >> 24) < 0x80); // rounded corner
    CHECK_EQ(t.data()[5] >> 24, 0xFFu);
    Image img;
    img.w = 2;
    img.h = 1;
    img.px = {0xFF000000u, 0xFFFFFFFFu};
    Image s = scale_image(img, 1, 1);
    CHECK(((s.px[0] >> 16) & 0xFF) > 0x7C && ((s.px[0] >> 16) & 0xFF) < 0x83);
}

// --- integration: a real book ---------------------------------------------------------------

TEST(engine_open_render) {
    const char* book = std::getenv("DECKREADER_TEST_BOOK");
    const char* fonts = std::getenv("DECKREADER_TEST_FONTS");
    const char* data = std::getenv("DECKREADER_TEST_ENGINE_DATA");
    if (!book || !fonts || !data) {
        std::fprintf(stderr, "  (skipped engine_open_render: set DECKREADER_TEST_BOOK/_FONTS/_ENGINE_DATA)\n");
        return;
    }
    CHECK(engine::init(fonts, data, temp_dir()));
    std::vector<std::string> faces = engine::font_faces();
    for (const FontDef& f : FONTS)
        if (f.face[0])
            CHECK(std::find(faces.begin(), faces.end(), f.face) != faces.end());
    CHECK(engine::check_book_file(book).empty());
    BookInfo info;
    Image cover;
    CHECK(engine::read_metadata(book, info, &cover, 120));
    CHECK(!info.title.empty());
    Book b;
    std::string err;
    CHECK(b.open(book, &err));
    ReaderSettings s;
    CHECK(b.configure(s, 1280, 800, 1.0f));
    CHECK(!b.configure(s, 1280, 800, 1.0f)); // nothing changed, no new layout
    CHECK(b.page_count() > 10);
    CHECK_EQ(b.pages_per_screen(), 2);
    std::vector<uint32_t> px(1280 * 800);
    CHECK(b.render(0, px.data(), 1280, 800));
    {
        // Margins hold from the very first layout, also at 4K (D25): with
        // 96 + 30 units at the bottom, the last 120 units stay blank.
        Book big;
        CHECK(big.open(book, &err));
        ReaderSettings m;
        m.vert_margin = 96;
        m.progress_line = true;
        const int W = 3840, H = 2160;
        const float u = H / 800.0f;
        CHECK(big.configure(m, W, H, u));
        std::vector<uint32_t> p4(static_cast<size_t>(W) * H);
        int bad_rows = 0;
        for (int page : {big.page_count() / 3, big.page_count() / 2}) {
            CHECK(big.render(page, p4.data(), W, H));
            const uint32_t bg = p4[static_cast<size_t>(H - 1) * W] & 0xFFFFFF;
            for (int y = H - static_cast<int>(120 * u); y < H; y++)
                for (int x = 0; x < W; x++)
                    if ((p4[static_cast<size_t>(y) * W + x] & 0xFFFFFF) != bg && x != W / 2 && std::abs(x - W / 2) > 3) {
                        bad_rows++;
                        break;
                    }
        }
        CHECK_EQ(bad_rows, 0);
    }
    b.go_to_page(10);
    std::string pos = b.position();
    CHECK(!pos.empty());
    s.font_size = 30;
    CHECK(b.configure(s, 1280, 800, 1.0f));
    CHECK(b.current_page() >= 10); // bigger text: the same words sit on a later page
    CHECK(std::abs(b.page_of_position(pos) - b.current_page()) <= 1);
    b.close();
    engine::shutdown();
}

// Every book in DECKREADER_TEST_BOOKS_DIR: reading settings changed in the
// middle of a book keep the reading position on screen, and going back to
// the first settings returns to exactly the same page (no drift, D14).
TEST(engine_position_stability) {
    const char* dir = std::getenv("DECKREADER_TEST_BOOKS_DIR");
    const char* fonts = std::getenv("DECKREADER_TEST_FONTS");
    const char* data = std::getenv("DECKREADER_TEST_ENGINE_DATA");
    if (!dir || !fonts || !data) {
        std::fprintf(stderr, "  (skipped engine_position_stability: set DECKREADER_TEST_BOOKS_DIR/_FONTS/_ENGINE_DATA)\n");
        return;
    }
    CHECK(engine::init(fonts, data, temp_dir()));
    std::vector<DirEntry> files;
    list_dir(dir, files);
    std::vector<ReaderSettings> variants;
    for (int f = 0; f < FONT_COUNT; f++) {
        ReaderSettings v;
        v.font = FONTS[f].id;
        variants.push_back(v);
    }
    ReaderSettings v;
    v.font_size = 31;
    variants.push_back(v);
    v = ReaderSettings{};
    v.line_spacing = 0;
    v.align = 2;
    variants.push_back(v);
    v = ReaderSettings{};
    v.book_styles = false;
    v.pages = 1;
    variants.push_back(v);
    v = ReaderSettings{};
    v.side_margin = 96;
    v.text_aa = 3;
    variants.push_back(v);
    v = ReaderSettings{};
    v.progress_line = true;
    v.vert_margin = 96;
    variants.push_back(v);
    int books = 0;
    for (const DirEntry& f : files) {
        if (!is_book_file(f.name))
            continue;
        books++;
        Book b;
        std::string err, path = path_join(dir, f.name);
        CHECK(b.open(path, &err));
        ReaderSettings base;
        b.configure(base, 1280, 800, 1.0f);
        b.go_to_page(b.page_count() / 2);
        const int start = b.current_page();
        const std::string anchor = b.position();
        for (const ReaderSettings& s : variants) {
            b.configure(s, 1280, 800, 1.0f);
            int p = b.page_of_position(anchor), cur = b.current_page();
            bool on_screen = p >= cur && p < cur + b.pages_per_screen();
            if (!on_screen)
                std::fprintf(stderr, "  %s, font %s: anchor on page %d, showing %d\n", f.name.c_str(), s.font.c_str(), p, cur);
            CHECK(on_screen);
        }
        b.configure(base, 1280, 800, 1.0f);
        if (b.current_page() != start)
            std::fprintf(stderr, "  %s: back on page %d, started on %d\n", f.name.c_str(), b.current_page(), start);
        CHECK_EQ(b.current_page(), start);
        CHECK_EQ(b.position(), anchor);
    }
    std::fprintf(stderr, "  position stability: %d books\n", books);
    engine::shutdown();
}

// --- audit 1 regressions (D22) ------------------------------------------------

TEST(archive_nested_and_total_ratio) {
    CHECK(nested_archive_name("OEBPS/inner.EPUB"));
    CHECK(nested_archive_name("a.docx"));
    CHECK(!nested_archive_name("OEBPS/ch1.xhtml"));
    std::vector<ArchiveEntry> nested = {{"mimetype", 20, 20}, {"x.epub", 100, 90}};
    CHECK(!check_archive(nested).empty());
    // 40 entries just under the per-entry threshold, 1000:1 overall.
    std::vector<ArchiveEntry> many(40, ArchiveEntry{"c.xhtml", 900000, 900});
    CHECK(!check_archive(many).empty());
    // A big ordinary book (3:1) passes.
    std::vector<ArchiveEntry> book(60, ArchiveEntry{"c.xhtml", 600000, 200000});
    CHECK(check_archive(book).empty());
    // No overflow with custom limits.
    ArchiveLimits l;
    l.max_total = UINT64_MAX - 10;
    l.max_entry = UINT64_MAX;
    l.ratio_min_size = UINT64_MAX;
    l.total_ratio_min_size = UINT64_MAX;
    std::vector<ArchiveEntry> big = {{"a", UINT64_MAX - 20, 1}, {"b", 100, 1}};
    CHECK(!check_archive(big, l).empty());
}

TEST(hostile_epubs) {
    const char* dir = std::getenv("DECKREADER_TEST_HOSTILE_DIR");
    if (!dir) {
        std::fprintf(stderr, "  (skipped hostile_epubs: set DECKREADER_TEST_HOSTILE_DIR)\n");
        return;
    }
    std::string d = dir;
    CHECK(!engine::check_book_file(d + "/nested.epub").empty());
    CHECK(!engine::check_book_file(d + "/many-small.epub").empty());
    // Central directory looks harmless (1000 bytes), so the check passes...
    CHECK(engine::check_book_file(d + "/size-mismatch.epub").empty());
    // ...and reading the entry stops at those 1000 bytes, although its
    // local header claims 2 GB and the data inflates to 20 MB.
    LVStreamRef file = LVOpenFileStream((d + "/size-mismatch.epub").c_str(), LVOM_READ);
    LVContainerRef arc = LVOpenArchieve(file);
    CHECK(!arc.isNull());
    if (!arc.isNull()) {
        LVStreamRef entry = arc->OpenStream(U"OEBPS/ch1.xhtml", LVOM_READ);
        CHECK(!entry.isNull());
        if (!entry.isNull()) {
            static char buf[65536];
            uint64_t total = 0;
            lvsize_t n = 0;
            while (entry->Read(buf, sizeof buf, &n) == LVERR_OK && n > 0 && total < (64u << 20))
                total += n;
            CHECK(total <= 1000);
        }
    }
    // A ZIP64 extra field running past its data: no crash, nothing big read.
    LVStreamRef bad = LVOpenFileStream((d + "/bad-zip64.epub").c_str(), LVOM_READ);
    LVContainerRef barc = bad.isNull() ? LVContainerRef() : LVOpenArchieve(bad);
    if (!barc.isNull()) {
        LVStreamRef e = barc->OpenStream(U"OEBPS/x.xhtml", LVOM_READ);
        if (!e.isNull()) {
            char buf[4096];
            lvsize_t n = 0;
            e->Read(buf, sizeof buf, &n);
            CHECK(n <= sizeof buf);
        }
    }
}

// Variation selectors are invisible: Standard Ebooks writes its back-links
// as "↩︎" (U+21A9 U+FE0E), which was drawn "↩?" (D26). The page must look
// exactly like the same text without them.
TEST(engine_variation_selectors_invisible) {
    const char* dir = std::getenv("DECKREADER_TEST_HOSTILE_DIR");
    const char* fonts = std::getenv("DECKREADER_TEST_FONTS");
    const char* data = std::getenv("DECKREADER_TEST_ENGINE_DATA");
    if (!dir || !fonts || !data) {
        std::fprintf(stderr, "  (skipped engine_variation_selectors_invisible: set DECKREADER_TEST_HOSTILE_DIR/_FONTS/_ENGINE_DATA)\n");
        return;
    }
    CHECK(engine::init(fonts, data, temp_dir()));
    const int W = 1280, H = 800;
    std::vector<uint32_t> with(W * H), without(W * H);
    for (int f = 0; f < FONT_COUNT; f++) {
        ReaderSettings s;
        s.font = FONTS[f].id;
        for (auto [name, px] : {std::pair<const char*, uint32_t*>{"/vs-with.epub", with.data()},
                                {"/vs-without.epub", without.data()}}) {
            Book b;
            std::string err;
            CHECK(b.open(std::string(dir) + name, &err));
            CHECK(b.configure(s, W, H, 1.0f));
            CHECK(b.render(0, px, W, H));
        }
        int ink = 0;
        for (uint32_t p : with)
            ink += (p & 0xFFFFFF) != (with[0] & 0xFFFFFF);
        CHECK(ink > 1000); // something was drawn
        CHECK(with == without);
    }
    engine::shutdown();
}

// Black-on-transparent line art (Standard Ebooks' engravings, diagrams)
// stays visible in dark themes: it is drawn over a plate of the text colour
// (D26). In light themes the page shows through, as before.
TEST(engine_line_art_plate_in_dark_themes) {
    const char* dir = std::getenv("DECKREADER_TEST_HOSTILE_DIR");
    const char* fonts = std::getenv("DECKREADER_TEST_FONTS");
    const char* data = std::getenv("DECKREADER_TEST_ENGINE_DATA");
    if (!dir || !fonts || !data) {
        std::fprintf(stderr, "  (skipped engine_line_art_plate_in_dark_themes: set DECKREADER_TEST_HOSTILE_DIR/_FONTS/_ENGINE_DATA)\n");
        return;
    }
    CHECK(engine::init(fonts, data, temp_dir()));
    const int W = 1280, H = 800;
    std::vector<uint32_t> px(W * H);
    for (int t = 0; t < THEME_COUNT; t++) {
        ReaderSettings s;
        s.theme = t;
        Book b;
        std::string err;
        CHECK(b.open(std::string(dir) + "/line-art.epub", &err));
        CHECK(b.configure(s, W, H, 1.0f));
        b.set_colors(THEMES[t].bg, THEMES[t].text); // as the reader does
        CHECK(b.render(0, px.data(), W, H));
        int plate = 0, ink = 0;
        for (uint32_t p : px) {
            plate += (p & 0xFFFFFF) == THEMES[t].text;
            ink += (p & 0xFFFFFF) == 0;
        }
        if (THEMES[t].dark) {
            CHECK(plate > 30000); // the transparent part of the 300x300 picture, at least
            CHECK(ink > 3000);    // and its black lines on it
        } else {
            CHECK(plate < 1000);
        }
    }
    engine::shutdown();
}

TEST(state_save_failure_backs_off) {
    StateStore s("/nonexistent-deckreader-dir/state.json");
    s.mark_dirty();
    CHECK_EQ(s.save_due_in(1000), 0);
    s.save_if_due(1000); // fails
    CHECK(s.dirty());
    CHECK_EQ(s.save_due_in(1000), 4000); // not again at once: 2 s << 1
    s.save_if_due(5000);                 // fails again
    CHECK_EQ(s.save_due_in(5000), 8000); // and longer each time
}

TEST(state_unreadable_file_is_kept) {
    std::string p = temp_dir() + "/state.json";
    const std::string original = "{\"version\":1,\"books\":{\"/x\":{\"percent\":5}}}";
    CHECK(write_file_atomic(p, original));
    ::chmod(p.c_str(), 0);
    StateStore s(p);
    bool loaded = s.load();
    s.mark_dirty();
    bool saved = s.save();
    ::chmod(p.c_str(), 0600);
    if (::geteuid() != 0) { // root reads anything
        CHECK(!loaded);
        CHECK(!saved);
        CHECK_EQ(s.save_due_in(0), -1);
        std::string back;
        CHECK(read_file(p, back));
        CHECK_EQ(back, original);
    }
}

TEST(config_accent_strict) {
    for (const char* bad : {"0x10A3", "+10A38", "-00001", "#12345G", "10A38F0"}) {
        Config c = parse_config(std::string("[interface]\naccent_color = ") + bad + "\n", "/h");
        CHECK_EQ(c.warnings.size(), 1u);
        CHECK_EQ(c.accent, 0x10A38Fu);
    }
    CHECK_EQ(parse_config("[interface]\naccent_color = #ff8800\n", "/h").accent, 0xFF8800u);
}

TEST(mountinfo_real_devices_only) {
    const std::string mi = "80 22 0:44 / /run/media rw - tmpfs tmpfs rw\n"
                           "82 80 0:50 / /run/media/deck rw - tmpfs tmpfs rw\n"
                           "83 22 0:51 / /media/x\\08y rw - fuse.portal portal rw\n";
    CHECK(removable_mounts(mi).empty()); // empty per-user tmpfs, no drive
    std::vector<Mount> all = parse_mountinfo(mi);
    CHECK_EQ(all[2].mount_point, "/media/x\\08y"); // "\\08y" is not octal: kept
}

TEST(make_dirs_rejects_files) {
    std::string d = temp_dir();
    CHECK(write_file_atomic(d + "/f", "x"));
    CHECK(!make_dirs(d + "/f"));
    CHECK(make_dirs(d + "/a/b"));
    CHECK(is_dir(d + "/a/b"));
}

TEST(toc_out_of_order) {
    // A TOC listing notes (page 90) before chapter 3 (page 40).
    std::vector<TocEntry> toc = {{"One", 0, 2}, {"Two", 0, 20}, {"Notes", 0, 90}, {"Three", 0, 40}, {"Four", 0, 60}};
    CHECK_EQ(toc_chapter_at(toc, 50), 3); // "Three", not stopped by "Notes"
    CHECK_EQ(toc_chapter_at(toc, 95), 2);
    CHECK_EQ(toc_chapter_at(toc, 1), -1);
    CHECK_EQ(toc_next_page(toc, 21), 40); // the nearest, not the first listed
    CHECK_EQ(toc_next_page(toc, 95), -1);
    CHECK_EQ(toc_prev_page(toc, 45), 40);
    CHECK_EQ(toc_prev_page(toc, 2), -1);
}

TEST(start_screen_recents) {
    std::string d = temp_dir();
    make_dirs(d + "/Books/Sub");
    for (const char* f : {"/Books/a.epub", "/Books/Sub/b.epub", "/elsewhere.epub", "/hidden.epub"})
        write_file_atomic(d + f, "x");
    Config cfg = parse_config("[library]\nfolder = " + d + "/Books\nremovable_media = no\n"
                              "[interface]\nrecently_read = 3\n", "/h");
    StateStore st;
    st.at(d + "/Books/a.epub").last_read = 10;
    st.at(d + "/Books/Sub/b.epub").last_read = 30;
    st.at(d + "/elsewhere.epub").last_read = 20;
    st.at(d + "/gone.epub").last_read = 40; // file deleted: left out
    BookState& h = st.at(d + "/hidden.epub");
    h.last_read = 50;
    h.hidden = true; // "Remove from recently read"
    std::vector<LibEntry> v = list_root(cfg, st, "", "/h");
    CHECK_EQ(v.size(), 5u); // source, header, three books
    if (v.size() == 5) {
        CHECK(v[0].kind == LibEntry::Source);
        CHECK(v[1].kind == LibEntry::Header);
        CHECK_EQ(v[2].path, d + "/Books/Sub/b.epub"); // most recent first
        CHECK_EQ(v[2].tag, "Local");
        CHECK(v[2].recent);
        CHECK_EQ(v[3].path, d + "/elsewhere.epub");
        CHECK_EQ(v[3].tag, ""); // not under any source
    }
    cfg.recently_read = 0;
    CHECK_EQ(list_root(cfg, st, "", "/h").size(), 1u); // no header without books
    Config bad = parse_config("[interface]\nrecently_read = 99\n", "/h");
    CHECK_EQ(bad.recently_read, 5);
    CHECK_EQ(bad.warnings.size(), 1u);
}

TEST(state_hidden_and_progress_line) {
    StateStore s;
    s.settings.progress_line = true;
    s.at("/a").last_read = 5;
    s.at("/b").last_read = 9;
    s.at("/b").hidden = true;
    StateStore t;
    CHECK(t.from_json(s.to_json()));
    CHECK(t.settings.progress_line);
    CHECK(t.find("/b")->hidden);
    CHECK_EQ(t.recent(10).size(), 1u);
    ReaderSettings a, b;
    b.progress_line = true;
    CHECK(a.layout_differs(b)); // the band under the text changes the layout
}

TEST(input_two_controllers) {
    Input in;
    std::vector<InputEvent> out;
    SDL_Event b{};
    b.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    b.gbutton.which = 1;
    b.gbutton.button = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    in.handle(b, 0, out);
    // The other controller is unplugged: pad 1 keeps repeating.
    SDL_Event r{};
    r.type = SDL_EVENT_GAMEPAD_REMOVED;
    r.gdevice.which = 2;
    in.handle(r, 10, out);
    CHECK(in.next_due(10) >= 0);
    // Pad 2's idle stick doesn't overwrite pad 1's scrub.
    in.set_stick_scrub(true);
    SDL_Event a{};
    a.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    a.gaxis.which = 1;
    a.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTX;
    a.gaxis.value = 30000;
    in.handle(a, 20, out);
    a.gaxis.which = 2;
    a.gaxis.value = 100;
    in.handle(a, 30, out);
    CHECK(in.scrub_x() > 0.8f);
    // Removing pad 1 forgets its held button.
    r.gdevice.which = 1;
    in.handle(r, 40, out);
    CHECK_EQ(in.next_due(40), -1);
}

int main() {
    for (auto& [name, fn] : tests()) {
        g_test = name;
        fn();
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
