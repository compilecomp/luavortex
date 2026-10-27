// CEP:FILE: src/emitter_emit_expr.cpp
// CEP:WHAT: The value half of the lowering pass: literals, static-type fast
//           paths, the metatable access protocols, calls, and function
//           values.
// CEP:WHY: Expression lowering decides where the engine's typed opcodes are
//           safe and where the runtime hooks must carry exact language
//           semantics; keeping that decision in one file makes the fast/slow
//           boundary auditable.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for the documented conformance gaps
//          (varargs, module values). Container growth throws
//          std::bad_alloc, which is fatal at the driver boundary.
// CEP:ASSUMES: Static types are conservative: Int values are engine Smis and
//          Float values are engine boxed doubles.
// CEP:COST: cold; linear in the expression size.
// CEP:EVIDENCE: operators, calls, and metatable examples execute in CI.
#include "emitter_internal.hpp"

namespace luavortex {
namespace emit_internal {

namespace support = vortex::support;

// CEP:WHAT: Emits a string-literal value from the module constant pool.
// CEP:WHY: The T0 conformance target executes no string-materializing
//          opcode, so literals travel as pool ids and materialize through
//          the lua.conststr hook.
// CEP:STATUS: complete
// CEP:FAILURE: none (pool interning is deterministic).
// CEP:ASSUMES: The runtime hook has access to the module constant pool.
// CEP:COST: cold; two instructions per literal.
// CEP:EVIDENCE: string examples print correctly in CI.
vortex::support::Result<Val> Emitter::emit_str_const(
    const std::string& text) {
    const uint16_t id_reg = st.temp_reg();
    st.builder->const_i32(
        id_reg, static_cast<int32_t>(st.module.intern_string(text)));
    Val id_val;
    id_val.reg = id_reg;
    const uint16_t out = st.temp_reg();
    auto call = emit_builtin_call(out, kBConstStr, {id_val});
    if (!call) return std::unexpected(call.error());
    Val v;
    v.reg = out;
    v.type = StType::Str;
    return v;
}

// CEP:WHAT: Calls a runtime builtin through a contiguous argument window.
// CEP:WHY: The engine's call ABI passes a contiguous frame window; moving
//          arguments together is the one rule every call site must honor.
// CEP:STATUS: complete
// CEP:FAILURE: none (argument count fits the headroom for valid programs).
// CEP:ASSUMES: Argument registers do not overlap the window base.
// CEP:COST: cold; one move per argument plus the call.
// CEP:EVIDENCE: every hook in the corpus routes through this helper.
vortex::support::Result<void> Emitter::emit_builtin_call(
    uint16_t dst, const char* name, const std::vector<Val>& args) {
    const uint16_t base = st.temp_reg();
    uint16_t cursor = base;
    for (const Val& a : args) {
        if (a.reg != cursor) {
            st.builder->emit(vortex::ugb::Op::MOVE, cursor, {a.reg});
        }
        ++cursor;
    }
    st.builder->call_builtin(dst, base, static_cast<uint16_t>(args.size()),
                             name);
    return vortex::support::ok();
}

// CEP:WHAT: Normalizes any value to an engine boolean via language
//           truthiness (only nil and false are falsy).
// CEP:WHY: The engine's branch truthiness treats Smi zero as falsy, which
//          contradicts the language; conditions must normalize first.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: The value register is already computed.
// CEP:COST: cold; up to five instructions per normalization.
// CEP:EVIDENCE: zero-valued conditions branch correctly in tests.
Val Emitter::normalize_truthy(Val v) {
    if (v.is_bool) return v;
    const uint16_t out = st.temp_reg();
    const uint16_t nil_check = st.temp_reg();
    const uint16_t false_reg = st.temp_reg();
    const uint16_t false_check = st.temp_reg();
    const std::string l_false = fresh_label("truthy_false");
    const std::string l_end = fresh_label("truthy_end");
    st.builder->emit(vortex::ugb::Op::EQ_NULL, nil_check, {v.reg});
    st.builder->jump_true(nil_check, l_false);
    st.builder->const_false(false_reg);
    st.builder->emit(vortex::ugb::Op::EQ_REF, false_check, {v.reg, false_reg});
    st.builder->jump_true(false_check, l_false);
    st.builder->const_true(out);
    st.builder->jump(l_end);
    st.builder->bind_label(l_false);
    st.builder->const_false(out);
    st.builder->bind_label(l_end);
    Val result;
    result.reg = out;
    result.type = StType::Bool;
    result.is_bool = true;
    return result;
}

// CEP:WHAT: Emits an expression and normalizes it to a condition boolean.
// CEP:WHY: All condition positions share one truthiness contract.
// CEP:STATUS: complete
// CEP:FAILURE: Propagates expression diagnostics.
// CEP:ASSUMES: none.
// CEP:COST: cold; the normalization cost above.
// CEP:EVIDENCE: every branch in the corpus normalizes here.
vortex::support::Result<Val> Emitter::emit_truthy(const Expr& e) {
    auto v = emit_expr(e);
    if (!v) return v;
    return normalize_truthy(*v);
}

// CEP:WHAT: Loads a table element with the full metatable protocol.
// CEP:WHY: Missing raw reads must walk the __index chain, calling function
//          metamethods through Call.Virtual, exactly as the language
//          specifies.
// CEP:STATUS: complete
// CEP:FAILURE: Cyclic __index chains loop (documented); hooks raise runtime
//          errors for malformed metatables.
// CEP:ASSUMES: The raw-get hook never allocates while holding element
//          pointers (GC-safety contract of the runtime module).
// CEP:COST: cold; the fast path is one call, the chain adds per-level work.
// CEP:EVIDENCE: __index table and function examples execute correctly.
vortex::support::Result<Val> Emitter::emit_index_load(Val obj, Val key) {
    const uint16_t obj_reg = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::MOVE, obj_reg, {obj.reg});
    const uint16_t key_reg = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::MOVE, key_reg, {key.reg});
    Val obj_val;
    obj_val.reg = obj_reg;
    Val key_val;
    key_val.reg = key_reg;

    const uint16_t out = st.temp_reg();

    const std::string l_done = fresh_label("idx_done");
    const std::string l_walk = fresh_label("idx_walk");
    const std::string l_have_mt = fresh_label("idx_mt");
    const std::string l_have_h = fresh_label("idx_h");
    const std::string l_is_table = fresh_label("idx_table");

    // The raw read is the loop head: every __index table rebinds the
    // receiver and re-reads the key from the top.
    st.builder->bind_label(l_walk);
    auto raw = emit_builtin_call(out, kBRawGet, {obj_val, key_val});
    if (!raw) return std::unexpected(raw.error());

    const uint16_t nil_check = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::EQ_NULL, nil_check, {out});
    st.builder->jump_false(nil_check, l_done);

    const uint16_t mt = st.temp_reg();
    auto mt_call = emit_builtin_call(mt, kBGetMetatable, {obj_val});
    if (!mt_call) return std::unexpected(mt_call.error());
    const uint16_t mt_nil = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::EQ_NULL, mt_nil, {mt});
    st.builder->jump_false(mt_nil, l_have_mt);
    st.builder->jump(l_done);
    st.builder->bind_label(l_have_mt);

    auto index_key = emit_str_const(kIndexKey);
    if (!index_key) return std::unexpected(index_key.error());
    Val mt_val;
    mt_val.reg = mt;
    const uint16_t handler = st.temp_reg();
    auto h_call = emit_builtin_call(handler, kBRawGet, {mt_val, *index_key});
    if (!h_call) return std::unexpected(h_call.error());
    const uint16_t h_nil = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::EQ_NULL, h_nil, {handler});
    st.builder->jump_false(h_nil, l_have_h);
    st.builder->jump(l_done);
    st.builder->bind_label(l_have_h);

    Val h_val;
    h_val.reg = handler;
    const uint16_t isfn = st.temp_reg();
    auto fn_call = emit_builtin_call(isfn, kBIsFunction, {h_val});
    if (!fn_call) return std::unexpected(fn_call.error());
    st.builder->jump_false(isfn, l_is_table);

    const uint16_t window =
        static_cast<uint16_t>(st.units.uniform_arity);
    const uint16_t base = st.temp_reg();
    st.reserve_temps(static_cast<uint16_t>(window - 1));
    st.builder->emit(vortex::ugb::Op::MOVE, base, {handler});
    st.builder->emit(vortex::ugb::Op::MOVE,
                     static_cast<uint16_t>(base + kCallArgSelf), {obj_reg});
    st.builder->emit(vortex::ugb::Op::MOVE,
                     static_cast<uint16_t>(base + kCallArgKey), {key_reg});
    uint16_t pad = static_cast<uint16_t>(base + kCallArgKey + 1);
    while (pad < static_cast<uint16_t>(base + window)) {
        st.builder->const_null(pad);
        ++pad;
    }
    const uint16_t result = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::CALL_VIRTUAL, result, {base, window},
                     true, st.module.intern_method(kCallMethodName));
    st.builder->emit(vortex::ugb::Op::MOVE, out, {result});
    st.builder->jump(l_done);
    st.builder->bind_label(l_is_table);
    st.builder->emit(vortex::ugb::Op::MOVE, obj_reg, {handler});
    st.builder->jump(l_walk);

    st.builder->bind_label(l_done);
    Val v;
    v.reg = out;
    return v;
}

// CEP:WHAT: Stores a table element with the full __newindex protocol.
// CEP:WHY: Stores into missing keys must honor the metamethod chain before
//          falling back to the raw store.
// CEP:STATUS: complete
// CEP:FAILURE: Cyclic chains loop (documented).
// CEP:ASSUMES: The raw-set hook upholds the runtime GC-safety contract.
// CEP:COST: cold; one raw probe plus protocol work on misses.
// CEP:EVIDENCE: __newindex examples execute correctly.
vortex::support::Result<void> Emitter::emit_index_store(Val obj, Val key,
                                                        Val value) {
    const uint16_t obj_reg = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::MOVE, obj_reg, {obj.reg});
    const uint16_t key_reg = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::MOVE, key_reg, {key.reg});
    const uint16_t value_reg = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::MOVE, value_reg, {value.reg});
    Val obj_val;
    obj_val.reg = obj_reg;
    Val key_val;
    key_val.reg = key_reg;
    Val value_val;
    value_val.reg = value_reg;

    const std::string l_done = fresh_label("idxs_done");
    const std::string l_retry = fresh_label("idxs_retry");
    const std::string l_raw = fresh_label("idxs_raw");
    const std::string l_have_mt = fresh_label("idxs_mt");
    const std::string l_have_h = fresh_label("idxs_h");
    const std::string l_is_table = fresh_label("idxs_table");

    st.builder->bind_label(l_retry);
    const uint16_t probe = st.temp_reg();
    auto probe_call = emit_builtin_call(probe, kBRawGet, {obj_val, key_val});
    if (!probe_call) return std::unexpected(probe_call.error());
    const uint16_t probe_nil = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::EQ_NULL, probe_nil, {probe});
    st.builder->jump_false(probe_nil, l_raw);

    const uint16_t mt = st.temp_reg();
    auto mt_call = emit_builtin_call(mt, kBGetMetatable, {obj_val});
    if (!mt_call) return std::unexpected(mt_call.error());
    const uint16_t mt_nil = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::EQ_NULL, mt_nil, {mt});
    st.builder->jump_false(mt_nil, l_have_mt);
    st.builder->jump(l_raw);
    st.builder->bind_label(l_have_mt);

    auto newindex_key = emit_str_const(kNewIndexKey);
    if (!newindex_key) return std::unexpected(newindex_key.error());
    Val mt_val;
    mt_val.reg = mt;
    const uint16_t handler = st.temp_reg();
    auto h_call =
        emit_builtin_call(handler, kBRawGet, {mt_val, *newindex_key});
    if (!h_call) return std::unexpected(h_call.error());
    const uint16_t h_nil = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::EQ_NULL, h_nil, {handler});
    st.builder->jump_false(h_nil, l_have_h);
    st.builder->jump(l_raw);
    st.builder->bind_label(l_have_h);

    Val h_val;
    h_val.reg = handler;
    const uint16_t isfn = st.temp_reg();
    auto fn_call = emit_builtin_call(isfn, kBIsFunction, {h_val});
    if (!fn_call) return std::unexpected(fn_call.error());
    st.builder->jump_false(isfn, l_is_table);

    const uint16_t window =
        static_cast<uint16_t>(st.units.uniform_arity);
    const uint16_t base = st.temp_reg();
    st.reserve_temps(static_cast<uint16_t>(window - 1));
    st.builder->emit(vortex::ugb::Op::MOVE, base, {handler});
    st.builder->emit(vortex::ugb::Op::MOVE,
                     static_cast<uint16_t>(base + kCallArgSelf), {obj_reg});
    st.builder->emit(vortex::ugb::Op::MOVE,
                     static_cast<uint16_t>(base + kCallArgKey), {key_reg});
    st.builder->emit(vortex::ugb::Op::MOVE,
                     static_cast<uint16_t>(base + kCallArgValue),
                     {value_reg});
    uint16_t pad = static_cast<uint16_t>(base + kCallArgValue + 1);
    while (pad < static_cast<uint16_t>(base + window)) {
        st.builder->const_null(pad);
        ++pad;
    }
    const uint16_t discard = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::CALL_VIRTUAL, discard, {base, window},
                     true, st.module.intern_method(kCallMethodName));
    st.builder->jump(l_done);
    st.builder->bind_label(l_is_table);
    st.builder->emit(vortex::ugb::Op::MOVE, obj_reg, {handler});
    st.builder->jump(l_retry);

    st.builder->bind_label(l_raw);
    const uint16_t discard_raw = st.temp_reg();
    auto raw_set =
        emit_builtin_call(discard_raw, kBRawSet,
                          {obj_val, key_val, value_val});
    if (!raw_set) return std::unexpected(raw_set.error());
    st.builder->bind_label(l_done);
    return vortex::support::ok();
}

// CEP:WHAT: Emits the language equality comparison with fast paths.
// CEP:WHY: Reference equality covers primitives and identity; cross-type
//          numeric equality (1 == 1.0) and string equality need the hooks.
// CEP:STATUS: complete
// CEP:FAILURE: none (mixed non-numeric types simply compare unequal).
// CEP:ASSUMES: Static Int values are Smis; static Str values are objects.
// CEP:COST: cold; one instruction on the fast path.
// CEP:EVIDENCE: equality examples across types execute correctly.
vortex::support::Result<Val> Emitter::emit_lua_eq(Val a, Val b) {
    const uint16_t out = st.temp_reg();
    if (a.type == StType::Int && b.type == StType::Int) {
        st.builder->emit(vortex::ugb::Op::EQ_I32, out, {a.reg, b.reg});
        Val v;
        v.reg = out;
        v.type = StType::Bool;
        v.is_bool = true;
        return v;
    }
    if (a.type == StType::Str && b.type == StType::Str) {
        auto call = emit_builtin_call(out, kBStrEq, {a, b});
        if (!call) return std::unexpected(call.error());
        Val v;
        v.reg = out;
        v.type = StType::Bool;
        v.is_bool = true;
        return v;
    }
    if (a.type == StType::Bool && b.type == StType::Bool) {
        st.builder->emit(vortex::ugb::Op::EQ_REF, out, {a.reg, b.reg});
        Val v;
        v.reg = out;
        v.type = StType::Bool;
        v.is_bool = true;
        return v;
    }
    const std::string l_done = fresh_label("eq_done");
    const std::string l_true = fresh_label("eq_true");
    const std::string l_false = fresh_label("eq_false");
    const std::string l_check_b = fresh_label("eq_check_b");
    const std::string l_f64 = fresh_label("eq_f64");
    const uint16_t ref_eq = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::EQ_REF, ref_eq, {a.reg, b.reg});
    st.builder->jump_true(ref_eq, l_true);

    const uint16_t na = st.temp_reg();
    auto na_call = emit_builtin_call(na, kBNumF64, {a});
    if (!na_call) return std::unexpected(na_call.error());
    const uint16_t na_nil = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::EQ_NULL, na_nil, {na});
    st.builder->jump_false(na_nil, l_check_b);
    st.builder->jump(l_false);
    st.builder->bind_label(l_check_b);
    const uint16_t nb = st.temp_reg();
    auto nb_call = emit_builtin_call(nb, kBNumF64, {b});
    if (!nb_call) return std::unexpected(nb_call.error());
    const uint16_t nb_nil = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::EQ_NULL, nb_nil, {nb});
    st.builder->jump_false(nb_nil, l_f64);
    st.builder->jump(l_false);
    st.builder->bind_label(l_f64);
    Val na_val;
    na_val.reg = na;
    Val nb_val;
    nb_val.reg = nb;
    const uint16_t f64_eq = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::EQ_F64, f64_eq, {na, nb});
    st.builder->jump_true(f64_eq, l_true);
    st.builder->jump(l_false);

    st.builder->bind_label(l_true);
    st.builder->const_true(out);
    st.builder->jump(l_done);
    st.builder->bind_label(l_false);
    st.builder->const_false(out);
    st.builder->bind_label(l_done);
    Val v;
    v.reg = out;
    v.type = StType::Bool;
    v.is_bool = true;
    return v;
}

// CEP:WHAT: Emits an ordering comparison with typed and hook paths.
// CEP:WHY: Numbers compare numerically, strings lexicographically, and
//          mixed operands must raise the language's ordering error.
// CEP:STATUS: complete
// CEP:FAILURE: The comparison hooks raise runtime errors for unorderable
//          operands.
// CEP:ASSUMES: Static Float registers hold boxed doubles (the typed float
//          compare requires them).
// CEP:COST: cold; one instruction on the static path.
// CEP:EVIDENCE: ordering examples across types execute correctly.
vortex::support::Result<Val> Emitter::emit_compare(const Expr& e) {
    auto l = emit_expr(*e.lhs);
    if (!l) return l;
    auto r = emit_expr(*e.rhs);
    if (!r) return r;
    const std::string op = e.op;
    const bool is_lt = op == "<";
    const bool is_le = op == "<=";
    const bool is_gt = op == ">";
    const bool is_ge = op == ">=";

    const uint16_t out = st.temp_reg();
    Val a = *l;
    Val b = *r;
    if (a.type == StType::Int && b.type == StType::Int) {
        vortex::ugb::Op opcode = vortex::ugb::Op::LT_S_I64;
        if (is_lt) opcode = vortex::ugb::Op::LT_S_I64;
        if (is_le) opcode = vortex::ugb::Op::LE_S_I64;
        if (is_gt) opcode = vortex::ugb::Op::GT_S_I64;
        if (is_ge) opcode = vortex::ugb::Op::GE_S_I64;
        st.builder->emit(opcode, out, {a.reg, b.reg});
        Val v;
        v.reg = out;
        v.type = StType::Bool;
        v.is_bool = true;
        return v;
    }
    if ((a.type == StType::Float || a.type == StType::Int) &&
        (b.type == StType::Float || b.type == StType::Int)) {
        // Normalize integer sides to boxed doubles for the typed compare.
        Val fa = a;
        Val fb = b;
        if (a.type == StType::Int) {
            const uint16_t boxed = st.temp_reg();
            st.builder->emit(vortex::ugb::Op::I64_TO_F64, boxed, {a.reg});
            fa.reg = boxed;
            fa.type = StType::Float;
        }
        if (b.type == StType::Int) {
            const uint16_t boxed = st.temp_reg();
            st.builder->emit(vortex::ugb::Op::I64_TO_F64, boxed, {b.reg});
            fb.reg = boxed;
            fb.type = StType::Float;
        }
        vortex::ugb::Op opcode = vortex::ugb::Op::LT_F64;
        if (is_lt) opcode = vortex::ugb::Op::LT_F64;
        if (is_le) opcode = vortex::ugb::Op::LE_F64;
        if (is_gt) opcode = vortex::ugb::Op::GT_F64;
        if (is_ge) opcode = vortex::ugb::Op::GE_F64;
        st.builder->emit(opcode, out, {fa.reg, fb.reg});
        Val v;
        v.reg = out;
        v.type = StType::Bool;
        v.is_bool = true;
        return v;
    }
    if (a.type == StType::Str && b.type == StType::Str) {
        const char* hook = (is_lt || is_gt) ? kBStrLt : kBStrLe;
        if (is_gt || is_ge) {
            // Descending forms swap the operands of the ascending hooks.
            auto call = emit_builtin_call(out, hook, {b, a});
            if (!call) return std::unexpected(call.error());
        } else {
            auto call = emit_builtin_call(out, hook, {a, b});
            if (!call) return std::unexpected(call.error());
        }
        Val v;
        v.reg = out;
        v.type = StType::Bool;
        v.is_bool = true;
        return v;
    }
    const char* hook = (is_lt || is_gt) ? kBOpLt : kBOpLe;
    if (is_gt || is_ge) {
        auto call = emit_builtin_call(out, hook, {b, a});
        if (!call) return std::unexpected(call.error());
    } else {
        auto call = emit_builtin_call(out, hook, {a, b});
        if (!call) return std::unexpected(call.error());
    }
    Val v;
    v.reg = out;
    v.type = StType::Bool;
    v.is_bool = true;
    return v;
}

// CEP:WHAT: Emits arithmetic, concat, and bitwise binaries.
// CEP:WHY: Static numeric knowledge selects engine opcodes; everything else
//          routes through hooks that implement coercion and error rules.
// CEP:STATUS: partial
// CEP:OWNER: luavortex-maintainers
// CEP:TICKET: compilecomp/luavortex#2
// CEP:FAILURE: Arithmetic hooks raise the language's runtime errors
//          (non-numeric operands, zero step, Smi-range overflow).
// CEP:ASSUMES: Static Int values are Smis; static Float values are boxed
//          doubles; integer overflow traps per the documented conformance
//          deviation.
// CEP:COST: cold; one instruction on static numeric paths.
// CEP:EVIDENCE: arithmetic examples across the corpus execute correctly.
vortex::support::Result<Val> Emitter::emit_arith(const Expr& e) {
    auto l = emit_expr(*e.lhs);
    if (!l) return l;
    auto r = emit_expr(*e.rhs);
    if (!r) return r;
    Val a = *l;
    Val b = *r;
    const std::string op = e.op;
    const uint16_t out = st.temp_reg();

    const bool a_num = a.type == StType::Int || a.type == StType::Float;
    const bool b_num = b.type == StType::Int || b.type == StType::Float;
    const bool any_float = a.type == StType::Float || b.type == StType::Float;

    if (op == "+") {
        if (a_num && b_num && !any_float) {
            st.builder->emit(vortex::ugb::Op::ADD_ANY, out, {a.reg, b.reg});
            Val v;
            v.reg = out;
            v.type = StType::Int;
            return v;
        }
        if (a_num && b_num) {
            to_float(a);
            to_float(b);
            st.builder->emit(vortex::ugb::Op::ADD_F64, out, {a.reg, b.reg});
            Val v;
            v.reg = out;
            v.type = StType::Float;
            return v;
        }
        auto call = emit_builtin_call(out, kBOpAdd, {a, b});
        if (!call) return std::unexpected(call.error());
        Val v;
        v.reg = out;
        return v;
    }
    if (op == "-") {
        if (a_num && b_num && !any_float) {
            st.builder->emit(vortex::ugb::Op::SUB_ANY, out, {a.reg, b.reg});
            Val v;
            v.reg = out;
            v.type = StType::Int;
            return v;
        }
        if (a_num && b_num) {
            to_float(a);
            to_float(b);
            st.builder->emit(vortex::ugb::Op::SUB_F64, out, {a.reg, b.reg});
            Val v;
            v.reg = out;
            v.type = StType::Float;
            return v;
        }
        auto call = emit_builtin_call(out, kBOpSub, {a, b});
        if (!call) return std::unexpected(call.error());
        Val v;
        v.reg = out;
        return v;
    }
    if (op == "*") {
        if (a_num && b_num && !any_float) {
            st.builder->emit(vortex::ugb::Op::MUL_ANY, out, {a.reg, b.reg});
            Val v;
            v.reg = out;
            v.type = StType::Int;
            return v;
        }
        if (a_num && b_num) {
            to_float(a);
            to_float(b);
            st.builder->emit(vortex::ugb::Op::MUL_F64, out, {a.reg, b.reg});
            Val v;
            v.reg = out;
            v.type = StType::Float;
            return v;
        }
        auto call = emit_builtin_call(out, kBOpMul, {a, b});
        if (!call) return std::unexpected(call.error());
        Val v;
        v.reg = out;
        return v;
    }
    if (op == "/") {
        if (a_num && b_num) {
            to_float(a);
            to_float(b);
            st.builder->emit(vortex::ugb::Op::DIV_F64, out, {a.reg, b.reg});
            Val v;
            v.reg = out;
            v.type = StType::Float;
            return v;
        }
        auto call = emit_builtin_call(out, kBOpDiv, {a, b});
        if (!call) return std::unexpected(call.error());
        Val v;
        v.reg = out;
        return v;
    }
    const char* hook = nullptr;
    if (op == "//") hook = kBOpFloordiv;
    if (op == "%") hook = kBOpMod;
    if (op == "^") hook = kBOpPow;
    if (op == "&") hook = kBOpBand;
    if (op == "|") hook = kBOpBor;
    if (op == "~") hook = kBOpBxor;
    if (op == "<<") hook = kBOpShl;
    if (op == ">>") hook = kBOpShr;
    if (op == "..") hook = kBConcat;
    if (hook != nullptr) {
        auto call = emit_builtin_call(out, hook, {a, b});
        if (!call) return std::unexpected(call.error());
        Val v;
        v.reg = out;
        v.type = op == ".." ? StType::Str : StType::Unknown;
        return v;
    }
    return vortex::support::fail(vortex::support::ErrorCode::SyntaxError,
                                 "unhandled binary operator '" + op + "'",
                                 e.line);
}

// CEP:WHAT: Converts a static numeric value in place to a boxed double.
// CEP:WHY: The typed float opcodes require boxed double operands; integer
//          sides convert at the boundary.
// CEP:STATUS: complete
// CEP:FAILURE: none (static Int registers hold Smis by construction).
// CEP:ASSUMES: The value type is Int or Float.
// CEP:COST: cold; one conversion instruction for integer sides.
// CEP:EVIDENCE: mixed arithmetic examples execute correctly.
void Emitter::to_float(Val& v) {
    if (v.type == StType::Float) return;
    const uint16_t boxed = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::I64_TO_F64, boxed, {v.reg});
    v.reg = boxed;
    v.type = StType::Float;
}

// CEP:WHAT: Emits the short-circuit and/or forms with value semantics.
// CEP:WHY: The operators return operand values, not booleans, and the right
//          operand must not evaluate unless the left one calls for it.
// CEP:STATUS: complete
// CEP:FAILURE: Propagates operand diagnostics.
// CEP:ASSUMES: none.
// CEP:COST: cold; one branch plus a move.
// CEP:EVIDENCE: short-circuit examples run in the corpus.
vortex::support::Result<Val> Emitter::emit_short_circuit(const Expr& e) {
    const bool is_and = e.op == "and";
    auto l = emit_expr(*e.lhs);
    if (!l) return l;
    Val norm = normalize_truthy(*l);
    const std::string l_other = fresh_label("sc_other");
    const std::string l_end = fresh_label("sc_end");
    const uint16_t out = st.temp_reg();
    if (is_and) {
        st.builder->jump_false(norm.reg, l_other);
    } else {
        st.builder->jump_true(norm.reg, l_other);
    }
    auto r = emit_expr(*e.rhs);
    if (!r) return r;
    st.builder->emit(vortex::ugb::Op::MOVE, out, {r->reg});
    st.builder->jump(l_end);
    st.builder->bind_label(l_other);
    st.builder->emit(vortex::ugb::Op::MOVE, out, {l->reg});
    st.builder->bind_label(l_end);
    Val v;
    v.reg = out;
    return v;
}

// CEP:WHAT: Emits equality and inequality.
// CEP:WHY: Equality has its own protocol; inequality inverts the result.
// CEP:STATUS: complete
// CEP:FAILURE: Propagates operand diagnostics.
// CEP:ASSUMES: none.
// CEP:COST: cold; equality cost plus one branch for the inversion.
// CEP:EVIDENCE: equality examples run in the corpus.
vortex::support::Result<Val> Emitter::emit_equality(const Expr& e) {
    auto l = emit_expr(*e.lhs);
    if (!l) return l;
    auto r = emit_expr(*e.rhs);
    if (!r) return r;
    auto eq = emit_lua_eq(*l, *r);
    if (!eq) return eq;
    if (e.op == "==") return eq;
    const uint16_t inv = st.temp_reg();
    const std::string l_true = fresh_label("ne_true");
    const std::string l_end = fresh_label("ne_end");
    st.builder->jump_true(eq->reg, l_true);
    st.builder->const_true(inv);
    st.builder->jump(l_end);
    st.builder->bind_label(l_true);
    st.builder->const_false(inv);
    st.builder->bind_label(l_end);
    Val v;
    v.reg = inv;
    v.type = StType::Bool;
    v.is_bool = true;
    return v;
}

// CEP:WHAT: Emits unary operators.
// CEP:WHY: Negation has typed paths; length and bitwise-not carry exact
//          language rules in their hooks.
// CEP:STATUS: complete
// CEP:FAILURE: Hooks raise runtime errors for invalid operands.
// CEP:ASSUMES: Static Float registers hold boxed doubles.
// CEP:COST: cold; one instruction on typed paths.
// CEP:EVIDENCE: unary examples execute correctly.
vortex::support::Result<Val> Emitter::emit_unary(const Expr& e) {
    if (e.op == "not") {
        auto v = emit_expr(*e.lhs);
        if (!v) return v;
        Val norm = normalize_truthy(*v);
        const uint16_t out = st.temp_reg();
        const std::string l_false = fresh_label("not_false");
        const std::string l_end = fresh_label("not_end");
        st.builder->jump_true(norm.reg, l_false);
        st.builder->const_true(out);
        st.builder->jump(l_end);
        st.builder->bind_label(l_false);
        st.builder->const_false(out);
        st.builder->bind_label(l_end);
        Val result;
        result.reg = out;
        result.type = StType::Bool;
        result.is_bool = true;
        return result;
    }
    auto v = emit_expr(*e.lhs);
    if (!v) return v;
    Val a = *v;
    const uint16_t out = st.temp_reg();
    if (e.op == "-") {
        if (a.type == StType::Int) {
            const uint16_t zero = st.temp_reg();
            st.builder->const_i32(zero, 0);
            st.builder->emit(vortex::ugb::Op::SUB_ANY, out, {zero, a.reg});
            Val result;
            result.reg = out;
            result.type = StType::Int;
            return result;
        }
        if (a.type == StType::Float) {
            st.builder->emit(vortex::ugb::Op::NEG_F64, out, {a.reg});
            Val result;
            result.reg = out;
            result.type = StType::Float;
            return result;
        }
        auto call = emit_builtin_call(out, kBOpNeg, {a});
        if (!call) return std::unexpected(call.error());
        Val result;
        result.reg = out;
        return result;
    }
    if (e.op == "#") {
        auto call = emit_builtin_call(out, kBLength, {a});
        if (!call) return std::unexpected(call.error());
        Val result;
        result.reg = out;
        result.type = StType::Int;
        return result;
    }
    // Bitwise not.
    auto call = emit_builtin_call(out, kBOpBnot, {a});
    if (!call) return std::unexpected(call.error());
    Val result;
    result.reg = out;
    return result;
}

// CEP:WHAT: Constructs a function value object for an escaping unit.
// CEP:WHY: The object's klass carries the call method; its declared fields
//          carry the environment table and capture boxes.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic if a capture is not resolvable at the
//          creation site (internal inconsistency).
// CEP:ASSUMES: The unit is escaping and its class tokens were interned.
// CEP:COST: cold; one allocation plus one store per captured slot.
// CEP:EVIDENCE: function-value examples invoke through Call.Virtual.
vortex::support::Result<Val> Emitter::emit_function_value(uint32_t unit_id) {
    const FnUnit& u = st.units.units[unit_id];
    const UnitLayout& l = layouts[unit_id];
    const uint16_t obj = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::NEW_OBJECT, obj, {}, true,
                     l.class_token);
    st.builder->emit(vortex::ugb::Op::SET_FIELD, 0, {obj, st.env_reg}, true,
                     l.env_field_token);
    for (uint32_t i = 0; i < u.captures.size(); ++i) {
        const auto& cap = u.captures[i];
        EmittedBinding binding;
        binding.reg = 0;
        bool found = false;
        for (size_t s = st.scopes.size(); s-- > 0 && !found;) {
            for (const auto& [name, b] : st.scopes[s].names) {
                (void)name;
                if (b.owner_unit == cap.owner_unit &&
                    b.local_index == cap.local_index) {
                    binding = b;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            return vortex::support::fail(
                vortex::support::ErrorCode::SyntaxError,
                "capture is not visible at the creation site", 0);
        }
        st.builder->emit(vortex::ugb::Op::SET_FIELD, 0, {obj, binding.reg},
                         true, l.box_tokens[i]);
    }
    Val v;
    v.reg = obj;
    return v;
}

// CEP:WHAT: Reads a native module member as a value.
// CEP:WHY: Numeric constants (math.pi, math.huge) are hook reads; function
//          members have no value form on this conformance target.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for function-valued module members.
// CEP:ASSUMES: The property is a constant or hook-readable member.
// CEP:COST: cold; one call per read.
// CEP:EVIDENCE: math constant examples execute correctly.
vortex::support::Result<Val> Emitter::emit_module_read(
    const NameResolution& res, const std::string& prop) {
    if (res.kind == NameResolution::Kind::ModuleString) {
        return support::fail(support::ErrorCode::SyntaxError,
                    "the string library has no value members; call "
                    "string functions directly",
                    0);
    }
    const std::string builtin = std::string(kBMathPrefix) + prop;
    const uint16_t out = st.temp_reg();
    auto call = emit_builtin_call(out, builtin.c_str(), {});
    if (!call) return std::unexpected(call.error());
    Val v;
    v.reg = out;
    return v;
}

// CEP:WHAT: Invokes a function value with the callable check.
// CEP:WHY: Value calls dispatch through the receiver klass's call method;
//          non-callable receivers must raise the language's call error.
// CEP:STATUS: complete
// CEP:FAILURE: The call-error hook raises a runtime error for non-functions.
// CEP:ASSUMES: The argument window fits the layout headroom.
// CEP:COST: cold; one check plus the virtual call.
// CEP:EVIDENCE: function-value examples invoke correctly end to end.
vortex::support::Result<Val> Emitter::emit_value_call(
    const Val& callee, const std::vector<Val>& args, bool check_callable) {
    if (check_callable) {
        const uint16_t isfn = st.temp_reg();
        auto check = emit_builtin_call(isfn, kBIsFunction, {callee});
        if (!check) return std::unexpected(check.error());
        const std::string l_call = fresh_label("call_fn");
        st.builder->jump_true(isfn, l_call);
        const uint16_t err_dst = st.temp_reg();
        auto err = emit_builtin_call(err_dst, kBCallError, {callee});
        if (!err) return std::unexpected(err.error());
        st.builder->bind_label(l_call);
    }
    // The uniform window: receiver, arguments, then nil padding so the
    // declared arity of every callee matches the verifier's arity law. The
    // whole window is reserved up front so padding cannot alias temporaries.
    const uint16_t window =
        static_cast<uint16_t>(st.units.uniform_arity);
    const uint16_t base = st.temp_reg();
    st.reserve_temps(static_cast<uint16_t>(window - 1));
    st.builder->emit(vortex::ugb::Op::MOVE, base, {callee.reg});
    uint16_t cursor = static_cast<uint16_t>(base + kValueCallBase);
    for (const Val& a : args) {
        if (cursor >= static_cast<uint16_t>(base + window)) break;
        if (a.reg != cursor) {
            st.builder->emit(vortex::ugb::Op::MOVE, cursor, {a.reg});
        }
        ++cursor;
    }
    while (cursor < static_cast<uint16_t>(base + window)) {
        st.builder->const_null(cursor);
        ++cursor;
    }
    const uint16_t out = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::CALL_VIRTUAL, out, {base, window}, true,
                     st.module.intern_method(kCallMethodName));
    Val v;
    v.reg = out;
    return v;
}

// CEP:WHAT: Emits a call expression of both call forms.
// CEP:WHY: Direct calls to provably-static functions skip the value
//          protocol; everything else goes through the callable check.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for module names used as callees.
// CEP:ASSUMES: Direct-call eligibility matches the analysis decisions.
// CEP:COST: cold; direct path is one call, value path adds the check.
// CEP:EVIDENCE: call examples across the corpus execute correctly.
vortex::support::Result<Val> Emitter::emit_call(const Expr& e) {
    const Expr& callee = *e.lhs;

    // Native module member call: math.floor(x), string.rep(s, n).
    if (callee.kind == ExprKind::Index &&
        callee.lhs->kind == ExprKind::Name &&
        callee.rhs->kind == ExprKind::Str) {
        const NameResolution base_res = resolve_name(callee.lhs->name);
        if (base_res.kind == NameResolution::Kind::ModuleMath ||
            base_res.kind == NameResolution::Kind::ModuleString) {
            const char* prefix = base_res.kind ==
                                         NameResolution::Kind::ModuleMath
                                     ? kBMathPrefix
                                     : kBStringPrefix;
            std::vector<Val> args;
            for (const auto& a : e.args) {
                auto v = emit_expr(*a);
                if (!v) return v;
                args.push_back(*v);
            }
            const uint16_t out = st.temp_reg();
            auto call = emit_builtin_call(
                out,
                (std::string(prefix) + callee.rhs->str_value).c_str(),
                args);
            if (!call) return std::unexpected(call.error());
            Val v;
            v.reg = out;
            return v;
        }
    }

    std::vector<Val> args;
    for (const auto& a : e.args) {
        auto v = emit_expr(*a);
        if (!v) return v;
        args.push_back(*v);
    }

    if (callee.kind == ExprKind::Name) {
        const NameResolution res = resolve_name(callee.name);
        if (res.kind == NameResolution::Kind::ModuleMath ||
            res.kind == NameResolution::Kind::ModuleString) {
            return support::fail(support::ErrorCode::SyntaxError,
                        "module '" + callee.name +
                            "' is not callable; index one of its members",
                        e.line);
        }
        if (res.kind == NameResolution::Kind::Global) {
            // Reserved native globals call their hooks directly; their
            // names are reserved and locals shadow them.
            if (const char* hook = native_hook_for(callee.name)) {
                const uint16_t out = st.temp_reg();
                auto call = emit_builtin_call(out, hook, args);
                if (!call) return std::unexpected(call.error());
                Val v;
                v.reg = out;
                return v;
            }
        }
        if (res.kind == NameResolution::Kind::OwnLocal ||
            res.kind == NameResolution::Kind::Capture) {
            if (res.binding.fn_unit != kNoUnit) {
                const FnUnit& fn = st.units.units[res.binding.fn_unit];
                const bool direct = !fn.escaping && !res.binding.reassigned &&
                                    res.binding.owner_unit == st.current_unit;
                if (direct) {
                    // Window layout matches the callee convention exactly:
                    // parameters, environment, capture cells; short calls
                    // pad with nil and surplus arguments drop (verifier's
                    // arity law).
                    const UnitLayout& fn_layout =
                        layouts[res.binding.fn_unit];
                    const uint16_t param_count = fn_layout.param_count;
                    const uint16_t base = st.temp_reg();
                    st.reserve_temps(static_cast<uint16_t>(
                        fn_layout.arg_count - 1));
                    uint16_t cursor = base;
                    for (uint16_t p = 0; p < param_count; ++p) {
                        if (p < args.size() && args[p].reg != cursor) {
                            st.builder->emit(vortex::ugb::Op::MOVE, cursor,
                                             {args[p].reg});
                        }
                        if (p >= args.size()) {
                            const uint16_t nil_reg = st.temp_reg();
                            st.builder->const_null(nil_reg);
                            st.builder->emit(vortex::ugb::Op::MOVE, cursor,
                                             {nil_reg});
                        }
                        ++cursor;
                    }
                    if (st.env_reg != cursor) {
                        st.builder->emit(vortex::ugb::Op::MOVE, cursor,
                                         {st.env_reg});
                    }
                    ++cursor;
                    for (uint32_t i = 0; i < fn.captures.size(); ++i) {
                        const auto& cap = fn.captures[i];
                        uint16_t box_reg = 0;
                        bool found = false;
                        for (size_t s = st.scopes.size(); s-- > 0 && !found;) {
                            for (const auto& [n, b] : st.scopes[s].names) {
                                (void)n;
                                if (b.owner_unit == cap.owner_unit &&
                                    b.local_index == cap.local_index) {
                                    box_reg = b.reg;
                                    found = true;
                                    break;
                                }
                            }
                        }
                        if (!found) {
                            return support::fail(support::ErrorCode::SyntaxError,
                                        "capture is not visible at the call "
                                        "site",
                                        e.line);
                        }
                        if (box_reg != cursor) {
                            st.builder->emit(vortex::ugb::Op::MOVE, cursor,
                                             {box_reg});
                        }
                        ++cursor;
                    }
                    const uint16_t out = st.temp_reg();
                    st.builder->call_direct(
                        out, base, static_cast<uint16_t>(cursor - base),
                        fn.method_name);
                    Val v;
                    v.reg = out;
                    return v;
                }
                // Escaping function binding: the slot holds the value.
                auto read = read_binding(res, e.line);
                if (!read) return read;
                return emit_value_call(*read, args, false);
            }
            auto read = read_binding(res, e.line);
            if (!read) return read;
            return emit_value_call(*read, args, true);
        }
        // Global function value.
        auto read = read_binding(res, e.line);
        if (!read) return read;
        return emit_value_call(*read, args, true);
    }

    auto callee_val = emit_expr(callee);
    if (!callee_val) return callee_val;
    return emit_value_call(*callee_val, args, true);
}

// CEP:WHAT: Emits a method call with the string-method fallback.
// CEP:WHY: Method syntax must dispatch table methods through the index
//          protocol and native string methods through the hook path.
// CEP:STATUS: complete
// CEP:FAILURE: The method-error hook raises the language's method-call
//          error for missing methods.
// CEP:ASSUMES: The receiver was emitted once and stays live.
// CEP:COST: cold; one index chain plus one call.
// CEP:EVIDENCE: method-call examples on strings and tables execute.
vortex::support::Result<Val> Emitter::emit_call_method(const Expr& e) {
    auto obj = emit_expr(*e.lhs);
    if (!obj) return obj;
    std::vector<Val> args;
    for (const auto& a : e.args) {
        auto v = emit_expr(*a);
        if (!v) return v;
        args.push_back(*v);
    }
    auto key = emit_str_const(e.name);
    if (!key) return std::unexpected(key.error());
    auto fv = emit_index_load(*obj, *key);
    if (!fv) return fv;

    const uint16_t out = st.temp_reg();
    const std::string l_end = fresh_label("mcall_end");
    const std::string l_try_string = fresh_label("mcall_str");
    const std::string l_have_fn = fresh_label("mcall_fn");
    const std::string l_error = fresh_label("mcall_err");

    const uint16_t nil_check = st.temp_reg();
    st.builder->emit(vortex::ugb::Op::EQ_NULL, nil_check, {fv->reg});
    st.builder->jump_true(nil_check, l_try_string);
    st.builder->jump(l_have_fn);

    st.builder->bind_label(l_try_string);
    Val obj_val;
    obj_val.reg = obj->reg;
    const uint16_t isstr = st.temp_reg();
    auto str_check = emit_builtin_call(isstr, kBIsString, {obj_val});
    if (!str_check) return std::unexpected(str_check.error());
    st.builder->jump_false(isstr, l_error);
    const uint16_t str_base = st.temp_reg();
    st.reserve_temps(static_cast<uint16_t>(args.size()));
    st.builder->emit(vortex::ugb::Op::MOVE, str_base, {obj->reg});
    uint16_t cursor = static_cast<uint16_t>(str_base + 1);
    for (const Val& a : args) {
        if (a.reg != cursor) {
            st.builder->emit(vortex::ugb::Op::MOVE, cursor, {a.reg});
        }
        ++cursor;
    }
    st.builder->call_builtin(
        out, str_base, static_cast<uint16_t>(args.size() + 1),
        (std::string(kBMethodPrefix) + e.name).c_str());
    st.builder->jump(l_end);

    st.builder->bind_label(l_have_fn);
    Val fn_val;
    fn_val.reg = fv->reg;
    const uint16_t isfn = st.temp_reg();
    auto fn_check = emit_builtin_call(isfn, kBIsFunction, {fn_val});
    if (!fn_check) return std::unexpected(fn_check.error());
    st.builder->jump_false(isfn, l_error);
    // Uniform window: receiver value, self, arguments, nil padding.
    const uint16_t window =
        static_cast<uint16_t>(st.units.uniform_arity);
    const uint16_t vbase = st.temp_reg();
    st.reserve_temps(static_cast<uint16_t>(window - 1));
    st.builder->emit(vortex::ugb::Op::MOVE, vbase, {fv->reg});
    st.builder->emit(vortex::ugb::Op::MOVE,
                     static_cast<uint16_t>(vbase + kCallArgSelf),
                     {obj->reg});
    cursor = static_cast<uint16_t>(vbase + kCallArgKey);
    for (const Val& a : args) {
        if (cursor >= static_cast<uint16_t>(vbase + window)) break;
        if (a.reg != cursor) {
            st.builder->emit(vortex::ugb::Op::MOVE, cursor, {a.reg});
        }
        ++cursor;
    }
    while (cursor < static_cast<uint16_t>(vbase + window)) {
        st.builder->const_null(cursor);
        ++cursor;
    }
    st.builder->emit(vortex::ugb::Op::CALL_VIRTUAL, out, {vbase, window},
                     true, st.module.intern_method(kCallMethodName));
    st.builder->jump(l_end);

    st.builder->bind_label(l_error);
    Val key_val;
    key_val.reg = key->reg;
    const uint16_t err_dst = st.temp_reg();
    auto err = emit_builtin_call(err_dst, kBMethodError, {obj_val, key_val});
    if (!err) return std::unexpected(err.error());
    st.builder->bind_label(l_end);
    Val v;
    v.reg = out;
    return v;
}

// CEP:WHAT: Emits one expression (the value dispatcher).
// CEP:WHY: One switch per expression kind keeps the lowering rules findable.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic for the documented conformance gaps.
// CEP:ASSUMES: The expression tree is parser-valid.
// CEP:COST: cold; linear in the expression size.
// CEP:EVIDENCE: every expression kind appears in the executed corpus.
vortex::support::Result<Val> Emitter::emit_expr(const Expr& e) {
    switch (e.kind) {
    case ExprKind::Nil: {
        const uint16_t out = st.temp_reg();
        st.builder->const_null(out);
        Val v;
        v.reg = out;
        return v;
    }
    case ExprKind::True: {
        const uint16_t out = st.temp_reg();
        st.builder->const_true(out);
        Val v;
        v.reg = out;
        v.type = StType::Bool;
        v.is_bool = true;
        return v;
    }
    case ExprKind::False: {
        const uint16_t out = st.temp_reg();
        st.builder->const_false(out);
        Val v;
        v.reg = out;
        v.type = StType::Bool;
        v.is_bool = true;
        return v;
    }
    case ExprKind::Int: {
        const uint16_t out = st.temp_reg();
        if (e.int_value >= kSmallestNonI32 || e.int_value <= -kSmallestNonI32) {
            st.builder->const_i64(out, e.int_value);
        } else {
            st.builder->const_i32(out, static_cast<int32_t>(e.int_value));
        }
        Val v;
        v.reg = out;
        v.type = StType::Int;
        return v;
    }
    case ExprKind::Float: {
        const uint16_t out = st.temp_reg();
        st.builder->const_f64(out, e.float_value);
        Val v;
        v.reg = out;
        v.type = StType::Float;
        return v;
    }
    case ExprKind::Str: return emit_str_const(e.str_value);
    case ExprKind::Vararg: {
        return support::fail(support::ErrorCode::SyntaxError,
                    "the vararg expression needs value-pack support that "
                    "this conformance target reserves for a later ticket",
                    e.line);
    }
    case ExprKind::Name: {
        const NameResolution res = resolve_name(e.name);
        switch (res.kind) {
        case NameResolution::Kind::OwnLocal:
        case NameResolution::Kind::Capture:
        case NameResolution::Kind::Global: {
            if (res.kind == NameResolution::Kind::Global &&
                native_hook_for(e.name) != nullptr) {
                return support::fail(support::ErrorCode::SyntaxError,
                            "native function '" + e.name +
                                "' has no value form; call it directly",
                            e.line);
            }
            return read_binding(res, e.line);
        }
        case NameResolution::Kind::ModuleMath:
        case NameResolution::Kind::ModuleString:
            return support::fail(support::ErrorCode::SyntaxError,
                        "module '" + e.name +
                            "' has no value form; index one of its members",
                        e.line);
        default: break;
        }
        return support::fail(support::ErrorCode::SyntaxError, "unresolvable name", e.line);
    }
    case ExprKind::Paren: return emit_expr(*e.lhs);
    case ExprKind::Function: {
        auto it = st.units.fn_units.find(&e);
        if (it == st.units.fn_units.end()) {
            return support::fail(support::ErrorCode::SyntaxError, "function unit missing",
                        e.line);
        }
        auto body_res = emit_unit(it->second);
        if (!body_res) return std::unexpected(body_res.error());
        // No temp reset here: earlier sub-expression values of the current
        // statement (sibling call arguments) stay live across the lifted
        // unit's emission.
        return emit_function_value(it->second);
    }
    case ExprKind::Table: {
        const uint16_t t = st.temp_reg();
        auto new_call = emit_builtin_call(t, kBNewTable, {});
        if (!new_call) return std::unexpected(new_call.error());
        Val t_val;
        t_val.reg = t;
        int64_t array_index = 0;
        for (const auto& field : e.fields) {
            Val key;
            if (field.form == TableField::Form::Positional) {
                ++array_index;
                const uint16_t key_reg = st.temp_reg();
                st.builder->const_i32(key_reg,
                                      static_cast<int32_t>(array_index));
                key.reg = key_reg;
                key.type = StType::Int;
            } else if (field.form == TableField::Form::NameKey) {
                auto k = emit_str_const(field.name);
                if (!k) return k;
                key = *k;
            } else {
                auto k = emit_expr(*field.key);
                if (!k) return k;
                key = *k;
            }
            auto value = emit_expr(*field.value);
            if (!value) return value;
            const uint16_t set_dst = st.temp_reg();
            auto set =
                emit_builtin_call(set_dst, kBRawSet, {t_val, key, *value});
            if (!set) return std::unexpected(set.error());
        }
        Val v;
        v.reg = t;
        return v;
    }
    case ExprKind::Index: {
        // Native module member read: math.pi and friends.
        if (e.lhs->kind == ExprKind::Name && e.rhs->kind == ExprKind::Str) {
            const NameResolution base_res = resolve_name(e.lhs->name);
            if (base_res.kind == NameResolution::Kind::ModuleMath ||
                base_res.kind == NameResolution::Kind::ModuleString) {
                return emit_module_read(base_res, e.rhs->str_value);
            }
        }
        auto obj = emit_expr(*e.lhs);
        if (!obj) return obj;
        auto key = emit_expr(*e.rhs);
        if (!key) return key;
        return emit_index_load(*obj, *key);
    }
    case ExprKind::CallObj: return emit_call(e);
    case ExprKind::CallMethod: return emit_call_method(e);
    case ExprKind::Binary: {
        if (e.op == "and" || e.op == "or") return emit_short_circuit(e);
        if (e.op == "==" || e.op == "~=") return emit_equality(e);
        if (e.op == "<" || e.op == "<=" || e.op == ">" || e.op == ">=") {
            return emit_compare(e);
        }
        return emit_arith(e);
    }
    case ExprKind::Unary: return emit_unary(e);
    }
    return support::fail(support::ErrorCode::SyntaxError, "unhandled expression kind", e.line);
}

}  // namespace emit_internal
}  // namespace luavortex
