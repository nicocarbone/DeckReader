// Unit tests for DeckReader's pure logic. Run by tests/run.sh (inside the
// SDK). With DECKREADER_TEST_BOOK set (and the fonts/engine folders), an
// integration test also opens, lays out and renders a real book; with
// DECKREADER_TEST_FAKE_WALLABAG (tests/fake-wallabag.py), the wallabag sync
// runs against a fake server on 127.0.0.1.
#include "about.h"
#include "articles.h"
#include "book.h"
#include "canvas.h"
#include "config.h"
#include "epubguard.h"
#include "epubwrite.h"
#include "fsutil.h"
#include "http.h"
#include "input.h"
#include "keepawake.h"
#include "library.h"
#include "menu.h"
#include "metacache.h"
#include "settings.h"
#include "state.h"
#include "util.h"
#include "wallabag.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <map>
#include <mutex>
#include <spawn.h>
#include <climits>
#include <cstdio>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <lvcontainer.h>
#include <lvcontaineriteminfo.h>
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

// A test setting from the environment; unset and empty both mean "not
// given" (tests/run.sh passes an empty book folder when there is none, as
// in CI).
static const char* test_env(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? v : nullptr;
}

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

TEST(config_keep_awake) {
    Config c = parse_config("", "/h");
    CHECK(c.keep_awake);
    CHECK_EQ(c.keep_awake_minutes, 10);
    c = parse_config("[reader]\nkeep_awake = no\nkeep-awake-minutes = 30\n", "/h");
    CHECK(!c.keep_awake);
    CHECK_EQ(c.keep_awake_minutes, 30);
    CHECK(c.warnings.empty());
    for (const char* bad : {"0", "241", "ten", "", "5 min"}) {
        c = parse_config(std::string("[reader]\nkeep_awake_minutes = ") + bad + "\n", "/h");
        CHECK_EQ(c.keep_awake_minutes, 10);
        CHECK_EQ(c.warnings.size(), 1u);
    }
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

// --- menus and About ------------------------------------------------------------------

TEST(menu_headers_skipped) {
    MenuItem head;
    head.label = "Section";
    head.header = true;
    MenuItem sep;
    sep.separator = true;
    Menu m;
    m.items = {head, {"One"}, {"Two"}, sep, head, {"Three"}};
    auto press = [&](Action a, bool repeat = false) {
        m.handle({a, Source::Script, repeat});
        return m.selected;
    };
    CHECK_EQ(press(Action::Down), 2);       // starts on "One", not the header
    CHECK_EQ(press(Action::Down), 5);       // over the line and the header
    CHECK_EQ(press(Action::Down), 1);       // wraps around, past the header
    CHECK_EQ(press(Action::Up), 5);
    CHECK_EQ(press(Action::Up, true), 2);   // held
    CHECK_EQ(press(Action::Up, true), 1);
    CHECK_EQ(press(Action::Up, true), 1);   // held: stops at the first row
    CHECK_EQ(press(Action::R2), 5);
    CHECK_EQ(press(Action::L2), 1);
    CHECK(m.handle({Action::Back, Source::Script, false}) == false);
}

TEST(about_helpers) {
    BuildInfo b = parse_build_info("date=2026-11-18\ncommit=1a2b3c4-modified\n");
    CHECK_EQ(b.date, "2026-11-18");
    CHECK_EQ(b.commit, "1a2b3c4-modified");
    CHECK_EQ(build_text(b), "2026-11-18 \xC2\xB7 1a2b3c4-modified");
    CHECK_EQ(build_text(parse_build_info("date=2026-11-18\ncommit=\n")), "2026-11-18");
    CHECK_EQ(build_text(parse_build_info("")), "development build");
    CHECK_EQ(device_name("Valve", "Galileo"), "Steam Deck OLED");
    CHECK_EQ(device_name("Valve", "Jupiter"), "Steam Deck LCD");
    CHECK_EQ(device_name("Micro-Star International Co., Ltd.", "MS-7C91"), "PC");
    CHECK_EQ(device_name("", ""), "PC");
    CHECK_EQ(short_path("/home/deck/.config/deckreader", "/home/deck"), "~/.config/deckreader");
    CHECK_EQ(short_path("/home/deckard/Books", "/home/deck"), "/home/deckard/Books");
    CHECK_EQ(short_path("/run/media/deck/SD", "/home/deck"), "/run/media/deck/SD");
    CHECK_EQ(short_path("/home/deck/x", ""), "/home/deck/x");
}

// --- keep awake (D28) -------------------------------------------------------------------

static std::string wait_status(KeepAwake& ka) {
    std::string st;
    for (int i = 0; i < 300; i++) {
        st = ka.status();
        if (st != "starting\xE2\x80\xA6" && (ka.held() || st != "Only while reading"))
            break;
        usleep(10000);
    }
    return st;
}

TEST(keep_awake_status_file) {
    CHECK_EQ(parse_keep_awake_status("request: screensaver accepted, portal not running\n"),
             "screensaver accepted, portal not running");
    CHECK_EQ(parse_keep_awake_status(""), "");
    CHECK_EQ(parse_keep_awake_status("garbage"), "");
}

// The helper's life, with a stand-in that reports like the real one.
TEST(keep_awake_lifecycle) {
    std::string dir = temp_dir();
    std::string fake = dir + "/fake-helper", status = dir + "/status";
    write_file_atomic(fake, "#!/bin/sh\n[ \"$1\" = --status ] && "
                            "printf 'request: screensaver accepted, portal not running\\n' > \"$2\"\n"
                            "exec sleep 30\n");
    chmod(fake.c_str(), 0755);
    KeepAwake ka;
    ka.setup(fake, status, 0); // keep_awake = no
    ka.activity(1000);
    CHECK(!ka.held());
    CHECK_EQ(ka.next_due(1000), -1);
    CHECK_EQ(ka.status(), "Off (config.ini)");

    ka.setup(fake, status, 10);
    CHECK_EQ(ka.status(), "Only while reading");
    ka.activity(1000);
    CHECK(ka.held());
    CHECK_EQ(ka.next_due(1000), 600000);
    CHECK_EQ(wait_status(ka), "screensaver accepted, portal not running");
    ka.activity(301000); // input: ten minutes from now
    CHECK_EQ(ka.next_due(301000), 600000);
    ka.tick(900999);
    CHECK(ka.held());
    ka.tick(901000); // ten minutes without input
    CHECK(!ka.held());
    CHECK_EQ(ka.next_due(901000), -1);
    CHECK(!is_file(status));
    CHECK_EQ(ka.status(), "Only while reading");
    ka.activity(902000); // reading again
    CHECK(ka.held());
    ka.release(); // book closed
    CHECK(!ka.held());

    // A missing helper is reported and not retried on every page turn.
    ka.setup(dir + "/missing", status, 10);
    ka.activity(0);
    CHECK(!ka.held());
    CHECK_EQ(ka.status(), "deckreader-keep-awake not found");
    ka.activity(1);
    CHECK(!ka.held());
}

// The real helper (built next to this test) without a session bus: it
// exits and the reason is shown.
TEST(keep_awake_real_helper) {
    char exe[PATH_MAX] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    std::string helper = path_join(path_dirname(std::string(exe, n > 0 ? static_cast<size_t>(n) : 0)),
                                   "deckreader-keep-awake");
    CHECK(is_file(helper));
    const char* old = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    std::string saved = old ? old : "";
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/bus", 1);
    KeepAwake ka;
    ka.setup(helper, temp_dir() + "/status", 10);
    ka.activity(0);
    if (old)
        setenv("DBUS_SESSION_BUS_ADDRESS", saved.c_str(), 1);
    else
        unsetenv("DBUS_SESSION_BUS_ADDRESS");
    std::string st;
    for (int i = 0; i < 300 && ka.held(); i++) {
        st = ka.status();
        usleep(10000);
    }
    st = ka.status();
    CHECK(!ka.held());
    CHECK_EQ(st, "screensaver no session bus, portal no session bus");
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
    const char* book = test_env("DECKREADER_TEST_BOOK");
    const char* fonts = test_env("DECKREADER_TEST_FONTS");
    const char* data = test_env("DECKREADER_TEST_ENGINE_DATA");
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
    const char* dir = test_env("DECKREADER_TEST_BOOKS_DIR");
    const char* fonts = test_env("DECKREADER_TEST_FONTS");
    const char* data = test_env("DECKREADER_TEST_ENGINE_DATA");
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
    const char* dir = test_env("DECKREADER_TEST_HOSTILE_DIR");
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
    const char* dir = test_env("DECKREADER_TEST_HOSTILE_DIR");
    const char* fonts = test_env("DECKREADER_TEST_FONTS");
    const char* data = test_env("DECKREADER_TEST_ENGINE_DATA");
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
    const char* dir = test_env("DECKREADER_TEST_HOSTILE_DIR");
    const char* fonts = test_env("DECKREADER_TEST_FONTS");
    const char* data = test_env("DECKREADER_TEST_ENGINE_DATA");
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

// --- wallabag (D31) ----------------------------------------------------------

TEST(config_wallabag) {
    Config cfg = parse_config("[wallabag]\n"
                              "server = https://app.wallabag.it/\n"
                              "client_id = 1_abc\n"
                              "client-secret = \"s3cr et\"\n"
                              "username = nico\n"
                              "password = \"  spaced # pass \"\n"
                              "max_articles = 20\n",
                              "/home/u");
    CHECK(cfg.warnings.empty());
    CHECK(cfg.wallabag.configured());
    CHECK_EQ(cfg.wallabag.server, "https://app.wallabag.it/");
    CHECK_EQ(cfg.wallabag.client_secret, "s3cr et");
    CHECK_EQ(cfg.wallabag.password, "  spaced # pass ");
    CHECK_EQ(cfg.wallabag.max_articles, 20);
    CHECK(cfg.wallabag.images);
    CHECK(!parse_config("[wallabag]\nimages = no\n", "/home/u").wallabag.images);

    Config off = parse_config("", "/home/u");
    CHECK(!off.wallabag.configured());
    CHECK_EQ(off.wallabag.max_articles, 50);
    CHECK(off.warnings.empty());

    // Half set up: off, and the log says what is missing.
    Config half = parse_config("[wallabag]\nserver = https://w.example\nusername = nico\nmax_articles = 0\n", "/home/u");
    CHECK(!half.wallabag.configured());
    CHECK_EQ(half.wallabag.max_articles, 50);
    CHECK_EQ(half.warnings.size(), 2u);
    CHECK(half.warnings.size() == 2 && half.warnings[1].find("missing: client_id, client_secret, password") != std::string::npos);

    // Not a web address; the password never appears in a warning.
    Config bad = parse_config("[wallabag]\nserver = app.wallabag.it\nclient_id = a\nclient_secret = b\n"
                              "username = c\npassword = hunter2\n",
                              "/home/u");
    CHECK(!bad.wallabag.configured());
    CHECK(!bad.warnings.empty() && bad.warnings[0].find("app.wallabag.it") != std::string::npos);
    for (const std::string& w : bad.warnings)
        CHECK(w.find("hunter2") == std::string::npos);

    // No host (V3-BUG-11); plain http on the internet is warned about,
    // at home it isn't (V3-SEC-4).
    const std::string rest = "client_id = a\nclient_secret = b\nusername = c\npassword = d\n";
    Config empty_host = parse_config("[wallabag]\nserver = https://\n" + rest, "/home/u");
    CHECK(!empty_host.wallabag.configured());
    CHECK(!empty_host.warnings.empty());
    Config plain = parse_config("[wallabag]\nserver = http://app.wallabag.it\n" + rest, "/home/u");
    CHECK(plain.wallabag.configured());
    CHECK(plain.warnings.size() == 1 && plain.warnings[0].find("unencrypted") != std::string::npos);
    for (const char* home : {"http://192.168.1.5:8080/wallabag", "http://localhost:8000", "http://[fd00::5]/"})
        CHECK(parse_config(std::string("[wallabag]\nserver = ") + home + "\n" + rest, "/home/u").warnings.empty());
}

TEST(wallabag_helpers) {
    CHECK_EQ(wallabag_base("https://app.wallabag.it/"), "https://app.wallabag.it");
    CHECK_EQ(wallabag_base("  http://192.168.1.5:8080/wallabag// "), "http://192.168.1.5:8080/wallabag");
    CHECK_EQ(wallabag_base("HTTPS://W.example"), "HTTPS://W.example");
    CHECK_EQ(wallabag_base("app.wallabag.it"), "");
    CHECK_EQ(wallabag_base("ftp://x.example"), "");
    CHECK_EQ(wallabag_base("https://"), "");

    CHECK_EQ(parse_iso8601("2026-11-19T10:20:30+0000"), 1795083630);
    CHECK_EQ(parse_iso8601("2026-11-19T10:20:30Z"), 1795083630);
    CHECK_EQ(parse_iso8601("2026-11-19T10:20:30"), 1795083630);
    CHECK_EQ(parse_iso8601("2026-11-19T08:00:00+0100"), 1795071600);
    CHECK_EQ(parse_iso8601("2026-11-19T08:00:00+01:00"), 1795071600);
    CHECK_EQ(parse_iso8601("2026-11-19T06:50:30.123-03:30"), 1795083630);
    CHECK_EQ(parse_iso8601(""), 0);
    CHECK_EQ(parse_iso8601("yesterday"), 0);
    CHECK_EQ(parse_iso8601("2026-13-19T10:20:30Z"), 0);
    CHECK_EQ(parse_iso8601("2026-11-19T10:20:30+1"), 0);
    CHECK_EQ(parse_iso8601("2026-11-19T10:20:30z"), 1795083630);
    CHECK_EQ(parse_iso8601("2026-11-19 10:20:30Z"), 1795083630);
    CHECK_EQ(parse_iso8601("2026-11-19T-1:20:30Z"), 0);
    CHECK_EQ(parse_iso8601("2026-11-19T10:-2:30Z"), 0);
    CHECK_EQ(parse_iso8601("2026-11-19T10:20:30+2500"), 0);
    CHECK_EQ(parse_iso8601("2026-11-19T10:20:30+0175"), 0);

    CHECK_EQ(parse_token(R"({"access_token":"abc","expires_in":3600,"token_type":"bearer"})"), "abc");
    CHECK_EQ(parse_token(R"({"error":"invalid_grant"})"), "");
    CHECK_EQ(parse_token("<html>"), "");
    CHECK_EQ(parse_token(R"({"access_token":"abc\r\nX-Other: 1"})"), ""); // it goes into a header
    CHECK_EQ(parse_token(R"({"access_token":"a b"})"), "");
    CHECK_EQ(parse_token(R"({"access_token":""})"), "");
    CHECK_EQ(token_error(400, R"({"error":"invalid_grant","error_description":"x"})", ""),
             "wallabag refused the username or password.");
    CHECK_EQ(token_error(400, R"({"error":"invalid_client"})", ""), "wallabag refused the client ID or secret.");
    CHECK_EQ(token_error(400, R"({"error":"unsupported_grant_type"})", ""),
             "wallabag refused the login (unsupported_grant_type).");
    CHECK_EQ(token_error(301, "", "https://w.example/wallabag/oauth/v2/token"),
             "The server sent the login to https://w.example/wallabag. Use that as the server address.");
    CHECK_EQ(token_error(302, "", ""), "The server redirected the login (HTTP 302). Check the server address.");
    CHECK_EQ(token_error(404, "<html>", ""), "No wallabag at this server address (HTTP 404).");
    CHECK_EQ(token_error(502, "", ""), "wallabag answered HTTP 502 to the login.");

    std::vector<WallabagEntry> entries;
    CHECK(parse_entries(R"({"page":1,"limit":30,"pages":1,"total":3,"_links":{},"_embedded":{"items":[
        {"id":12,"title":"Rice","created_at":"2026-11-19T08:00:00+0100","url":"https://e.example/1"},
        {"id":"13","title":"string id"},
        {"id":11,"title":null,"created_at":null}]}})",
                        entries));
    CHECK_EQ(entries.size(), 2u);
    CHECK(entries.size() == 2 && entries[0].id == 12 && entries[0].title == "Rice" && entries[0].created == 1795071600);
    CHECK(entries.size() == 2 && entries[1].id == 11 && entries[1].title.empty() && entries[1].created == 0);
    int pages = 0;
    CHECK(parse_entries(R"({"page":1,"pages":3,"_embedded":{"items":[]}})", entries, &pages) && entries.empty() &&
          pages == 3);
    CHECK(parse_entries(R"({"_embedded":{"items":[]}})", entries, &pages) && entries.empty() && pages == 1);
    CHECK(!parse_entries(R"({"error":"x"})", entries));
    CHECK(!parse_entries("not json", entries));

    CHECK_EQ(article_file_name(42), "42.epub");
    CHECK_EQ(article_id("42.epub"), 42);
    CHECK_EQ(article_id("042.epub"), 0);
    CHECK_EQ(article_id("4a2.epub"), 0);
    CHECK_EQ(article_id("42.EPUB"), 0);
    CHECK_EQ(article_id(".epub"), 0);
    CHECK_EQ(article_id("42.epub.tmp-AbC123"), 0);
    CHECK_EQ(article_id("Some Book.epub"), 0);
    CHECK_EQ(article_id("1234567890123456789.epub"), 0); // too long for an id

    // 3 was marked as read on the Deck: not downloaded again, not kept.
    SyncPlan plan = plan_sync({{5, "", 0}, {4, "", 0}, {3, "", 0}, {2, "", 0}, {5, "", 0}}, {2, 3, 4, 9}, {3});
    CHECK_EQ(plan.download.size(), 1u);
    CHECK(!plan.download.empty() && plan.download[0].id == 5);
    CHECK(plan.remove == std::vector<int64_t>({3, 9}));

    CHECK_EQ(form_encode({{"a b", "c&d=\xC3\xA9+%"}, {"x", ""}}), "a%20b=c%26d%3D%C3%A9%2B%25&x=");
    CHECK_EQ(url_host("https://user@app.wallabag.it:443/x?y"), "app.wallabag.it:443");
    CHECK_EQ(url_host("http://127.0.0.1"), "127.0.0.1");
    CHECK_EQ(host_name("App.Example.com:443"), "app.example.com");
    CHECK_EQ(host_name("[::1]:8080"), "::1");
    CHECK_EQ(host_name("127.0.0.1"), "127.0.0.1");

    // Pictures only from the internet (V3-SEC-1, D32).
    for (const char* ip : {"8.8.8.8", "172.32.0.1", "100.128.0.1", "::ffff:8.8.8.8", "2001:4860:4860::8888"})
        CHECK(public_ip(ip));
    for (const char* ip : {"127.0.0.1", "127.1.2.3", "10.1.2.3", "172.16.0.1", "172.31.255.255", "192.168.1.1",
                           "169.254.169.254", "100.64.0.1", "0.0.0.0", "192.0.0.1", "198.18.0.1", "224.0.0.1",
                           "255.255.255.255", "::1", "::", "fe80::1", "fd00::1", "fc00::1", "ff02::1", "fec0::1",
                           "::ffff:127.0.0.1", "::ffff:192.168.0.1", "::127.0.0.1", "localhost", "", "8.8.8"})
        CHECK(!public_ip(ip));
    CHECK(is_ip("10.0.0.1") && is_ip("::1") && !is_ip("example.com") && !is_ip("localhost"));
}

TEST(wallabag_when_text) {
    const char* old = std::getenv("TZ");
    std::string saved = old ? old : "";
    setenv("TZ", "UTC", 1);
    tzset();
    const int64_t t = 1795082700; // 2026-11-19 10:05 UTC
    CHECK_EQ(when_text(t, t + 8 * 3600, true), "today 10:05");
    CHECK_EQ(when_text(t, t, false), "today 10:05 AM");
    CHECK_EQ(when_text(t + 12 * 3600, t + 13 * 3600, false), "today 10:05 PM");
    CHECK_EQ(when_text(t, t + 24 * 3600, true), "yesterday 10:05");
    CHECK_EQ(when_text(t, t + 3 * 86400, true), "19 Nov");
    CHECK_EQ(when_text(t, t + 60 * 86400, true), "19 Nov 2026");
    CHECK_EQ(when_text(t - 3600, t, true), "today 09:05"); // as the reading clock
    CHECK_EQ(when_text(t - 3600, t, false), "today 9:05 AM");
    if (old)
        setenv("TZ", saved.c_str(), 1);
    else
        unsetenv("TZ");
    tzset();
}

TEST(wallabag_state_and_library) {
    StateStore st;
    st.wallabag_archive = {5, 7};
    st.wallabag_updated = 1795082700;
    st.at("/a/5.epub").percent = 100;
    StateStore back;
    CHECK(back.from_json(st.to_json()));
    CHECK(back.wallabag_archive == std::vector<int64_t>({5, 7}));
    CHECK_EQ(back.wallabag_updated, 1795082700);
    back.forget("/a/5.epub");
    CHECK(back.find("/a/5.epub") == nullptr);
    CHECK(back.dirty());
    StateStore none;
    CHECK(none.from_json(R"({"books":{},"wallabag":{"archive":[3,"x",-1],"updated":"bad"}})"));
    CHECK(none.wallabag_archive == std::vector<int64_t>({3}));
    CHECK_EQ(none.wallabag_updated, 0);
    CHECK(none.to_json().find("wallabag") != std::string::npos);
    StateStore empty;
    CHECK(empty.to_json().find("wallabag") == std::string::npos);

    // The Articles source comes after the book folders.
    Config cfg = parse_config("[library]\nfolder = /books\nremovable_media = no\n", "/home/u");
    StateStore s2;
    std::vector<LibEntry> root = list_root(cfg, s2, "", "/home/u", "/data/articles");
    CHECK_EQ(root.size(), 2u);
    CHECK(root.size() == 2 && root[1].articles && root[1].name == "Articles" && root[1].path == "/data/articles" &&
          root[1].tag == "Article" && root[1].icon == LibEntry::IconArticles);
    CHECK_EQ(list_root(cfg, s2, "", "/home/u").size(), 1u);

    // Newest first (an article's file date is when it was saved).
    MetaCache meta;
    std::vector<LibEntry> list(3);
    list[0].name = "a.epub";
    list[0].path = "/x/a.epub";
    list[0].mtime = 100;
    list[1].name = "b.epub";
    list[1].path = "/x/b.epub";
    list[1].mtime = 300;
    list[2].name = "c.epub";
    list[2].path = "/x/c.epub";
    list[2].mtime = 200;
    sort_entries(list, SortMode::Newest, meta, s2);
    CHECK(list[0].name == "b.epub" && list[1].name == "c.epub" && list[2].name == "a.epub");
    CHECK_EQ(std::string(sort_label(SortMode::Newest)), "Sorted by newest");
}

// The names and contents of a ZIP, read with the engine's own archive code,
// to their end: a stored entry's stream stops at the entry's end
// (patches/crengine-ng-stream-fragment.patch, D32).
static std::map<std::string, std::string> zip_entries(const std::string& path) {
    std::map<std::string, std::string> out;
    LVStreamRef file = LVOpenFileStream(path.c_str(), LVOM_READ);
    LVContainerRef arc = file.isNull() ? LVContainerRef() : LVOpenArchieve(file);
    if (arc.isNull())
        return out;
    for (int i = 0; i < arc->GetObjectCount(); i++) {
        const LVContainerItemInfo* item = arc->GetObjectInfo(i);
        if (!item || item->IsContainer())
            continue;
        LVStreamRef e = arc->OpenStream(item->GetName(), LVOM_READ);
        std::string data;
        if (!e.isNull()) {
            char buf[4096];
            lvsize_t n = 0;
            while (e->Read(buf, sizeof buf, &n) == LVERR_OK && n > 0)
                data.append(buf, n);
            if (data.size() != e->GetSize())
                data = "<read " + std::to_string(data.size()) + " of " + std::to_string(e->GetSize()) + " bytes>";
        }
        out[UnicodeToUtf8(lString32(item->GetName())).c_str()] = data;
    }
    return out;
}

TEST(epub_writer) {
    CHECK_EQ(xml_escape("a<b>&\"c'"), "a&lt;b&gt;&amp;&quot;c'");
    CHECK_EQ(xml_escape(std::string("a\0b\x01\tc\x1f", 7)), "ab\tc"); // what XML can't hold

    const std::string base = "https://www.example.com/news/2026/story.html?x=1#top";
    CHECK_EQ(resolve_url(base, "https://cdn.example.net/a.png"), "https://cdn.example.net/a.png");
    CHECK_EQ(resolve_url(base, "//cdn.example.net/a.png"), "https://cdn.example.net/a.png");
    CHECK_EQ(resolve_url(base, "/img/a.png"), "https://www.example.com/img/a.png");
    CHECK_EQ(resolve_url(base, "img/a.png#x"), "https://www.example.com/news/2026/img/a.png");
    CHECK_EQ(resolve_url("https://example.com", "a.png"), "https://example.com/a.png");
    CHECK_EQ(resolve_url(base, "data:image/gif;base64,R0lGOD"), "");
    CHECK_EQ(resolve_url(base, "javascript:alert(1)"), "");
    CHECK_EQ(resolve_url(base, ""), "");
    CHECK_EQ(resolve_url("file:///etc/passwd", "a.png"), "");
    // As browsers do (V3-BUG-3, V3-BUG-4 in the second audit, D32).
    CHECK_EQ(resolve_url("https://example.com?p=123", "img.png"), "https://example.com/img.png");
    CHECK_EQ(resolve_url(base, "?w=800"), "https://www.example.com/news/2026/story.html?w=800");
    CHECK_EQ(resolve_url(base, "../img/a.png"), "https://www.example.com/news/img/a.png");
    CHECK_EQ(resolve_url(base, "./a/../b.png"), "https://www.example.com/news/2026/b.png");
    CHECK_EQ(resolve_url("https://example.com/a/b", "/../../x.png"), "https://example.com/x.png");
    CHECK_EQ(resolve_url("https://example.com/a/b/../c/", "d.png"), "https://example.com/a/c/d.png");
    CHECK_EQ(resolve_url(base, "my photo.jpg"), "https://www.example.com/news/2026/my%20photo.jpg");
    CHECK_EQ(resolve_url(base, "/caf\xC3\xA9.png?q=a b"), "https://www.example.com/caf%C3%A9.png?q=a%20b");
    CHECK_EQ(resolve_url(base, "/a\n.png"), "https://www.example.com/a.png");
    CHECK_EQ(resolve_url(base, "img/a:b.png"), "https://www.example.com/news/2026/img/a:b.png");
    CHECK_EQ(resolve_url(base, "?t=1:2"), "https://www.example.com/news/2026/story.html?t=1:2");
    CHECK_EQ(resolve_url(base, "HTTPS://cdn.example.net/x%20y.png"), "https://cdn.example.net/x%20y.png");
    CHECK_EQ(resolve_url(base, "mailto:a@example.com"), "");

    const std::string html =
        "<p>One</p><figure><IMG class=x src=\"/a.png\" alt=\"A &amp; B\" width=640></figure>"
        "<img src=\"data:image/gif;base64,R0lG\" data-src='https://cdn.example.net/lazy.jpg'/>"
        "<picture><source srcset=\"/b.webp 1x\" type=\"image/webp\"><img src=\"/a.png\"></picture>"
        "<img srcset=\"/s-300.jpg 300w, /s-1200.jpg 1200w, /s-4000.jpg 4000w\">"
        "<img src=\"/q.png?w=1&amp;h=2\"><img alt=\"no source\"><imgx src=\"/not.png\">"
        "<p>a > b</p><img src=\"/c.avif\">";
    std::vector<std::string> pics = article_images(html, base);
    CHECK(pics == std::vector<std::string>({"https://www.example.com/a.png", "https://cdn.example.net/lazy.jpg",
                                            "https://www.example.com/s-1200.jpg", "https://www.example.com/q.png?w=1&h=2",
                                            "https://www.example.com/c.avif"}));
    std::string out = rewrite_html(html, base,
                                     {{"https://www.example.com/a.png", "images/1.png"},
                                      {"https://cdn.example.net/lazy.jpg", "images/2.jpg"},
                                      {"https://www.example.com/q.png?w=1&h=2", "images/3.png"}});
    CHECK_EQ(out, "<p>One</p><figure><img src=\"images/1.png\" alt=\"A &amp; B\"/></figure>"
                  "<img src=\"images/2.jpg\" alt=\"\"/>"
                  "<picture><img src=\"images/1.png\" alt=\"\"/></picture>"
                  "<img src=\"images/3.png\" alt=\"\"/><imgx src=\"/not.png\">"
                  "<p>a > b</p>");
    CHECK_EQ(rewrite_html("<p>x</p>", base, {}), "<p>x</p>");
    CHECK_EQ(rewrite_html("<p>x<img src=\"/a.png\"", base, {}), "<p>x<img src=\"/a.png\""); // cut off: kept

    // Image CDNs put commas in their addresses (Substack, Cloudinary...).
    const std::string cdn = "https://substackcdn.com/image/fetch/w_%d,c_limit,f_webp,q_auto:good/https%%3A%%2F%%2Fs3.example%%2Fx.png";
    CHECK(article_images(strf(("<img srcset=\"" + cdn + " 424w, " + cdn + " 848w,\n" + cdn + " 4000w\">").c_str(), 424, 848,
                              4000),
                         base) == std::vector<std::string>({strf(cdn.c_str(), 848)}));
    CHECK(article_images("<img srcset=\"/one.png, /two.png 2x\">", base) ==
          std::vector<std::string>({"https://www.example.com/one.png"}));
    // Lazy loading: the picture in data-*, a real placeholder in src.
    CHECK(article_images("<img src=\"https://www.example.com/blank.gif\" data-lazy-src=\"/real.jpg\">"
                         "<img src=\"/blur.jpg\" data-srcset=\"/big.jpg 1200w\">",
                         base) == std::vector<std::string>({"https://www.example.com/real.jpg", "https://www.example.com/big.jpg"}));
    // Comments, scripts and styles: no pictures, and gone from the book.
    const std::string noisy = "<p>a</p><!-- <img src=\"/hidden.png\"> --><script>var s = \"<img src=/x.png>\";</script>"
                              "<STYLE type=\"text/css\">p { display: none }</style><style/><p>b</p><script src=\"/s.js\"/>"
                              "<img src=\"/shown.png\" alt=\"A &mdash; B &#8212; C&#x1; &amp; &#12abc; &lt;\"><!-- open";
    CHECK(article_images(noisy, base) == std::vector<std::string>({"https://www.example.com/shown.png"}));
    CHECK_EQ(rewrite_html(noisy, base, {{"https://www.example.com/shown.png", "images/1.png"}}),
             "<p>a</p><p>b</p><img src=\"images/1.png\" alt=\"A &mdash; B \xE2\x80\x94 C &amp; &amp;#12abc; &lt;\"/><!-- open");
    CHECK_EQ(rewrite_html(std::string("<p>a\0b\x0b</p>", 11), base, {}), "<p>ab</p>");
    CHECK_EQ(rewrite_html("<script>never closed <img src=\"/a.png\">", base, {}), "<script>never closed ");

    std::string media, ext;
    CHECK(image_type("\xFF\xD8\xFF\xE0rest", &media, &ext) && media == "image/jpeg" && ext == "jpg");
    CHECK(image_type(std::string("\x89PNG\r\n\x1a\n\0\0", 10), &media, &ext) && ext == "png");
    CHECK(image_type("GIF89a...", &media, &ext) && ext == "gif");
    CHECK(image_type(std::string("RIFF\x10\0\0\0WEBPVP8 ", 16), &media, &ext) && ext == "webp");
    CHECK(image_type("\xEF\xBB\xBF <?xml version=\"1.0\"?><!-- x --><svg xmlns=\"...\"/>", &media, &ext) &&
          media == "image/svg+xml");
    CHECK(!image_type("<!DOCTYPE html><html><body><svg/></body></html>", &media, &ext));
    CHECK(!image_type("<!doctype html><div><svg></svg></div>", &media, &ext));
    CHECK(!image_type("<div><svg/></div>", &media, &ext));
    CHECK(!image_type("<svgx/>", &media, &ext));
    CHECK(image_type("<?xml version='1.0'?>\n<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" "
                     "\"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd\">\n<svg width=\"1\"/>",
                     &media, &ext));
    CHECK(image_type("<!DOCTYPE svg [ <!ENTITY a \"<b>\"> ]><svg>", &media, &ext));
    CHECK(!image_type(std::string("\0\0\0\x1c" "ftypavif", 12), &media, &ext));
    CHECK(!image_type("", &media, &ext));

    // A book the archive checks accept (D5) and the engine reads.
    ArticleBook b;
    b.id = 42;
    b.title = "Rice & <beans>\x01";
    b.author = "Ana P\xC3\xA9rez";
    b.language = "es-AR";
    b.url = "https://example.com/rice?a=1&b=2";
    b.byline = "Ana P\xC3\xA9rez \xC2\xB7 example.com \xC2\xB7 3 min read";
    b.html = "<p>Arroz.</p><img src=\"images/1.png\" alt=\"\"/>";
    b.images.push_back({"images/1.png", "image/png", std::string("\x89PNG\r\n\x1a\nfake", 12)});
    b.cover = "images/1.png";
    const std::string dir = temp_dir();
    CHECK(write_file_atomic(dir + "/42.epub", make_article_epub(b)));
    CHECK_EQ(engine::check_book_file(dir + "/42.epub"), "");
    std::map<std::string, std::string> z = zip_entries(dir + "/42.epub");
    CHECK_EQ(z.size(), 7u);
    CHECK_EQ(z["mimetype"], "application/epub+zip");
    CHECK(z["OEBPS/content.opf"].find("<dc:title>Rice &amp; &lt;beans&gt;</dc:title>") != std::string::npos);
    CHECK(z["OEBPS/content.opf"].find("<dc:language>es-AR</dc:language>") != std::string::npos);
    CHECK(z["OEBPS/content.opf"].find("properties=\"cover-image\"") != std::string::npos);
    CHECK(z["OEBPS/article.xhtml"].find("<p class=\"byline\">Ana P\xC3\xA9rez") != std::string::npos);
    CHECK(z["OEBPS/article.xhtml"].find("https://example.com/rice?a=1&amp;b=2") != std::string::npos);
    CHECK_EQ(z["OEBPS/images/1.png"], b.images[0].data);
    // The same article gives the same file.
    CHECK(make_article_epub(b) == make_article_epub(b));

    // A stored entry's stream stays within the entry (V3-SEC-5, D32).
    {
        LVStreamRef file = LVOpenFileStream((dir + "/42.epub").c_str(), LVOM_READ);
        LVContainerRef arc = file.isNull() ? LVContainerRef() : LVOpenArchieve(file);
        LVStreamRef png = arc.isNull() ? LVStreamRef() : arc->OpenStream(U"OEBPS/images/1.png", LVOM_READ);
        CHECK(!png.isNull());
        if (!png.isNull()) {
            char buf[64];
            lvsize_t n = 0;
            lvpos_t pos = 0;
            CHECK(png->Seek(-4, LVSEEK_END, &pos) == LVERR_OK && pos == 8);
            CHECK(png->Read(buf, sizeof buf, &n) == LVERR_OK && n == 4 && std::string(buf, 4) == "fake");
            CHECK(png->Read(buf, sizeof buf, &n) != LVERR_OK && n == 0);
            CHECK(png->Seek(2, LVSEEK_SET, &pos) == LVERR_OK && png->Seek(3, LVSEEK_CUR, &pos) == LVERR_OK && pos == 5);
            CHECK(png->Seek(1, LVSEEK_END, &pos) != LVERR_OK && png->Seek(-1, LVSEEK_SET, &pos) != LVERR_OK);
            CHECK(png->Read(buf, 3, &n) == LVERR_OK && n == 3 && std::string(buf, 3) == "\n\x1a\n");
        }
    }

    const char* fonts = test_env("DECKREADER_TEST_FONTS");
    const char* data = test_env("DECKREADER_TEST_ENGINE_DATA");
    if (fonts && data && engine::init(fonts, data, temp_dir())) {
        BookInfo info;
        CHECK(engine::read_metadata(dir + "/42.epub", info, nullptr, 0));
        CHECK_EQ(info.title, "Rice & <beans>");
        // A removed article leaves nothing in the library's cache (V3-OPT-3).
        const std::string cache = temp_dir();
        MetaCache mc(cache);
        CHECK(mc.fetch(dir + "/42.epub") && mc.get(dir + "/42.epub"));
        mc.forget(dir + "/42.epub");
        CHECK(!mc.get(dir + "/42.epub"));
        mc.save_if_dirty();
        std::string saved;
        CHECK(read_file(cache + "/metadata.json", saved) && saved.find("42.epub") == std::string::npos);
        CHECK_EQ(info.author, "Ana P\xC3\xA9rez");
        CHECK_EQ(info.language, "es-AR");
    }
}

TEST(wallabag_article) {
    WallabagArticle a;
    CHECK(parse_article(R"({"id": 7, "title": "  Rice  ", "url": "https://www.example.com/r",
        "domain_name": "www.example.com", "language": "en_GB", "content": "<p>x</p>",
        "published_at": "2009-03-27T10:00:00+0000", "published_by": ["", "Ana", " Bo "],
        "reading_time": 4, "created_at": "2026-11-19T08:00:00+0100", "preview_picture": null})",
                        a));
    CHECK_EQ(a.id, 7);
    CHECK_EQ(a.title, "Rice");
    CHECK(a.authors == std::vector<std::string>({"Ana", "Bo"}));
    CHECK_EQ(a.created, 1795071600);
    CHECK_EQ(a.reading_time, 4);
    CHECK(a.preview_picture.empty());
    ArticleBook b = article_book(a);
    CHECK_EQ(b.title, "Rice");
    CHECK_EQ(b.author, "Ana, Bo");
    CHECK_EQ(b.language, "en-GB");
    CHECK_EQ(b.byline, "Ana, Bo \xC2\xB7 example.com \xC2\xB7 27 Mar 2009 \xC2\xB7 4 min read");
    CHECK_EQ(b.html, "<p>x</p>");

    // No authors, title or date: the site stands in.
    CHECK(parse_article(R"({"id": 8, "url": "https://news.example.org/a", "published_by": null,
        "language": "fr<script>", "reading_time": 0})",
                        a));
    b = article_book(a);
    CHECK_EQ(b.title, "news.example.org");
    CHECK_EQ(b.author, "news.example.org");
    CHECK_EQ(b.byline, "news.example.org");
    CHECK_EQ(b.language, "");
    CHECK(!parse_article(R"({"error": "x"})", a));
    CHECK(!parse_article("<html>", a));
}

namespace {
// tests/fake-wallabag.py, started inside the SDK sandbox (loopback only).
struct FakeWallabag {
    pid_t pid = -1;
    int port = 0;
    explicit FakeWallabag(const char* script) {
        int fds[2];
        if (pipe(fds) != 0)
            return;
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
        posix_spawn_file_actions_addclose(&fa, fds[0]);
        char* argv[] = {const_cast<char*>("python3"), const_cast<char*>(script), nullptr};
        if (posix_spawnp(&pid, "python3", &fa, nullptr, argv, environ) != 0)
            pid = -1;
        posix_spawn_file_actions_destroy(&fa);
        close(fds[1]);
        std::string line;
        char ch;
        while (pid > 0 && line.size() < 16 && read(fds[0], &ch, 1) == 1 && ch != '\n')
            line += ch;
        close(fds[0]);
        port = std::atoi(line.c_str());
    }
    ~FakeWallabag() {
        if (pid > 0) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }
    }
    std::string url() const { return strf("http://127.0.0.1:%d", port); }
    bool reset(const std::string& json) {
        HttpRequest q;
        q.method = "POST";
        q.url = url() + "/test/reset";
        q.body = json;
        q.content_type = "application/json";
        return http_request(q).status == 200;
    }
    std::string state() {
        HttpRequest q;
        q.url = url() + "/test/state";
        return http_request(q).body;
    }
};
} // namespace

TEST(wallabag_sync_fake_server) {
    const char* script = test_env("DECKREADER_TEST_FAKE_WALLABAG");
    if (!script) {
        std::printf("wallabag_sync_fake_server: skipped (no DECKREADER_TEST_FAKE_WALLABAG)\n");
        return;
    }
    http_global_init();
    FakeWallabag fake(script);
    CHECK(fake.port > 0);
    if (fake.port <= 0)
        return;
    CHECK(fake.reset(R"({"entries": [
        {"id": 1, "title": "One", "created_at": "2026-11-15T08:00:00+0100"},
        {"id": 2, "title": "Two", "created_at": "2026-11-16T08:00:00+0100"},
        {"id": 3, "title": "Three", "created_at": "2026-11-17T08:00:00+0100"},
        {"id": 4, "title": "Four", "created_at": "2026-11-18T08:00:00+0100"},
        {"id": 5, "title": "Five", "created_at": "2026-11-19T08:00:00+0100"}], "broken": [4]})"));
    const std::string dir = temp_dir();
    write_file_atomic(dir + "/2.epub", "old copy");
    write_file_atomic(dir + "/9.epub", "read elsewhere");
    const WallabagSettings account{fake.url() + "/", "cid", "secret", "reader", "pa ss&w\xC3\xB6rd=1+2%", 10};
    std::atomic<bool> cancel{false};

    // Download what is new; 4 fails on the server; 9 is no longer unread.
    SyncRequest req;
    req.settings = account;
    req.dir = dir;
    req.local = {2, 9};
    std::vector<SyncProgress> seen;
    SyncResult r = run_sync(req, cancel, [&](const SyncProgress& p) { seen.push_back(p); });
    CHECK_EQ(r.error, "");
    CHECK(r.listed);
    CHECK_EQ(r.unread, 5);
    CHECK_EQ(r.downloaded, 3);
    CHECK_EQ(r.failed, 1);
    CHECK(r.remove == std::vector<int64_t>({9}));
    std::string data;
    for (const char* f : {"/1.epub", "/3.epub", "/5.epub"})
        CHECK(read_file(dir + f, data) && data.compare(0, 4, "PK\x03\x04") == 0);
    CHECK(read_file(dir + "/2.epub", data) && data == "old copy");
    CHECK(!is_file(dir + "/4.epub"));
    int64_t mtime = 0;
    CHECK(file_stat(dir + "/5.epub", nullptr, &mtime) && mtime == 1795071600);
    CHECK(!seen.empty() && seen.back().phase == SyncProgress::Downloading && seen.back().done == 4 &&
          seen.back().total == 4);
    std::string st = fake.state();
    CHECK(st.find("archive=0") != std::string::npos && st.find("detail=metadata") != std::string::npos &&
          st.find("perPage=10") != std::string::npos && st.find("order=desc") != std::string::npos);
    // The downloaded file is a book the checks accept (D5).
    CHECK_EQ(engine::check_book_file(dir + "/5.epub"), "");

    // Marked as read on the Deck: archived; 77 is gone from wallabag (404).
    SyncRequest arch;
    arch.settings = account;
    arch.dir = dir;
    arch.archive = {3, 77};
    arch.update = false;
    r = run_sync(arch, cancel, nullptr);
    CHECK_EQ(r.error, "");
    CHECK(r.archived == std::vector<int64_t>({3, 77}));
    CHECK(!r.listed);
    CHECK(fake.state().find("\"archived\": [3]") != std::string::npos);

    // Next update: 3 is no longer unread, so it goes from the Deck.
    req.local = {1, 2, 3, 5};
    r = run_sync(req, cancel, nullptr);
    CHECK_EQ(r.error, "");
    CHECK_EQ(r.unread, 4);
    CHECK(r.remove == std::vector<int64_t>({3}));
    CHECK_EQ(r.downloaded, 0);
    CHECK_EQ(r.failed, 1); // 4, still broken

    // Only the newest articles, up to max_articles.
    req.settings.max_articles = 2;
    r = run_sync(req, cancel, nullptr);
    CHECK_EQ(r.unread, 2);
    CHECK(fake.state().find("perPage=2") != std::string::npos);
    req.settings = account;

    // A server that sends at most 2 per page: every page is asked for, so
    // articles on later pages don't look read (V3-BUG-8, D32).
    const std::string five = R"("entries": [
        {"id": 1, "title": "One", "created_at": "2026-11-15T08:00:00+0100"},
        {"id": 2, "title": "Two", "created_at": "2026-11-16T08:00:00+0100"},
        {"id": 3, "title": "Three", "created_at": "2026-11-17T08:00:00+0100"},
        {"id": 4, "title": "Four", "created_at": "2026-11-18T08:00:00+0100"},
        {"id": 5, "title": "Five", "created_at": "2026-11-19T08:00:00+0100"}])";
    CHECK(fake.reset("{\"max_per_page\": 2, " + five + "}"));
    req.local = {1, 2, 3, 4, 5};
    r = run_sync(req, cancel, nullptr);
    CHECK_EQ(r.error, "");
    CHECK_EQ(r.unread, 5);
    CHECK(r.remove.empty());
    CHECK_EQ(r.downloaded, 0);
    st = fake.state();
    CHECK(st.find("\"list_calls\": 3") != std::string::npos && st.find("page=3&perPage=10") != std::string::npos);
    // ... and no more pages than max_articles needs.
    req.settings.max_articles = 3;
    r = run_sync(req, cancel, nullptr);
    CHECK_EQ(r.unread, 3);
    CHECK(r.remove == std::vector<int64_t>({1, 2}));
    st = fake.state();
    CHECK(st.find("\"list_calls\": 5") != std::string::npos && st.find("page=2&perPage=3") != std::string::npos);
    req.settings = account;
    CHECK(fake.reset("{" + five + "}"));

    // Login problems, a moved server, no server.
    req.settings.password = "wrong";
    r = run_sync(req, cancel, nullptr);
    CHECK_EQ(r.error, "wallabag refused the username or password.");
    CHECK(!r.listed);
    req.settings = account;
    req.settings.client_secret = "wrong";
    CHECK_EQ(run_sync(req, cancel, nullptr).error, "wallabag refused the client ID or secret.");
    req.settings = account;
    req.settings.server = fake.url() + "/old";
    CHECK_EQ(run_sync(req, cancel, nullptr).error,
             "The server sent the login to " + fake.url() + ". Use that as the server address.");
    req.settings.server = "http://127.0.0.1:1";
    r = run_sync(req, cancel, nullptr);
    CHECK(starts_with(r.error, "Couldn't connect to 127.0.0.1:1"));
    req.settings.server = "app.wallabag.it";
    CHECK_EQ(run_sync(req, cancel, nullptr).error, "The wallabag server address must start with https:// or http://.");
    req.settings = account;

    // Cancelled (quitting the app).
    cancel = true;
    r = run_sync(req, cancel, nullptr);
    CHECK(r.cancelled && r.error.empty());
    cancel = false;

    // Pictures: downloaded from their sites (without the wallabag token),
    // redirects followed; missing ones and formats the engine can't show
    // are left out, not shown as broken squares.
    CHECK(fake.reset(R"({"entries": [{"id": 20, "title": "Pictures", "created_at": "2026-11-20T08:00:00Z",
        "published_by": ["Ana"], "preview_picture": "/img/cover.png",
        "content": "<p>Text.</p><img src=\"/img/a.png\" alt=\"chart\"><img src=\"img/b.svg\"><img src=\"/img/c.avif\"><img src=\"/img/missing.png\"><img src=\"/img/r.png\"><img data-src=\"/img/a.png\" src=\"data:image/gif;base64,R0lG\">"}]})"));
    req.local = {};
    r = run_sync(req, cancel, nullptr);
    CHECK_EQ(r.error, "");
    CHECK_EQ(r.downloaded, 1);
    std::map<std::string, std::string> z = zip_entries(dir + "/20.epub");
    CHECK_EQ(engine::check_book_file(dir + "/20.epub"), "");
    // The cover first (it is never the one left out), then the article's.
    CHECK(z.count("OEBPS/images/1.png") && z.count("OEBPS/images/2.png") && z.count("OEBPS/images/3.svg") &&
          z.count("OEBPS/images/4.png"));
    CHECK(!z.count("OEBPS/images/5.png"));
    const std::string page = z["OEBPS/article.xhtml"];
    CHECK(page.find("<img src=\"images/2.png\" alt=\"chart\"/><img src=\"images/3.svg\" alt=\"\"/>"
                    "<img src=\"images/4.png\" alt=\"\"/><img src=\"images/2.png\" alt=\"\"/>") != std::string::npos);
    CHECK(page.find("avif") == std::string::npos && page.find("missing") == std::string::npos);
    CHECK(page.find("Ana \xC2\xB7 example.com \xC2\xB7 3 min read") != std::string::npos);
    CHECK(z["OEBPS/content.opf"].find("href=\"images/1.png\" media-type=\"image/png\" properties=\"cover-image\"") !=
          std::string::npos);
    CHECK(z["OEBPS/content.opf"].find("<dc:creator>Ana</dc:creator>") != std::string::npos);
    CHECK(fake.state().find("\"picture_auth\": 0") != std::string::npos);
    // images = no: text only, nothing fetched from the sites.
    req.settings.images = false;
    ::unlink((dir + "/20.epub").c_str());
    r = run_sync(req, cancel, nullptr);
    CHECK_EQ(r.downloaded, 1);
    z = zip_entries(dir + "/20.epub");
    CHECK_EQ(z.size(), 6u);
    CHECK(z["OEBPS/article.xhtml"].find("<img") == std::string::npos);
    req.settings = account;

    // Pictures on this device or the local network aren't fetched (the
    // wallabag server's own are: here, 127.0.0.1); one bad address doesn't
    // make its site look unreachable; with more than 60 pictures the cover
    // still comes (V3-SEC-1, V3-BUG-3, V3-BUG-7; D32).
    std::string many;
    for (int i = 0; i < 61; i++)
        many += strf("<img src='/img/a.png?%d'>", i);
    CHECK(fake.reset(strf(R"({"entries": [
        {"id": 21, "title": "Local", "created_at": "2026-11-20T08:00:00Z", "preview_picture": "/img/cover.png",
         "content": "<img src='http://localhost:%d/img/a.png'><img src='/img/bad name.png'><img src='/img/b.svg'>"},
        {"id": 22, "title": "Many", "created_at": "2026-11-20T09:00:00Z", "preview_picture": "/img/cover.png",
         "content": "%s"}]})",
                          fake.port, many.c_str())));
    req.local = {};
    r = run_sync(req, cancel, nullptr);
    CHECK_EQ(r.error, "");
    CHECK_EQ(r.downloaded, 2);
    z = zip_entries(dir + "/21.epub");
    CHECK(z.count("OEBPS/images/1.png") && z.count("OEBPS/images/2.svg") && !z.count("OEBPS/images/3.png"));
    CHECK(z["OEBPS/article.xhtml"].find("<img src=\"images/2.svg\" alt=\"\"/>") != std::string::npos);
    CHECK(z["OEBPS/article.xhtml"].find("localhost") == std::string::npos);
    z = zip_entries(dir + "/22.epub");
    int pictures = 0;
    for (const auto& [name, d] : z)
        pictures += starts_with(name, "OEBPS/images/");
    CHECK_EQ(pictures, 60);
    CHECK(z["OEBPS/content.opf"].find("href=\"images/1.png\" media-type=\"image/png\" properties=\"cover-image\"") !=
          std::string::npos);

    // A site that stalls is given up on, not waited for (V3-SEC-3).
    HttpRequest stall;
    stall.url = fake.url() + "/test/stall";
    stall.low_speed_s = 1;
    const uint64_t t0 = mono_ms();
    HttpResponse sr = http_request(stall);
    CHECK(sr.unreachable && !sr.error.empty());
    CHECK(mono_ms() - t0 < 4000);

    // In the background thread, waking the caller.
    CHECK(fake.reset(R"({"entries": [{"id": 8, "title": "Eight", "created_at": "2026-11-20T08:00:00Z"}]})"));
    std::mutex mu;
    std::condition_variable cv;
    int wakes = 0;
    SyncWorker worker;
    req.local = {};
    CHECK(worker.start(req, [&] {
        std::lock_guard<std::mutex> l(mu);
        wakes++;
        cv.notify_all();
    }));
    CHECK(worker.busy());
    CHECK(!worker.start(req, nullptr)); // one at a time
    SyncResult wr;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!worker.take_result(wr) && std::chrono::steady_clock::now() < deadline) {
        std::unique_lock<std::mutex> l(mu);
        cv.wait_for(l, std::chrono::milliseconds(200));
    }
    CHECK(!worker.busy());
    CHECK_EQ(wr.error, "");
    CHECK_EQ(wr.downloaded, 1);
    CHECK(is_file(dir + "/8.epub"));
    CHECK(wakes >= 2);
}

int main() {
    for (auto& [name, fn] : tests()) {
        g_test = name;
        fn();
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
