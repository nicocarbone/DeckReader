#include "app.h"

#include "about.h"
#include "book.h"
#include "fsutil.h"
#include "http.h"
#include "log.h"
#include "util.h"
#include "version.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstdlib>
#include <pwd.h>
#include <unistd.h>

namespace dr {

static std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v && *v ? std::string(v) : fallback;
}

Paths Paths::detect() {
    Paths p;
    p.home = env_or("HOME", "");
    if (p.home.empty())
        if (passwd* pw = getpwuid(getuid()))
            p.home = pw->pw_dir;
    // Settings in the real ~/.config (not the Flatpak's private one) so they
    // are easy to find and edit, as in DeckVideoPlayer.
    p.config_dir = env_or("DECKREADER_CONFIG_DIR", path_join(p.home, ".config/deckreader"));
    p.data_dir = env_or("DECKREADER_DATA_DIR", path_join(env_or("XDG_DATA_HOME", path_join(p.home, ".local/share")), "deckreader"));
    p.cache_dir = env_or("DECKREADER_CACHE_DIR", path_join(env_or("XDG_CACHE_HOME", path_join(p.home, ".cache")), "deckreader"));
    char exe[PATH_MAX] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    p.bin_dir = n > 0 ? path_dirname(std::string(exe, static_cast<size_t>(n))) : "/app/bin";
    std::string prefix = path_dirname(p.bin_dir);
    p.app_data = env_or("DECKREADER_APPDIR", path_join(prefix, "share/deckreader"));
    std::string engine = DECKREADER_CRENGINE_DATA;
    if (!is_dir(engine))
        engine = path_join(prefix, "share/crengine-ng");
    p.engine_data = env_or("DECKREADER_CRENGINE_DATA", engine);
    return p;
}

App::App() = default;

App::~App() {
    articles.reset(); // stops its thread
    wikipedia.reset();
    for (auto& [id, pad] : gamepads_)
        SDL_CloseGamepad(pad);
    library_.reset();
    reader_.reset();
    menus_.clear();
    graveyard_.clear();
    if (engine_state_ == 1)
        engine::shutdown();
}

bool App::engine_ready() {
    if (engine_state_ == 0) {
        uint64_t t0 = mono_ms();
        engine_state_ = engine::init(path_join(paths.app_data, "fonts"), paths.engine_data, paths.cache_dir) ? 1 : -1;
        LOG_I("book engine ready in %llu ms", static_cast<unsigned long long>(mono_ms() - t0));
    }
    return engine_state_ == 1;
}

std::string App::mountinfo() const {
    std::string text;
    read_file("/proc/self/mountinfo", text, 4u << 20);
    return text;
}

void App::toast(const std::string& message, uint64_t ms, bool yield) {
    if (yield && !toast_.empty() && now_ < toast_until_ && toast_ms_ > ms)
        return;
    toast_ = message;
    toast_ms_ = ms;
    toast_until_ = now_ + ms;
    redraw();
}

void App::push_menu(std::unique_ptr<Menu> m) {
    menus_.push_back(std::move(m));
    redraw();
}

// Menus are closed from inside their own item callbacks, so they are moved
// aside and destroyed after the current input has been handled.
void App::pop_menu() {
    if (menus_.empty())
        return;
    graveyard_.push_back(std::move(menus_.back()));
    menus_.pop_back();
    redraw();
}

void App::close_menus() {
    while (!menus_.empty())
        pop_menu();
}

void App::open_book(const std::string& path, bool from_start) {
    pending_.path = path;
    pending_.from_start = from_start;
    pending_.active = true;
    pending_.shown = false;
    redraw();
}

std::string App::open_book_path() const {
    return screen_ == Screen::Reader && reader_ && reader_->is_open() ? reader_->book().path() : std::string();
}

bool App::showing_articles(ArticleSource source) const {
    return screen_ == Screen::Library && library_ && library_->in_articles(source);
}

Articles* App::article_source(ArticleSource source) {
    return source == ArticleSource::Wallabag    ? articles.get()
           : source == ArticleSource::Wikipedia ? wikipedia.get()
                                                : nullptr;
}

std::vector<Articles*> App::article_sources() {
    std::vector<Articles*> out;
    for (Articles* a : {articles.get(), wikipedia.get()})
        if (a && a->configured())
            out.push_back(a);
    return out;
}

void App::update_articles() {
    bool started = false, running = false;
    for (Articles* a : article_sources()) {
        if (a->busy())
            running = true;
        else
            started |= a->update();
    }
    if (started)
        toast("Updating articles\xE2\x80\xA6");
    else if (running)
        toast("Already updating the articles");
}

void App::stop_articles() {
    for (Articles* a : article_sources())
        a->stop();
}

bool App::articles_busy() {
    for (Articles* a : article_sources())
        if (a->busy())
            return true;
    return false;
}

void App::refresh_library() {
    if (screen_ == Screen::Library && library_)
        library_->refresh();
    redraw();
}

void App::show_library() {
    keep_awake.release();
    if (reader_)
        reader_->close();
    state.save();
    screen_ = Screen::Library;
    library_->refresh();
    redraw();
}

void App::open_about() {
    push_menu(make_about_menu(*this));
}

// --- input -------------------------------------------------------------------------

void App::dispatch(const InputEvent& e) {
    if (e.action == Action::None)
        return;
    LOG_D("action %s%s", action_name(e.action), e.repeat ? " (repeat)" : "");
    if (e.action == Action::Quit) {
        quit();
        return;
    }
    if (pending_.active)
        return; // a book is opening
    reading_activity();
    // B dismisses what is on screen first (DeckVideoPlayer D63).
    if (e.action == Action::Back && !toast_.empty() && menus_.empty() && !e.repeat) {
        toast_.clear();
        redraw();
        return;
    }
    if (Menu* m = top_menu()) {
        if (!m->handle(e))
            pop_menu();
        redraw();
        return;
    }
    if (screen_ == Screen::Reader)
        reader_->handle(e);
    else
        library_->handle(e);
}

void App::reading_activity() {
    if (screen_ == Screen::Reader && reader_ && reader_->is_open())
        keep_awake.activity(now_);
}

void App::tap(float x_px, float y_px) {
    if (pending_.active)
        return;
    reading_activity();
    float scale = display.height() / Ui::BASE_HEIGHT * static_cast<float>(cfg.ui_scale);
    float ux = x_px / scale, uy = y_px / scale, uw = display.width() / scale;
    if (Menu* m = top_menu()) {
        if (!m->tap(ux, uy, uw))
            pop_menu(); // tapping outside the drawer closes it
        redraw();
        return;
    }
    if (screen_ == Screen::Reader)
        reader_->tap(x_px / display.width(), y_px / display.height());
    else
        library_->tap(ux, uy);
}

void App::handle_event(const SDL_Event& e) {
    for (Articles* a : {articles.get(), wikipedia.get()})
        if (a && e.type == a->event_type()) {
            a->on_wake();
            return;
        }
    std::vector<InputEvent> actions;
    switch (e.type) {
    case SDL_EVENT_QUIT:
        quit();
        return;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
        resized();
        return;
    case SDL_EVENT_WINDOW_EXPOSED:
        redraw();
        return;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        // Steam's overlay or a suspend may follow: write what we have.
        flush();
        return;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!gamepads_.count(e.gdevice.which))
            if (SDL_Gamepad* g = SDL_OpenGamepad(e.gdevice.which)) {
                gamepads_[e.gdevice.which] = g;
                LOG_I("controller: %s", SDL_GetGamepadName(g));
            }
        return;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (auto it = gamepads_.find(e.gdevice.which); it != gamepads_.end()) {
            SDL_CloseGamepad(it->second);
            gamepads_.erase(it);
        }
        break; // Input forgets its held buttons too
    case SDL_EVENT_MOUSE_MOTION:
        if (e.motion.which != SDL_TOUCH_MOUSEID) {
            SDL_ShowCursor();
            cursor_hide_at_ = now_ + 2500;
        }
        return;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e.button.button == SDL_BUTTON_LEFT) {
            float density = SDL_GetWindowPixelDensity(display.window());
            pointer_down_ = true;
            down_x_ = e.button.x * density;
            down_y_ = e.button.y * density;
        }
        return;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button == SDL_BUTTON_LEFT && pointer_down_) {
            pointer_down_ = false;
            float density = SDL_GetWindowPixelDensity(display.window());
            float x = e.button.x * density, y = e.button.y * density;
            float dx = x - down_x_, dy = y - down_y_, slop = display.height() * 0.03f;
            if (std::fabs(dx) <= slop && std::fabs(dy) <= slop)
                tap(x, y);
            else if (screen_ == Screen::Reader && menus_.empty() && !pending_.active &&
                     std::fabs(dx) > display.width() * 0.08f && std::fabs(dx) > 2 * std::fabs(dy)) {
                reading_activity();
                reader_->swipe(dx < 0 ? 1 : -1); // swipe left: next page
            }
            // other drags do nothing
        }
        return;
    default:
        break;
    }
    input.handle(e, now_, actions);
    if (!actions.empty() && cursor_hide_at_) {
        SDL_HideCursor();
        cursor_hide_at_ = 0;
    }
    for (const InputEvent& a : actions)
        dispatch(a);
}

// --- timers and work ----------------------------------------------------------------

void App::tick() {
    std::vector<InputEvent> repeats;
    input.tick(now_, repeats);
    for (const InputEvent& a : repeats)
        dispatch(a);
    if (screen_ == Screen::Reader)
        reader_->tick(now_);
    keep_awake.tick(now_);
    if (!toast_.empty() && now_ >= toast_until_) {
        toast_.clear();
        redraw();
    }
    if (cursor_hide_at_ && now_ >= cursor_hide_at_) {
        SDL_HideCursor();
        cursor_hide_at_ = 0;
    }
}

int64_t App::next_timeout() {
    if (dirty_ || (pending_.active && pending_.shown))
        return 0;
    bool idle = screen_ == Screen::Reader ? reader_->has_idle_work() : library_->has_idle_work();
    if (idle)
        return 0;
    int64_t best = -1;
    auto consider = [&](int64_t d) {
        if (d >= 0 && (best < 0 || d < best))
            best = d;
    };
    consider(input.next_due(now_));
    if (screen_ == Screen::Reader)
        consider(reader_->next_due(now_));
    if (!toast_.empty())
        consider(toast_until_ > now_ ? static_cast<int64_t>(toast_until_ - now_) : 0);
    if (cursor_hide_at_)
        consider(cursor_hide_at_ > now_ ? static_cast<int64_t>(cursor_hide_at_ - now_) : 0);
    consider(state.save_due_in(now_));
    consider(keep_awake.next_due(now_)); // one wake-up, minutes away
    if (script_ && !script_->done())
        consider(script_->ready_at > now_ ? static_cast<int64_t>(std::min<uint64_t>(script_->ready_at - now_, 50)) : 0);
    return best;
}

// Runs the test-script command that is due. Returns true to run another now.
bool App::run_script_step() {
    if (!script_ || script_->done() || now_ < script_->ready_at)
        return false;
    const ScriptCommand& c = *script_->current();
    std::vector<InputEvent> out;
    if (c.name == "wait") {
        script_->ready_at = now_ + static_cast<uint64_t>(std::max(0, std::atoi(c.arg.c_str())));
    } else if (c.name == "key") {
        Action a = action_from_name(to_lower(c.arg).c_str());
        if (a == Action::None)
            LOG_E("script: unknown key %s", c.arg.c_str());
        dispatch({a, Source::Script, false});
    } else if (c.name == "stick") {
        input.inject_stick_x(static_cast<float>(std::atof(c.arg.c_str())), now_, out);
        for (const InputEvent& a : out)
            dispatch(a);
    } else if (c.name == "tap") {
        std::vector<std::string> xy = split(c.arg, ',');
        if (xy.size() == 2)
            tap(static_cast<float>(std::atof(xy[0].c_str())) * display.width(),
                static_cast<float>(std::atof(xy[1].c_str())) * display.height());
    } else if (c.name == "idle") {
        bool busy = dirty_ || pending_.active ||
                    (screen_ == Screen::Reader ? reader_->has_idle_work() : library_->has_idle_work());
        if (busy) {
            if (!script_->idle_since)
                script_->idle_since = now_;
            if (now_ - script_->idle_since < 15000)
                return false; // check again next iteration
        }
        script_->idle_since = 0;
    } else if (c.name == "shot") {
        screenshot_ = c.arg;
        redraw();
    } else if (c.name == "quit") {
        quit();
    } else {
        LOG_E("script: unknown command %s", c.name.c_str());
    }
    script_->advance();
    // One command per frame, so each screenshot shows the effect of the
    // commands before it.
    return false;
}

void App::flush() {
    if (screen_ == Screen::Reader && reader_)
        reader_->save_position();
    if (state.dirty())
        state.save();
    meta.save_if_dirty();
}

// --- drawing -------------------------------------------------------------------------

void App::resized() {
    if (!display.update_size())
        return;
    overlay_.resize(display.width(), display.height());
    make_backdrop();
    if (reader_ && reader_->is_open())
        reader_->resized();
    redraw();
}

void App::make_backdrop() {
    // 1/8 of the screen, smooth enough once the GPU stretches it.
    Canvas small;
    small.resize(std::max(16, display.width() / 8), std::max(10, display.height() / 8));
    small.fill_background(theme.bg, theme.glow);
    display.set_backdrop(small);
}

void App::draw_toast(Ui& ui) {
    // Centred over the part of the screen a drawer menu leaves free.
    // Centred over the part of the screen a drawer menu leaves free, below
    // the reading overlay's top bar when it is shown (D22).
    float free_w = ui.W();
    if (!menus_.empty()) {
        float left = ui.W() - menus_.back()->drawn_width(ui.W());
        free_w = left >= 240 ? left : ui.W();
    }
    float w = std::min(ui.text_w(toast_, 19) + 56, free_w - 40);
    float y = screen_ == Screen::Reader && reader_->hud_visible() ? 100 : 28, h = 52;
    float x = (free_w - w) / 2;
    ui.rect(x, y, w, h, ui.col(theme.panel2, 0.96f), h / 2);
    ui.text(x + w / 2, y + h / 2, toast_, 19, ui.col(theme.text), false, ALIGN_CENTER, w - 40);
}

void App::draw_loading(Ui& ui) {
    const BookMeta* m = meta.get(pending_.path);
    std::string title = m && !m->title.empty() ? m->title : strip_extension(path_basename(pending_.path));
    float w = std::min(620.0f, ui.W() - 80), h = 120;
    float x = (ui.W() - w) / 2, y = (ui.H() - h) / 2;
    ui.rect(0, 0, ui.W(), ui.H(), ui.col(0x000000, 0.45f));
    ui.rect(x, y, w, h, ui.col(theme.panel, 0.98f), 18);
    ui.text(x + 32, y + 42, "Opening", 16, ui.col(theme.accent), true);
    ui.text(x + 32, y + 78, title, 22, ui.col(theme.text), true, ALIGN_LEFT, w - 64);
}

void App::render() {
    dirty_ = false;
    SDL_Texture* page = nullptr;
    uint32_t bg = theme.bg;
    bool overlay = false;
    const bool reading = screen_ == Screen::Reader && reader_->is_open();
    const bool extras = !menus_.empty() || !toast_.empty() || pending_.active;

    if (reading) {
        page = reader_->page_texture();
        bg = reader_->background();
        overlay = reader_->overlay_needed() || extras;
        // While just reading, the screen-sized overlay buffer is freed
        // (4 MB on the Deck, 33 MB at 4K) and made again when needed.
        if (!overlay)
            overlay_.release();
    } else {
        overlay = true;
    }
    if (overlay) {
        if (overlay_.width() != display.width() || overlay_.height() != display.height())
            overlay_.resize(display.width(), display.height());
        else
            overlay_.clear(0);
    }
    if (overlay) {
        Ui ui(overlay_, text, cfg.ui_scale, theme);
        if (reading)
            reader_->draw_overlay(ui);
        else
            library_->draw(ui);
        if (!menus_.empty())
            menus_.back()->draw(ui);
        if (pending_.active)
            draw_loading(ui);
        if (!toast_.empty())
            draw_toast(ui);
        display.upload_overlay(overlay_);
    }
    display.present(page, !reading, overlay, bg, screenshot_);
    screenshot_.clear();
    if (pending_.active)
        pending_.shown = true;
}

// --- main loop -------------------------------------------------------------------------

void App::loop() {
    while (running_) {
        now_ = mono_ms();
        int64_t timeout = next_timeout();
        SDL_Event e;
        bool got = timeout < 0 ? SDL_WaitEvent(&e) : SDL_WaitEventTimeout(&e, static_cast<Sint32>(std::min<int64_t>(timeout, INT32_MAX)));
        now_ = mono_ms();
        if (got) {
            handle_event(e);
            while (running_ && SDL_PollEvent(&e))
                handle_event(e);
        }
        graveyard_.clear();
        while (run_script_step())
            graveyard_.clear();
        tick();
        graveyard_.clear();
        if (!running_)
            break;
        if (dirty_)
            render();

        // A book opens only after the "Opening..." frame is on screen.
        if (pending_.active && pending_.shown) {
            std::string error;
            PendingOpen p = pending_;
            pending_ = PendingOpen{};
            close_menus();
            if (reader_->open(p.path, p.from_start, &error)) {
                screen_ = Screen::Reader;
                reading_activity();
            } else {
                toast(error.empty() ? "The book could not be opened." : error);
                library_->refresh();
            }
            redraw();
            continue;
        }

        state.save_if_due(now_);
        // Background work, one piece at a time, only while no input waits.
        if (!dirty_ && !SDL_HasEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST)) {
            if (screen_ == Screen::Reader)
                reader_->idle_work();
            else
                library_->idle_work();
        }
    }
}

int App::run(const AppOptions& opt) {
    uint64_t t0 = mono_ms();
    http_global_init(); // before any thread (D31)
    paths = Paths::detect();
    make_dirs(paths.config_dir);
    make_dirs(paths.data_dir);
    make_dirs(paths.cache_dir);

    std::string cfg_path = path_join(paths.config_dir, "config.ini");
    std::string cfg_text;
    if (!read_file(cfg_path, cfg_text)) {
        // First start: the documented template becomes the user's file.
        if (read_file(path_join(paths.app_data, "config.ini"), cfg_text))
            write_file_atomic(cfg_path, cfg_text);
    }
    cfg = parse_config(cfg_text, paths.home);
    for (const std::string& w : cfg.warnings)
        LOG_W("config.ini: %s", w.c_str());
    theme.accent = cfg.accent;
    // The helper's status file goes where only this user can read it.
    std::string runtime = env_or("XDG_RUNTIME_DIR", paths.cache_dir);
    keep_awake.setup(path_join(paths.bin_dir, "deckreader-keep-awake"),
                     path_join(runtime, strf("deckreader-keep-awake-%d.status", static_cast<int>(getpid()))),
                     cfg.keep_awake ? cfg.keep_awake_minutes : 0);

    state = StateStore(path_join(paths.data_dir, "state.json"));
    state.load();
    meta = MetaCache(paths.cache_dir);
    meta.load();

    if (!display.init(opt.display))
        return 1;
    // Game mode: no pointer unless the trackpad/mouse moves (requirements 5.2).
    SDL_HideCursor();
    if (!text.load(path_join(paths.app_data, "fonts/Inter-Regular.ttf"), path_join(paths.app_data, "fonts/Inter-Bold.ttf")))
        return 1;
    overlay_.resize(display.width(), display.height());
    make_backdrop();

    articles = std::make_unique<Articles>(*this, ArticleSource::Wallabag);
    wikipedia = std::make_unique<Articles>(*this, ArticleSource::Wikipedia);
    library_ = std::make_unique<LibraryScreen>(*this);
    reader_ = std::make_unique<ReaderScreen>(*this);
    if (!opt.script.empty())
        script_ = std::make_unique<TestScript>(opt.script);
    if (!opt.open_path.empty()) {
        // The same book must have the same key in state.json however it
        // was named on the command line (D22).
        char* real = ::realpath(opt.open_path.c_str(), nullptr);
        if (real && is_file(real))
            open_book(real, false);
        else
            toast("Book not found: " + opt.open_path);
        std::free(real);
    }

    now_ = mono_ms();
    render();
    LOG_I("first frame after %llu ms", static_cast<unsigned long long>(mono_ms() - t0));
    loop();

    flush();
    reader_->close();
    // A sync still running is cancelled; what it downloaded stays, what it
    // archived is archived again next time (harmless).
    articles.reset();
    wikipedia.reset();
    http_global_cleanup();
    state.save();
    return 0;
}

} // namespace dr
