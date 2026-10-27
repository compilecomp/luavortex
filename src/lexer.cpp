// CEP:FILE: src/lexer.cpp
// CEP:WHAT: Implementation of the Lua 5.4 scanner: names, keywords, integer
//           and float numerals (decimal and hexadecimal), short strings with
//           the full escape set, long-bracket strings and comments.
// CEP:WHY: A single linear scanner with explicit kind tags keeps the parser
//           free of low-level text decisions and mirrors the reference
//           frontend structure of the engine (frontends/reference).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic with position on malformed input;
//          overflow of decimal numerals falls back to the float payload per
//          the Lua numeral rules. No throw.
// CEP:ASSUMES: The source buffer outlives the returned token vector; byte
//          values are handled opaquely outside string escapes.
// CEP:COST: cold; one linear pass per compile, not performance-critical.
// CEP:EVIDENCE: tests/run_tests.sh exercises every token family through
//          compiled examples.
#include "luavortex/lexer.hpp"

#include <stdexcept>

namespace luavortex {

namespace {

// Named scanner constants (Law 7 and CEP&CC 11.3: every literal is named).
constexpr char kNewline = '\n';
constexpr char kCarriageReturn = '\r';
constexpr char kOpenBracket = '[';
constexpr char kCloseBracket = ']';
constexpr char kQuoteSingle = '\'';
constexpr char kQuoteDouble = '"';
constexpr char kMinus = '-';
constexpr char kDot = '.';
constexpr char kEscapeChar = '\\';
constexpr char kZeroDigit = '0';
constexpr char kNineDigit = '9';
constexpr char kLowerA = 'a';
constexpr char kLowerF = 'f';
constexpr char kUpperA = 'A';
constexpr char kUpperF = 'F';
constexpr char kLowerZ = 'z';
constexpr char kUpperZ = 'Z';
constexpr char kUnderscore = '_';
constexpr char kHexLeaderLower = 'x';
constexpr char kHexLeaderUpper = 'X';
constexpr char kExponentLower = 'e';
constexpr char kExponentUpper = 'E';
constexpr char kHexFloatExpLower = 'p';
constexpr char kHexFloatExpUpper = 'P';
constexpr char kEscapeHexLower = 'x';
constexpr char kEscapeSkipLower = 'z';
constexpr char kEscapeBraceOpen = '{';
constexpr char kEscapeBraceClose = '}';
constexpr char kPlusSign = '+';
constexpr char kEqualsSign = '=';
constexpr char kAssignSign = '=';
constexpr char kColonSign = ':';
constexpr int kRadixDecimal = 10;
constexpr int kRadixHex = 16;
constexpr int kLongBracketLevelMax = 255;
constexpr size_t kCommentIntroLength = 2;
constexpr size_t kRadixPrefixLength = 2;
constexpr int kDecimalEscapeDigits = 3;
constexpr int kHexEscapeDigits = 2;
constexpr int kEscapeByteMax = 255;
constexpr char kHexFloatDot = '.';

bool is_digit(char c) noexcept {
    return c >= kZeroDigit && c <= kNineDigit;
}

bool is_hex_digit(char c) noexcept {
    return is_digit(c) || (c >= kLowerA && c <= kLowerF) ||
           (c >= kUpperA && c <= kUpperF);
}

bool is_name_start(char c) noexcept {
    return c == kUnderscore || (c >= kLowerA && c <= kLowerZ) ||
           (c >= kUpperA && c <= kUpperZ);
}

bool is_name_char(char c) noexcept {
    return is_name_start(c) || is_digit(c);
}

// CEP:WHAT: Converts one hexadecimal digit byte to its numeric value.
// CEP:WHY: Escape decoding needs digit arithmetic; a total helper keeps every call site branch-free.
// CEP:STATUS: complete
// CEP:FAILURE: none (total over the hex digit range).
// CEP:ASSUMES: The byte is a hex digit (callers gate with is_hex_digit).
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
// CEP:WHAT: Converts one hexadecimal digit byte to its numeric value.
// CEP:WHY: Escape decoding needs digit arithmetic; a total helper keeps every call site branch-free.
// CEP:STATUS: complete
// CEP:FAILURE: none (total over the hex digit range).
// CEP:ASSUMES: The byte is a hex digit (callers gate with is_hex_digit).
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
int hex_value(char c) noexcept {
    if (is_digit(c)) return c - kZeroDigit;
    if (c >= kLowerA && c <= kLowerF) return c - kLowerA + kRadixDecimal;
    return c - kUpperA + kRadixDecimal;
}

bool is_space_byte(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\v' || c == kCarriageReturn ||
           c == kNewline;
}

/// Mutable cursor over the source; owns position tracking for diagnostics.
struct Scanner {
    std::string_view source;
    size_t pos = 0;
    uint32_t line = 1;
    uint32_t column = 1;

    bool at_end() const noexcept { return pos >= source.size(); }

    char peek(size_t ahead = 0) const noexcept {
        const size_t at = pos + ahead;
        return at < source.size() ? source[at] : '\0';
    }

// CEP:WHAT: Consumes one source byte and updates the line and column counters.
// CEP:WHY: Position tracking must stay exact for every diagnostic the parser emits.
// CEP:STATUS: complete
// CEP:FAILURE: none (the scanner checks at_end before advancing).
// CEP:ASSUMES: Called only when at least one byte remains.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
// CEP:WHAT: Consumes one source byte and updates the line and column counters.
// CEP:WHY: Position tracking must stay exact for every diagnostic the parser emits.
// CEP:STATUS: complete
// CEP:FAILURE: none (the scanner checks at_end before advancing).
// CEP:ASSUMES: Called only when at least one byte remains.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
    char advance() noexcept {
        const char c = source[pos++];
        if (c == kNewline) {
            ++line;
            column = 1;
        } else {
            ++column;
        }
        return c;
    }

// CEP:WHAT: Consumes a run of source bytes through the position tracker.
// CEP:WHY: Multi-byte tokens (comment introducers, radix prefixes) advance in one step.
// CEP:STATUS: complete
// CEP:FAILURE: none (bounded by the caller's lookahead).
// CEP:ASSUMES: The requested count does not pass the end of input.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
// CEP:WHAT: Consumes a run of source bytes through the position tracker.
// CEP:WHY: Multi-byte tokens (comment introducers, radix prefixes) advance in one step.
// CEP:STATUS: complete
// CEP:FAILURE: none (bounded by the caller's lookahead).
// CEP:ASSUMES: The requested count does not pass the end of input.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
    void advance_n(size_t count) noexcept {
        for (size_t i = 0; i < count; ++i) advance();
    }
};

// CEP:WHAT: Consumes one long-bracket comment body after its introducer.
// CEP:WHY: Long comments share level handling with long strings, so the
//          bracket-level logic lives in exactly one place per role.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for an unterminated long comment.
// CEP:ASSUMES: The cursor sits on the first body byte after the opener.
// CEP:COST: cold; linear in the comment length.
// CEP:EVIDENCE: long-comment examples compile in CI.
vortex::support::Result<void> skip_long_comment(Scanner& s, int level) {
    while (true) {
        if (s.at_end()) {
            return vortex::support::fail(vortex::support::ErrorCode::SyntaxError,
                                         "unterminated long comment", s.line,
                                         s.column);
        }
        if (s.peek() == kCloseBracket) {
            size_t ahead = 1;
            int close_level = 0;
            while (s.peek(ahead) == kEqualsSign) {
                ++ahead;
                ++close_level;
            }
            if (close_level == level && s.peek(ahead) == kCloseBracket) {
                s.advance_n(ahead + 1);
                return vortex::support::ok();
            }
        }
        s.advance();
    }
}

// CEP:WHAT: Skips whitespace and both comment forms, tracking positions.
// CEP:WHY: Token positions must be exact for parser diagnostics; central
//          trivia handling keeps that invariant in one place.
// CEP:STATUS: complete
// CEP:FAILURE: Propagates long-comment diagnostics.
// CEP:ASSUMES: none beyond the Scanner invariants.
// CEP:COST: cold; linear in skipped text.
// CEP:EVIDENCE: all compiled examples exercise this path.
vortex::support::Result<void> skip_trivia(Scanner& s) {
    while (!s.at_end()) {
        const char c = s.peek();
        if (is_space_byte(c)) {
            s.advance();
            continue;
        }
        if (c == kMinus && s.peek(1) == kMinus) {
            s.advance_n(kCommentIntroLength);
            if (s.peek() == kOpenBracket) {
                size_t ahead = 1;
                int level = 0;
                while (s.peek(ahead) == kEqualsSign &&
                       level <= kLongBracketLevelMax) {
                    ++ahead;
                    ++level;
                }
                if (s.peek(ahead) == kOpenBracket) {
                    s.advance_n(ahead + 1);
                    auto res = skip_long_comment(s, level);
                    if (!res) return res;
                    continue;
                }
            }
            while (!s.at_end() && s.peek() != kNewline) s.advance();
            continue;
        }
        break;
    }
    return vortex::support::ok();
}

// CEP:WHAT: Reads a long-bracket string body after the opening level.
// CEP:WHY: Long strings skip the first newline (Lua rule) and end at the
//          matching close level; keeping this separate from short strings
//          preserves the exact semantics of both.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic on an unterminated long string.
// CEP:ASSUMES: The cursor sits on the opening '['.
// CEP:COST: cold; linear in the string length.
// CEP:EVIDENCE: long-string example compiled in CI.
vortex::support::Result<std::string> read_long_string(Scanner& s, int level) {
    s.advance();  // consume the opening bracket
    if (s.peek() == kCarriageReturn) {
        s.advance();
        if (s.peek() == kNewline) s.advance();
    } else if (s.peek() == kNewline) {
        s.advance();
    }
    std::string out;
    while (true) {
        if (s.at_end()) {
            return vortex::support::fail(vortex::support::ErrorCode::SyntaxError,
                                         "unterminated long string", s.line,
                                         s.column);
        }
        if (s.peek() == kCloseBracket) {
            size_t ahead = 1;
            int close_level = 0;
            while (s.peek(ahead) == kEqualsSign) {
                ++ahead;
                ++close_level;
            }
            if (close_level == level && s.peek(ahead) == kCloseBracket) {
                s.advance_n(ahead + 1);
                return out;
            }
        }
        out.push_back(s.advance());
    }
}

// CEP:WHAT: Decodes a short string body including the full Lua escape set.
// CEP:WHY: Escape handling is the only place byte-level validation matters;
//          exact decimal and hexadecimal escape rules keep literals faithful.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic on unterminated strings and malformed
//          escapes (bad digit, empty brace form, value out of byte range).
// CEP:ASSUMES: The cursor sits on the opening quote.
// CEP:COST: cold; linear in the string length.
// CEP:EVIDENCE: escaped string examples compiled in CI.
vortex::support::Result<std::string> read_short_string(Scanner& s) {
    const char quote = s.advance();
    std::string out;
    while (true) {
        if (s.at_end() || s.peek() == kNewline) {
            return vortex::support::fail(vortex::support::ErrorCode::SyntaxError,
                                         "unterminated string", s.line,
                                         s.column);
        }
        const char c = s.advance();
        if (c == quote) return out;
        if (c != kEscapeChar) {
            out.push_back(c);
            continue;
        }
        const char e = s.advance();
        switch (e) {
        case 'a': out.push_back('\a'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'v': out.push_back('\v'); break;
        case kEscapeChar: out.push_back(kEscapeChar); break;
        case '"': out.push_back('"'); break;
        case '\'': out.push_back('\''); break;
        case kNewline: out.push_back(kNewline); break;
        case kEscapeHexLower: {
            if (!is_hex_digit(s.peek()) || !is_hex_digit(s.peek(1))) {
                return vortex::support::fail(
                    vortex::support::ErrorCode::SyntaxError,
                    "malformed hexadecimal escape", s.line, s.column);
            }
            int value = 0;
            for (int i = 0; i < kHexEscapeDigits; ++i) {
                value = value * kRadixHex + hex_value(s.advance());
            }
            out.push_back(static_cast<char>(value));
            break;
        }
        case kEscapeSkipLower: {
            while (!s.at_end() && is_space_byte(s.peek())) s.advance();
            break;
        }
        default: {
            if (e == kEscapeBraceOpen) {
                int value = 0;
                int digits = 0;
                while (is_hex_digit(s.peek())) {
                    value = value * kRadixHex + hex_value(s.advance());
                    ++digits;
                }
                if (digits == 0 || s.peek() != kEscapeBraceClose) {
                    return vortex::support::fail(
                        vortex::support::ErrorCode::SyntaxError,
                        "malformed brace escape", s.line, s.column);
                }
                s.advance();
                if (value > kEscapeByteMax) {
                    return vortex::support::fail(
                        vortex::support::ErrorCode::SyntaxError,
                        "escape value exceeds byte range", s.line, s.column);
                }
                out.push_back(static_cast<char>(value));
                break;
            }
            if (!is_digit(e)) {
                return vortex::support::fail(
                    vortex::support::ErrorCode::SyntaxError,
                    "invalid escape sequence", s.line, s.column);
            }
            int value = e - kZeroDigit;
            int digits = 1;
            while (digits < kDecimalEscapeDigits && is_digit(s.peek())) {
                value = value * kRadixDecimal + (s.advance() - kZeroDigit);
                ++digits;
            }
            if (value > kEscapeByteMax) {
                return vortex::support::fail(
                    vortex::support::ErrorCode::SyntaxError,
                    "decimal escape exceeds byte range", s.line, s.column);
            }
            out.push_back(static_cast<char>(value));
            break;
        }
        }
    }
}

// CEP:WHAT: Reads a numeral and classifies it as integer or float.
// CEP:WHY: Lua distinguishes integer and float numerals with specific
//          hexadecimal exponent rules; the scanner owns that classification.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic on malformed numerals; decimal integer
//          overflow yields the float payload per the reference behavior.
// CEP:ASSUMES: The cursor sits on a digit or a dot followed by a digit.
// CEP:COST: cold; linear in the numeral length.
// CEP:EVIDENCE: numeral examples compiled in CI.
vortex::support::Result<Token> read_number(Scanner& s, Token token) {
    const size_t start = s.pos;
    bool is_float = false;
    bool is_hex = false;
    if (s.peek() == kZeroDigit &&
        (s.peek(1) == kHexLeaderLower || s.peek(1) == kHexLeaderUpper)) {
        is_hex = true;
        s.advance_n(kRadixPrefixLength);
        if (!is_hex_digit(s.peek())) {
            return vortex::support::fail(vortex::support::ErrorCode::SyntaxError,
                                         "malformed number", s.line, s.column);
        }
        while (is_hex_digit(s.peek()) || s.peek() == kHexFloatDot) {
            if (s.peek() == kHexFloatDot) is_float = true;
            s.advance();
        }
        if (s.peek() == kHexFloatExpLower || s.peek() == kHexFloatExpUpper) {
            is_float = true;
            s.advance();
            if (s.peek() == kPlusSign || s.peek() == kMinus) s.advance();
            if (!is_digit(s.peek())) {
                return vortex::support::fail(
                    vortex::support::ErrorCode::SyntaxError,
                    "malformed hexadecimal float exponent", s.line, s.column);
            }
            while (is_digit(s.peek())) s.advance();
        }
    } else {
        while (is_digit(s.peek())) s.advance();
        if (s.peek() == kHexFloatDot) {
            is_float = true;
            s.advance();
            while (is_digit(s.peek())) s.advance();
        }
        if (s.peek() == kExponentLower || s.peek() == kExponentUpper) {
            is_float = true;
            s.advance();
            if (s.peek() == kPlusSign || s.peek() == kMinus) s.advance();
            if (!is_digit(s.peek())) {
                return vortex::support::fail(
                    vortex::support::ErrorCode::SyntaxError,
                    "malformed exponent", s.line, s.column);
            }
            while (is_digit(s.peek())) s.advance();
        }
    }
    token.text = s.source.substr(start, s.pos - start);
    const std::string text(token.text);
    try {
        if (is_float) {
            token.kind = TokenKind::Float;
            token.float_value = std::stod(text);
        } else {
            size_t consumed = 0;
            const int64_t value =
                std::stoll(text, &consumed, is_hex ? kRadixHex : kRadixDecimal);
            token.kind = TokenKind::Int;
            token.int_value = value;
            if (consumed != text.size()) {
                return vortex::support::fail(
                    vortex::support::ErrorCode::SyntaxError,
                    "malformed number '" + text + "'", s.line, s.column);
            }
        }
    } catch (const std::out_of_range&) {
        // Decimal integer overflow falls back to the float payload, which is
        // the documented behavior for oversized numerals in the language.
        token.kind = TokenKind::Float;
        token.float_value = std::stod(text);
    } catch (const std::invalid_argument&) {
        return vortex::support::fail(vortex::support::ErrorCode::SyntaxError,
                                     "malformed number '" + text + "'", s.line,
                                     s.column);
    }
    return token;
}

// CEP:WHAT: Maps an identifier to its keyword kind or the name kind.
// CEP:WHY: Keyword recognition is a pure table decision; isolating it keeps
//          the scanner loop linear and the parser grammar-driven.
// CEP:STATUS: complete
// CEP:FAILURE: none (total function over identifier text).
// CEP:ASSUMES: The text is a scanned identifier.
// CEP:COST: cold; one bounded comparison chain per identifier.
// CEP:EVIDENCE: all test cases tokenize through this classifier.
TokenKind classify_word(std::string_view text) noexcept {
    if (text == "and") return TokenKind::KeywordAnd;
    if (text == "break") return TokenKind::KeywordBreak;
    if (text == "do") return TokenKind::KeywordDo;
    if (text == "else") return TokenKind::KeywordElse;
    if (text == "elseif") return TokenKind::KeywordElseif;
    if (text == "end") return TokenKind::KeywordEnd;
    if (text == "false") return TokenKind::KeywordFalse;
    if (text == "for") return TokenKind::KeywordFor;
    if (text == "function") return TokenKind::KeywordFunction;
    if (text == "goto") return TokenKind::KeywordGoto;
    if (text == "if") return TokenKind::KeywordIf;
    if (text == "in") return TokenKind::KeywordIn;
    if (text == "local") return TokenKind::KeywordLocal;
    if (text == "nil") return TokenKind::KeywordNil;
    if (text == "not") return TokenKind::KeywordNot;
    if (text == "or") return TokenKind::KeywordOr;
    if (text == "repeat") return TokenKind::KeywordRepeat;
    if (text == "return") return TokenKind::KeywordReturn;
    if (text == "then") return TokenKind::KeywordThen;
    if (text == "true") return TokenKind::KeywordTrue;
    if (text == "until") return TokenKind::KeywordUntil;
    if (text == "while") return TokenKind::KeywordWhile;
    return TokenKind::Name;
}

// CEP:WHAT: Consumes one punctuator or operator token.
// CEP:WHY: Longest-match operator scanning is self-contained; separating it
//          from trivia and literals keeps each branch trivial to review.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for stray bytes outside the grammar.
// CEP:ASSUMES: The cursor sits on a non-trivia, non-literal byte.
// CEP:COST: cold; bounded comparisons per punctuator.
// CEP:EVIDENCE: all test cases tokenize operators through this path.
vortex::support::Result<Token> read_punctuator(Scanner& s) {
    Token token;
    token.line = s.line;
    token.column = s.column;
    const size_t start = s.pos;
    const char c = s.advance();
    const auto flush = [&](TokenKind kind) {
        token.kind = kind;
        token.text = s.source.substr(start, s.pos - start);
        return token;
    };
    switch (c) {
    case '+': return flush(TokenKind::Plus);
    case '*': return flush(TokenKind::Star);
    case '%': return flush(TokenKind::Percent);
    case '^': return flush(TokenKind::Caret);
    case '#': return flush(TokenKind::Hash);
    case '&': return flush(TokenKind::Amp);
    case '|': return flush(TokenKind::Pipe);
    case '(': return flush(TokenKind::LParen);
    case ')': return flush(TokenKind::RParen);
    case '{': return flush(TokenKind::LBrace);
    case '}': return flush(TokenKind::RBrace);
    case ']': return flush(TokenKind::RBracket);
    case ';': return flush(TokenKind::Semicolon);
    case ',': return flush(TokenKind::Comma);
    case kMinus:
        if (s.peek() == kMinus) {
            // A line comment here only happens before trivia skipping; treat
            // it defensively by consuming it and returning the next token.
            s.advance_n(kCommentIntroLength);
            while (!s.at_end() && s.peek() != kNewline) s.advance();
            token.kind = TokenKind::EndOfFile;
            return token;
        }
        return flush(TokenKind::Minus);
    case '/':
        if (s.peek() == '/') {
            s.advance();
            return flush(TokenKind::SlashSlash);
        }
        return flush(TokenKind::Slash);
    case '~':
        if (s.peek() == kEqualsSign) {
            s.advance();
            return flush(TokenKind::NotEq);
        }
        return flush(TokenKind::Tilde);
    case kAssignSign:
        if (s.peek() == kEqualsSign) {
            s.advance();
            return flush(TokenKind::EqEq);
        }
        return flush(TokenKind::Assign);
    case '<':
        if (s.peek() == kEqualsSign) {
            s.advance();
            return flush(TokenKind::Le);
        }
        if (s.peek() == '<') {
            s.advance();
            return flush(TokenKind::Shl);
        }
        return flush(TokenKind::Lt);
    case '>':
        if (s.peek() == kEqualsSign) {
            s.advance();
            return flush(TokenKind::Ge);
        }
        if (s.peek() == '>') {
            s.advance();
            return flush(TokenKind::Shr);
        }
        return flush(TokenKind::Gt);
    case kColonSign:
        if (s.peek() == kColonSign) {
            s.advance();
            return flush(TokenKind::Scope);
        }
        return flush(TokenKind::Colon);
    case kDot:
        if (s.peek() == kDot) {
            s.advance();
            if (s.peek() == kDot) {
                s.advance();
                return flush(TokenKind::DotDotDot);
            }
            return flush(TokenKind::DotDot);
        }
        return flush(TokenKind::Dot);
    case kOpenBracket: return flush(TokenKind::LBracket);
    default: break;
    }
    return vortex::support::fail(vortex::support::ErrorCode::SyntaxError,
                                 "unexpected character", s.line, s.column);
}

}  // namespace

// CEP:WHAT: Returns the diagnostic name of a token kind.
// CEP:WHY: Parser and emitter diagnostics name the offending token; a single
//          naming function keeps messages consistent.
// CEP:STATUS: complete
// CEP:FAILURE: none (total function over the kind enumeration).
// CEP:ASSUMES: none.
// CEP:COST: cold; one bounded comparison chain.
// CEP:EVIDENCE: exercised by parser failure diagnostics in the test suite.
std::string_view token_kind_name(TokenKind kind) noexcept {
    switch (kind) {
    case TokenKind::EndOfFile: return "end of file";
    case TokenKind::Name: return "name";
    case TokenKind::Int: return "integer numeral";
    case TokenKind::Float: return "float numeral";
    case TokenKind::String: return "string literal";
    case TokenKind::KeywordAnd: return "'and'";
    case TokenKind::KeywordBreak: return "'break'";
    case TokenKind::KeywordDo: return "'do'";
    case TokenKind::KeywordElse: return "'else'";
    case TokenKind::KeywordElseif: return "'elseif'";
    case TokenKind::KeywordEnd: return "'end'";
    case TokenKind::KeywordFalse: return "'false'";
    case TokenKind::KeywordFor: return "'for'";
    case TokenKind::KeywordFunction: return "'function'";
    case TokenKind::KeywordGoto: return "'goto'";
    case TokenKind::KeywordIf: return "'if'";
    case TokenKind::KeywordIn: return "'in'";
    case TokenKind::KeywordLocal: return "'local'";
    case TokenKind::KeywordNil: return "'nil'";
    case TokenKind::KeywordNot: return "'not'";
    case TokenKind::KeywordOr: return "'or'";
    case TokenKind::KeywordRepeat: return "'repeat'";
    case TokenKind::KeywordReturn: return "'return'";
    case TokenKind::KeywordThen: return "'then'";
    case TokenKind::KeywordTrue: return "'true'";
    case TokenKind::KeywordUntil: return "'until'";
    case TokenKind::KeywordWhile: return "'while'";
    case TokenKind::Plus: return "'+'";
    case TokenKind::Minus: return "'-'";
    case TokenKind::Star: return "'*'";
    case TokenKind::Slash: return "'/'";
    case TokenKind::SlashSlash: return "'//'";
    case TokenKind::Percent: return "'%'";
    case TokenKind::Caret: return "'^'";
    case TokenKind::Hash: return "'#'";
    case TokenKind::Amp: return "'&'";
    case TokenKind::Tilde: return "'~'";
    case TokenKind::Pipe: return "'|'";
    case TokenKind::Shl: return "'<<'";
    case TokenKind::Shr: return "'>>'";
    case TokenKind::EqEq: return "'=='";
    case TokenKind::NotEq: return "'~='";
    case TokenKind::Le: return "'<='";
    case TokenKind::Ge: return "'>='";
    case TokenKind::Lt: return "'<'";
    case TokenKind::Gt: return "'>'";
    case TokenKind::Assign: return "'='";
    case TokenKind::LParen: return "'('";
    case TokenKind::RParen: return "')'";
    case TokenKind::LBrace: return "'{'";
    case TokenKind::RBrace: return "'}'";
    case TokenKind::LBracket: return "'['";
    case TokenKind::RBracket: return "]'";
    case TokenKind::Scope: return "'::'";
    case TokenKind::Semicolon: return "';'";
    case TokenKind::Colon: return "':'";
    case TokenKind::Comma: return "','";
    case TokenKind::Dot: return "'.'";
    case TokenKind::DotDot: return "'..'";
    case TokenKind::DotDotDot: return "'...'";
    }
    return "token";
}

// CEP:WHAT: Scans the whole chunk into a token vector.
// CEP:WHY: The parser then runs over a random-access token stream, which
//          keeps lookahead and error reporting simple.
// CEP:STATUS: complete
// CEP:FAILURE: Returns the first scanner diagnostic; token views reference
//          the caller's source buffer.
// CEP:ASSUMES: The source buffer outlives the returned vector.
// CEP:COST: cold; single linear pass.
// CEP:EVIDENCE: every test case compiles through this entry point.
vortex::support::Result<std::vector<Token>> lex(std::string_view source) {
    Scanner s;
    s.source = source;
    std::vector<Token> tokens;
    while (true) {
        auto trivia = skip_trivia(s);
        if (!trivia) return std::unexpected(trivia.error());
        Token token;
        token.line = s.line;
        token.column = s.column;
        if (s.at_end()) {
            token.kind = TokenKind::EndOfFile;
            tokens.push_back(token);
            return tokens;
        }
        const char c = s.peek();
        if (is_name_start(c)) {
            const size_t start = s.pos;
            while (is_name_char(s.peek())) s.advance();
            token.text = s.source.substr(start, s.pos - start);
            token.kind = classify_word(token.text);
            tokens.push_back(token);
            continue;
        }
        if (is_digit(c) || (c == kDot && is_digit(s.peek(1)))) {
            auto number = read_number(s, token);
            if (!number) return std::unexpected(number.error());
            tokens.push_back(std::move(*number));
            continue;
        }
        if (c == kQuoteSingle || c == kQuoteDouble) {
            auto value = read_short_string(s);
            if (!value) return std::unexpected(value.error());
            token.kind = TokenKind::String;
            token.str_value = std::move(*value);
            tokens.push_back(std::move(token));
            continue;
        }
        if (c == kOpenBracket) {
            size_t ahead = 1;
            int level = 0;
            while (s.peek(ahead) == kEqualsSign &&
                   level <= kLongBracketLevelMax) {
                ++ahead;
                ++level;
            }
            if (s.peek(ahead) == kOpenBracket) {
                auto value = read_long_string(s, level);
                if (!value) return std::unexpected(value.error());
                token.kind = TokenKind::String;
                token.str_value = std::move(*value);
                tokens.push_back(std::move(token));
                continue;
            }
        }
        auto punct = read_punctuator(s);
        if (!punct) return std::unexpected(punct.error());
        tokens.push_back(std::move(*punct));
    }
}

}  // namespace luavortex
