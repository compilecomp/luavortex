// CEP:FILE: include/luavortex/parser.hpp
// CEP:WHAT: Lua 5.4 AST (flat kind-tagged nodes) and the recursive-descent
//           parser producing it from a token stream.
// CEP:WHY: Kind-tagged flat nodes keep the whole frontend free of RTTI and
//           virtual dispatch (Rule 68 discipline) and make the emitter's
//           switch dispatch exhaustive and reviewable.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic with line information for syntax errors;
//          first error wins. No throw.
// CEP:ASSUMES: Token views reference a source buffer that outlives the AST.
// CEP:COST: cold; one linear parse per compile, not performance-critical.
// CEP:EVIDENCE: the full example set parses and compiles in CI.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "luavortex/lexer.hpp"
#include "vortex/support/result.hpp"

namespace luavortex {

enum class ExprKind : uint8_t {
    Nil,
    True,
    False,
    Vararg,
    Int,
    Float,
    Str,
    Function,
    Table,
    Index,
    CallObj,
    CallMethod,
    Name,
    Binary,
    Unary,
    Paren,
};

enum class StmtKind : uint8_t {
    Local,
    Assign,
    Call,
    Do,
    While,
    Repeat,
    If,
    NumFor,
    GenFor,
    FuncDecl,
    LocalFunc,
    Return,
    Break,
    Goto,
    Label,
};

/// Local attribute spellings after a declaration name.
enum class Attrib : uint8_t { None, Const, Close };

struct Block;
struct Expr;

using ExprPtr = std::unique_ptr<Expr>;
using BlockPtr = std::unique_ptr<Block>;

/// One table-constructor field: positional, name-keyed, or expression-keyed.
struct TableField {
    enum class Form : uint8_t { Positional, NameKey, ExprKey };
    Form form = Form::Positional;
    std::string name;  // payload for Form::NameKey
    ExprPtr key;       // payload for Form::ExprKey
    ExprPtr value;
};

/// Flat expression node; unused members stay defaulted for the active
/// kind. Aggregates only: the factory assigns the kind tag, so no
/// user-defined constructor exists for the extractor to trip on.
struct Expr {
    ExprKind kind = ExprKind::Nil;
    uint32_t line = 1;

    int64_t int_value = 0;      // Int
    double float_value = 0.0;   // Float
    std::string str_value;      // Str
    std::string name;           // Name; method name for CallMethod
    std::string op;             // Binary and Unary operator spelling

    ExprPtr lhs;                // Binary lhs, Unary operand, Index object,
                                // CallObj callee
    ExprPtr rhs;                // Binary rhs; ExprKey field key
    std::vector<ExprPtr> args;  // CallObj and CallMethod arguments

    // Function literal payload.
    std::vector<std::string> params;
    bool has_vararg = false;
    BlockPtr body;

    // Table constructor payload.
    std::vector<TableField> fields;
};

/// Flat statement node; unused members stay defaulted for the active
/// kind. Aggregates only: see the note on Expr.
struct Stmt {
    StmtKind kind = StmtKind::Local;
    uint32_t line = 1;

    // Local and GenFor declared names; attribs parallel to names for Local.
    std::vector<std::string> names;
    std::vector<Attrib> attribs;

    // Assignment and call payloads.
    std::vector<ExprPtr> targets;   // Assign (suffixed expressions)
    std::vector<ExprPtr> exprs;     // Assign rhs, Return, Call (callee),
                                    // While/Repeat condition, NumFor
                                    // (start, limit, step), GenFor (iterator
                                    // expression list)

    // Bodies: Do/While/Repeat/NumFor/GenFor use bodies[0]; If stores one body
    // per condition plus else_body.
    std::vector<BlockPtr> bodies;
    BlockPtr else_body;

    // Function declaration payload (FuncDecl and LocalFunc).
    std::vector<std::string> path;  // dotted name path; last segment is the
                                    // method name when is_method is set
    bool is_method = false;

    // Goto and Label payload.
    std::string label;
};

struct Block {
    std::vector<std::unique_ptr<Stmt>> stmts;
};

/// Parsed chunk: the top-level block plus the source name for diagnostics.
struct Chunk {
    std::string name = "chunk";
    BlockPtr body;
};

/// Parses a token stream into a Chunk. Never throws (Rule 65 discipline).
vortex::support::Result<Chunk> parse(const std::vector<Token>& tokens);

}  // namespace luavortex
