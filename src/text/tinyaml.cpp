// Implementation of the tinyaml SAX-style YAML parser.
//
// This is a hand-written recursive-descent parser over the whole input
// buffered in memory (buffering is needed because YAML's block structure is
// indentation-driven: deciding where a node ends generally requires looking
// at the following line). No DOM is ever built though -- only the handler
// callbacks carry parsed data out of the parser, matching the SAX model
// described in tinyaml.hpp.
//
// Known simplifications (documented rather than silently wrong):
//  - Complex (non-scalar) mapping keys ("? ... : ...") are not supported and
//    are reported as a parse error, as are multi-line mapping keys.
//  - Merge keys ("<<: *anchor") are not treated specially; they show up as
//    an ordinary key/value pair.
//  - beginList/beginMap are always called with length == (size_t)-1: tinyaml
//    never pre-scans a collection to count its items.
//  - Anchors and aliases are reported to the handler (reference/alias) but
//    never resolved internally -- tinyaml keeps no anchor table.
//  - Tag shorthands are passed through as raw text; %TAG directives are
//    parsed and skipped but not used to expand shorthands.
#include "tinyaml.hpp"
#include <istream>
#include <sstream>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <limits>
#include <cmath>

namespace tinyaml {
namespace {

// Thrown internally to unwind the parser when a handler returns ABORT.
struct AbortParsing {};

class ParseError : public std::runtime_error {
public:
    ParseError (const std::string& msg, size_t ln, size_t col)
        : std::runtime_error(msg + " (line " + std::to_string(ln) + ", column " + std::to_string(col + 1) + ")")
        , line_(ln), column_(col) {}
    size_t line () const { return line_; }
    size_t column () const { return column_; }
private:
    size_t line_, column_;
};

void appendUtf8 (std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back((char)cp);
    } else if (cp <= 0x7FF) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

// Reads the whole stream into memory, normalizing CRLF/CR line endings to
// LF, and offers a cursor with line/column tracking plus cheap save/restore
// (used for the small amounts of backtracking the parser needs).
class Reader {
public:
    explicit Reader (std::istream& in) {
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string raw = ss.str();
        buf_.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); ++i) {
            char c = raw[i];
            if (c == '\r') {
                if (i + 1 < raw.size() && raw[i + 1] == '\n') continue;
                buf_.push_back('\n');
            } else {
                buf_.push_back(c);
            }
        }
    }

    struct Mark { size_t pos, line, col; };

    bool eof () const { return pos_ >= buf_.size(); }
    char peek (size_t offset = 0) const {
        size_t p = pos_ + offset;
        return p < buf_.size() ? buf_[p] : '\0';
    }
    char get () {
        char c = buf_[pos_++];
        if (c == '\n') { ++line_; col_ = 0; } else { ++col_; }
        return c;
    }
    size_t line () const { return line_; }
    size_t column () const { return col_; }
    size_t pos () const { return pos_; }
    Mark mark () const { return {pos_, line_, col_}; }
    void reset (Mark m) { pos_ = m.pos; line_ = m.line; col_ = m.col; }

private:
    std::string buf_;
    size_t pos_ = 0, line_ = 1, col_ = 0;
};

// Recognizes the YAML 1.2 core schema for plain scalars: null, bool, int
// (decimal/hex/octal) and float (including .inf/.nan). Returns false if the
// text does not match, in which case the caller should treat it as a string.
bool parseCoreInt (const std::string& s, long long& sval, unsigned long long& uval, bool& negative) {
    if (s.empty()) return false;
    size_t i = 0;
    negative = false;
    if (s[i] == '+') { ++i; }
    else if (s[i] == '-') { negative = true; ++i; }
    if (i >= s.size()) return false;
    std::string digits = s.substr(i);
    int base = 10;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) { base = 16; digits = digits.substr(2); }
    else if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'o' || digits[1] == 'O')) { base = 8; digits = digits.substr(2); }
    if (digits.empty()) return false;
    for (char c : digits) {
        if (base == 16) { if (!std::isxdigit((unsigned char)c)) return false; }
        else if (base == 8) { if (c < '0' || c > '7') return false; }
        else { if (!std::isdigit((unsigned char)c)) return false; }
    }
    errno = 0;
    char* end = nullptr;
    unsigned long long mag = std::strtoull(digits.c_str(), &end, base);
    if (errno == ERANGE) return false;
    if (!negative) {
        uval = mag;
    } else {
        unsigned long long limit = (unsigned long long)std::numeric_limits<long long>::max() + 1ULL;
        if (mag > limit) return false;
        sval = (mag == limit) ? std::numeric_limits<long long>::min() : -(long long)mag;
    }
    return true;
}

bool parseCoreFloat (const std::string& s, double& out) {
    if (s.empty()) return false;
    size_t i = 0;
    bool negative = false;
    if (s[i] == '+') { ++i; }
    else if (s[i] == '-') { negative = true; ++i; }
    std::string rest = s.substr(i);
    if (rest == ".inf" || rest == ".Inf" || rest == ".INF") {
        out = negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        return true;
    }
    if (rest == ".nan" || rest == ".NaN" || rest == ".NAN") {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    for (char c : s) {
        if (!(std::isdigit((unsigned char)c) || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')) return false;
    }
    if (s.find_first_of("0123456789") == std::string::npos) return false;
    if (s.find('.') == std::string::npos && s.find_first_of("eE") == std::string::npos) return false;
    errno = 0;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

class Parser {
public:
    Parser (Reader& r, handler& h) : r_(r), h_(&h) {}

    void parseStream () {
        for (;;) {
            skipBlankLinesAndComments();
            if (eof()) return;
            while (!eof() && column() == 0 && peek() == '%') {
                skipToEOL();
                skipBlankLinesAndComments();
            }
            if (eof()) return;
            if (column() == 0 && peek() == '-' && peek(1) == '-' && peek(2) == '-' && isSep(peek(3))) {
                get(); get(); get();
                skipInlineSpaces();
                if (!eof() && peek() == '#') skipToEOL();
            }
            int r = emit([&] { return h_->beginDocument(); });
            if (r == handler::SKIP) {
                skipToNextDocumentBoundary();
                continue;
            }
            skipBlankLinesAndComments();
            if (!eof() && !atDocumentMarker()) {
                parseNode(-1);
            } else {
                emitNullValue();
            }
            skipBlankLinesAndComments();
            if (!eof() && column() == 0 && peek() == '.' && peek(1) == '.' && peek(2) == '.' && isSep(peek(3))) {
                get(); get(); get();
                skipToEOL();
            }
            emit([&] { return h_->endDocument(); });
        }
    }

private:
    Reader& r_;
    handler* h_;
    std::string pendingTag_;

    struct SkipGuard {
        Parser& p; handler* saved; bool active;
        SkipGuard (Parser& p_, bool active_) : p(p_), saved(p_.h_), active(active_) { if (active) p.h_ = nullptr; }
        ~SkipGuard () { if (active) p.h_ = saved; }
    };

    struct ScalarScanResult { std::string text; bool hadColon; bool wasQuoted; };

    // --- low-level cursor helpers -----------------------------------
    bool eof () const { return r_.eof(); }
    char peek (size_t o = 0) const { return r_.peek(o); }
    char get () { return r_.get(); }
    size_t line () const { return r_.line(); }
    size_t column () const { return r_.column(); }
    size_t pos () const { return r_.pos(); }
    using Mark = Reader::Mark;
    Mark mark () const { return r_.mark(); }
    void reset (Mark m) { r_.reset(m); }

    static bool isSep (char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\0'; }

    [[noreturn]] void fail (const std::string& msg) { throw ParseError(msg, line(), column()); }

    // Every event reported to the handler goes through this helper, which
    // reports the current cursor position first (handler::position) and
    // then fires the event itself, both subject to the same
    // skip-mode/ABORT handling.
    template <class F>
    int emit (F&& f) {
        if (!h_) return handler::CONTINUE;
        int pr = h_->position(pos(), line(), column());
        if (pr == handler::ABORT) throw AbortParsing{};
        int r = f();
        if (r == handler::ABORT) throw AbortParsing{};
        return r;
    }

    void skipInlineSpaces () { while (!eof() && (peek() == ' ' || peek() == '\t')) get(); }
    void skipToEOL () { while (!eof() && peek() != '\n') get(); }

    void skipBlankLinesAndComments () {
        for (;;) {
            skipInlineSpaces();
            if (eof()) return;
            char c = peek();
            if (c == '\n') { get(); continue; }
            if (c == '#') { skipToEOL(); continue; }
            return;
        }
    }

    bool atDocumentMarker () const {
        if (column() != 0) return false;
        if (peek() == '-' && peek(1) == '-' && peek(2) == '-' && isSep(peek(3))) return true;
        if (peek() == '.' && peek(1) == '.' && peek(2) == '.' && isSep(peek(3))) return true;
        return false;
    }

    void skipToNextDocumentBoundary () {
        for (;;) {
            if (eof()) return;
            if (column() == 0 && ((peek() == '-' && peek(1) == '-' && peek(2) == '-') ||
                                   (peek() == '.' && peek(1) == '.' && peek(2) == '.'))) return;
            skipToEOL();
            if (!eof()) get();
        }
    }

    std::string scanIndicatorName () {
        std::string s;
        while (!eof()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == ',' || c == '[' || c == ']' || c == '{' || c == '}') break;
            s.push_back(c);
            get();
        }
        if (s.empty()) fail("expected a name after '&'/'!'/'*'");
        return s;
    }

    void parseAnchorPrefix () {
        get(); // '&'
        std::string name = scanIndicatorName();
        emit([&] { return h_->reference(name); });
    }

    void parseTagPrefix () {
        get(); // '!'
        std::string name;
        if (!eof() && peek() == '<') {
            get();
            std::string uri;
            while (!eof() && peek() != '>') uri.push_back(get());
            if (eof()) fail("unterminated verbatim tag");
            get();
            name = uri;
        } else {
            name = "!";
            if (!eof() && peek() == '!') { name += "!"; get(); }
            name += scanIndicatorNameAllowEmpty();
        }
        pendingTag_ = name;
        emit([&] { return h_->tag(name); });
    }

    // Like scanIndicatorName but allows an empty result (bare "!" tag).
    std::string scanIndicatorNameAllowEmpty () {
        std::string s;
        while (!eof()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == ',' || c == '[' || c == ']' || c == '{' || c == '}') break;
            s.push_back(c);
            get();
        }
        return s;
    }

    void parseAliasAndEmit () {
        get(); // '*'
        std::string name = scanIndicatorName();
        pendingTag_.clear();
        emit([&] { return h_->alias(name); });
    }

    // --- scalar scanning ---------------------------------------------
    uint32_t readHexCode (int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            if (eof()) fail("truncated unicode escape");
            char c = get();
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else fail("invalid hex digit in unicode escape");
        }
        return v;
    }

    std::string scanSingleQuoted () {
        get(); // opening '
        std::string result;
        for (;;) {
            if (eof()) fail("unterminated single-quoted scalar");
            char c = get();
            if (c == '\'') {
                if (!eof() && peek() == '\'') { result.push_back('\''); get(); continue; }
                break;
            }
            if (c == '\n') {
                while (!result.empty() && (result.back() == ' ' || result.back() == '\t')) result.pop_back();
                size_t blanks = 0;
                for (;;) {
                    Mark lm = mark();
                    skipInlineSpaces();
                    if (!eof() && peek() == '\n') { ++blanks; get(); continue; }
                    reset(lm);
                    break;
                }
                skipInlineSpaces();
                if (blanks == 0) result.push_back(' '); else result.append(blanks, '\n');
                continue;
            }
            result.push_back(c);
        }
        return result;
    }

    std::string scanDoubleQuoted () {
        get(); // opening "
        std::string result;
        for (;;) {
            if (eof()) fail("unterminated double-quoted scalar");
            char c = get();
            if (c == '"') break;
            if (c == '\\') {
                if (eof()) fail("unterminated escape sequence");
                char e = get();
                switch (e) {
                    case '0': result.push_back('\0'); break;
                    case 'a': result.push_back('\a'); break;
                    case 'b': result.push_back('\b'); break;
                    case 't': case '\t': result.push_back('\t'); break;
                    case 'n': result.push_back('\n'); break;
                    case 'v': result.push_back('\v'); break;
                    case 'f': result.push_back('\f'); break;
                    case 'r': result.push_back('\r'); break;
                    case 'e': result.push_back('\x1b'); break;
                    case ' ': result.push_back(' '); break;
                    case '"': result.push_back('"'); break;
                    case '/': result.push_back('/'); break;
                    case '\\': result.push_back('\\'); break;
                    case 'N': appendUtf8(result, 0x85); break;
                    case '_': appendUtf8(result, 0xA0); break;
                    case 'L': appendUtf8(result, 0x2028); break;
                    case 'P': appendUtf8(result, 0x2029); break;
                    case 'x': appendUtf8(result, readHexCode(2)); break;
                    case 'u': appendUtf8(result, readHexCode(4)); break;
                    case 'U': appendUtf8(result, readHexCode(8)); break;
                    case '\n': skipInlineSpaces(); break; // escaped line break: no fold, no char
                    default: fail(std::string("unknown escape sequence '\\") + e + "'");
                }
                continue;
            }
            if (c == '\n') {
                while (!result.empty() && (result.back() == ' ' || result.back() == '\t')) result.pop_back();
                size_t blanks = 0;
                for (;;) {
                    Mark lm = mark();
                    skipInlineSpaces();
                    if (!eof() && peek() == '\n') { ++blanks; get(); continue; }
                    reset(lm);
                    break;
                }
                skipInlineSpaces();
                if (blanks == 0) result.push_back(' '); else result.append(blanks, '\n');
                continue;
            }
            result.push_back(c);
        }
        return result;
    }

    // Scans a plain (unquoted) scalar in block context, starting at the
    // current position. Stops before ": " / ":" at end of line (possible
    // mapping key separator), before " #" (comment), or at a dedent below
    // ownIndent (after folding across continuation lines).
    std::string scanPlainScalarBlock (size_t ownIndent) {
        std::string result;
        for (;;) {
            std::string lineText;
            while (!eof()) {
                char c = peek();
                if (c == '\n') break;
                if (c == '#' && !lineText.empty() && (lineText.back() == ' ' || lineText.back() == '\t')) break;
                if (c == ':' && isSep(peek(1))) break;
                lineText.push_back(c);
                get();
            }
            while (!lineText.empty() && (lineText.back() == ' ' || lineText.back() == '\t')) lineText.pop_back();
            result += lineText;
            if (!eof() && (peek() == ':' || peek() == '#')) return result;
            if (eof()) return result;
            get(); // consume '\n'
            size_t blanks = 0;
            for (;;) {
                Mark lm = mark();
                skipInlineSpaces();
                if (!eof() && peek() == '\n') { ++blanks; get(); continue; }
                reset(lm);
                break;
            }
            skipInlineSpaces();
            if (eof() || atDocumentMarker() || column() <= ownIndent) return result;
            result += (blanks == 0) ? std::string(1, ' ') : std::string(blanks, '\n');
        }
    }

    // Scans a plain scalar in flow context (inside [...] / {...}), stopping
    // at any of , [ ] { } : (the last only as a separator) or a real
    // terminator reached after folding.
    std::string scanPlainScalarFlow () {
        std::string result;
        for (;;) {
            std::string lineText;
            while (!eof()) {
                char c = peek();
                if (c == '\n') break;
                if (c == ',' || c == '[' || c == ']' || c == '{' || c == '}') break;
                if (c == ':' && (isSep(peek(1)) || peek(1) == ',' || peek(1) == ']' || peek(1) == '}')) break;
                if (c == '#' && !lineText.empty() && (lineText.back() == ' ' || lineText.back() == '\t')) break;
                lineText.push_back(c);
                get();
            }
            while (!lineText.empty() && (lineText.back() == ' ' || lineText.back() == '\t')) lineText.pop_back();
            result += lineText;
            if (eof() || peek() != '\n') return result;
            get(); // consume '\n'
            size_t blanks = 0;
            for (;;) {
                Mark lm = mark();
                skipInlineSpaces();
                if (!eof() && peek() == '\n') { ++blanks; get(); continue; }
                reset(lm);
                break;
            }
            skipInlineSpaces();
            if (eof()) return result;
            char c = peek();
            if (c == ',' || c == '[' || c == ']' || c == '{' || c == '}' || c == ':' || c == '#') return result;
            result += (blanks == 0) ? std::string(1, ' ') : std::string(blanks, '\n');
        }
    }

    ScalarScanResult scanScalarAndCheckColon (size_t ownIndent) {
        char c = eof() ? '\0' : peek();
        std::string text;
        bool wasQuoted = false;
        if (c == '\'') { text = scanSingleQuoted(); wasQuoted = true; }
        else if (c == '"') { text = scanDoubleQuoted(); wasQuoted = true; }
        else if (c == '?') { fail("explicit complex mapping keys ('?') are not supported"); }
        else { text = scanPlainScalarBlock(ownIndent); }
        Mark m = mark();
        skipInlineSpaces();
        bool hadColon = false;
        if (!eof() && peek() == ':' && isSep(peek(1))) {
            get();
            hadColon = true;
        } else {
            reset(m);
        }
        return {text, hadColon, wasQuoted};
    }

    // --- block scalars ("|" and ">") ----------------------------------
    std::string parseBlockScalarAndReturn (char style, int parentIndent) {
        get(); // '|' or '>'
        int explicitIndent = -1;
        char chomp = 0;
        for (int i = 0; i < 2 && !eof(); ++i) {
            char c = peek();
            if (c >= '1' && c <= '9') { explicitIndent = c - '0'; get(); }
            else if (c == '-' || c == '+') { chomp = c; get(); }
            else break;
        }
        skipInlineSpaces();
        if (!eof() && peek() == '#') skipToEOL();
        if (!eof() && peek() == '\n') get();
        else if (!eof()) fail("unexpected characters after block scalar header");

        bool determined = explicitIndent >= 0;
        size_t blockIndent = determined ? (size_t)std::max(0, parentIndent) + (size_t)explicitIndent : 0;
        std::vector<std::string> contentLines;
        for (;;) {
            if (eof()) break;
            Mark lineStart = mark();
            size_t indent = 0;
            while (!eof() && peek() == ' ') { ++indent; get(); }
            if (eof() || peek() == '\n') {
                contentLines.push_back("");
                if (!eof()) get();
                continue;
            }
            if (!determined) { blockIndent = indent; determined = true; }
            if (indent < blockIndent) { reset(lineStart); break; }
            std::string content(indent - blockIndent, ' ');
            while (!eof() && peek() != '\n') content.push_back(get());
            contentLines.push_back(content);
            if (!eof()) get();
        }
        size_t trailingBlanks = 0;
        while (!contentLines.empty() && contentLines.back().empty()) {
            ++trailingBlanks;
            contentLines.pop_back();
        }
        std::string text;
        if (style == '|') {
            for (size_t i = 0; i < contentLines.size(); ++i) {
                if (i) text += '\n';
                text += contentLines[i];
            }
        } else {
            for (size_t i = 0; i < contentLines.size(); ++i) {
                if (i == 0) { text += contentLines[i]; continue; }
                bool curIndented = !contentLines[i].empty() && contentLines[i][0] == ' ';
                bool prevIndented = !contentLines[i - 1].empty() && contentLines[i - 1][0] == ' ';
                if (contentLines[i - 1].empty() || contentLines[i].empty() || curIndented || prevIndented) text += '\n';
                else text += ' ';
                text += contentLines[i];
            }
        }
        if (chomp == '-') {
            // strip: nothing more to add
        } else if (chomp == '+') {
            if (!contentLines.empty()) text += '\n';
            for (size_t i = 0; i < trailingBlanks; ++i) text += '\n';
        } else {
            if (!contentLines.empty() || trailingBlanks > 0) text += '\n';
        }
        return text;
    }

    // --- scalar value emission -----------------------------------------
    void emitStringValue (const std::string& text) { emit([&] { return h_->value(text); }); }

    void emitResolvedScalar (const std::string& text, bool isPlain) {
        std::string tag = pendingTag_;
        pendingTag_.clear();
        if (tag == "!!str") { emitStringValue(text); return; }
        if (!isPlain) { emitStringValue(text); return; }
        if (text.empty() || text == "~" || text == "null" || text == "Null" || text == "NULL") {
            emit([&] { return h_->value(nullptr); });
            return;
        }
        if (text == "true" || text == "True" || text == "TRUE") { emit([&] { return h_->value(true); }); return; }
        if (text == "false" || text == "False" || text == "FALSE") { emit([&] { return h_->value(false); }); return; }
        long long sval = 0; unsigned long long uval = 0; bool neg = false;
        if (parseCoreInt(text, sval, uval, neg)) {
            if (neg) emit([&] { return h_->value(sval); });
            else emit([&] { return h_->value(uval); });
            return;
        }
        double dval = 0;
        if (parseCoreFloat(text, dval)) { emit([&] { return h_->value(dval); }); return; }
        emitStringValue(text);
    }

    void emitNullValue () {
        pendingTag_.clear();
        emit([&] { return h_->value(nullptr); });
    }

    // --- block mapping / sequence ---------------------------------------
    void parseBlockMappingBody (size_t ownIndent, const std::string* firstKey) {
        bool isFirst = true;
        for (;;) {
            std::string keyText;
            if (isFirst && firstKey) {
                keyText = *firstKey;
            } else {
                skipBlankLinesAndComments();
                if (eof() || atDocumentMarker() || column() != ownIndent) break;
                if (peek() == '-' && isSep(peek(1))) break;
                auto res = scanScalarAndCheckColon(ownIndent);
                if (!res.hadColon) fail("expected ':' after mapping key");
                keyText = res.text;
            }
            isFirst = false;
            int kr = emit([&] { return h_->key(keyText); });
            bool skipVal = (kr == handler::SKIP);
            skipInlineSpaces();
            {
                SkipGuard g(*this, skipVal);
                parseNode((int)ownIndent);
            }
        }
    }

    void beginBlockMappingFromKey (size_t ownIndent, const std::string& firstKeyText) {
        int r = emit([&] { return h_->beginMap((size_t)-1); });
        bool skipAll = (r == handler::SKIP);
        {
            SkipGuard g(*this, skipAll);
            parseBlockMappingBody(ownIndent, &firstKeyText);
        }
        if (!skipAll) emit([&] { return h_->endMap(); });
    }

    void parseBlockSequenceBody (size_t ownIndent) {
        for (;;) {
            get(); // consume '-'
            if (!eof() && (peek() == ' ' || peek() == '\t')) get();
            parseNode((int)ownIndent);
            skipBlankLinesAndComments();
            if (eof() || atDocumentMarker() || column() != ownIndent) break;
            if (!(peek() == '-' && isSep(peek(1)))) break;
        }
    }

    void parseBlockSequence (size_t ownIndent) {
        int r = emit([&] { return h_->beginList((size_t)-1); });
        bool skipAll = (r == handler::SKIP);
        {
            SkipGuard g(*this, skipAll);
            parseBlockSequenceBody(ownIndent);
        }
        if (!skipAll) emit([&] { return h_->endList(); });
    }

    // --- flow collections -------------------------------------------------
    void skipFlowWhitespace () {
        for (;;) {
            if (eof()) return;
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\n') { get(); continue; }
            if (c == '#') { skipToEOL(); continue; }
            return;
        }
    }

    void parseFlowNode () {
        skipFlowWhitespace();
        pendingTag_.clear();
        while (!eof() && (peek() == '&' || peek() == '!')) {
            if (peek() == '&') parseAnchorPrefix(); else parseTagPrefix();
            skipFlowWhitespace();
        }
        if (!eof() && peek() == '*') { parseAliasAndEmit(); return; }
        char c = eof() ? '\0' : peek();
        if (c == '[') { parseFlowSequence(); return; }
        if (c == '{') { parseFlowMapping(); return; }
        if (c == '\'') { emitResolvedScalar(scanSingleQuoted(), false); return; }
        if (c == '"') { emitResolvedScalar(scanDoubleQuoted(), false); return; }
        if (c == ',' || c == ']' || c == '}' || c == '\0') { emitResolvedScalar("", true); return; }
        std::string t = scanPlainScalarFlow();
        emitResolvedScalar(t, true);
    }

    struct FlowKeyScan { std::string text; bool hadColon; };

    FlowKeyScan scanFlowKey () {
        while (!eof() && (peek() == '&' || peek() == '!')) {
            if (peek() == '&') parseAnchorPrefix(); else parseTagPrefix();
            skipFlowWhitespace();
        }
        char c = eof() ? '\0' : peek();
        std::string text;
        if (c == '\'') text = scanSingleQuoted();
        else if (c == '"') text = scanDoubleQuoted();
        else if (c == '?') fail("explicit complex mapping keys ('?') are not supported");
        else text = scanPlainScalarFlow();
        skipFlowWhitespace();
        bool hadColon = false;
        if (!eof() && peek() == ':' && (isSep(peek(1)) || peek(1) == ',' || peek(1) == '}' || peek(1) == ']')) {
            get();
            hadColon = true;
        }
        return {text, hadColon};
    }

    void parseFlowSequence () {
        get(); // '['
        int r = emit([&] { return h_->beginList((size_t)-1); });
        bool skipAll = (r == handler::SKIP);
        {
            SkipGuard g(*this, skipAll);
            skipFlowWhitespace();
            while (!eof() && peek() != ']') {
                parseFlowNode();
                skipFlowWhitespace();
                if (!eof() && peek() == ',') { get(); skipFlowWhitespace(); }
                else break;
            }
        }
        skipFlowWhitespace();
        if (eof() || peek() != ']') fail("expected ']'");
        get();
        if (!skipAll) emit([&] { return h_->endList(); });
    }

    void parseFlowMapping () {
        get(); // '{'
        int r = emit([&] { return h_->beginMap((size_t)-1); });
        bool skipAll = (r == handler::SKIP);
        {
            SkipGuard g(*this, skipAll);
            skipFlowWhitespace();
            while (!eof() && peek() != '}') {
                auto ks = scanFlowKey();
                int kr = emit([&] { return h_->key(ks.text); });
                bool skipVal = (kr == handler::SKIP);
                skipFlowWhitespace();
                {
                    SkipGuard g2(*this, skipVal);
                    if (ks.hadColon) { skipFlowWhitespace(); parseFlowNode(); }
                    else { emit([&] { return h_->value(nullptr); }); }
                }
                skipFlowWhitespace();
                if (!eof() && peek() == ',') { get(); skipFlowWhitespace(); }
                else break;
            }
        }
        skipFlowWhitespace();
        if (eof() || peek() != '}') fail("expected '}'");
        get();
        if (!skipAll) emit([&] { return h_->endMap(); });
    }

    // --- generic node dispatch --------------------------------------------
    void dispatchContent () {
        size_t ownIndent = column();
        if (eof()) { emitNullValue(); return; }
        char c = peek();
        if (c == '-' && isSep(peek(1))) { pendingTag_.clear(); parseBlockSequence(ownIndent); return; }
        if (c == '[') { pendingTag_.clear(); parseFlowSequence(); return; }
        if (c == '{') { pendingTag_.clear(); parseFlowMapping(); return; }
        if (c == '|' || c == '>') {
            std::string text = parseBlockScalarAndReturn(c, (int)ownIndent);
            emitResolvedScalar(text, false);
            return;
        }
        auto res = scanScalarAndCheckColon(ownIndent);
        if (res.hadColon) beginBlockMappingFromKey(ownIndent, res.text);
        else emitResolvedScalar(res.text, !res.wasQuoted);
    }

    void parseNode (int parentIndent) {
        skipInlineSpaces();
        pendingTag_.clear();
        while (!eof() && (peek() == '&' || peek() == '!')) {
            if (peek() == '&') parseAnchorPrefix(); else parseTagPrefix();
            skipInlineSpaces();
        }
        if (!eof() && peek() == '*') { parseAliasAndEmit(); return; }
        if (eof() || peek() == '\n' || peek() == '#') {
            skipBlankLinesAndComments();
            if (eof() || atDocumentMarker() || (int)column() <= parentIndent) {
                emitNullValue();
                return;
            }
            dispatchContent();
            return;
        }
        dispatchContent();
    }
};

} // namespace

int parse (std::istream& input, handler& h) {
    Reader r(input);
    Parser p(r, h);
    try {
        p.parseStream();
    } catch (const AbortParsing&) {
        return handler::ABORT;
    } catch (const std::exception& e) {
        h.error(e);
        return handler::ABORT;
    }
    return handler::CONTINUE;
}

} // namespace tinyaml
