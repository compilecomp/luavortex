// CEP:FILE: include/luavortex/lexer.hpp
// CEP:WHAT: Lua 5.4 tokenizer: converts chunk source text into a token stream.
// CEP:WHY: The frontend needs a faithful Lua 5.4 scanner (numbers, long-bracket
//          strings, all operator spellings) before parsing; a single-pass
//          scanner with explicit token kinds keeps the parser grammar-driven.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic with line and column on any malformed
//          token (bad escape, unterminated string, malformed number). No
//          allocation failure is recoverable; the scanner allocates only the
//          token vector. No throw.
// CEP:ASSUMES: Source text is valid UTF-8 or byte-oriented Latin-1 content;
//          the scanner treats bytes opaquely except inside string escapes.
// CEP:COST: cold; one linear pass per compile, not performance-critical.
// CEP:EVIDENCE: tests/cases lexer coverage via tests/run_tests.sh; unit smoke
//          through lvxc on examples/fib.lua.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vortex/support/result.hpp"

namespace luavortex {

/// Token kinds produced by the scanner. Kind tags drive the parser; no RTTI
/// anywhere in the frontend (Rule 68 discipline adopted from the engine).
enum class TokenKind : uint8_t {
    EndOfFile,
    Name,
    Int,
    Float,
    String,
    KeywordAnd,
    KeywordBreak,
    KeywordDo,
    KeywordElse,
    KeywordElseif,
    KeywordEnd,
    KeywordFalse,
    KeywordFor,
    KeywordFunction,
    KeywordGoto,
    KeywordIf,
    KeywordIn,
    KeywordLocal,
    KeywordNil,
    KeywordNot,
    KeywordOr,
    KeywordRepeat,
    KeywordReturn,
    KeywordThen,
    KeywordTrue,
    KeywordUntil,
    KeywordWhile,
    Plus,
    Minus,
    Star,
    Slash,
    SlashSlash,
    Percent,
    Caret,
    Hash,
    Amp,
    Tilde,
    Pipe,
    Shl,
    Shr,
    EqEq,
    NotEq,
    Le,
    Ge,
    Lt,
    Gt,
    Assign,
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Scope,
    Semicolon,
    Colon,
    Comma,
    Dot,
    DotDot,
    DotDotDot,
};

/// One scanned token. String tokens carry the decoded payload; number tokens
/// carry the parsed value; source positions are 1-based lines and columns.
struct Token {
    TokenKind kind = TokenKind::EndOfFile;
    std::string_view text;    // raw source slice; view into the source buffer
    std::string str_value;    // decoded payload for TokenKind::String
    int64_t int_value = 0;    // payload for TokenKind::Int
    double float_value = 0.0; // payload for TokenKind::Float
    uint32_t line = 1;
    uint32_t column = 1;
};

/// Scans the whole chunk up front. The returned views reference `source`, so
/// the source buffer must outlive the token vector.
vortex::support::Result<std::vector<Token>> lex(std::string_view source);

/// Human-readable token kind name for diagnostics.
std::string_view token_kind_name(TokenKind kind) noexcept;

}  // namespace luavortex
