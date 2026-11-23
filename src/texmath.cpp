#include "texmath.h"

#include "epubwrite.h"
#include "fsutil.h"
#include "log.h"
#include "util.h"

#include <quickjs.h>

#include <cctype>
#include <cstring>

namespace dr {

// --- finding the formulas -----------------------------------------------------------

namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

// s[pos..] starts with `what` (lower case), ignoring case.
bool starts_ci(const std::string& s, size_t pos, const char* what) {
    const size_t n = std::strlen(what);
    if (pos + n > s.size())
        return false;
    for (size_t k = 0; k < n; k++)
        if (std::tolower(static_cast<unsigned char>(s[pos + k])) != what[k])
            return false;
    return true;
}

// Elements whose text isn't prose: no formulas inside (counted, they nest).
const char* const NO_MATH[] = {"code", "pre", "kbd", "samp", "tt", "svg", "math"};
// Raw-text elements: skipped to their end tag ('<' inside is text).
const char* const RAW_TEXT[] = {"script", "style", "textarea"};
// Environments that make a displayed formula of their own.
const char* const DISPLAY_ENVS[] = {"equation", "equation*", "align",    "align*",   "gather",
                                    "gather*",  "multline",  "multline*", "eqnarray", "eqnarray*"};

// From `from`, the end delimiter of a formula in [from, end): `close` (")",
// "]" after a backslash; "$" or "$$"), outside braces; a backslash and the
// character after it are one unit ("\$", "\{", "\\"). npos if none.
size_t find_close(const std::string& h, size_t from, size_t end, const char* close, bool after_backslash) {
    const size_t n = std::strlen(close);
    int depth = 0;
    for (size_t p = from; p < end; p++) {
        const char c = h[p];
        if (c == '\\') {
            if (after_backslash && depth == 0 && p + 1 + n <= end && h.compare(p + 1, n, close) == 0)
                return p;
            p++; // the escaped character
            continue;
        }
        if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth = depth > 0 ? depth - 1 : 0;
        } else if (!after_backslash && depth == 0 && p + n <= end && h.compare(p, n, close) == 0) {
            if (n == 2 || p + 1 >= end || h[p + 1] != '$') // "$" doesn't stop at a "$$"
                return p;
        }
    }
    return std::string::npos;
}

// $..$ content that reads like math: TeX markup, or no word of two or more
// letters between spaces ("$5 and 10$" is prose; "$x + y$", "$dx$" math).
bool looks_like_math(const std::string& s) {
    if (s.find_first_of("\\^_{}=") != std::string::npos)
        return true;
    for (size_t i = 0; i < s.size();) {
        if (!is_alpha(s[i])) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < s.size() && is_alpha(s[j]))
            j++;
        if (j - i >= 2 && i > 0 && is_space(s[i - 1]) && j < s.size() && is_space(s[j]))
            return false;
        i = j;
    }
    return true;
}

void add(std::vector<TexSpan>& out, const std::string& h, size_t begin, size_t end, size_t tex_begin,
         size_t tex_end, bool display) {
    const std::string tex = decode_entities(h.substr(tex_begin, tex_end - tex_begin));
    if (tex.find_first_not_of(" \t\r\n\f") == std::string::npos)
        return; // "\(\)", "$$ $$": nothing to draw
    if (tex.size() > (display ? TEX_MAX_DISPLAY : TEX_MAX_INLINE))
        return;
    out.push_back({begin, end, tex, display});
}

// The formulas in one run of text [a, b) (no tags inside).
void scan_text(const std::string& h, size_t a, size_t b, std::vector<TexSpan>& out) {
    size_t k = a;
    while (k < b && out.size() < TEX_MAX_FORMULAS) {
        const char c = h[k];
        if (c == '\\' && k + 1 < b) {
            const char d = h[k + 1];
            if (d == '(' || d == '[') {
                size_t e = find_close(h, k + 2, b, d == '(' ? ")" : "]", true);
                if (e == std::string::npos) {
                    k += 2;
                    continue;
                }
                add(out, h, k, e + 2, k + 2, e, d == '[');
                k = e + 2;
                continue;
            }
            if (h.compare(k, 7, "\\begin{") == 0) {
                const size_t name_end = h.find('}', k + 7);
                if (name_end != std::string::npos && name_end < b && name_end - (k + 7) <= 12) {
                    const std::string env = h.substr(k + 7, name_end - (k + 7));
                    bool display = false;
                    for (const char* e : DISPLAY_ENVS)
                        display = display || env == e;
                    const std::string close = "\\end{" + env + "}";
                    const size_t e = display ? h.find(close, name_end + 1) : std::string::npos;
                    if (e != std::string::npos && e + close.size() <= b) {
                        add(out, h, k, e + close.size(), k, e + close.size(), true);
                        k = e + close.size();
                        continue;
                    }
                }
            }
            k += 2; // "\$" is a dollar sign; other escapes are text
            continue;
        }
        if (c == '$') {
            if (k + 1 < b && h[k + 1] == '$') {
                size_t e = find_close(h, k + 2, b, "$$", false);
                if (e == std::string::npos) {
                    k += 2;
                    continue;
                }
                add(out, h, k, e + 2, k + 2, e, true);
                k = e + 2;
                continue;
            }
            // Pandoc's rule: a non-space after the opening dollar; the
            // closing one after a non-space and before no digit.
            if (k + 1 < b && !is_space(h[k + 1])) {
                size_t p = k + 1, e = std::string::npos;
                while ((p = find_close(h, p, b, "$", false)) != std::string::npos) {
                    if (!is_space(h[p - 1]) && !(p + 1 < b && is_digit(h[p + 1]))) {
                        e = p;
                        break;
                    }
                    p++;
                }
                if (e != std::string::npos && e - (k + 1) <= TEX_MAX_INLINE &&
                    looks_like_math(decode_entities(h.substr(k + 1, e - (k + 1))))) {
                    add(out, h, k, e + 1, k + 1, e, false);
                    k = e + 1;
                    continue;
                }
            }
        }
        k++;
    }
}

} // namespace

std::vector<TexSpan> find_tex(const std::string& html) {
    std::vector<TexSpan> out;
    const std::string& h = html;
    const size_t n = h.size();
    int no_math = 0; // depth inside NO_MATH elements
    size_t i = 0;
    while (i < n && out.size() < TEX_MAX_FORMULAS) {
        // The run of text up to the next tag.
        size_t t = i;
        while (t < n && !(h[t] == '<' && t + 1 < n && (is_alpha(h[t + 1]) || h[t + 1] == '/' || h[t + 1] == '!' || h[t + 1] == '?')))
            t++;
        if (t > i && no_math == 0)
            scan_text(h, i, t, out);
        if (t >= n)
            break;
        // The tag (or comment) at t.
        if (h.compare(t, 4, "<!--") == 0) {
            size_t e = h.find("-->", t + 4);
            i = e == std::string::npos ? n : e + 3;
            continue;
        }
        if (h[t + 1] == '!' || h[t + 1] == '?') {
            size_t e = h.find('>', t);
            i = e == std::string::npos ? n : e + 1;
            continue;
        }
        const bool closing = h[t + 1] == '/';
        size_t p = t + (closing ? 2 : 1), name_begin = p;
        while (p < n && (is_alpha(h[p]) || is_digit(h[p]) || h[p] == '-' || h[p] == ':'))
            p++;
        std::string name = to_lower(h.substr(name_begin, p - name_begin));
        // The end of the tag, past quoted attribute values.
        char quote = 0;
        while (p < n && (quote || h[p] != '>')) {
            if (quote) {
                if (h[p] == quote)
                    quote = 0;
            } else if (h[p] == '"' || h[p] == '\'') {
                quote = h[p];
            }
            p++;
        }
        if (p >= n)
            break; // an unclosed tag: no more text
        const bool self_closing = h[p - 1] == '/';
        i = p + 1;
        if (const size_t colon = name.find(':'); colon != std::string::npos)
            name = name.substr(colon + 1); // <m:math>, <svg:svg>
        bool raw = false;
        for (const char* r : RAW_TEXT)
            raw = raw || name == r;
        if (raw) {
            if (!closing && !self_closing) {
                size_t e = i;
                while ((e = h.find("</", e)) != std::string::npos && !starts_ci(h, e + 2, name.c_str()))
                    e += 2;
                size_t gt = e == std::string::npos ? e : h.find('>', e);
                i = gt == std::string::npos ? n : gt + 1;
            }
            continue;
        }
        for (const char* m : NO_MATH) {
            if (name != m || self_closing)
                continue;
            no_math = closing ? (no_math > 0 ? no_math - 1 : 0) : no_math + 1;
        }
    }
    return out;
}

std::string replace_tex(const std::string& html, const std::vector<TexSpan>& spans, const TexConverter& convert,
                        int* converted, int* failed) {
    std::string out;
    out.reserve(html.size() + html.size() / 2);
    size_t copied = 0;
    int ok = 0, bad = 0;
    std::string mathml;
    for (const TexSpan& s : spans) {
        if (s.begin < copied || s.end > html.size())
            continue; // not in order: left alone
        out.append(html, copied, s.begin - copied);
        mathml.clear();
        if (convert(s.tex, s.display, mathml) && !mathml.empty()) {
            out += mathml;
            ok++;
        } else {
            out.append(html, s.begin, s.end - s.begin);
            bad++;
        }
        copied = s.end;
    }
    out.append(html, copied, std::string::npos);
    if (converted)
        *converted = ok;
    if (failed)
        *failed = bad;
    return out;
}

// --- line breaks in displayed formulas ---------------------------------------------------

namespace {

// The top-level elements of an XML fragment (Temml's output: well formed),
// as [begin, end) ranges. Text between elements is not expected.
std::vector<std::pair<size_t, size_t>> xml_children(const std::string& s, size_t begin, size_t end) {
    std::vector<std::pair<size_t, size_t>> out;
    int depth = 0;
    size_t start = begin;
    for (size_t i = begin; i < end; i++) {
        if (s[i] != '<')
            continue;
        const size_t gt = s.find('>', i);
        if (gt == std::string::npos || gt >= end)
            return {};
        if (depth == 0)
            start = i;
        if (s[i + 1] == '/')
            depth--;
        else if (s[gt - 1] != '/')
            depth++;
        if (depth < 0)
            return {};
        if (depth == 0)
            out.emplace_back(start, gt + 1);
        i = gt;
    }
    return depth == 0 ? out : std::vector<std::pair<size_t, size_t>>{};
}

// The text of a top-level <mo> element; "" for anything else.
std::string mo_text(const std::string& s, size_t b, size_t e) {
    if (s.compare(b, 3, "<mo") != 0 || (s[b + 3] != '>' && s[b + 3] != ' '))
        return "";
    const size_t gt = s.find('>', b);
    const size_t close = s.rfind("</mo>", e);
    return gt < close && close != std::string::npos ? s.substr(gt + 1, close - gt - 1) : "";
}

bool is_relation(const std::string& t) {
    static const char* const rel[] = {"=", "&lt;", "&gt;", "<", ">", ":=", "≠", "≤", "≥", "≈", "≡",
                                      "∼", "≃", "≅", "∝", "∈", "∉", "⊂", "⊃", "⊆", "⊇", "→",
                                      "←", "↔", "⇒", "⇐", "⇔", "↦", "≪", "≫", "≔"};
    for (const char* r : rel)
        if (t == r)
            return true;
    return false;
}

} // namespace

std::string mathml_break_points(const std::string& m) {
    // <math ...><mrow>CHILDREN</mrow></math>, nothing else.
    const size_t open_end = m.find('>');
    if (m.compare(0, 5, "<math") != 0 || open_end == std::string::npos || m.compare(open_end + 1, 6, "<mrow>") != 0)
        return m;
    static const std::string tail = "</mrow></math>";
    if (m.size() < tail.size() || m.compare(m.size() - tail.size(), tail.size(), tail) != 0)
        return m;
    const size_t inner = open_end + 7, inner_end = m.size() - tail.size();
    const auto kids = xml_children(m, inner, inner_end);
    if (kids.size() < 2 || kids.front().first != inner || kids.back().second != inner_end)
        return m; // one child, or the outer <mrow> doesn't hold them all
    auto is_space_at = [&](size_t k) { return k < kids.size() && m.compare(kids[k].first, 7, "<mspace") == 0; };
    // A list ("a = 1,\qquad b = 2") breaks between its items only, so no
    // "b =" is left at the end of a line; other formulas after relations.
    bool list = false;
    for (size_t k = 0; k + 1 < kids.size() && !list; k++)
        list = mo_text(m, kids[k].first, kids[k].second) == "," && is_space_at(k + 1);
    std::vector<std::pair<size_t, size_t>> groups; // ranges of children, [first, last]
    size_t first = 0;
    for (size_t k = 0; k < kids.size(); k++) {
        const std::string t = mo_text(m, kids[k].first, kids[k].second);
        const bool end_here = list ? t == "," && is_space_at(k + 1) : is_relation(t);
        if (!end_here)
            continue;
        while (is_space_at(k + 1))
            k++; // the space after it stays on its line
        groups.emplace_back(first, k);
        first = k + 1;
    }
    if (first < kids.size())
        groups.emplace_back(first, kids.size() - 1);
    if (groups.size() < 2)
        return m;
    std::string out = m.substr(0, open_end + 1);
    for (const auto& [a, b] : groups)
        out += "<mrow>" + m.substr(kids[a].first, kids[b].second - kids[a].first) + "</mrow>";
    return out + "</math>";
}

// --- Temml in QuickJS -------------------------------------------------------------------

struct TexToMathML::Impl {
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    JSValue fn = JS_UNDEFINED;
    const std::atomic<bool>* cancel = nullptr;
    uint64_t deadline = 0; // mono_ms; 0 = none
    bool loaded = false;   // `fn` holds the converter
    bool broken = false;

    ~Impl() {
        if (ctx) {
            JS_FreeValue(ctx, fn);
            JS_FreeContext(ctx);
        }
        if (rt)
            JS_FreeRuntime(rt);
    }

    // Called by QuickJS now and then while it runs: non-zero stops it.
    static int interrupt(JSContext*, void* opaque) {
        const Impl* im = static_cast<const Impl*>(opaque);
        if (im->cancel && im->cancel->load())
            return 1;
        return im->deadline && mono_ms() > im->deadline ? 1 : 0;
    }

    // The pending exception's message (and clears it).
    std::string exception() {
        JSValue e = JS_GetException(ctx);
        std::string msg = "unknown error";
        if (const char* s = JS_ToCString(ctx, e)) {
            msg = s;
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, e);
        return msg.substr(0, 160);
    }
};

TexToMathML::TexToMathML() : impl_(std::make_unique<Impl>()) {}
TexToMathML::~TexToMathML() = default;

bool TexToMathML::usable() const {
    return impl_->loaded && !impl_->broken;
}

bool TexToMathML::load(const std::string& temml_js, const std::atomic<bool>* cancel, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error)
            *error = why;
        impl_ = std::make_unique<Impl>();
        return false;
    };
    impl_ = std::make_unique<Impl>();
    std::string src;
    if (!read_file(temml_js, src, 4u << 20))
        return fail("can't read " + temml_js);
    Impl& im = *impl_;
    im.cancel = cancel;
    im.rt = JS_NewRuntime();
    if (!im.rt)
        return fail("no JavaScript runtime");
    JS_SetMemoryLimit(im.rt, MEMORY_LIMIT);
    JS_SetMaxStackSize(im.rt, STACK_LIMIT);
    JS_SetInterruptHandler(im.rt, &Impl::interrupt, &im);
    im.ctx = JS_NewContext(im.rt); // plain ECMAScript: no std/os modules, no I/O
    if (!im.ctx)
        return fail("no JavaScript context");
    im.deadline = mono_ms() + 10000;
    JSValue r = JS_Eval(im.ctx, src.c_str(), src.size(), "temml.min.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r))
        return fail("temml.min.js: " + im.exception());
    JS_FreeValue(im.ctx, r);
    // trust: false refuses \href, \url, \includegraphics...; maxExpand stops
    // macro loops; maxSize caps \rule and spaces (20 em, 300 pt);
    // throwOnError, so a formula that fails stays as TeX; xml, for XHTML;
    // wrap: inline formulas break after relations and operators, as in TeX
    // (displayed ones: mathml_break_points).
    static const char wrapper[] =
        "(function (tex, display) { return temml.renderToString(tex, {displayMode: display, xml: true, "
        "throwOnError: true, trust: false, maxExpand: 1000, maxSize: [20, 300], annotate: false, wrap: 'tex'}); })";
    im.fn = JS_Eval(im.ctx, wrapper, sizeof(wrapper) - 1, "<deckreader>", JS_EVAL_TYPE_GLOBAL);
    im.deadline = 0;
    if (JS_IsException(im.fn)) {
        im.fn = JS_UNDEFINED;
        return fail("Temml: " + im.exception());
    }
    im.loaded = true;
    return true;
}

bool TexToMathML::convert(const std::string& tex, bool display, std::string& mathml) {
    if (!usable())
        return false;
    Impl& im = *impl_;
    if (im.cancel && im.cancel->load()) { // QuickJS checks it only now and then
        im.broken = true;
        return false;
    }
    JSValue args[2] = {JS_NewStringLen(im.ctx, tex.data(), tex.size()), JS_NewBool(im.ctx, display)};
    im.deadline = mono_ms() + FORMULA_MS;
    JSValue r = JS_Call(im.ctx, im.fn, JS_UNDEFINED, 2, args);
    im.deadline = 0;
    JS_FreeValue(im.ctx, args[0]);
    JS_FreeValue(im.ctx, args[1]);
    if (JS_IsException(r)) {
        const std::string msg = im.exception();
        // Stopped from the menu, or out of memory: no more formulas.
        if ((im.cancel && im.cancel->load()) || msg.find("out of memory") != std::string::npos)
            im.broken = true;
        LOG_D("texmath: formula not converted: %s", msg.c_str());
        return false;
    }
    bool ok = false;
    size_t len = 0;
    if (const char* s = JS_IsString(r) ? JS_ToCStringLen(im.ctx, &len, r) : nullptr) {
        mathml.assign(s, len);
        JS_FreeCString(im.ctx, s);
        ok = mathml.compare(0, 5, "<math") == 0;
        if (ok && display)
            mathml = mathml_break_points(mathml);
    }
    JS_FreeValue(im.ctx, r);
    return ok;
}

} // namespace dr
