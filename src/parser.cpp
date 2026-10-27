// CEP:FILE: src/parser.cpp
// CEP:WHAT: Recursive-descent parser for the full Lua 5.4 grammar with the
//           reference operator priority table, producing the flat AST.
// CEP:WHY: The grammar is small and stable; a direct recursive-descent
//           encoding with an explicit priority table keeps every production
//           auditable against the reference manual.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic naming the unexpected token for any
//          syntax error; first error wins. No throw.
// CEP:ASSUMES: The token stream ends with an EndOfFile token.
// CEP:COST: cold; linear parse with bounded lookahead.
// CEP:EVIDENCE: the example corpus parses through this parser in CI.
#include "luavortex/parser.hpp"

namespace luavortex {

namespace {

// Binary operator priorities per the reference manual table; unary priority
// and right associativity are encoded alongside (Law 7: named, not inlined).
constexpr int kPriorityOr = 1;
constexpr int kPriorityAnd = 2;
constexpr int kPriorityCompare = 3;
constexpr int kPriorityOrBit = 4;
constexpr int kPriorityXorBit = 5;
constexpr int kPriorityAndBit = 6;
constexpr int kPriorityShift = 7;
constexpr int kPriorityConcat = 9;
constexpr int kPriorityAdditive = 10;
constexpr int kPriorityMultiplicative = 11;
constexpr int kPriorityUnary = 12;
constexpr int kPriorityPower = 14;
constexpr int kLeftAssoc = 0;
constexpr int kRightAssoc = 1;

struct BinPriority {
    int left;
    int right;
};

// CEP:WHAT: Returns the left and right binding powers of a binary operator.
// CEP:WHY: The priority table is the normative grammar surface for
//          expressions; encoding it as data keeps associativity exact.
// CEP:STATUS: complete
// CEP:FAILURE: Returns zero priorities for non-binary kinds (callers gate on
//          token kind before consulting the table).
// CEP:ASSUMES: The token kind is a candidate binary operator.
// CEP:COST: cold; bounded comparison chain per operator token.
// CEP:EVIDENCE: expression-heavy examples parse in CI.
BinPriority bin_priority(TokenKind kind) noexcept {
    switch (kind) {
    case TokenKind::KeywordOr: return {kPriorityOr, kPriorityOr};
    case TokenKind::KeywordAnd: return {kPriorityAnd, kPriorityAnd};
    case TokenKind::Lt:
    case TokenKind::Gt:
    case TokenKind::Le:
    case TokenKind::Ge:
    case TokenKind::NotEq:
    case TokenKind::EqEq: return {kPriorityCompare, kPriorityCompare};
    case TokenKind::Pipe: return {kPriorityOrBit, kPriorityOrBit};
    case TokenKind::Tilde: return {kPriorityXorBit, kPriorityXorBit};
    case TokenKind::Amp: return {kPriorityAndBit, kPriorityAndBit};
    case TokenKind::Shl:
    case TokenKind::Shr: return {kPriorityShift, kPriorityShift};
    case TokenKind::DotDot: return {kPriorityConcat, kPriorityConcat + 1};
    case TokenKind::Plus:
    case TokenKind::Minus: return {kPriorityAdditive, kPriorityAdditive};
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::SlashSlash:
    case TokenKind::Percent:
        return {kPriorityMultiplicative, kPriorityMultiplicative};
    case TokenKind::Caret: return {kPriorityPower, kPriorityPower + 1};
    default: return {kLeftAssoc, kLeftAssoc};
    }
}

/// Parser state: token cursor plus convenience accessors.
struct Parser {
    const std::vector<Token>& tokens;
    size_t pos = 0;

    const Token& peek(size_t ahead = 0) const noexcept {
        const size_t at = pos + ahead < tokens.size() ? pos + ahead
                                                      : tokens.size() - 1;
        return tokens[at];
    }

// CEP:WHAT: Consumes the current token and moves the cursor forward.
// CEP:WHY: The cursor stays clamped at EndOfFile so lookahead past the end is safe.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: none.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
// CEP:WHAT: Consumes the current token and moves the cursor forward.
// CEP:WHY: The cursor stays clamped at EndOfFile so lookahead past the end is safe.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: none.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
    const Token& advance() noexcept {
        const Token& t = tokens[pos];
        if (pos + 1 < tokens.size()) ++pos;
        return t;
    }

    bool check(TokenKind kind) const noexcept {
        return peek().kind == kind;
    }

    bool check_name(std::string_view text) const noexcept {
        return peek().kind == TokenKind::Name && peek().text == text;
    }

    vortex::support::Diagnostic error_here(std::string what) const {
        return vortex::support::Diagnostic(
            vortex::support::ErrorCode::SyntaxError, std::move(what),
            peek().line, peek().column);
    }

// CEP:WHAT: Consumes the current token when it matches, or reports a named error.
// CEP:WHY: Every grammar production names the token it requires; one helper keeps the wording uniform.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic naming the expected and found tokens on mismatch.
// CEP:ASSUMES: none.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
// CEP:WHAT: Consumes the current token when it matches, or reports a named error.
// CEP:WHY: Every grammar production names the token it requires; one helper keeps the wording uniform.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic naming the expected and found tokens on mismatch.
// CEP:ASSUMES: none.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
    vortex::support::Result<const Token*> expect(TokenKind kind,
                                                 std::string_view what) {
        if (peek().kind != kind) {
            std::string msg = "expected " + std::string(what) + " near '" +
                              std::string(peek().text) + "'";
            return std::unexpected(error_here(std::move(msg)));
        }
        return &advance();
    }

// CEP:WHAT: Consumes the current identifier token, or reports an error.
// CEP:WHY: Names appear in every declaration and reference production; the wording is shared.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic when the token is not an identifier.
// CEP:ASSUMES: none.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
// CEP:WHAT: Consumes the current identifier token, or reports an error.
// CEP:WHY: Names appear in every declaration and reference production; the wording is shared.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic when the token is not an identifier.
// CEP:ASSUMES: none.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
    vortex::support::Result<const Token*> expect_name() {
        if (peek().kind != TokenKind::Name) {
            return std::unexpected(
                error_here("expected a name near '" +
                           std::string(peek().text) + "'"));
        }
        return &advance();
    }

    bool block_follows() const noexcept {
        switch (peek().kind) {
        case TokenKind::KeywordEnd:
        case TokenKind::KeywordElse:
        case TokenKind::KeywordElseif:
        case TokenKind::KeywordUntil:
        case TokenKind::EndOfFile: return true;
        default: return false;
        }
    }
};

// CEP:WHAT: Allocates an expression node with its kind tag and source line.
// CEP:WHY: Every node carries its kind explicitly (Rule 68 discipline); one factory keeps that invariant.
// CEP:STATUS: complete
// CEP:FAILURE: none (allocation failure propagates through the operator new path).
// CEP:ASSUMES: none.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
// CEP:WHAT: Allocates an expression node with its kind tag and source line.
// CEP:WHY: Every node carries its kind explicitly (Rule 68 discipline); one factory keeps that invariant.
// CEP:STATUS: complete
// CEP:FAILURE: none (allocation failure propagates through the operator new path).
// CEP:ASSUMES: none.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
ExprPtr make_expr(ExprKind kind, uint32_t line) {
    auto node = std::make_unique<Expr>();
    node->kind = kind;
    node->line = line;
    return node;
}

// CEP:WHAT: Allocates a statement node with its kind tag and source line.
// CEP:WHY: Statement dispatch relies on the explicit kind tag; one factory keeps it exact.
// CEP:STATUS: complete
// CEP:FAILURE: none (allocation failure propagates through the operator new path).
// CEP:ASSUMES: none.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
// CEP:WHAT: Allocates a statement node with its kind tag and source line.
// CEP:WHY: Statement dispatch relies on the explicit kind tag; one factory keeps it exact.
// CEP:STATUS: complete
// CEP:FAILURE: none (allocation failure propagates through the operator new path).
// CEP:ASSUMES: none.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
std::unique_ptr<Stmt> make_stmt(StmtKind kind, uint32_t line) {
    auto node = std::make_unique<Stmt>();
    node->kind = kind;
    node->line = line;
    return node;
}

// Forward declarations for the mutually recursive grammar procedures.
vortex::support::Result<BlockPtr> parse_block(Parser& p);
vortex::support::Result<ExprPtr> parse_expression(Parser& p);
vortex::support::Result<ExprPtr> parse_suffixed(Parser& p, ExprPtr primary);
vortex::support::Result<ExprPtr> parse_suffixed_expression(Parser& p);
vortex::support::Result<void> parse_funcbody(Parser& p, Expr& fn,
                                             std::string_view context);

// CEP:WHAT: Parses a primary expression (literals, names, parens, function
//           literals, table constructors).
// CEP:WHY: Primary expressions are the grammar's entry points for every
//          suffixed form; handling them here keeps precedence logic isolated.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for malformed primaries.
// CEP:ASSUMES: The cursor sits on the first token of an expression.
// CEP:COST: cold; bounded per primary.
// CEP:EVIDENCE: the example corpus exercises every primary form.
vortex::support::Result<ExprPtr> parse_primary(Parser& p) {
    const Token& t = p.peek();
    switch (t.kind) {
    case TokenKind::KeywordNil: {
        p.advance();
        return make_expr(ExprKind::Nil, t.line);
    }
    case TokenKind::KeywordTrue: {
        p.advance();
        return make_expr(ExprKind::True, t.line);
    }
    case TokenKind::KeywordFalse: {
        p.advance();
        return make_expr(ExprKind::False, t.line);
    }
    case TokenKind::DotDotDot: {
        p.advance();
        return make_expr(ExprKind::Vararg, t.line);
    }
    case TokenKind::Int: {
        p.advance();
        auto node = make_expr(ExprKind::Int, t.line);
        node->int_value = t.int_value;
        return node;
    }
    case TokenKind::Float: {
        p.advance();
        auto node = make_expr(ExprKind::Float, t.line);
        node->float_value = t.float_value;
        return node;
    }
    case TokenKind::String: {
        p.advance();
        auto node = make_expr(ExprKind::Str, t.line);
        node->str_value = t.str_value;
        return node;
    }
    case TokenKind::Name: {
        p.advance();
        auto node = make_expr(ExprKind::Name, t.line);
        node->name = std::string(t.text);
        return node;
    }
    case TokenKind::LParen: {
        p.advance();
        auto inner = parse_expression(p);
        if (!inner) return inner;
        auto open = p.expect(TokenKind::RParen, "')'");
        if (!open) return std::unexpected(open.error());
        auto wrap = make_expr(ExprKind::Paren, t.line);
        wrap->lhs = std::move(*inner);
        return wrap;
    }
    case TokenKind::KeywordFunction: {
        p.advance();
        auto node = make_expr(ExprKind::Function, t.line);
        auto body = parse_funcbody(p, *node, "function");
        if (!body) return std::unexpected(body.error());
        return node;
    }
    case TokenKind::LBrace: {
        p.advance();
        auto node = make_expr(ExprKind::Table, t.line);
        while (!p.check(TokenKind::RBrace)) {
            TableField field;
            if (p.check(TokenKind::LBracket)) {
                p.advance();
                auto key = parse_expression(p);
                if (!key) return key;
                auto close = p.expect(TokenKind::RBracket, "']'");
                if (!close) return std::unexpected(close.error());
                auto assign = p.expect(TokenKind::Assign, "'='");
                if (!assign) return std::unexpected(assign.error());
                field.form = TableField::Form::ExprKey;
                field.key = std::move(*key);
                auto value = parse_expression(p);
                if (!value) return value;
                field.value = std::move(*value);
            } else if (p.check(TokenKind::Name) &&
                       p.peek(1).kind == TokenKind::Assign) {
                field.form = TableField::Form::NameKey;
                field.name = std::string(p.advance().text);
                p.advance();  // '='
                auto value = parse_expression(p);
                if (!value) return value;
                field.value = std::move(*value);
            } else {
                field.form = TableField::Form::Positional;
                auto value = parse_expression(p);
                if (!value) return value;
                field.value = std::move(*value);
            }
            node->fields.push_back(std::move(field));
            if (p.check(TokenKind::Comma) || p.check(TokenKind::Semicolon)) {
                p.advance();
                continue;
            }
            break;
        }
        auto close = p.expect(TokenKind::RBrace, "'}'");
        if (!close) return std::unexpected(close.error());
        return node;
    }
    default: break;
    }
    return std::unexpected(
        p.error_here("unexpected symbol near '" + std::string(t.text) + "'"));
}

// CEP:WHAT: Parses the argument list of a call (parens, table literal, or
//           string literal sugar).
// CEP:WHY: Call syntax has three argument spellings; centralizing them keeps
//          both call statement kinds consistent.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for malformed argument lists.
// CEP:ASSUMES: The cursor sits on the first argument token.
// CEP:COST: cold; bounded per call.
// CEP:EVIDENCE: call forms across the example corpus parse in CI.
vortex::support::Result<std::vector<ExprPtr>> parse_call_args(Parser& p) {
    std::vector<ExprPtr> args;
    if (p.check(TokenKind::String)) {
        const Token& t = p.advance();
        auto node = make_expr(ExprKind::Str, t.line);
        node->str_value = t.str_value;
        args.push_back(std::move(node));
        return args;
    }
    auto open = p.expect(TokenKind::LParen, "'('");
    if (!open) return std::unexpected(open.error());
    if (!p.check(TokenKind::RParen)) {
        while (true) {
            auto arg = parse_expression(p);
            if (!arg) return std::unexpected(arg.error());
            args.push_back(std::move(*arg));
            if (p.check(TokenKind::Comma)) {
                p.advance();
                continue;
            }
            break;
        }
    }
    auto close = p.expect(TokenKind::RParen, "')'");
    if (!close) return std::unexpected(close.error());
    return args;
}

// CEP:WHAT: Parses suffixes of a primary expression: indexing, field access,
//           calls, and method calls, left-associatively.
// CEP:WHY: Suffix chains produce the Index, CallObj, and CallMethod shapes
//          that drive both runtime dispatch and assignment lowering.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for malformed chains.
// CEP:ASSUMES: `primary` is a complete expression prefix.
// CEP:COST: cold; linear in the chain length.
// CEP:EVIDENCE: chains across the example corpus parse in CI.
vortex::support::Result<ExprPtr> parse_suffixed(Parser& p, ExprPtr primary) {
    while (true) {
        if (p.check(TokenKind::Dot)) {
            const Token& dot = p.advance();
            auto name = p.expect_name();
            if (!name) return std::unexpected(name.error());
            auto index = make_expr(ExprKind::Index, dot.line);
            index->lhs = std::move(primary);
            auto key = make_expr(ExprKind::Str, dot.line);
            key->str_value = std::string((*name)->text);
            index->rhs = std::move(key);
            primary = std::move(index);
            continue;
        }
        if (p.check(TokenKind::LBracket)) {
            const Token& open = p.advance();
            auto key = parse_expression(p);
            if (!key) return key;
            auto close = p.expect(TokenKind::RBracket, "']'");
            if (!close) return std::unexpected(close.error());
            auto index = make_expr(ExprKind::Index, open.line);
            index->lhs = std::move(primary);
            index->rhs = std::move(*key);
            primary = std::move(index);
            continue;
        }
        if (p.check(TokenKind::Colon)) {
            const Token& colon = p.advance();
            auto name = p.expect_name();
            if (!name) return std::unexpected(name.error());
            auto call = make_expr(ExprKind::CallMethod, colon.line);
            call->lhs = std::move(primary);
            call->name = std::string((*name)->text);
            auto args = parse_call_args(p);
            if (!args) return std::unexpected(args.error());
            call->args = std::move(*args);
            primary = std::move(call);
            continue;
        }
        if (p.check(TokenKind::LParen) || p.check(TokenKind::String) ||
            p.check(TokenKind::LBrace)) {
            const Token& at = p.peek();
            auto call = make_expr(ExprKind::CallObj, at.line);
            call->lhs = std::move(primary);
            auto args = parse_call_args(p);
            if (!args) return std::unexpected(args.error());
            call->args = std::move(*args);
            primary = std::move(call);
            continue;
        }
        break;
    }
    return primary;
}

// CEP:WHAT: Parses a unary expression chain with right binding.
// CEP:WHY: The unary operators bind at a fixed priority above the additive
//          family and below the power operator; a dedicated procedure
//          encodes that exactly.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for malformed unary forms.
// CEP:ASSUMES: The cursor sits on the first token of an expression.
// CEP:COST: cold; bounded per operand.
// CEP:EVIDENCE: unary examples parse in CI.
vortex::support::Result<ExprPtr> parse_unary(Parser& p, int limit) {
    const Token& t = p.peek();
    if (t.kind == TokenKind::KeywordNot || t.kind == TokenKind::Minus ||
        t.kind == TokenKind::Hash || t.kind == TokenKind::Tilde) {
        if (kPriorityUnary <= limit) {
            return std::unexpected(
                p.error_here("unexpected symbol near '" +
                             std::string(t.text) + "'"));
        }
        p.advance();
        auto operand = parse_unary(p, kPriorityUnary);
        if (!operand) return operand;
        auto node = make_expr(ExprKind::Unary, t.line);
        node->op = t.kind == TokenKind::KeywordNot
                       ? "not"
                       : (t.kind == TokenKind::Minus
                              ? "-"
                              : (t.kind == TokenKind::Hash ? "#" : "~"));
        node->lhs = std::move(*operand);
        return node;
    }
    auto primary = parse_primary(p);
    if (!primary) return primary;
    return parse_suffixed(p, std::move(*primary));
}

// CEP:WHAT: Parses a binary expression chain with the priority table.
// CEP:WHY: Precedence climbing over the named priority table reproduces the
//          reference associativity rules in a few lines.
// CEP:STATUS: complete
// CEP:FAILURE: Returns diagnostics from the operand procedures.
// CEP:ASSUMES: The cursor sits on the first token of an expression.
// CEP:COST: cold; linear in expression size.
// CEP:EVIDENCE: expression-heavy examples parse in CI.
vortex::support::Result<ExprPtr> parse_binary(Parser& p, int limit) {
    auto lhs = parse_unary(p, limit);
    if (!lhs) return lhs;
    while (true) {
        const Token& op = p.peek();
        const BinPriority prio = bin_priority(op.kind);
        if (prio.left == kLeftAssoc || prio.left <= limit) break;
        p.advance();
        auto rhs = parse_binary(p, prio.right);
        if (!rhs) return rhs;
        auto node = make_expr(ExprKind::Binary, op.line);
        node->op = std::string(op.text);
        node->lhs = std::move(*lhs);
        node->rhs = std::move(*rhs);
        lhs = std::move(node);
    }
    return lhs;
}

vortex::support::Result<ExprPtr> parse_expression(Parser& p) {
    return parse_binary(p, kLeftAssoc);
}

// CEP:WHAT: Parses a function body: parameter list plus block.
// CEP:WHY: Bodies appear from four syntax forms; one procedure keeps the
//          parameter and vararg rules uniform.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for malformed parameter lists.
// CEP:ASSUMES: The cursor sits on '('.
// CEP:COST: cold; bounded per body.
// CEP:EVIDENCE: function forms across the corpus parse in CI.
vortex::support::Result<void> parse_funcbody(Parser& p, Expr& fn,
                                             std::string_view context) {
    (void)context;
    auto open = p.expect(TokenKind::LParen, "'('");
    if (!open) return std::unexpected(open.error());
    if (!p.check(TokenKind::RParen)) {
        while (true) {
            if (p.check(TokenKind::DotDotDot)) {
                p.advance();
                fn.has_vararg = true;
                break;
            }
            auto name = p.expect_name();
            if (!name) return std::unexpected(name.error());
            fn.params.push_back(std::string((*name)->text));
            if (p.check(TokenKind::Comma)) {
                p.advance();
                continue;
            }
            break;
        }
    }
    auto close = p.expect(TokenKind::RParen, "')'");
    if (!close) return std::unexpected(close.error());
    auto body = parse_block(p);
    if (!body) return std::unexpected(body.error());
    fn.body = std::move(*body);
    auto end = p.expect(TokenKind::KeywordEnd, "'end'");
    if (!end) return std::unexpected(end.error());
    return vortex::support::ok();
}

// CEP:WHAT: Parses a function name path for declaration statements.
// CEP:WHY: Declaration targets carry dotted paths with an optional method
//          tail; the path encoding drives the emitter's assignment lowering.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for malformed name paths.
// CEP:ASSUMES: The cursor sits on the leading name.
// CEP:COST: cold; bounded per path.
// CEP:EVIDENCE: declaration forms across the corpus parse in CI.
vortex::support::Result<void> parse_funcname(Parser& p, Stmt& decl) {
    auto first = p.expect_name();
    if (!first) return std::unexpected(first.error());
    decl.path.push_back(std::string((*first)->text));
    while (p.check(TokenKind::Dot)) {
        p.advance();
        auto segment = p.expect_name();
        if (!segment) return std::unexpected(segment.error());
        decl.path.push_back(std::string((*segment)->text));
    }
    if (p.check(TokenKind::Colon)) {
        p.advance();
        auto method = p.expect_name();
        if (!method) return std::unexpected(method.error());
        decl.path.push_back(std::string((*method)->text));
        decl.is_method = true;
    }
    return vortex::support::ok();
}

// CEP:WHAT: Parses one statement.
// CEP:WHY: The statement grammar is the middle layer of the language; one
//          switch per statement kind keeps each production reviewable.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for malformed statements.
// CEP:ASSUMES: The cursor sits on the first token of a statement.
// CEP:COST: cold; bounded per statement.
// CEP:EVIDENCE: the example corpus covers every statement kind.
vortex::support::Result<std::unique_ptr<Stmt>> parse_statement(Parser& p) {
    const Token& t = p.peek();
    switch (t.kind) {
    case TokenKind::Semicolon: {
        p.advance();
        return parse_statement(p);
    }
    case TokenKind::Scope: {
        p.advance();
        auto name = p.expect_name();
        if (!name) return std::unexpected(name.error());
        auto close = p.expect(TokenKind::Scope, "'::'");
        if (!close) return std::unexpected(close.error());
        auto node = make_stmt(StmtKind::Label, t.line);
        node->label = std::string((*name)->text);
        return node;
    }
    case TokenKind::KeywordBreak: {
        p.advance();
        return make_stmt(StmtKind::Break, t.line);
    }
    case TokenKind::KeywordGoto: {
        p.advance();
        auto name = p.expect_name();
        if (!name) return std::unexpected(name.error());
        auto node = make_stmt(StmtKind::Goto, t.line);
        node->label = std::string((*name)->text);
        return node;
    }
    case TokenKind::KeywordDo: {
        p.advance();
        auto node = make_stmt(StmtKind::Do, t.line);
        auto body = parse_block(p);
        if (!body) return std::unexpected(body.error());
        node->bodies.push_back(std::move(*body));
        auto end = p.expect(TokenKind::KeywordEnd, "'end'");
        if (!end) return std::unexpected(end.error());
        return node;
    }
    case TokenKind::KeywordWhile: {
        p.advance();
        auto node = make_stmt(StmtKind::While, t.line);
        auto cond = parse_expression(p);
        if (!cond) return std::unexpected(cond.error());
        node->exprs.push_back(std::move(*cond));
        auto do_kw = p.expect(TokenKind::KeywordDo, "'do'");
        if (!do_kw) return std::unexpected(do_kw.error());
        auto body = parse_block(p);
        if (!body) return std::unexpected(body.error());
        node->bodies.push_back(std::move(*body));
        auto end = p.expect(TokenKind::KeywordEnd, "'end'");
        if (!end) return std::unexpected(end.error());
        return node;
    }
    case TokenKind::KeywordRepeat: {
        p.advance();
        auto node = make_stmt(StmtKind::Repeat, t.line);
        auto body = parse_block(p);
        if (!body) return std::unexpected(body.error());
        node->bodies.push_back(std::move(*body));
        auto until_kw = p.expect(TokenKind::KeywordUntil, "'until'");
        if (!until_kw) return std::unexpected(until_kw.error());
        auto cond = parse_expression(p);
        if (!cond) return std::unexpected(cond.error());
        node->exprs.push_back(std::move(*cond));
        return node;
    }
    case TokenKind::KeywordIf: {
        p.advance();
        auto node = make_stmt(StmtKind::If, t.line);
        while (true) {
            auto cond = parse_expression(p);
            if (!cond) return std::unexpected(cond.error());
            node->exprs.push_back(std::move(*cond));
            auto then_kw = p.expect(TokenKind::KeywordThen, "'then'");
            if (!then_kw) return std::unexpected(then_kw.error());
            auto body = parse_block(p);
            if (!body) return std::unexpected(body.error());
            node->bodies.push_back(std::move(*body));
            if (p.check(TokenKind::KeywordElseif)) {
                p.advance();
                continue;
            }
            break;
        }
        if (p.check(TokenKind::KeywordElse)) {
            p.advance();
            auto body = parse_block(p);
            if (!body) return std::unexpected(body.error());
            node->else_body = std::move(*body);
        }
        auto end = p.expect(TokenKind::KeywordEnd, "'end'");
        if (!end) return std::unexpected(end.error());
        return node;
    }
    case TokenKind::KeywordFor: {
        p.advance();
        auto first = p.expect_name();
        if (!first) return std::unexpected(first.error());
        if (p.check(TokenKind::Assign)) {
            p.advance();
            auto node = make_stmt(StmtKind::NumFor, t.line);
            node->names.push_back(std::string((*first)->text));
            auto start = parse_expression(p);
            if (!start) return std::unexpected(start.error());
            node->exprs.push_back(std::move(*start));
            auto comma = p.expect(TokenKind::Comma, "','");
            if (!comma) return std::unexpected(comma.error());
            auto limit = parse_expression(p);
            if (!limit) return std::unexpected(limit.error());
            node->exprs.push_back(std::move(*limit));
            if (p.check(TokenKind::Comma)) {
                p.advance();
                auto step = parse_expression(p);
                if (!step) return std::unexpected(step.error());
                node->exprs.push_back(std::move(*step));
            }
            auto do_kw = p.expect(TokenKind::KeywordDo, "'do'");
            if (!do_kw) return std::unexpected(do_kw.error());
            auto body = parse_block(p);
            if (!body) return std::unexpected(body.error());
            node->bodies.push_back(std::move(*body));
            auto end = p.expect(TokenKind::KeywordEnd, "'end'");
            if (!end) return std::unexpected(end.error());
            return node;
        }
        auto node = make_stmt(StmtKind::GenFor, t.line);
        node->names.push_back(std::string((*first)->text));
        while (p.check(TokenKind::Comma)) {
            p.advance();
            auto name = p.expect_name();
            if (!name) return std::unexpected(name.error());
            node->names.push_back(std::string((*name)->text));
        }
        auto in_kw = p.expect(TokenKind::KeywordIn, "'in'");
        if (!in_kw) return std::unexpected(in_kw.error());
        while (true) {
            auto e = parse_expression(p);
            if (!e) return std::unexpected(e.error());
            node->exprs.push_back(std::move(*e));
            if (p.check(TokenKind::Comma)) {
                p.advance();
                continue;
            }
            break;
        }
        auto do_kw = p.expect(TokenKind::KeywordDo, "'do'");
        if (!do_kw) return std::unexpected(do_kw.error());
        auto body = parse_block(p);
        if (!body) return std::unexpected(body.error());
        node->bodies.push_back(std::move(*body));
        auto end = p.expect(TokenKind::KeywordEnd, "'end'");
        if (!end) return std::unexpected(end.error());
        return node;
    }
    case TokenKind::KeywordFunction: {
        p.advance();
        auto node = make_stmt(StmtKind::FuncDecl, t.line);
        auto name = parse_funcname(p, *node);
        if (!name) return std::unexpected(name.error());
        auto fn = make_expr(ExprKind::Function, t.line);
        auto body = parse_funcbody(p, *fn, "function declaration");
        if (!body) return std::unexpected(body.error());
        // The declaration payload rides in exprs[0] as a function literal.
        node->exprs.push_back(std::move(fn));
        return node;
    }
    case TokenKind::KeywordLocal: {
        p.advance();
        if (p.check(TokenKind::KeywordFunction)) {
            p.advance();
            auto node = make_stmt(StmtKind::LocalFunc, t.line);
            auto name = p.expect_name();
            if (!name) return std::unexpected(name.error());
            node->names.push_back(std::string((*name)->text));
            auto fn = make_expr(ExprKind::Function, t.line);
            auto body = parse_funcbody(p, *fn, "local function");
            if (!body) return std::unexpected(body.error());
            node->exprs.push_back(std::move(fn));
            return node;
        }
        auto node = make_stmt(StmtKind::Local, t.line);
        while (true) {
            auto name = p.expect_name();
            if (!name) return std::unexpected(name.error());
            node->names.push_back(std::string((*name)->text));
            Attrib attrib = Attrib::None;
            if (p.check(TokenKind::Lt)) {
                p.advance();
                auto attr = p.expect_name();
                if (!attr) return std::unexpected(attr.error());
                const std::string attr_text((*attr)->text);
                if (attr_text == "const") {
                    attrib = Attrib::Const;
                } else if (attr_text == "close") {
                    attrib = Attrib::Close;
                } else {
                    return std::unexpected(p.error_here(
                        "unknown attribute '<" + attr_text + ">'"));
                }
                auto close = p.expect(TokenKind::Gt, "'>'");
                if (!close) return std::unexpected(close.error());
            }
            node->attribs.push_back(attrib);
            if (p.check(TokenKind::Comma)) {
                p.advance();
                continue;
            }
            break;
        }
        if (p.check(TokenKind::Assign)) {
            p.advance();
            while (true) {
                auto e = parse_expression(p);
                if (!e) return std::unexpected(e.error());
                node->exprs.push_back(std::move(*e));
                if (p.check(TokenKind::Comma)) {
                    p.advance();
                    continue;
                }
                break;
            }
        }
        return node;
    }
    case TokenKind::KeywordReturn: {
        p.advance();
        auto node = make_stmt(StmtKind::Return, t.line);
        if (!p.block_follows() && !p.check(TokenKind::Semicolon)) {
            while (true) {
                auto e = parse_expression(p);
                if (!e) return std::unexpected(e.error());
                node->exprs.push_back(std::move(*e));
                if (p.check(TokenKind::Comma)) {
                    p.advance();
                    continue;
                }
                break;
            }
        }
        if (p.check(TokenKind::Semicolon)) p.advance();
        return node;
    }
    default: break;
    }
    // Expression statement: assignment or call. The target list parses
    // first because a comma is valid in both grammars.
    auto prefix = parse_suffixed_expression(p);
    if (!prefix) return std::unexpected(prefix.error());
    std::vector<ExprPtr> prefixes;
    prefixes.push_back(std::move(*prefix));
    while (p.check(TokenKind::Comma)) {
        p.advance();
        auto next_target = parse_suffixed_expression(p);
        if (!next_target) return std::unexpected(next_target.error());
        prefixes.push_back(std::move(*next_target));
    }
    if (p.check(TokenKind::Assign)) {
        p.advance();
        auto node = make_stmt(StmtKind::Assign, t.line);
        node->targets = std::move(prefixes);
        while (true) {
            auto e = parse_expression(p);
            if (!e) return std::unexpected(e.error());
            node->exprs.push_back(std::move(*e));
            if (p.check(TokenKind::Comma)) {
                p.advance();
                continue;
            }
            break;
        }
        return node;
    }
    if (prefixes.size() != 1) {
        return std::unexpected(p.error_here(
            "syntax error near '" + std::string(p.peek().text) + "'"));
    }
    const ExprKind prefix_kind = prefixes[0]->kind;
    if (prefix_kind != ExprKind::CallObj &&
        prefix_kind != ExprKind::CallMethod) {
        return std::unexpected(p.error_here(
            "syntax error near '" + std::string(p.peek().text) + "'"));
    }
    auto node = make_stmt(StmtKind::Call, t.line);
    node->exprs.push_back(std::move(prefixes[0]));
    return node;
}

// CEP:WHAT: Parses a statement-leading suffixed expression.
// CEP:WHY: Assignment targets and call statements share the prefix grammar;
//          a dedicated entry point avoids duplicating the suffix loop.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for malformed prefixes.
// CEP:ASSUMES: The cursor sits on the first token of the expression.
// CEP:COST: cold; bounded per prefix.
// CEP:EVIDENCE: assignment and call examples parse in CI.
vortex::support::Result<ExprPtr> parse_suffixed_expression(Parser& p) {
    auto primary = parse_primary(p);
    if (!primary) return primary;
    return parse_suffixed(p, std::move(*primary));
}

// CEP:WHAT: Parses a block up to a block-following keyword.
// CEP:WHY: Blocks are the scope units of the language; the emitter derives
//          register scoping directly from their nesting.
// CEP:STATUS: complete
// CEP:FAILURE: Returns diagnostics from the statement procedure.
// CEP:ASSUMES: The cursor sits at a statement or block terminator.
// CEP:COST: cold; linear in the block size.
// CEP:EVIDENCE: every test case is a block parse.
vortex::support::Result<BlockPtr> parse_block(Parser& p) {
    auto block = std::make_unique<Block>();
    while (!p.block_follows()) {
        auto stmt = parse_statement(p);
        if (!stmt) return std::unexpected(stmt.error());
        const bool is_return = (*stmt)->kind == StmtKind::Return;
        block->stmts.push_back(std::move(*stmt));
        if (is_return) break;
    }
    return block;
}

}  // namespace

// CEP:WHAT: Parses a full token stream into a Chunk.
// CEP:WHY: This is the parser entry point used by the compiler driver.
// CEP:STATUS: complete
// CEP:FAILURE: Returns the first syntax diagnostic.
// CEP:ASSUMES: The stream ends with EndOfFile.
// CEP:COST: cold; linear in the chunk size.
// CEP:EVIDENCE: the example corpus compiles through this entry point.
vortex::support::Result<Chunk> parse(const std::vector<Token>& tokens) {
    Parser p{tokens, 0};
    Chunk chunk;
    auto body = parse_block(p);
    if (!body) return std::unexpected(body.error());
    chunk.body = std::move(*body);
    if (!p.check(TokenKind::EndOfFile)) {
        return std::unexpected(p.error_here(
            "unexpected symbol near '" + std::string(p.peek().text) + "'"));
    }
    return chunk;
}

}  // namespace luavortex
