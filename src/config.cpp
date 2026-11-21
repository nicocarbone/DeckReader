#include "config.h"
#include "http.h"

#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace dr {

static bool parse_bool(const std::string& v, bool* out) {
    std::string s = to_lower(v);
    if (s == "yes" || s == "true" || s == "on" || s == "1") {
        *out = true;
        return true;
    }
    if (s == "no" || s == "false" || s == "off" || s == "0") {
        *out = false;
        return true;
    }
    return false;
}

Config parse_config(const std::string& text, const std::string& home) {
    Config cfg;
    bool folders_seen = false;
    std::string section;
    int lineno = 0;
    for (std::string line : split(text, '\n')) {
        lineno++;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line[0] == '[') {
            size_t end = line.find(']');
            section = to_lower(trim(line.substr(1, end == std::string::npos ? std::string::npos : end - 1)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            cfg.warnings.push_back(strf("line %d: expected key = value", lineno));
            continue;
        }
        std::string key = to_lower(trim(line.substr(0, eq)));
        for (char& c : key)
            if (c == '-' || c == ' ')
                c = '_';
        std::string value = trim(line.substr(eq + 1));
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                                  (value.front() == '\'' && value.back() == '\'')))
            value = value.substr(1, value.size() - 2);

        bool ok = true;
        if (section == "library" && key == "folder") {
            if (!folders_seen) {
                cfg.folders.clear();
                folders_seen = true;
            }
            if (!value.empty())
                cfg.folders.push_back(expand_user(value, home));
        } else if (section == "library" && key == "removable_media") {
            ok = parse_bool(value, &cfg.removable_media);
        } else if (section == "interface" && key == "ui_scale") {
            char* end = nullptr;
            double v = std::strtod(value.c_str(), &end);
            ok = end && *end == '\0' && v >= 0.5 && v <= 3.0;
            if (ok)
                cfg.ui_scale = v;
        } else if (section == "interface" && key == "recently_read") {
            char* end = nullptr;
            long v = std::strtol(value.c_str(), &end, 10);
            ok = !value.empty() && end && *end == '\0' && v >= 0 && v <= 20;
            if (ok)
                cfg.recently_read = static_cast<int>(v);
        } else if (section == "interface" && key == "clock") {
            std::string v = to_lower(value);
            ok = v == "24h" || v == "12h";
            if (ok)
                cfg.clock_24h = v == "24h";
        } else if (section == "interface" && key == "accent_color") {
            std::string v = value;
            if (!v.empty() && v[0] == '#')
                v = v.substr(1);
            ok = v.size() == 6 && std::all_of(v.begin(), v.end(), [](char ch) {
                     return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
                 });
            if (ok)
                cfg.accent = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 16));
        } else if (section == "reader" && key == "tap_zones") {
            ok = parse_bool(value, &cfg.tap_zones);
        } else if (section == "reader" && key == "keep_awake") {
            ok = parse_bool(value, &cfg.keep_awake);
        } else if (section == "reader" && key == "keep_awake_minutes") {
            char* end = nullptr;
            long v = std::strtol(value.c_str(), &end, 10);
            ok = !value.empty() && end && *end == '\0' && v >= 1 && v <= 240;
            if (ok)
                cfg.keep_awake_minutes = static_cast<int>(v);
        } else if (section == "wallabag" && key == "server") {
            cfg.wallabag.server = value;
            const std::string lower = to_lower(value);
            ok = value.empty() || ((starts_with(lower, "https://") || starts_with(lower, "http://")) &&
                                   !url_host(value).empty());
            if (!ok) {
                cfg.wallabag.server.clear();
            } else if (starts_with(lower, "http://")) {
                // Fine at home; on the internet the password travels unencrypted.
                const std::string host = host_name(url_host(value));
                const bool local = host == "localhost" || (is_ip(host) && !public_ip(host));
                if (!local)
                    cfg.warnings.push_back("[wallabag] server uses http://: the password and the articles are sent "
                                           "unencrypted; use https:// unless the server is on your own network");
            }
        } else if (section == "wallabag" && key == "client_id") {
            cfg.wallabag.client_id = value;
        } else if (section == "wallabag" && key == "client_secret") {
            cfg.wallabag.client_secret = value;
        } else if (section == "wallabag" && key == "username") {
            cfg.wallabag.username = value;
        } else if (section == "wallabag" && key == "password") {
            cfg.wallabag.password = value;
        } else if (section == "wallabag" && key == "images") {
            ok = parse_bool(value, &cfg.wallabag.images);
        } else if (section == "wallabag" && key == "max_articles") {
            char* end = nullptr;
            long v = std::strtol(value.c_str(), &end, 10);
            ok = !value.empty() && end && *end == '\0' && v >= 1 && v <= 500;
            if (ok)
                cfg.wallabag.max_articles = static_cast<int>(v);
        } else {
            cfg.warnings.push_back(strf("line %d: unknown setting [%s] %s", lineno, section.c_str(), key.c_str()));
            continue;
        }
        if (!ok)
            cfg.warnings.push_back(strf("line %d: bad value for %s: %s", lineno, key.c_str(),
                                        key == "password" || key == "client_secret" ? "(hidden)" : value.c_str()));
    }
    const WallabagSettings& w = cfg.wallabag;
    if (!w.configured() && (!w.server.empty() || !w.client_id.empty() || !w.client_secret.empty() ||
                            !w.username.empty() || !w.password.empty())) {
        std::string missing;
        for (auto [name, v] : {std::pair<const char*, const std::string*>{"server", &w.server},
                               {"client_id", &w.client_id},
                               {"client_secret", &w.client_secret},
                               {"username", &w.username},
                               {"password", &w.password}})
            if (v->empty())
                missing += std::string(missing.empty() ? "" : ", ") + name;
        cfg.warnings.push_back("[wallabag] is not complete (missing: " + missing + "); articles are off");
    }
    if (!folders_seen)
        cfg.folders.push_back(path_join(home, "Books"));
    return cfg;
}

} // namespace dr
