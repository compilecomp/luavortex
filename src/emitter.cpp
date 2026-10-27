// CEP:FILE: src/emitter.cpp
// CEP:WHAT: The analysis half of the lowering pass: whole-program scope
//           resolution, lambda lifting into function units, capture and
//           escape classification, and the compile_lua driver.
// CEP:WHY: Lambda lifting needs complete capture and escape facts before any
//          code can be laid out; a dedicated pass produces them once so the
//          emitter works from a closed, reviewable plan.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic on internal resolution inconsistencies;
//          language-level diagnostics belong to the parser and emitter. No
//          throw.
// CEP:ASSUMES: The AST outlives the analysis; unit ids grow monotonically so
//          reverse-order absorption visits children before parents.
// CEP:COST: cold; one linear AST walk plus one absorption pass.
// CEP:EVIDENCE: the example corpus compiles and executes end to end in CI.
#include "emitter_internal.hpp"

#include <algorithm>

namespace luavortex {

namespace {

namespace support = vortex::support;
namespace ei = emit_internal;

constexpr uint32_t kNoLocal = emit_internal::kNoLocal;
constexpr uint32_t kLoopHiddenSlots = emit_internal::kLoopHiddenSlots;
// kNoUnit resolves through luavortex::kNoUnit.

/// One binding visible in a lexical scope during analysis.
struct AnalysisBinding {
    uint32_t owner_unit = kNoUnit;
    uint32_t local_index = kNoLocal;
    uint32_t fn_unit = kNoUnit;
    bool reassigned = false;
    bool is_const = false;
};

struct AnalysisScope {
    uint32_t unit_id = kNoUnit;
    std::map<std::string, AnalysisBinding> names;
};

/// The analysis walk: builds the unit tree, capture lists, and escape flags.
struct Analyzer {
    const Chunk& chunk;
    ProgramUnits out;
    std::vector<AnalysisScope> scopes;
    uint32_t current_unit = kNoUnit;
    uint32_t unit_counter = 0;
    size_t max_call_window_ = luavortex::kMinUniformArity;

    FnUnit& unit(uint32_t id) { return out.units[id]; }

// CEP:WHAT: Creates one lifted function unit with its method name and parameter facts.
// CEP:WHY: Lambda lifting needs one unit per function literal; ids grow monotonically so absorption visits children first.
// CEP:STATUS: complete
// CEP:FAILURE: none (allocation failure propagates through the vector growth path).
// CEP:ASSUMES: The literal pointer outlives the analysis.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
// CEP:WHAT: Creates one lifted function unit with its method name and parameter facts.
// CEP:WHY: Lambda lifting needs one unit per function literal; ids grow monotonically so absorption visits children first.
// CEP:STATUS: complete
// CEP:FAILURE: none (allocation failure propagates through the vector growth path).
// CEP:ASSUMES: The literal pointer outlives the analysis.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
    uint32_t fresh_unit(uint32_t parent, const Expr* fn,
                        const std::string& hint, bool escaping) {
        FnUnit u;
        u.id = unit_counter++;
        u.parent = parent;
        u.fn = fn;
        u.escaping = escaping;
        u.method_name = emit_internal::kFnClassPrefix + std::to_string(u.id) +
                        "." + hint;
        if (fn != nullptr) {
            u.param_count = static_cast<uint32_t>(fn->params.size());
            if (fn->has_vararg) u.has_vararg = true;
            out.fn_units[fn] = u.id;
        }
        out.units.push_back(std::move(u));
        return u.id;
    }

// CEP:WHAT: Opens one lexical scope bound to a unit id.
// CEP:WHY: Scope frames carry the owner unit so capture resolution can distinguish own slots from captures.
// CEP:STATUS: complete
// CEP:FAILURE: none (allocation failure propagates through the vector growth path).
// CEP:ASSUMES: Popped in the same procedure that pushed it.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
// CEP:WHAT: Opens one lexical scope bound to a unit id.
// CEP:WHY: Scope frames carry the owner unit so capture resolution can distinguish own slots from captures.
// CEP:STATUS: complete
// CEP:FAILURE: none (allocation failure propagates through the vector growth path).
// CEP:ASSUMES: Popped in the same procedure that pushed it.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
    void push_scope(uint32_t unit_id) {
        AnalysisScope s;
        s.unit_id = unit_id;
        scopes.push_back(std::move(s));
    }

    void pop_scope() { scopes.pop_back(); }

// CEP:WHAT: Finds a binding by name across the scope stack, innermost first.
// CEP:WHY: Lexical shadowing is innermost-wins; one search keeps that rule in a single place.
// CEP:STATUS: complete
// CEP:FAILURE: Returns nullptr for names without a binding (globals and modules).
// CEP:ASSUMES: none.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
// CEP:WHAT: Finds a binding by name across the scope stack, innermost first.
// CEP:WHY: Lexical shadowing is innermost-wins; one search keeps that rule in a single place.
// CEP:STATUS: complete
// CEP:FAILURE: Returns nullptr for names without a binding (globals and modules).
// CEP:ASSUMES: none.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
    AnalysisBinding* resolve(const std::string& name) {
        for (size_t i = scopes.size(); i-- > 0;) {
            auto it = scopes[i].names.find(name);
            if (it != scopes[i].names.end()) return &it->second;
        }
        return nullptr;
    }

// CEP:WHAT: Records one capture of an ancestor local into the current unit.
// CEP:WHY: Lifted functions read captured cells; the capture list drives both conventions.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: The capture belongs to an ancestor of the current unit.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
// CEP:WHAT: Records one capture of an ancestor local into the current unit.
// CEP:WHY: Lifted functions read captured cells; the capture list drives both conventions.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: The capture belongs to an ancestor of the current unit.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
    void record_capture(uint32_t owner_unit, uint32_t local_index) {
        FnUnit& u = unit(current_unit);
        for (const auto& c : u.captures) {
            if (c.owner_unit == owner_unit && c.local_index == local_index) {
                return;
            }
        }
        FnUnit::Capture cap;
        cap.owner_unit = owner_unit;
        cap.local_index = local_index;
        u.captures.push_back(cap);
    }

    void mark_escaping(uint32_t fn_unit_id) {
        if (fn_unit_id != kNoUnit) unit(fn_unit_id).escaping = true;
    }

    /// Resolves `name`, recording captures and escape facts.
// CEP:WHAT: Resolves a name reference, recording captures and escape facts.
// CEP:WHY: Escape classification decides the calling convention before emission; direct-callee detection lives exactly here.
// CEP:STATUS: complete
// CEP:FAILURE: none (unresolved names stay unresolved for the emission pass).
// CEP:ASSUMES: The scope stack reflects the current walk position.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
// CEP:WHAT: Resolves a name reference, recording captures and escape facts.
// CEP:WHY: Escape classification decides the calling convention before emission; direct-callee detection lives exactly here.
// CEP:STATUS: complete
// CEP:FAILURE: none (unresolved names stay unresolved for the emission pass).
// CEP:ASSUMES: The scope stack reflects the current walk position.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
    AnalysisBinding* use_name(const std::string& name, bool direct_callee) {
        AnalysisBinding* b = resolve(name);
        if (b == nullptr) return nullptr;
        if (b->owner_unit != current_unit) {
            record_capture(b->owner_unit, b->local_index);
        }
        const bool direct_ok = direct_callee && b->owner_unit == current_unit &&
                               !b->reassigned;
        if (b->fn_unit != kNoUnit && !direct_ok) mark_escaping(b->fn_unit);
        return b;
    }

// CEP:WHAT: Declares one local of the current unit with its binding facts.
// CEP:WHY: Declaration order fixes the slot indices shared between analysis and emission.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: The current scope frame belongs to the current unit.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
// CEP:WHAT: Declares one local of the current unit with its binding facts.
// CEP:WHY: Declaration order fixes the slot indices shared between analysis and emission.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: The current scope frame belongs to the current unit.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
    uint32_t declare_local(const std::string& name, uint32_t fn_unit_id,
                           bool is_const) {
        FnUnit& u = unit(current_unit);
        FnUnit::LocalInfo info;
        info.name = name;
        info.boxed = false;
        u.locals.push_back(info);
        const uint32_t idx = static_cast<uint32_t>(u.locals.size() - 1);
        AnalysisBinding b;
        b.owner_unit = current_unit;
        b.local_index = idx;
        b.fn_unit = fn_unit_id;
        b.is_const = is_const;
        scopes.back().names[name] = b;
        return idx;
    }

    // CEP:WHAT: Walks one statement, maintaining scopes and unit facts.
    // CEP:WHY: Every scoping rule of the language lives here, so capture and
    //          escape classification stay consistent with the grammar.
    // CEP:STATUS: complete
    // CEP:FAILURE: Returns a Diagnostic for declarations the analysis cannot
    //          bind (duplicate labels are checked at emission).
    // CEP:ASSUMES: The statement tree is well-formed (parser-guaranteed).
    // CEP:COST: cold; linear in the statement subtree size.
    // CEP:EVIDENCE: the example corpus analyzes and compiles in CI.
    vortex::support::Result<void> walk_stmt(const Stmt& s) {
        switch (s.kind) {
        case StmtKind::Local: {
            for (const auto& e : s.exprs) {
                auto res = walk_expr(*e, false);
                if (!res) return res;
            }
            const size_t init_count = s.exprs.size();
            for (size_t i = 0; i < s.names.size(); ++i) {
                if (s.attribs[i] == Attrib::Close) {
                    return support::fail(support::ErrorCode::SyntaxError,
                                "to-be-closed variables need engine "
                                "close-hook support",
                                s.line);
                }
                uint32_t fn_id = kNoUnit;
                if (i < init_count &&
                    s.exprs[i]->kind == ExprKind::Function) {
                    fn_id = fresh_unit(current_unit, s.exprs[i].get(),
                                       s.names[i], false);
                }
                declare_local(s.names[i], fn_id, s.attribs[i] == Attrib::Const);
            }
            return vortex::support::ok();
        }
        case StmtKind::LocalFunc: {
            const Expr& fn = *s.exprs[0];
            const uint32_t fn_id =
                fresh_unit(current_unit, &fn, s.names[0], false);
            declare_local(s.names[0], fn_id, false);
            push_scope(fn_id);
            declare_into_unit_locals(fn_id, fn, false);
            auto res = walk_block_in_unit(fn_id, *fn.body);
            pop_scope();
            return res;
        }
        case StmtKind::FuncDecl: {
            const Expr& fn = *s.exprs[0];
            uint32_t param_count = static_cast<uint32_t>(fn.params.size());
            if (s.is_method) ++param_count;  // implicit self
            const uint32_t fn_id =
                fresh_unit(current_unit, &fn, s.path.back(), true);
            unit(fn_id).param_count = param_count;
            push_scope(fn_id);
            declare_into_unit_locals(fn_id, fn, s.is_method);
            auto res = walk_block_in_unit(fn_id, *fn.body);
            pop_scope();
            return res;
        }
        case StmtKind::Assign: {
            for (const auto& e : s.exprs) {
                auto res = walk_expr(*e, false);
                if (!res) return res;
            }
            for (const auto& t : s.targets) {
                if (t->kind == ExprKind::Name) {
                    AnalysisBinding* b = resolve(t->name);
                    if (b != nullptr && b->is_const) {
                        return support::fail(support::ErrorCode::SyntaxError,
                                    "assignment to const variable '" +
                                        t->name + "'",
                                    s.line);
                    }
                    if (b != nullptr && b->owner_unit == current_unit) {
                        b->reassigned = true;
                    }
                    if (b != nullptr && b->fn_unit != kNoUnit) {
                        // A reassigned function binding always takes the
                        // value convention.
                        mark_escaping(b->fn_unit);
                    }
                } else {
                    auto res = walk_expr(*t, false);
                    if (!res) return res;
                }
            }
            return vortex::support::ok();
        }
        case StmtKind::Call: {
            return walk_expr(*s.exprs[0], false);
        }
        case StmtKind::Do: {
            push_scope(current_unit);
// CEP:WHAT: Walks one block of statements under the current scope.
// CEP:WHY: Blocks are the scope units; the walk maintains their frame lifetimes.
// CEP:STATUS: complete
// CEP:FAILURE: Propagates statement diagnostics.
// CEP:ASSUMES: The caller pushed the enclosing scope.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
// CEP:WHAT: Walks one block of statements under the current scope.
// CEP:WHY: Blocks are the scope units; the walk maintains their frame lifetimes.
// CEP:STATUS: complete
// CEP:FAILURE: Propagates statement diagnostics.
// CEP:ASSUMES: The caller pushed the enclosing scope.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
            auto res = walk_block(*s.bodies[0]);
            pop_scope();
            return res;
        }
        case StmtKind::While: {
            auto cond = walk_expr(*s.exprs[0], false);
            if (!cond) return cond;
            push_scope(current_unit);
            auto body = walk_block(*s.bodies[0]);
            pop_scope();
            return body;
        }
        case StmtKind::Repeat: {
            push_scope(current_unit);
            auto body = walk_block(*s.bodies[0]);
            if (!body) {
                pop_scope();
                return body;
            }
            auto cond = walk_expr(*s.exprs[0], false);
            pop_scope();
            return cond;
        }
        case StmtKind::If: {
            for (size_t i = 0; i < s.exprs.size(); ++i) {
                auto cond = walk_expr(*s.exprs[i], false);
                if (!cond) return cond;
                push_scope(current_unit);
                auto body = walk_block(*s.bodies[i]);
                pop_scope();
                if (!body) return body;
            }
            if (s.else_body != nullptr) {
                push_scope(current_unit);
                auto body = walk_block(*s.else_body);
                pop_scope();
                if (!body) return body;
            }
            return vortex::support::ok();
        }
        case StmtKind::NumFor: {
            for (const auto& e : s.exprs) {
                auto res = walk_expr(*e, false);
                if (!res) return res;
            }
            unit(current_unit).reserved_slot_count += kLoopHiddenSlots;
            declare_local(s.names[0], kNoUnit, false);
            push_scope(current_unit);
            auto body = walk_block(*s.bodies[0]);
            pop_scope();
            return body;
        }
        case StmtKind::GenFor: {
            for (const auto& e : s.exprs) {
                auto res = walk_expr(*e, false);
                if (!res) return res;
            }
            unit(current_unit).reserved_slot_count += kLoopHiddenSlots;
            for (const auto& n : s.names) declare_local(n, kNoUnit, false);
            push_scope(current_unit);
            auto body = walk_block(*s.bodies[0]);
            pop_scope();
            return body;
        }
        case StmtKind::Return: {
            for (const auto& e : s.exprs) {
                auto res = walk_expr(*e, false);
                if (!res) return res;
            }
            return vortex::support::ok();
        }
        case StmtKind::Break:
        case StmtKind::Goto:
        case StmtKind::Label:
            return vortex::support::ok();
        }
        return support::fail(support::ErrorCode::SyntaxError, "unhandled statement kind",
                    s.line);
    }

    // CEP:WHAT: Declares a lifted function's parameters as unit locals.
    // CEP:WHY: Parameter slots join the owner's local list so the register
    //          layout and capture machinery treat every slot uniformly.
    // CEP:STATUS: complete
    // CEP:FAILURE: none (names are parser-validated identifiers).
    // CEP:ASSUMES: `fn` is a Function literal; the caller pushed the unit's
    //          scope.
    // CEP:COST: cold; linear in the parameter count.
    // CEP:EVIDENCE: functions with parameters run in the example corpus.
    void declare_into_unit_locals(uint32_t fn_id, const Expr& fn,
                                  bool add_self) {
        const uint32_t saved = current_unit;
        current_unit = fn_id;
        if (add_self) declare_local("self", kNoUnit, false);
        for (const auto& p : fn.params) declare_local(p, kNoUnit, false);
        current_unit = saved;
    }

    // CEP:WHAT: Walks a block as the body of the given unit.
    // CEP:WHY: Nested function bodies form their own scopes; routing the
    //          walk through one entry point keeps unit switching exact.
    // CEP:STATUS: complete
    // CEP:FAILURE: Propagates statement diagnostics.
    // CEP:ASSUMES: The caller pushed a scope for the unit.
    // CEP:COST: cold; linear in the block size.
    // CEP:EVIDENCE: nested closures in the example corpus compile.
    vortex::support::Result<void> walk_block_in_unit(uint32_t unit_id,
                                                     const Block& block) {
        const uint32_t saved = current_unit;
        current_unit = unit_id;
        auto res = walk_block(block);
        current_unit = saved;
        return res;
    }

    // CEP:WHAT: Walks one block of statements under the current scope.
    // CEP:WHY: Blocks are the scope units of the language; the walk
    //          maintains their frame lifetimes so captures resolve against
    //          exact scopes.
    // CEP:STATUS: complete
    // CEP:FAILURE: Propagates statement diagnostics.
    // CEP:ASSUMES: The caller pushed the enclosing scope frame.
    // CEP:COST: cold; linear in the block size.
    // CEP:EVIDENCE: the example corpus analyzes through this walk.
    vortex::support::Result<void> walk_block(const Block& block) {
        for (const auto& s : block.stmts) {
            auto res = walk_stmt(*s);
            if (!res) return res;
        }
        return vortex::support::ok();
    }

    // CEP:WHAT: Walks one expression, creating units for function literals.
    // CEP:WHY: Expression walks decide escape facts and record captures;
    //          direct-callee detection lives exactly here.
    // CEP:STATUS: complete
    // CEP:FAILURE: Returns a Diagnostic for invalid uses (varargs outside a
    //          variadic declaration are rejected at emission).
    // CEP:ASSUMES: The expression tree is well-formed.
    // CEP:COST: cold; linear in the expression size.
    // CEP:EVIDENCE: the example corpus exercises every expression kind.
    vortex::support::Result<void> walk_expr(const Expr& e,
                                            bool direct_callee) {
        switch (e.kind) {
        case ExprKind::Nil:
        case ExprKind::True:
        case ExprKind::False:
        case ExprKind::Int:
        case ExprKind::Float:
        case ExprKind::Str:
        case ExprKind::Vararg:
            return vortex::support::ok();
        case ExprKind::Name: {
            use_name(e.name, direct_callee);
            return vortex::support::ok();
        }
        case ExprKind::Paren: {
            return walk_expr(*e.lhs, false);
        }
        case ExprKind::Function: {
            const uint32_t fn_id = fresh_unit(current_unit, &e, "anon", true);
            push_scope(fn_id);
            declare_into_unit_locals(fn_id, e, false);
            auto res = walk_block_in_unit(fn_id, *e.body);
            pop_scope();
            return res;
        }
        case ExprKind::Table: {
            for (const auto& f : e.fields) {
                if (f.key != nullptr) {
                    auto k = walk_expr(*f.key, false);
                    if (!k) return k;
                }
                auto v = walk_expr(*f.value, false);
                if (!v) return v;
            }
            return vortex::support::ok();
        }
        case ExprKind::Index: {
            auto o = walk_expr(*e.lhs, false);
            if (!o) return o;
            return walk_expr(*e.rhs, false);
        }
        case ExprKind::CallObj: {
            max_call_window_ = std::max(
                max_call_window_,
                static_cast<size_t>(ei::kValueCallBase + e.args.size()));
            const Expr& callee = *e.lhs;
            if (callee.kind == ExprKind::Name) {
                auto c = walk_expr(callee, true);
                if (!c) return c;
            } else {
                auto c = walk_expr(callee, false);
                if (!c) return c;
            }
            for (const auto& a : e.args) {
                auto r = walk_expr(*a, false);
                if (!r) return r;
            }
            return vortex::support::ok();
        }
        case ExprKind::CallMethod: {
            max_call_window_ = std::max(
                max_call_window_,
                static_cast<size_t>(ei::kMethodWindowBase + e.args.size()));
            auto o = walk_expr(*e.lhs, false);
            if (!o) return o;
            for (const auto& a : e.args) {
                auto r = walk_expr(*a, false);
                if (!r) return r;
            }
            return vortex::support::ok();
        }
        case ExprKind::Binary: {
            if (e.op == "and" || e.op == "or") {
                auto l = walk_expr(*e.lhs, false);
                if (!l) return l;
                return walk_expr(*e.rhs, false);
            }
            auto l = walk_expr(*e.lhs, false);
            if (!l) return l;
            return walk_expr(*e.rhs, false);
        }
        case ExprKind::Unary: {
            return walk_expr(*e.lhs, false);
        }
        }
        return support::fail(support::ErrorCode::SyntaxError, "unhandled expression kind",
                    e.line);
    }

    // CEP:WHAT: Absorbs child capture needs bottom-up and marks boxed slots.
    // CEP:WHY: A creator must hold the boxes its children consume; the
    //          absorption pass turns per-unit captures into parent facts.
    // CEP:STATUS: complete
    // CEP:FAILURE: none (pure bookkeeping over validated ids).
    // CEP:ASSUMES: Unit ids grow monotonically, so reverse order visits
    //          children before parents.
    // CEP:COST: cold; quadratic in nesting depth, linear in captures.
    // CEP:EVIDENCE: multi-level closures in the example corpus compile.
    void absorb_captures() {
        uint32_t max_value_arity = std::max<size_t>(
            max_call_window_, luavortex::kMinUniformArity);
        uint32_t max_escaping_params = 0;
        for (const auto& u : out.units) {
            if (u.escaping && !u.is_chunk) {
                max_escaping_params =
                    std::max(max_escaping_params, u.param_count + 1);
            }
        }
        max_value_arity = std::max(max_value_arity, max_escaping_params);
        out.uniform_arity = max_value_arity;
        for (size_t i = out.units.size(); i-- > 0;) {
            FnUnit& u = out.units[i];
            if (u.parent == kNoUnit) continue;
            FnUnit& parent = out.units[u.parent];
            for (const auto& c : u.captures) {
                if (c.owner_unit == u.parent) {
                    parent.locals[c.local_index].boxed = true;
                    continue;
                }
                bool present = false;
                for (const auto& pc : parent.captures) {
                    if (pc.owner_unit == c.owner_unit &&
                        pc.local_index == c.local_index) {
                        present = true;
                        break;
                    }
                }
                if (!present) parent.captures.push_back(c);
            }
        }
    }
};

}  // namespace

// CEP:WHAT: Runs the analysis pass over a parsed chunk.
// CEP:WHY: The emitter consumes the resulting unit tree as its plan; tests
//          inspect it directly.
// CEP:STATUS: complete
// CEP:FAILURE: Returns the first analysis diagnostic.
// CEP:ASSUMES: The chunk is parser-valid.
// CEP:COST: cold; linear in the chunk size.
// CEP:EVIDENCE: the example corpus compiles through this entry point.
vortex::support::Result<ProgramUnits> analyze_units(const Chunk& chunk) {
    Analyzer a{.chunk = chunk};
    FnUnit root;
    root.id = a.unit_counter++;
    root.parent = kNoUnit;
    root.is_chunk = true;
    root.method_name = "main";
    a.out.units.push_back(std::move(root));
    a.out.root_unit = 0;
    a.current_unit = 0;
    a.push_scope(0);
    auto res = a.walk_block(*chunk.body);
    if (!res) return std::unexpected(res.error());
    a.pop_scope();
    a.absorb_captures();
    return std::move(a.out);
}

// CEP:WHAT: Compiles Lua source text into a UGB module end to end.
// CEP:WHY: This is the frontend entry point the lvxc driver links against;
//          it also fixes the module-level identity and capability facts.
// CEP:STATUS: complete
// CEP:FAILURE: Returns the first frontmost diagnostic across lexing,
//          parsing, analysis, and emission. Container growth throws
//          std::bad_alloc, which is fatal at the driver boundary.
// CEP:ASSUMES: The source buffer outlives the call.
// CEP:COST: cold; dominated by emission for large chunks.
// CEP:EVIDENCE: tests/run_tests.sh compiles every case through this entry.
vortex::support::Result<CompiledLua> compile_lua(std::string_view source,
                                                 std::string_view chunk_name) {
    auto tokens = lex(source);
    if (!tokens) return std::unexpected(tokens.error());
    auto parsed = parse(*tokens);
    if (!parsed) return std::unexpected(parsed.error());
    Chunk chunk = std::move(*parsed);
    chunk.name = std::string(chunk_name);

    auto analyzed = analyze_units(chunk);
    if (!analyzed) return std::unexpected(analyzed.error());
    ProgramUnits units = std::move(*analyzed);

    CompiledLua result;
    vortex::ugb::UGBModule& module = result.module;
    module.language_name = "lua";
    module.language_id = emit_internal::kLuaLanguageId;

    auto emitted = emit_internal::emit_program(chunk, units, module);
    if (!emitted) return std::unexpected(emitted.error());
    result.fn_classes = std::move(*emitted);

    // Rule 3: the frontend declares what its lowered methods require; the
    // engine negotiates at load time (Interpreter::run -> check_capabilities).
    const uint8_t caps[] = {
        static_cast<uint8_t>(vortex::ugb::Capability::Arrays),
        static_cast<uint8_t>(vortex::ugb::Capability::Classes),
        static_cast<uint8_t>(vortex::ugb::Capability::VirtualCalls),
        static_cast<uint8_t>(vortex::ugb::Capability::BuiltinCalls),
        static_cast<uint8_t>(vortex::ugb::Capability::Debug),
    };
    vortex::ugb::CapabilitySet mask;
    for (const uint8_t c : caps) mask.add(vortex::ugb::Capability(c));
    module.capability_mask = mask.raw();
    for (auto& m : module.method_table) {
        m.required_capabilities.assign(std::begin(caps), std::end(caps));
    }
    return result;
}

}  // namespace luavortex
