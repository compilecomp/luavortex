// CEP:FILE: src/emitter_emit.cpp
// CEP:WHAT: The emission half of the lowering pass: unit layout, the
//           recursive unit emitter, statement lowering, and name resolution.
// CEP:WHY: Statements own control flow, scoping, and the calling conventions
//           of lifted functions; isolating them from value lowering keeps
//           each contract independently reviewable.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for unresolved labels, duplicate labels,
//          and internal resolution inconsistencies. Container growth
//          throws std::bad_alloc, which is fatal at the driver boundary.
// CEP:ASSUMES: The analysis pass completed and produced one unit per
//          function literal, with monotonic unit ids.
// CEP:COST: cold; linear in the chunk size.
// CEP:EVIDENCE: the example corpus executes on the T0 interpreter in CI.
#include "emitter_internal.hpp"

namespace luavortex {
namespace emit_internal {

namespace support = vortex::support;
namespace ei = emit_internal;

// CEP:WHAT: Allocates the next temporary register for the current statement.
// CEP:WHY: Temps are recycled per statement so register pressure stays
//          bounded by the largest single statement.
// CEP:STATUS: complete
// CEP:FAILURE: none (headroom is guaranteed by the layout computation).
// CEP:ASSUMES: The caller resets temps per statement and keeps per-statement
//          temp use under the layout headroom.
// CEP:COST: cold; one addition per allocation.
// CEP:EVIDENCE: the whole corpus emits through this allocator.
uint16_t EmitState::temp_reg() {
    return static_cast<uint16_t>(temps_base + temp_counter++);
}

// CEP:WHAT: Reserves a contiguous block of temporary registers.
// CEP:WHY: Call windows occupy consecutive registers; reserving them keeps
//          later temporaries from aliasing window slots mid-statement.
// CEP:STATUS: complete
// CEP:FAILURE: none (headroom covers the window sizes of valid programs).
// CEP:ASSUMES: Called immediately after allocating the window base.
// CEP:COST: cold; one addition per reservation.
// CEP:EVIDENCE: every call site reserves through this helper.
void EmitState::reserve_temps(uint16_t count) {
    temp_counter = static_cast<uint16_t>(temp_counter + count);
}

// CEP:WHAT: Resets the per-statement temporary counter.
// CEP:WHY: Statement boundaries are the reuse points of the temporary pool.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Called between statements.
// CEP:COST: cold; one store.
// CEP:EVIDENCE: exercised by every emitted statement.
void EmitState::reset_temps() {
    temp_counter = 0;
}

// CEP:WHAT: Computes the register layout of every unit.
// CEP:WHY: Layouts fix the calling convention before any emission so hidden
//          environment and capture slots are stable across call sites.
// CEP:STATUS: complete
// CEP:FAILURE: none (derived from analysis facts only).
// CEP:ASSUMES: Analysis filled param counts, locals, captures, and the
//          reserved loop slot counts.
// CEP:COST: cold; linear in the unit count.
// CEP:EVIDENCE: layout invariants hold across the corpus (tests execute it).
void Emitter::compute_layouts() {
    layouts.resize(st.units.units.size());
    for (const FnUnit& u : st.units.units) {
        UnitLayout& l = layouts[u.id];
        l.escaping = u.escaping;
        l.is_chunk = u.is_chunk;
        l.param_count = static_cast<uint16_t>(u.param_count);
        if (u.is_chunk) {
            l.env_reg = 0;
            l.locals_base = 1;  // slot 0 holds the environment table
            l.arg_count = 0;
        } else if (!u.escaping) {
            l.env_reg = l.param_count;
            l.locals_base = static_cast<uint16_t>(
                l.param_count + kHiddenEnvSlots + u.captures.size());
            l.arg_count = l.locals_base;
            for (uint32_t i = 0; i < u.captures.size(); ++i) {
                l.capture_regs.push_back(static_cast<uint16_t>(
                    l.param_count + 1 + i));
            }
        } else {
            // v0 is the environment object; a dedicated slot caches the
            // environment table read in the prologue. Declared arity is the
            // program-wide uniform count so Call.Virtual sites match the
            // verifier's arity law with a runtime-unknown callee.
            l.env_reg = static_cast<uint16_t>(l.param_count +
                                              kHiddenEnvSlots);
            l.locals_base = static_cast<uint16_t>(
                l.param_count + kHiddenEnvSlots + 1);
            l.arg_count = static_cast<uint16_t>(st.units.uniform_arity);
        }
        l.loop_base = static_cast<uint16_t>(l.locals_base + u.locals.size());
        l.temps_base =
            static_cast<uint16_t>(l.loop_base + u.reserved_slot_count);
        l.reg_count = static_cast<uint16_t>(l.temps_base + kTempHeadroom);
    }
}

// CEP:WHAT: Interns the class and declared field tokens of one escaping unit.
// CEP:WHY: Escaping functions are values via New.Object of a per-function
//          klass; the environment table and capture boxes live in declared
//          fields so GetField and SetField resolve at runtime.
// CEP:STATUS: complete
// CEP:FAILURE: none (interning is deterministic).
// CEP:ASSUMES: Called before any emission that references the tokens.
// CEP:COST: cold; proportional to the capture count.
// CEP:EVIDENCE: function-value examples run through Call.Virtual in CI.
void Emitter::intern_class_for(uint32_t unit_id) {
    const FnUnit& u = st.units.units[unit_id];
    UnitLayout& l = layouts[unit_id];
    l.class_token = st.module.intern_class(u.method_name);
    vortex::ugb::FieldTokenInfo env_field;
    env_field.name = kEnvTableName;
    env_field.klass_token = l.class_token;
    env_field.declared = true;
    st.module.fields.push_back(env_field);
    l.env_field_token =
        static_cast<uint32_t>(st.module.fields.size() - 1);
    for (uint32_t i = 0; i < u.captures.size(); ++i) {
        vortex::ugb::FieldTokenInfo box_field;
        box_field.name = std::string(kBoxFieldPrefix) + std::to_string(i);
        box_field.klass_token = l.class_token;
        box_field.declared = true;
        st.module.fields.push_back(box_field);
        l.box_tokens.push_back(
            static_cast<uint32_t>(st.module.fields.size() - 1));
    }
    fn_classes.push_back(FnClassBinding{l.class_token, u.method_name});
}

// CEP:WHAT: Emits one lifted unit: prologue, body, implicit return.
// CEP:WHY: Units share one recursive emission walk so ancestor scope frames
//          stay live for capture resolution.
// CEP:STATUS: complete
// CEP:FAILURE: Returns diagnostics from the body; fails on unresolved goto
//          labels after the walk.
// CEP:ASSUMES: Layouts and class tokens are computed.
// CEP:COST: cold; linear in the unit body size.
// CEP:EVIDENCE: every compiled method in the corpus comes from this path.
vortex::support::Result<void> Emitter::emit_unit(uint32_t id) {
    const FnUnit& u = st.units.units[id];
    const UnitLayout& l = layouts[id];
    auto saved_builder = std::move(st.builder);
    const uint32_t saved_unit = st.current_unit;
    const uint16_t saved_cursor = unit_slot_cursor;
    const uint16_t saved_temps = st.temps_base;
    const uint16_t saved_counter = st.temp_counter;
    const uint16_t saved_env = st.env_reg;
    const size_t saved_breaks = st.break_labels.size();
    const size_t saved_pending = st.pending_gotos.size();
    const size_t saved_label_names = st.label_names.size();
    const size_t saved_label_locals = st.label_locals.size();

    st.builder = std::make_unique<vortex::ugb::MethodBuilder>(
        st.module, u.method_name, l.reg_count, l.arg_count);
    st.current_unit = id;
    unit_slot_cursor = 0;
    st.temps_base = l.temps_base;
    // The child statement walk resets the counter; isolating it here keeps
    // the parent statement's live temporaries intact across the boundary.
    st.temp_counter = 0;
    st.break_labels.clear();
    st.label_names.clear();
    st.label_locals.clear();

    ScopeFrame frame;
    frame.unit_id = id;
    st.scopes.push_back(std::move(frame));

    // Parameters bind to the incoming argument registers before the body.
    for (uint32_t i = 0; i < u.param_count; ++i) {
        declare_binding(u.locals[i].name, kNoUnit, false);
    }

    // Prologue: make the environment table reachable.
    if (u.is_chunk) {
        st.env_reg = l.env_reg;
        auto res = emit_builtin_call(l.env_reg, kBNewTable, {});
        if (!res) return res;
    } else if (l.escaping) {
        st.env_reg = l.env_reg;
        st.builder->emit(vortex::ugb::Op::GET_FIELD, l.env_reg, {0}, true,
                         l.env_field_token);
    } else {
        st.env_reg = l.env_reg;
    }

    const Block& body = u.is_chunk ? *st.chunk.body : *u.fn->body;
    auto res = emit_block(body);
    if (!res) return res;

    if (!st.pending_gotos.empty()) {
        const EmitState::PendingGoto& p = st.pending_gotos.front();
        return vortex::support::fail(vortex::support::ErrorCode::SyntaxError,
                                     "no visible label '" + p.label + "'",
                                     p.line);
    }

    if (u.is_chunk) {
        st.builder->emit(vortex::ugb::Op::RETURN_UNIT, 0, {});
    } else {
        const uint16_t nil_reg = st.temp_reg();
        st.builder->const_null(nil_reg);
        st.builder->ret(nil_reg);
    }

    auto finished = st.builder->finish();
    if (!finished) return std::unexpected(finished.error());

    st.scopes.pop_back();
    st.builder = std::move(saved_builder);
    st.current_unit = saved_unit;
    unit_slot_cursor = saved_cursor;
    st.temps_base = saved_temps;
    st.temp_counter = saved_counter;
    st.env_reg = saved_env;
    st.break_labels.resize(saved_breaks);
    st.pending_gotos.resize(saved_pending);
    st.label_names.erase(std::next(st.label_names.begin(),
                                   static_cast<long>(saved_label_names)),
                         st.label_names.end());
    st.label_locals.erase(std::next(st.label_locals.begin(),
                                    static_cast<long>(saved_label_locals)),
                          st.label_locals.end());
    return vortex::support::ok();
}

// CEP:WHAT: Runs the whole emission pass.
// CEP:WHY: The driver prepares tokens and layouts, emits the root unit (all
//          nested units emit recursively), and hands the class bindings to
//          the host runtime.
// CEP:STATUS: complete
// CEP:FAILURE: Returns the first emission diagnostic.
// CEP:ASSUMES: The unit tree came from the analysis pass of the same chunk.
// CEP:COST: cold; linear in the chunk size.
// CEP:EVIDENCE: compile_lua drives this in every test.
vortex::support::Result<std::vector<FnClassBinding>> emit_program(
    const Chunk& chunk, const ProgramUnits& units,
    vortex::ugb::UGBModule& module) {
    // Designated initialization names every member so the missing-field
    // warning cannot fire on the aggregate.
    Emitter emitter{EmitState{
        .module = module,
        .units = units,
        .chunk = chunk,
        .scopes = {},
        .builder = nullptr,
        .current_unit = kNoUnit,
        .temps_base = 0,
        .temp_counter = 0,
        .env_reg = 0,
        .loop_slot_cursor = 0,
        .locals_declared = 0,
        .break_labels = {},
        .label_names = {},
        .label_locals = {},
        .pending_gotos = {},
        .label_counter = 0,
    }};
    emitter.compute_layouts();
    for (const FnUnit& u : units.units) {
        if (u.escaping) emitter.intern_class_for(u.id);
    }
    auto res = emitter.emit_unit(units.root_unit);
    if (!res) return std::unexpected(res.error());

    // The "call" token resolves verifier-side to this method; runtime
    // dispatch never reaches it because Call.Virtual resolves per receiver
    // klass. Its declared arity is the uniform call window.
    vortex::ugb::MethodBuilder dummy(module, kCallMethodName, 1,
                                     static_cast<uint16_t>(units.uniform_arity));
    dummy.emit(vortex::ugb::Op::RETURN_UNIT, 0, {});
    auto finished = dummy.finish();
    if (!finished) return std::unexpected(finished.error());

    return std::move(emitter.fn_classes);
}

// CEP:WHAT: Emits one block of statements.
// CEP:WHY: Blocks delimit temporary reuse and scope lifetimes.
// CEP:STATUS: complete
// CEP:FAILURE: Propagates statement diagnostics.
// CEP:ASSUMES: The caller manages the scope frame.
// CEP:COST: cold; linear in the block size.
// CEP:EVIDENCE: the whole corpus is emitted block by block.
vortex::support::Result<void> Emitter::emit_block(const Block& b) {
    for (const auto& s : b.stmts) {
        auto res = emit_stmt(*s);
        if (!res) return res;
    }
    return vortex::support::ok();
}

// CEP:WHAT: Emits a source-position debug record.
// CEP:WHY: Line-accurate diagnostics in the engine need position metadata;
//          the Debug capability carries it at negligible cost.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: The module declares the Debug capability.
// CEP:COST: one NOP-mapped instruction per statement.
// CEP:EVIDENCE: engine diagnostics report lines for failing test cases.
vortex::support::Result<void> Emitter::emit_debug_pos(uint32_t line) {
    st.builder->emit(vortex::ugb::Op::DEBUG_SRCPOS, 0, {}, true, line);
    return vortex::support::ok();
}

// CEP:WHAT: Emits one statement.
// CEP:WHY: The statement dispatcher keeps the per-kind procedures small.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for unhandled kinds (internal error).
// CEP:ASSUMES: The statement tree is parser-valid.
// CEP:COST: cold; linear per statement.
// CEP:EVIDENCE: every statement kind appears in the corpus.
vortex::support::Result<void> Emitter::emit_stmt(const Stmt& s) {
    st.reset_temps();
    auto pos = emit_debug_pos(s.line);
    if (!pos) return pos;
    switch (s.kind) {
    case StmtKind::Local: return emit_local_decl(s);
    case StmtKind::Assign: return emit_assign(s);
    case StmtKind::Call: {
        auto v = emit_expr(*s.exprs[0]);
        if (!v) return std::unexpected(v.error());
        return vortex::support::ok();
    }
    case StmtKind::Do: {
        ScopeFrame frame;
        frame.unit_id = st.current_unit;
        st.scopes.push_back(std::move(frame));
        auto res = emit_block(*s.bodies[0]);
        st.scopes.pop_back();
        return res;
    }
    case StmtKind::While: return emit_while(s);
    case StmtKind::Repeat: return emit_repeat(s);
    case StmtKind::If: return emit_if(s);
    case StmtKind::NumFor: return emit_num_for(s);
    case StmtKind::GenFor: return emit_gen_for(s);
    case StmtKind::FuncDecl: return emit_func_decl(s);
    case StmtKind::LocalFunc: return emit_local_func(s);
    case StmtKind::Return: return emit_return(s);
    case StmtKind::Break: {
        if (st.break_labels.empty()) {
            return support::fail(support::ErrorCode::SyntaxError,
                        "break outside a loop", s.line);
        }
        st.builder->jump(st.break_labels.back());
        return vortex::support::ok();
    }
    case StmtKind::Goto: return emit_goto(s);
    case StmtKind::Label: return emit_label(s);
    }
    return support::fail(support::ErrorCode::SyntaxError, "unhandled statement kind", s.line);
}

// CEP:WHAT: Emits a local declaration with Lua padding rules.
// CEP:WHY: Missing initializers read as nil and surplus initializers are
//          dropped; boxing happens here for captured slots.
// CEP:STATUS: complete
// CEP:FAILURE: Propagates initializer diagnostics.
// CEP:ASSUMES: The analysis pass created units for function initializers.
// CEP:COST: cold; linear in the declared name count.
// CEP:EVIDENCE: declaration forms across the corpus execute.
vortex::support::Result<void> Emitter::emit_local_decl(const Stmt& s) {
    std::vector<Val> vals;
    for (const auto& e : s.exprs) {
        auto v = emit_expr(*e);
        if (!v) return std::unexpected(v.error());
        vals.push_back(*v);
    }
    std::vector<EmittedBinding> declared;
    for (size_t i = 0; i < s.names.size(); ++i) {
        uint32_t fn_unit = kNoUnit;
        if (i < s.exprs.size() &&
            s.exprs[i]->kind == ExprKind::Function) {
            auto it = st.units.fn_units.find(s.exprs[i].get());
            if (it != st.units.fn_units.end()) fn_unit = it->second;
        }
        EmittedBinding* b = declare_binding(
            s.names[i], fn_unit, s.attribs[i] == Attrib::Const);
        declared.push_back(*b);
    }
    for (size_t i = 0; i < declared.size(); ++i) {
        uint16_t value_reg;
        if (i < vals.size()) {
            value_reg = vals[i].reg;
        } else {
            const uint16_t nil_reg = st.temp_reg();
            st.builder->const_null(nil_reg);
            value_reg = nil_reg;
        }
        if (declared[i].boxed) {
            const uint16_t one = st.temp_reg();
            st.builder->const_i32(one, 1);
            st.builder->emit(vortex::ugb::Op::NEW_ARRAY, declared[i].reg,
                             {one});
            const uint16_t zero = st.temp_reg();
            st.builder->const_i32(zero, 0);
            st.builder->emit(vortex::ugb::Op::ARRAY_SET, 0,
                             {declared[i].reg, zero, value_reg});
        } else if (declared[i].reg != value_reg) {
            st.builder->emit(vortex::ugb::Op::MOVE, declared[i].reg,
                             {value_reg});
        }
    }
    return vortex::support::ok();
}

// CEP:WHAT: Emits an assignment statement with Lua evaluation order.
// CEP:WHY: All right-hand values are computed before any store, which the
//          language requires for swaps and paired updates.
// CEP:STATUS: complete
// CEP:FAILURE: Propagates operand diagnostics; rejects non-variable targets
//          (the parser already filters most shapes).
// CEP:ASSUMES: Targets are Name or Index expressions.
// CEP:COST: cold; linear in the target count.
// CEP:EVIDENCE: swap and table-store examples execute.
vortex::support::Result<void> Emitter::emit_assign(const Stmt& s) {
    std::vector<Val> vals;
    for (const auto& e : s.exprs) {
        auto v = emit_expr(*e);
        if (!v) return std::unexpected(v.error());
        // Snapshot into a fresh temp: reads of local slots would otherwise
        // race the stores (a, b = b, a must see the old values).
        const uint16_t snap = st.temp_reg();
        st.builder->emit(vortex::ugb::Op::MOVE, snap, {v->reg});
        Val snapshot;
        snapshot.reg = snap;
        snapshot.type = v->type;
        vals.push_back(snapshot);
    }
    struct Target {
        bool is_index = false;
        NameResolution name;
        Val obj;
        Val key;
    };
    std::vector<Target> targets;
    for (const auto& t : s.targets) {
        if (t->kind == ExprKind::Name) {
            Target target;
            target.name = resolve_name(t->name);
            if (target.name.kind != NameResolution::Kind::OwnLocal &&
                target.name.kind != NameResolution::Kind::Capture &&
                target.name.kind != NameResolution::Kind::Global) {
                return support::fail(support::ErrorCode::SyntaxError,
                            "cannot assign to '" + t->name + "'", s.line);
            }
            targets.push_back(std::move(target));
        } else if (t->kind == ExprKind::Index) {
            auto obj = emit_expr(*t->lhs);
            if (!obj) return std::unexpected(obj.error());
            auto key = emit_expr(*t->rhs);
            if (!key) return std::unexpected(key.error());
            Target target;
            target.is_index = true;
            target.obj = *obj;
            target.key = *key;
            targets.push_back(std::move(target));
        } else {
            return support::fail(support::ErrorCode::SyntaxError,
                        "invalid assignment target", s.line);
        }
    }
    for (size_t i = 0; i < targets.size(); ++i) {
        uint16_t value_reg;
        if (i < vals.size()) {
            value_reg = vals[i].reg;
        } else {
            const uint16_t nil_reg = st.temp_reg();
            st.builder->const_null(nil_reg);
            value_reg = nil_reg;
        }
        if (targets[i].is_index) {
            Val value;
            value.reg = value_reg;
            auto res = emit_index_store(targets[i].obj, targets[i].key, value);
            if (!res) return res;
        } else {
            auto res = store_local(targets[i].name, value_reg);
            if (!res) return res;
        }
    }
    return vortex::support::ok();
}

// CEP:WHAT: Emits an if/elseif/else chain.
// CEP:WHY: Chained conditions lower to a jump tree with a shared end label.
// CEP:STATUS: complete
// CEP:FAILURE: Propagates condition and body diagnostics.
// CEP:ASSUMES: Conditions normalize through the language truthiness rules.
// CEP:COST: cold; linear in the clause count.
// CEP:EVIDENCE: branching examples execute with correct arms.
vortex::support::Result<void> Emitter::emit_if(const Stmt& s) {
    const std::string l_end = fresh_label("if_end");
    const size_t clauses = s.exprs.size();
    for (size_t i = 0; i < clauses; ++i) {
        st.reset_temps();
        auto cond = emit_truthy(*s.exprs[i]);
        if (!cond) return std::unexpected(cond.error());
        ScopeFrame frame;
        frame.unit_id = st.current_unit;
        const bool has_next = i + 1 < clauses;
        const bool has_else = s.else_body != nullptr;
        if (has_next) {
            const std::string l_next = fresh_label("if_next");
            st.builder->jump_false(cond->reg, l_next);
            auto res = emit_block(*s.bodies[i]);
            if (!res) return res;
            st.builder->jump(l_end);
            st.builder->bind_label(l_next);
        } else if (has_else) {
            const std::string l_else = fresh_label("if_else");
            st.builder->jump_false(cond->reg, l_else);
            auto res = emit_block(*s.bodies[i]);
            if (!res) return res;
            st.builder->jump(l_end);
            st.builder->bind_label(l_else);
            st.scopes.push_back(std::move(frame));
            auto else_res = emit_block(*s.else_body);
            st.scopes.pop_back();
            if (!else_res) return else_res;
        } else {
            st.builder->jump_false(cond->reg, l_end);
            auto res = emit_block(*s.bodies[i]);
            if (!res) return res;
        }
    }
    st.builder->bind_label(l_end);
    return vortex::support::ok();
}

// CEP:WHAT: Emits a while loop.
// CEP:WHY: While shares the loop-exit and break machinery with the other
//          loop forms.
// CEP:STATUS: complete
// CEP:FAILURE: Propagates diagnostics.
// CEP:ASSUMES: Conditions normalize through the truthiness rules.
// CEP:COST: cold; constant overhead per loop.
// CEP:EVIDENCE: loop examples execute with correct trip counts.
vortex::support::Result<void> Emitter::emit_while(const Stmt& s) {
    const std::string l_cond = fresh_label("while_cond");
    const std::string l_end = fresh_label("while_end");
    st.builder->bind_label(l_cond);
    st.reset_temps();
    auto cond = emit_truthy(*s.exprs[0]);
    if (!cond) return std::unexpected(cond.error());
    st.builder->jump_false(cond->reg, l_end);
    ScopeFrame frame;
    frame.unit_id = st.current_unit;
    st.scopes.push_back(std::move(frame));
    st.break_labels.push_back(l_end);
    auto body = emit_block(*s.bodies[0]);
    st.break_labels.pop_back();
    st.scopes.pop_back();
    if (!body) return body;
    st.builder->jump(l_cond);
    st.builder->bind_label(l_end);
    return vortex::support::ok();
}

// CEP:WHAT: Emits a repeat loop with its bottom condition.
// CEP:WHY: The condition scope includes the body locals, which the shared
//          scope frame provides.
// CEP:STATUS: complete
// CEP:FAILURE: Propagates diagnostics.
// CEP:ASSUMES: Body locals are visible to the condition expression.
// CEP:COST: cold; constant overhead per loop.
// CEP:EVIDENCE: repeat examples execute with correct trip counts.
vortex::support::Result<void> Emitter::emit_repeat(const Stmt& s) {
    const std::string l_body = fresh_label("repeat_body");
    const std::string l_end = fresh_label("repeat_end");
    st.builder->bind_label(l_body);
    ScopeFrame frame;
    frame.unit_id = st.current_unit;
    st.scopes.push_back(std::move(frame));
    st.break_labels.push_back(l_end);
    auto body = emit_block(*s.bodies[0]);
    st.break_labels.pop_back();
    if (!body) {
        st.scopes.pop_back();
        return body;
    }
    st.reset_temps();
    auto cond = emit_truthy(*s.exprs[0]);
    if (!cond) {
        st.scopes.pop_back();
        return std::unexpected(cond.error());
    }
    st.builder->jump_true(cond->reg, l_end);
    st.builder->jump(l_body);
    st.builder->bind_label(l_end);
    st.scopes.pop_back();
    return vortex::support::ok();
}

// CEP:WHAT: Emits a numeric for loop with exact evaluation-once semantics.
// CEP:WHY: Start, limit, and step evaluate once into reserved slots; the
//          body variable is a fresh copy (and fresh box when captured) per
//          iteration, matching the language's capture rules.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for missing loop machinery; the step
//          check raises a runtime error for a zero step.
// CEP:ASSUMES: The reserved slot region covers this loop (analysis counted
//          it in the same AST order).
// CEP:COST: cold; three reserved slots per active loop.
// CEP:EVIDENCE: numeric-for examples including negative steps execute.
vortex::support::Result<void> Emitter::emit_num_for(const Stmt& s) {
    auto start = emit_expr(*s.exprs[0]);
    if (!start) return std::unexpected(start.error());
    auto limit = emit_expr(*s.exprs[1]);
    if (!limit) return std::unexpected(limit.error());
    uint16_t step_reg;
    if (s.exprs.size() > ei::kNumForStepArg) {
        auto step = emit_expr(*s.exprs[ei::kNumForStepArg]);
        if (!step) return std::unexpected(step.error());
        step_reg = step->reg;
    } else {
        const uint16_t one = st.temp_reg();
        st.builder->const_i32(one, 1);
        step_reg = one;
    }
    const uint16_t ctrl =
        static_cast<uint16_t>(layouts[st.current_unit].loop_base +
                              st.loop_slot_cursor * kLoopHiddenSlots);
    const uint16_t limit_reg = static_cast<uint16_t>(ctrl + 1);
    const uint16_t step_slot = static_cast<uint16_t>(ctrl + 2);
    ++st.loop_slot_cursor;
    st.builder->emit(vortex::ugb::Op::MOVE, ctrl, {start->reg});
    st.builder->emit(vortex::ugb::Op::MOVE, limit_reg, {limit->reg});
    st.builder->emit(vortex::ugb::Op::MOVE, step_slot, {step_reg});

    const uint16_t check_dst = st.temp_reg();
    Val step_val;
    step_val.reg = step_slot;
    auto check = emit_builtin_call(check_dst, kBCheckStep, {step_val});
    if (!check) return check;

    EmittedBinding* var = declare_binding(s.names[0], kNoUnit, false);

    const std::string l_cond = fresh_label("for_cond");
    const std::string l_end = fresh_label("for_end");
    st.builder->bind_label(l_cond);
    st.reset_temps();
    Val ctrl_val;
    ctrl_val.reg = ctrl;
    Val limit_val;
    limit_val.reg = limit_reg;
    const uint16_t cont = st.temp_reg();
    auto cont_call = emit_builtin_call(cont, kBForCont,
                                       {ctrl_val, limit_val, step_val});
    if (!cont_call) return cont_call;
    st.builder->jump_false(cont, l_end);

    ScopeFrame frame;
    frame.unit_id = st.current_unit;
    st.scopes.push_back(std::move(frame));
    st.break_labels.push_back(l_end);
    if (var->boxed) {
        const uint16_t one = st.temp_reg();
        st.builder->const_i32(one, 1);
        st.builder->emit(vortex::ugb::Op::NEW_ARRAY, var->reg, {one});
        const uint16_t zero = st.temp_reg();
        st.builder->const_i32(zero, 0);
        st.builder->emit(vortex::ugb::Op::ARRAY_SET, 0,
                         {var->reg, zero, ctrl});
    } else {
        st.builder->emit(vortex::ugb::Op::MOVE, var->reg, {ctrl});
    }
    auto body = emit_block(*s.bodies[0]);
    st.break_labels.pop_back();
    if (!body) {
        st.scopes.pop_back();
        return body;
    }
    st.scopes.pop_back();
    st.reset_temps();
    const uint16_t next = st.temp_reg();
    auto step_call = emit_builtin_call(next, kBForStep,
                                       {ctrl_val, step_val});
    if (!step_call) return step_call;
    st.builder->emit(vortex::ugb::Op::MOVE, ctrl, {next});
    st.builder->jump(l_cond);
    st.builder->bind_label(l_end);
    return vortex::support::ok();
}

// CEP:WHAT: Emits a generic for loop with the three iterator forms.
// CEP:WHY: pairs and ipairs use the engine-side key/value protocol; user
//          iterators use the single-value Call.Virtual protocol.
// CEP:STATUS: partial
// CEP:OWNER: luavortex-maintainers
// CEP:TICKET: compilecomp/luavortex#1
// CEP:FAILURE: Returns a Diagnostic for iterator machinery problems; a
//          non-function iterator raises a runtime error via the hook.
// CEP:ASSUMES: Multi-value user iterators reduce to their first value (the
//          documented conformance deviation).
// CEP:COST: cold; three reserved slots per active loop.
// CEP:EVIDENCE: pairs, ipairs, and custom iterator examples execute.
vortex::support::Result<void> Emitter::emit_gen_for(const Stmt& s) {
    const uint16_t slot_f =
        static_cast<uint16_t>(layouts[st.current_unit].loop_base +
                              st.loop_slot_cursor * kLoopHiddenSlots);
    const uint16_t slot_s = static_cast<uint16_t>(slot_f + 1);
    const uint16_t slot_ctrl = static_cast<uint16_t>(slot_f + 2);
    ++st.loop_slot_cursor;

    // Iterator-form detection: pairs(t) and ipairs(t) get the native
    // key/value protocol; anything else takes the value-call protocol.
    bool is_pairs = false;
    bool is_ipairs = false;
    const Expr* first = s.exprs[0].get();
    if (first->kind == ExprKind::CallObj &&
        first->lhs->kind == ExprKind::Name &&
        (first->lhs->name == "pairs" || first->lhs->name == "ipairs")) {
        is_pairs = first->lhs->name == "pairs";
        is_ipairs = first->lhs->name == "ipairs";
    }
    const std::string l_cond = fresh_label("gfor_cond");
    const std::string l_end = fresh_label("gfor_end");

    if (is_pairs || is_ipairs) {
        auto t = emit_expr(*first->args[0]);
        if (!t) return std::unexpected(t.error());
        st.builder->emit(vortex::ugb::Op::MOVE, slot_f, {t->reg});
        if (is_ipairs) {
            const uint16_t zero = st.temp_reg();
            st.builder->const_i32(zero, 0);
            st.builder->emit(vortex::ugb::Op::MOVE, slot_ctrl, {zero});
        } else {
            const uint16_t nil_reg = st.temp_reg();
            st.builder->const_null(nil_reg);
            st.builder->emit(vortex::ugb::Op::MOVE, slot_ctrl, {nil_reg});
        }
        const char* hook = is_pairs ? kBNext : kBInext;
        st.builder->bind_label(l_cond);
        st.reset_temps();
        const uint16_t kv = st.temp_reg();
        Val t_val;
        t_val.reg = slot_f;
        Val c_val;
        c_val.reg = slot_ctrl;
        auto kv_call = emit_builtin_call(kv, hook, {t_val, c_val});
        if (!kv_call) return kv_call;
        const uint16_t kvnil = st.temp_reg();
        st.builder->emit(vortex::ugb::Op::EQ_NULL, kvnil, {kv});
        st.builder->jump_true(kvnil, l_end);
        const uint16_t zero = st.temp_reg();
        st.builder->const_i32(zero, 0);
        const uint16_t one = st.temp_reg();
        st.builder->const_i32(one, 1);
        const uint16_t k = st.temp_reg();
        st.builder->emit(vortex::ugb::Op::ARRAY_GET, k, {kv, zero});
        const uint16_t v = st.temp_reg();
        st.builder->emit(vortex::ugb::Op::ARRAY_GET, v, {kv, one});

        ScopeFrame frame;
        frame.unit_id = st.current_unit;
        st.scopes.push_back(std::move(frame));
        st.break_labels.push_back(l_end);
        std::vector<EmittedBinding> bindings;
        for (const auto& n : s.names) {
            bindings.push_back(*declare_binding(n, kNoUnit, false));
        }
        for (size_t i = 0; i < bindings.size(); ++i) {
            if (i == 0) {
                if (bindings[i].boxed) {
                    // A captured loop variable needs a fresh box per step.
                    const uint16_t one_box = st.temp_reg();
                    st.builder->const_i32(one_box, 1);
                    st.builder->emit(vortex::ugb::Op::NEW_ARRAY,
                                     bindings[i].reg, {one_box});
                    const uint16_t zero_box = st.temp_reg();
                    st.builder->const_i32(zero_box, 0);
                    st.builder->emit(vortex::ugb::Op::ARRAY_SET, 0,
                                     {bindings[i].reg, zero_box, k});
                } else {
                    st.builder->emit(vortex::ugb::Op::MOVE, bindings[i].reg,
                                     {k});
                }
            } else if (i == 1) {
                st.builder->emit(vortex::ugb::Op::MOVE, bindings[i].reg, {v});
            } else {
                const uint16_t nil_reg = st.temp_reg();
                st.builder->const_null(nil_reg);
                st.builder->emit(vortex::ugb::Op::MOVE, bindings[i].reg,
                                 {nil_reg});
            }
        }
        auto body = emit_block(*s.bodies[0]);
        st.break_labels.pop_back();
        if (!body) {
            st.scopes.pop_back();
            return body;
        }
        st.scopes.pop_back();
        st.reset_temps();
        st.builder->emit(vortex::ugb::Op::MOVE, slot_ctrl, {k});
        st.builder->jump(l_cond);
        st.builder->bind_label(l_end);
        return vortex::support::ok();
    }

    // Generic value-call iterator: f, s, ctrl.
    std::vector<uint16_t> init_regs;
    for (size_t i = 0; i < kLoopHiddenSlots; ++i) {
        if (i < s.exprs.size()) {
            auto v = emit_expr(*s.exprs[i]);
            if (!v) return std::unexpected(v.error());
            init_regs.push_back(v->reg);
        } else {
            const uint16_t nil_reg = st.temp_reg();
            st.builder->const_null(nil_reg);
            init_regs.push_back(nil_reg);
        }
    }
    st.builder->emit(vortex::ugb::Op::MOVE, slot_f, {init_regs[0]});
    st.builder->emit(vortex::ugb::Op::MOVE, slot_s, {init_regs[1]});
    st.builder->emit(vortex::ugb::Op::MOVE,
                     slot_ctrl, {init_regs[ei::kValueArgIndex]});

    st.builder->bind_label(l_cond);
    st.reset_temps();
    Val f_val;
    f_val.reg = slot_f;
    const uint16_t isfn = st.temp_reg();
    auto fn_check = emit_builtin_call(isfn, kBIsFunction, {f_val});
    if (!fn_check) return fn_check;
    const std::string l_call = fresh_label("gfor_call");
    st.builder->jump_true(isfn, l_call);
    const uint16_t err_dst = st.temp_reg();
    auto err = emit_builtin_call(err_dst, kBCallError, {f_val});
    if (!err) return err;
    st.builder->bind_label(l_call);
    // Uniform window: iterator value, state, control, nil padding.
    const uint16_t window =
        static_cast<uint16_t>(st.units.uniform_arity);
    const uint16_t base = st.temp_reg();
    st.reserve_temps(static_cast<uint16_t>(window - 1));
    st.builder->emit(vortex::ugb::Op::MOVE, base, {slot_f});
    st.builder->emit(vortex::ugb::Op::MOVE,
                     static_cast<uint16_t>(base + kValueCallBase), {slot_s});
    st.builder->emit(vortex::ugb::Op::MOVE,
                     static_cast<uint16_t>(base + kCallArgKey), {slot_ctrl});
    uint16_t pad = static_cast<uint16_t>(base + kCallArgValue);
    while (pad < static_cast<uint16_t>(base + window)) {
        st.builder->const_null(pad);
        ++pad;
    }
    const uint16_t r = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::CALL_VIRTUAL, r, {base, window}, true,
                     st.module.intern_method(kCallMethodName));
    const uint16_t rnil = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::EQ_NULL, rnil, {r});
    st.builder->jump_true(rnil, l_end);

    ScopeFrame frame;
    frame.unit_id = st.current_unit;
    st.scopes.push_back(std::move(frame));
    st.break_labels.push_back(l_end);
    for (size_t i = 0; i < s.names.size(); ++i) {
        EmittedBinding* b = declare_binding(s.names[i], kNoUnit, false);
        if (i == 0) {
            if (b->boxed) {
                const uint16_t one = st.temp_reg();
                st.builder->const_i32(one, 1);
                st.builder->emit(vortex::ugb::Op::NEW_ARRAY, b->reg, {one});
                const uint16_t zero = st.temp_reg();
                st.builder->const_i32(zero, 0);
                st.builder->emit(vortex::ugb::Op::ARRAY_SET, 0,
                                 {b->reg, zero, r});
            } else {
                st.builder->emit(vortex::ugb::Op::MOVE, b->reg, {r});
            }
        } else {
            const uint16_t nil_reg = st.temp_reg();
            st.builder->const_null(nil_reg);
            st.builder->emit(vortex::ugb::Op::MOVE, b->reg, {nil_reg});
        }
    }
    auto body = emit_block(*s.bodies[0]);
    st.break_labels.pop_back();
    if (!body) {
        st.scopes.pop_back();
        return body;
    }
    st.scopes.pop_back();
    st.reset_temps();
    st.builder->emit(vortex::ugb::Op::MOVE, slot_ctrl, {r});
    st.builder->jump(l_cond);
    st.builder->bind_label(l_end);
    return vortex::support::ok();
}

// CEP:WHAT: Emits a global function declaration.
// CEP:WHY: Declarations assign the function value into the environment or a
//          nested table, with the implicit self parameter already bound by
//          the analysis pass.
// CEP:STATUS: complete
// CEP:FAILURE: Propagates diagnostics.
// CEP:ASSUMES: The unit for the literal exists in fn_units.
// CEP:COST: cold; one value construction per declaration.
// CEP:EVIDENCE: global and method declarations run across the corpus.
vortex::support::Result<void> Emitter::emit_func_decl(const Stmt& s) {
    auto it = st.units.fn_units.find(s.exprs[0].get());
    if (it == st.units.fn_units.end()) {
        return vortex::support::fail(vortex::support::ErrorCode::SyntaxError,
                                     "declaration unit missing", s.line);
    }
    const uint32_t unit_id = it->second;
    auto body_res = emit_unit(unit_id);
    if (!body_res) return body_res;
    st.reset_temps();
    auto value = emit_function_value(unit_id);
    if (!value) return std::unexpected(value.error());

    if (s.path.size() == 1) {
        const uint16_t id_reg = st.temp_reg();
        st.builder->const_i32(id_reg,
                              static_cast<int32_t>(st.module.intern_string(
                                  s.path[0])));
        Val env;
        env.reg = st.env_reg;
        Val id_val;
        id_val.reg = id_reg;
        const uint16_t set_dst = st.temp_reg();
        auto set = emit_builtin_call(set_dst, kBSetEnv, {env, id_val, *value});
        if (!set) return set;
        return vortex::support::ok();
    }
    NameResolution prefix = resolve_name(s.path[0]);
    Val base;
    if (prefix.kind == NameResolution::Kind::OwnLocal ||
        prefix.kind == NameResolution::Kind::Capture) {
        auto read = read_binding(prefix, s.line);
        if (!read) return std::unexpected(read.error());
        base = *read;
    } else {
        const uint16_t id_reg = st.temp_reg();
        st.builder->const_i32(id_reg,
                              static_cast<int32_t>(st.module.intern_string(
                                  s.path[0])));
        Val env;
        env.reg = st.env_reg;
        Val id_val;
        id_val.reg = id_reg;
        const uint16_t dst = st.temp_reg();
        auto get = emit_builtin_call(dst, kBGetEnv, {env, id_val});
        if (!get) return get;
        base.reg = dst;
    }
    for (size_t i = 1; i + 1 < s.path.size(); ++i) {
        auto key = emit_str_const(s.path[i]);
        if (!key) return std::unexpected(key.error());
        auto next = emit_index_load(base, *key);
        if (!next) return std::unexpected(next.error());
        base = *next;
    }
    auto key = emit_str_const(s.path.back());
    if (!key) return std::unexpected(key.error());
    auto store = emit_index_store(base, *key, *value);
    if (!store) return store;
    return vortex::support::ok();
}

// CEP:WHAT: Emits a local function declaration.
// CEP:WHY: The binding is declared before the body so recursion resolves;
//          escaping functions store the value object, direct-only functions
//          bind the unit for Call.Direct.
// CEP:STATUS: complete
// CEP:FAILURE: Propagates diagnostics.
// CEP:ASSUMES: The unit exists in fn_units.
// CEP:COST: cold; one value construction when escaping.
// CEP:EVIDENCE: local function examples run in the corpus.
vortex::support::Result<void> Emitter::emit_local_func(const Stmt& s) {
    auto it = st.units.fn_units.find(s.exprs[0].get());
    if (it == st.units.fn_units.end()) {
        return vortex::support::fail(vortex::support::ErrorCode::SyntaxError,
                                     "declaration unit missing", s.line);
    }
    const uint32_t unit_id = it->second;
    EmittedBinding* b = declare_binding(s.names[0], unit_id, false);
    auto body_res = emit_unit(unit_id);
    if (!body_res) return body_res;
    if (!st.units.units[unit_id].escaping) {
        return vortex::support::ok();
    }
    st.reset_temps();
    auto value = emit_function_value(unit_id);
    if (!value) return std::unexpected(value.error());
    if (b->boxed) {
        const uint16_t one = st.temp_reg();
        st.builder->const_i32(one, 1);
        st.builder->emit(vortex::ugb::Op::NEW_ARRAY, b->reg, {one});
        const uint16_t zero = st.temp_reg();
        st.builder->const_i32(zero, 0);
        st.builder->emit(vortex::ugb::Op::ARRAY_SET, 0,
                         {b->reg, zero, value->reg});
    } else {
        st.builder->emit(vortex::ugb::Op::MOVE, b->reg, {value->reg});
    }
    return vortex::support::ok();
}

// CEP:WHAT: Emits a return statement.
// CEP:WHY: The conformance target returns one value; the first expression
//          is the result and the rest drop per the documented deviation.
// CEP:STATUS: complete
// CEP:FAILURE: Propagates expression diagnostics.
// CEP:ASSUMES: The engine's Return passes one register.
// CEP:COST: cold; one return per statement.
// CEP:EVIDENCE: returns across the corpus observe correct results.
vortex::support::Result<void> Emitter::emit_return(const Stmt& s) {
    if (s.exprs.empty()) {
        const uint16_t nil_reg = st.temp_reg();
        st.builder->const_null(nil_reg);
        st.builder->ret(nil_reg);
        return vortex::support::ok();
    }
    auto v = emit_expr(*s.exprs[0]);
    if (!v) return std::unexpected(v.error());
    st.builder->ret(v->reg);
    return vortex::support::ok();
}

// CEP:WHAT: Emits a goto, reserving the target label name if needed.
// CEP:WHY: Forward references resolve at the label bind; the reserved name
//          keeps builder labels unique per occurrence.
// CEP:STATUS: complete
// CEP:FAILURE: none here; scope-entry violations surface at the bind.
// CEP:ASSUMES: The enclosing unit owns the label tables.
// CEP:COST: cold; one jump per statement.
// CEP:EVIDENCE: goto examples run in the corpus.
vortex::support::Result<void> Emitter::emit_goto(const Stmt& s) {
    auto it = st.label_names.find(s.label);
    std::string name;
    if (it != st.label_names.end()) {
        name = it->second;
    } else {
        name = fresh_label(s.label);
        st.label_names[s.label] = name;
        EmitState::PendingGoto pending;
        pending.label = s.label;
        pending.locals_count = st.locals_declared;
        pending.line = s.line;
        st.pending_gotos.push_back(std::move(pending));
    }
    st.builder->jump(name);
    return vortex::support::ok();
}

// CEP:WHAT: Binds a label and validates pending gotos against scope entry.
// CEP:WHY: The language forbids jumping into the scope of a local declared
//          between the goto and its label; the live-local ordinal encodes
//          that check exactly.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for duplicate labels and scope-entry
//          violations.
// CEP:ASSUMES: Labels bind once per unit.
// CEP:COST: cold; one bind per label.
// CEP:EVIDENCE: goto examples including scope-error cases run in tests.
vortex::support::Result<void> Emitter::emit_label(const Stmt& s) {
    if (st.label_locals.find(s.label) != st.label_locals.end()) {
        return support::fail(support::ErrorCode::SyntaxError,
                    "label '" + s.label + "' already defined", s.line);
    }
    auto it = st.label_names.find(s.label);
    std::string name =
        it != st.label_names.end()
            ? it->second
            : fresh_label(s.label);
    st.label_names[s.label] = name;
    st.label_locals[s.label] = st.locals_declared;
    std::vector<EmitState::PendingGoto> remaining;
    for (const auto& p : st.pending_gotos) {
        if (p.label != s.label) {
            remaining.push_back(p);
            continue;
        }
        if (p.locals_count < st.locals_declared) {
            return support::fail(support::ErrorCode::SyntaxError,
                        "goto for label '" + s.label +
                            "' jumps into the scope of a local",
                        p.line);
        }
    }
    st.pending_gotos = std::move(remaining);
    st.builder->bind_label(name);
    return vortex::support::ok();
}

// CEP:WHAT: Resolves a name against the scope stack.
// CEP:WHY: Own slots, captures, globals, and the two native modules are the
//          only name kinds; this is the single place that decides.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a None resolution only for internal errors; unknown
//          names resolve as globals (runtime nil).
// CEP:ASSUMES: Frames carry owner unit ids matching the analysis.
// CEP:COST: cold; linear in the frame depth.
// CEP:EVIDENCE: every name reference in the corpus resolves here.
NameResolution Emitter::resolve_name(const std::string& name) {
    for (size_t i = st.scopes.size(); i-- > 0;) {
        auto it = st.scopes[i].names.find(name);
        if (it == st.scopes[i].names.end()) continue;
        NameResolution res;
        res.name = name;
        res.binding = it->second;
        if (st.scopes[i].unit_id == st.current_unit) {
            res.kind = NameResolution::Kind::OwnLocal;
            return res;
        }
        const FnUnit& u = st.units.units[st.current_unit];
        for (uint32_t c = 0; c < u.captures.size(); ++c) {
            const auto& cap = u.captures[c];
            if (cap.owner_unit == st.scopes[i].unit_id &&
                cap.local_index == it->second.local_index) {
                res.kind = NameResolution::Kind::Capture;
                res.capture_index = c;
                return res;
            }
        }
    }
    NameResolution res;
    res.name = name;
    if (name == "math") {
        res.kind = NameResolution::Kind::ModuleMath;
    } else if (name == "string") {
        res.kind = NameResolution::Kind::ModuleString;
    } else {
        res.kind = NameResolution::Kind::Global;
    }
    return res;
}

// CEP:WHAT: Declares a binding in the current frame with its register.
// CEP:WHY: Parameters land in the incoming argument registers (the engine
//          copies the call window there); declared locals take the slots
//          after them, in analysis-matching order.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: The unit's local list declares parameters first, in the same
//          order as the analysis pass.
// CEP:COST: cold; one map insert per declaration.
// CEP:EVIDENCE: layout parity is exercised by every executed test.
EmittedBinding* Emitter::declare_binding(const std::string& name,
                                         uint32_t fn_unit, bool is_const) {
    const FnUnit& u = st.units.units[st.current_unit];
    const UnitLayout& l = layouts[st.current_unit];
    const uint32_t local_index = unit_slot_cursor;
    ++unit_slot_cursor;
    ++st.locals_declared;
    EmittedBinding b;
    if (local_index < u.param_count && !u.is_chunk) {
        // Parameter register: escaping units keep v0 for the environment
        // object; non-escaping units start parameters at v0.
        const uint16_t param_base = l.escaping ? 1 : 0;
        b.reg = static_cast<uint16_t>(param_base + local_index);
    } else {
        const uint32_t locals_index = local_index - u.param_count;
        b.reg = static_cast<uint16_t>(l.locals_base + locals_index);
    }
    b.owner_unit = st.current_unit;
    b.local_index = local_index;
    b.boxed = local_index < u.locals.size() && u.locals[local_index].boxed;
    b.is_const = is_const;
    b.fn_unit = fn_unit;
    auto it = st.scopes.back().names.emplace(name, b);
    return &it.first->second;
}

// CEP:WHAT: Returns the box register (or loads the box field) for a capture.
// CEP:WHY: Captures live in hidden parameters or environment-object fields
//          depending on the unit's convention.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic if the capture index is out of range
//          (internal inconsistency).
// CEP:ASSUMES: The index was produced by resolve_name.
// CEP:COST: cold; one load per access for escaping units.
// CEP:EVIDENCE: upvalue examples read and write shared cells correctly.
vortex::support::Result<uint16_t> Emitter::capture_box_reg(uint32_t index,
                                                           uint32_t line) {
    const UnitLayout& l = layouts[st.current_unit];
    if (l.escaping) {
        if (index >= l.box_tokens.size()) {
            return vortex::support::fail(
                vortex::support::ErrorCode::SyntaxError,
                "capture index out of range", line);
        }
        const uint16_t box = st.temp_reg();
        st.builder->emit(vortex::ugb::Op::GET_FIELD, box, {0}, true,
                         l.box_tokens[index]);
        return box;
    }
    if (index >= l.capture_regs.size()) {
        return vortex::support::fail(
            vortex::support::ErrorCode::SyntaxError,
            "capture index out of range", line);
    }
    return l.capture_regs[index];
}

// CEP:WHAT: Reads a resolved binding into a value.
// CEP:WHY: Boxed slots read through the cell; globals read through the
//          environment hook; both return a temp the caller owns.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for missing captures (internal).
// CEP:ASSUMES: The resolution came from resolve_name.
// CEP:COST: cold; one load per read.
// CEP:EVIDENCE: variable examples across the corpus read correctly.
vortex::support::Result<Val> Emitter::read_binding(const NameResolution& res,
                                                   uint32_t line) {
    switch (res.kind) {
    case NameResolution::Kind::OwnLocal: {
        if (res.binding.boxed) {
            const uint16_t zero = st.temp_reg();
            st.builder->const_i32(zero, 0);
            const uint16_t out = st.temp_reg();
            st.builder->emit(vortex::ugb::Op::ARRAY_GET, out,
                             {res.binding.reg, zero});
            Val v;
            v.reg = out;
            return v;
        }
        Val v;
        v.reg = res.binding.reg;
        return v;
    }
    case NameResolution::Kind::Capture: {
        auto box = capture_box_reg(res.capture_index, line);
        if (!box) return std::unexpected(box.error());
        const uint16_t zero = st.temp_reg();
        st.builder->const_i32(zero, 0);
        const uint16_t out = st.temp_reg();
        st.builder->emit(vortex::ugb::Op::ARRAY_GET, out, {*box, zero});
        Val v;
        v.reg = out;
        return v;
    }
    case NameResolution::Kind::Global: {
        const uint16_t id_reg = st.temp_reg();
        st.builder->const_i32(
            id_reg,
            static_cast<int32_t>(st.module.intern_string(res.name)));
        Val env;
        env.reg = st.env_reg;
        Val id_val;
        id_val.reg = id_reg;
        const uint16_t out = st.temp_reg();
        auto call = emit_builtin_call(out, kBGetEnv, {env, id_val});
        if (!call) return std::unexpected(call.error());
        Val v;
        v.reg = out;
        return v;
    }
    default: break;
    }
    return vortex::support::fail(vortex::support::ErrorCode::SyntaxError,
                                 "name is not a readable value", line);
}

// CEP:WHAT: Stores a value into a resolved variable target.
// CEP:WHY: Writes go through cells for boxed slots and through the
//          environment hook for globals.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for missing captures (internal).
// CEP:ASSUMES: The resolution came from resolve_name in an assignable kind.
// CEP:COST: cold; one store per write.
// CEP:EVIDENCE: assignment examples across the corpus store correctly.
vortex::support::Result<void> Emitter::store_local(const NameResolution& res,
                                                   uint16_t value_reg) {
    switch (res.kind) {
    case NameResolution::Kind::OwnLocal: {
        if (res.binding.boxed) {
            const uint16_t zero = st.temp_reg();
            st.builder->const_i32(zero, 0);
            st.builder->emit(vortex::ugb::Op::ARRAY_SET, 0,
                             {res.binding.reg, zero, value_reg});
        } else {
            st.builder->emit(vortex::ugb::Op::MOVE, res.binding.reg,
                             {value_reg});
        }
        return vortex::support::ok();
    }
    case NameResolution::Kind::Capture: {
        auto box = capture_box_reg(res.capture_index, 0);
        if (!box) return std::unexpected(box.error());
        const uint16_t zero = st.temp_reg();
        st.builder->const_i32(zero, 0);
        st.builder->emit(vortex::ugb::Op::ARRAY_SET, 0,
                         {*box, zero, value_reg});
        return vortex::support::ok();
    }
    case NameResolution::Kind::Global: {
        const uint16_t id_reg = st.temp_reg();
        st.builder->const_i32(
            id_reg,
            static_cast<int32_t>(st.module.intern_string(res.name)));
        Val env;
        env.reg = st.env_reg;
        Val id_val;
        id_val.reg = id_reg;
        Val value;
        value.reg = value_reg;
        const uint16_t set_dst = st.temp_reg();
        return emit_builtin_call(set_dst, kBSetEnv, {env, id_val, value});
    }
    default: break;
    }
    return vortex::support::fail(vortex::support::ErrorCode::SyntaxError,
                                 "name is not an assignable slot", 0);
}

// CEP:WHAT: Mints a unique label name inside the current unit.
// CEP:WHY: Builder labels are global per method, so hints need disambiguation.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: none.
// CEP:COST: cold; string concatenation per label.
// CEP:EVIDENCE: every loop and branch in the corpus mints labels here.
std::string Emitter::fresh_label(const std::string& hint) {
    return "L" + std::to_string(st.label_counter++) + "_" + hint;
}

}  // namespace emit_internal
}  // namespace luavortex
