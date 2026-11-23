// Math in articles (D36): TeX formulas in an article's HTML are turned into
// MathML when the article's EPUB is made, and crengine-ng draws the MathML
// with the bundled math font (STIX Two Math).
//
// Finding the formulas is pure and unit-tested; the conversion is Temml
// (TeX to MathML, JavaScript) running in QuickJS, with no I/O and with
// memory, stack and time limits. Used by the articles thread only.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dr {

struct TexSpan {
    size_t begin = 0, end = 0; // [begin, end) in the HTML, delimiters included
    std::string tex;           // the formula, entities decoded; environments keep \begin..\end
    bool display = false;      // \[..\], $$..$$, an environment: a formula of its own
};

constexpr size_t TEX_MAX_INLINE = 2000;   // characters of an inline formula
constexpr size_t TEX_MAX_DISPLAY = 10000; // ... of a displayed one
constexpr size_t TEX_MAX_FORMULAS = 500;  // per article

// The formulas in an article's HTML, in order. Only in its text: never in
// tags, attributes or comments, nor inside <code>, <pre>, <kbd>, <samp>,
// <tt>, <script>, <style>, <textarea>, <svg> or <math>; a formula doesn't
// span tags. Delimiters: \(..\) inline; \[..\], $$..$$ and the display
// environments (equation, align, gather, multline, eqnarray, starred too)
// displayed; $..$ inline by pandoc's rule (no space inside the dollars, no
// digit after the closing one) when it looks like math (see texmath.cpp).
// \$ is a dollar sign.
std::vector<TexSpan> find_tex(const std::string& html);

// A displayed formula's MathML with places to break its line: crengine
// draws each <mrow> unbroken but breaks one that is wider than the page
// anywhere inside, and Temml puts a displayed formula in a single <mrow>.
// Its top level is split into <mrow>s: a list of formulas ("S = …,\qquad
// M = …") after each comma followed by a space, any other formula after
// each relation (=, <, ∈, →...). Other MathML comes back unchanged.
std::string mathml_break_points(const std::string& mathml);

// Converts one formula into MathML; false when it can't (it stays as TeX).
using TexConverter = std::function<bool(const std::string& tex, bool display, std::string& mathml)>;
// The HTML with each formula `convert` accepts replaced by its MathML.
std::string replace_tex(const std::string& html, const std::vector<TexSpan>& spans, const TexConverter& convert,
                        int* converted = nullptr, int* failed = nullptr);

// Temml in QuickJS. One per thread: it must be used by the thread that
// loaded it.
class TexToMathML {
public:
    static constexpr size_t MEMORY_LIMIT = 64u << 20;
    static constexpr size_t STACK_LIMIT = 1u << 20;
    static constexpr uint64_t FORMULA_MS = 2000;

    TexToMathML();
    ~TexToMathML();
    TexToMathML(const TexToMathML&) = delete;
    TexToMathML& operator=(const TexToMathML&) = delete;

    // Reads and runs temml.min.js. `cancel` (may be null) interrupts a
    // conversion when it turns true.
    bool load(const std::string& temml_js, const std::atomic<bool>* cancel, std::string* error = nullptr);
    // False if the formula isn't valid TeX (or uses what trust: false
    // refuses), takes too long or too much memory, or the engine stopped
    // being usable (after an interruption).
    bool convert(const std::string& tex, bool display, std::string& mathml);
    bool usable() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dr
