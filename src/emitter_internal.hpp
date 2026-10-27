// CEP:FILE: src/emitter_internal.hpp
// CEP:WHAT: Internal contracts shared by the analysis and emission halves of
//           the lowering pass: scope frames, emitted bindings, static value
//           types, and the builtin-name table.
// CEP:WHY: Splitting analysis from emission keeps each pass reviewable while
//          letting both share one naming and layout vocabulary.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none (type and constant declarations only).
// CEP:ASSUMES: Used only by emitter.cpp and emitter_emit.cpp.
// CEP:COST: compile-time only; no runtime instructions.
// CEP:EVIDENCE: both halves compile against this contract in CI.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "luavortex/emitter.hpp"
#include "luavortex/parser.hpp"
#include "vortex/support/result.hpp"
#include "vortex/ugb/builder.hpp"
#include "vortex/ugb/module.hpp"

namespace luavortex {
namespace emit_internal {

// Named layout constants (Law 7: every bound is a documented knob).
inline constexpr uint16_t kTempHeadroom = 256;
inline constexpr uint32_t kNoUnit = luavortex::kNoUnit;
inline constexpr uint32_t kNoLocal = 0xFFFFFFFFu;
inline constexpr int64_t kSmallestNonI32 = 2147483648LL;
inline constexpr int64_t kLargestI32 = 2147483647LL;
inline constexpr uint32_t kLuaLanguageId = 0x4c554101u;  // "LUA" + rev 1
inline constexpr uint32_t kLoopHiddenSlots = 3;  // per loop: state, limit, ctrl
/// Call window slot offsets (relative to the window base register).
inline constexpr uint16_t kCallArgSelf = 1;
inline constexpr uint16_t kCallArgKey = 2;
inline constexpr uint16_t kCallArgValue = 3;
/// Argument-count bases for call windows.
inline constexpr size_t kValueCallBase = 1;
inline constexpr size_t kNumForStepArg = 2;  // third numeric-for expression
inline constexpr size_t kValueArgIndex = 2;  // third hook argument
inline constexpr size_t kForLimitArg = 1;
inline constexpr size_t kForStepArg = 2;
inline constexpr size_t kMethodWindowBase = 2;
/// Hidden slots per lifted function: the environment table.
inline constexpr uint16_t kHiddenEnvSlots = 1;

// Runtime hook names the emitter references. The lvxc driver registers a
// native for every name the compiled module declares.
inline constexpr const char* kBNewTable = "lua.new_table";
inline constexpr const char* kBRawGet = "lua.raw_get";
inline constexpr const char* kBRawSet = "lua.raw_set";
inline constexpr const char* kBGetMetatable = "lua.getmetatable";
inline constexpr const char* kBSetMetatable = "lua.setmetatable";
inline constexpr const char* kBIsFunction = "lua.is_function";
inline constexpr const char* kBIsString = "lua.is_string";
inline constexpr const char* kBGetEnv = "lua.getenv";
inline constexpr const char* kBSetEnv = "lua.setenv";
inline constexpr const char* kBConstStr = "lua.conststr";
inline constexpr const char* kBLength = "lua.length";
inline constexpr const char* kBConcat = "lua.concat";
inline constexpr const char* kBOpNeg = "lua.op_neg";
inline constexpr const char* kBOpAdd = "lua.op_add";
inline constexpr const char* kBOpSub = "lua.op_sub";
inline constexpr const char* kBOpMul = "lua.op_mul";
inline constexpr const char* kBOpDiv = "lua.op_div";
inline constexpr const char* kBOpFloordiv = "lua.op_floordiv";
inline constexpr const char* kBOpMod = "lua.op_mod";
inline constexpr const char* kBOpPow = "lua.op_pow";
inline constexpr const char* kBOpBnot = "lua.op_bnot";
inline constexpr const char* kBOpBand = "lua.op_band";
inline constexpr const char* kBOpBor = "lua.op_bor";
inline constexpr const char* kBOpBxor = "lua.op_bxor";
inline constexpr const char* kBOpShl = "lua.op_shl";
inline constexpr const char* kBOpShr = "lua.op_shr";
inline constexpr const char* kBOpEq = "lua.op_eq";
inline constexpr const char* kBOpLt = "lua.op_lt";
inline constexpr const char* kBOpLe = "lua.op_le";
inline constexpr const char* kBCheckStep = "lua.check_step";
inline constexpr const char* kBForCont = "lua.for_cont";
inline constexpr const char* kBForStep = "lua.for_step";
inline constexpr const char* kBNext = "lua.next";
inline constexpr const char* kBInext = "lua.inext";
inline constexpr const char* kBCallError = "lua.call_error";
inline constexpr const char* kBMethodError = "lua.method_error";
inline constexpr const char* kBPrint = "lua.print";
inline constexpr const char* kBType = "lua.type";
inline constexpr const char* kBTonumber = "lua.tonumber";
inline constexpr const char* kBTointeger = "lua.tointeger";
inline constexpr const char* kBTostring = "lua.tostring";
inline constexpr const char* kBAssert = "lua.assert";
inline constexpr const char* kBError = "lua.error";
inline constexpr const char* kBStrEq = "lua.str_eq";
inline constexpr const char* kBStrLt = "lua.str_lt";
inline constexpr const char* kBStrLe = "lua.str_le";
inline constexpr const char* kBNumF64 = "lua.num_f64";
inline constexpr const char* kBMethodPrefix = "lua.mpath.";
inline constexpr const char* kBMathPrefix = "lua.math.";
inline constexpr const char* kBStringPrefix = "lua.string.";

// Reserved names on function-object classes (escaping-unit environment).
inline constexpr const char* kEnvTableName = "lvx.env.table";
inline constexpr const char* kBoxFieldPrefix = "lvx.box.";
inline constexpr const char* kFnClassPrefix = "lvx.fn.";
inline constexpr const char* kCallMethodName = "call";

// Reserved metamethod keys.
inline constexpr const char* kIndexKey = "__index";
inline constexpr const char* kNewIndexKey = "__newindex";

/// Conservative static type of an emitted value; drives fast-path selection.
enum class StType : uint8_t { Unknown, Int, Float, Str, Bool };

/// A value produced by expression emission: register plus static knowledge.
struct Val {
    uint16_t reg = 0;
    StType type = StType::Unknown;
    bool is_bool = false;   // the register already holds an engine boolean
};

/// One resolved name in an emission scope frame.
struct EmittedBinding {
    uint16_t reg = 0;               // value slot, or the box for boxed ones
    uint32_t owner_unit = kNoUnit;
    uint32_t local_index = kNoLocal;
    bool boxed = false;
    bool is_const = false;
    bool reassigned = false;
    uint32_t fn_unit = kNoUnit;     // function-literal binding, if any
};

struct ScopeFrame {
    uint32_t unit_id = kNoUnit;
    std::map<std::string, EmittedBinding> names;
};

/// Emission state shared by the statement and expression procedures.
struct EmitState {
    vortex::ugb::UGBModule& module;
    const ProgramUnits& units;
    const Chunk& chunk;
    std::vector<ScopeFrame> scopes;
    std::unique_ptr<vortex::ugb::MethodBuilder> builder;
    uint32_t current_unit = kNoUnit;
    uint16_t temps_base = 0;
    uint16_t temp_counter = 0;
    uint16_t env_reg = 0;                 // resolved once per unit
    uint16_t loop_slot_cursor = 0;        // next reserved loop slot triple
    uint32_t locals_declared = 0;         // live-local ordinal for scope checks
    std::vector<std::string> break_labels;
    std::map<std::string, std::string> label_names;  // label -> unique name
    std::map<std::string, uint32_t> label_locals;    // label -> ordinal at bind
    struct PendingGoto {
        std::string label;
        uint32_t locals_count = 0;
        uint32_t line = 0;
    };
    std::vector<PendingGoto> pending_gotos;
    uint32_t label_counter = 0;

    uint16_t temp_reg();
    void reserve_temps(uint16_t count);
    void reset_temps();
};

/// A compiled function class binding handed to the host runtime.
using FnClassBinding = luavortex::FnClassBinding;

/// Register-layout facts for one emitted unit.
struct UnitLayout {
    uint16_t param_count = 0;
    uint16_t locals_base = 0;
    uint16_t loop_base = 0;
    uint16_t temps_base = 0;
    uint16_t reg_count = 0;
    uint16_t arg_count = 0;
    uint16_t env_reg = 0;
    bool escaping = false;
    bool is_chunk = false;
    uint32_t class_token = 0;
    uint32_t env_field_token = 0;
    std::vector<uint32_t> box_tokens;   // per capture (escaping units)
    std::vector<uint16_t> capture_regs; // per capture (non-escaping units)
};

/// Resolution of one name at emission time.
struct NameResolution {
    enum class Kind : uint8_t {
        None,
        OwnLocal,
        Capture,
        Global,
        ModuleMath,
        ModuleString,
    };
    Kind kind = Kind::None;
    std::string name;
    EmittedBinding binding;
    uint32_t capture_index = 0;
};

/// The emission pass: lowers the analyzed unit tree into UGB. Method
/// definitions live in emitter_emit.cpp (driver and statements) and
/// emitter_emit_expr.cpp (values, operators, calls, indexing).
struct Emitter {
    EmitState st;
    std::vector<UnitLayout> layouts;
    std::vector<FnClassBinding> fn_classes;
    uint16_t unit_slot_cursor = 0;

    vortex::support::Result<std::vector<FnClassBinding>> run();

    // ---- unit layer -------------------------------------------------------
    vortex::support::Result<void> emit_unit(uint32_t id);
    void compute_layouts();
    void intern_class_for(uint32_t unit_id);

    // ---- statements -------------------------------------------------------
    vortex::support::Result<void> emit_block(const Block& b);
    vortex::support::Result<void> emit_stmt(const Stmt& s);
    vortex::support::Result<void> emit_local_decl(const Stmt& s);
    vortex::support::Result<void> emit_assign(const Stmt& s);
    vortex::support::Result<void> emit_if(const Stmt& s);
    vortex::support::Result<void> emit_while(const Stmt& s);
    vortex::support::Result<void> emit_repeat(const Stmt& s);
    vortex::support::Result<void> emit_num_for(const Stmt& s);
    vortex::support::Result<void> emit_gen_for(const Stmt& s);
    vortex::support::Result<void> emit_func_decl(const Stmt& s);
    vortex::support::Result<void> emit_local_func(const Stmt& s);
    vortex::support::Result<void> emit_return(const Stmt& s);
    vortex::support::Result<void> emit_goto(const Stmt& s);
    vortex::support::Result<void> emit_label(const Stmt& s);
    vortex::support::Result<void> emit_debug_pos(uint32_t line);

    // ---- expressions ------------------------------------------------------
    vortex::support::Result<Val> emit_expr(const Expr& e);
    vortex::support::Result<Val> emit_truthy(const Expr& e);
    Val normalize_truthy(Val v);
    vortex::support::Result<Val> emit_compare(const Expr& e);
    vortex::support::Result<Val> emit_arith(const Expr& e);
    vortex::support::Result<Val> emit_short_circuit(const Expr& e);
    vortex::support::Result<Val> emit_equality(const Expr& e);
    void to_float(Val& v);
    vortex::support::Result<Val> emit_unary(const Expr& e);
    vortex::support::Result<Val> emit_call(const Expr& e);
    vortex::support::Result<Val> emit_call_method(const Expr& e);
    vortex::support::Result<Val> emit_index_load(Val obj, Val key);
    vortex::support::Result<void> emit_index_store(Val obj, Val key,
                                                   Val value);
    vortex::support::Result<Val> emit_lua_eq(Val a, Val b);
    vortex::support::Result<Val> emit_str_const(const std::string& text);
    vortex::support::Result<Val> emit_function_value(uint32_t unit_id);
    vortex::support::Result<Val> emit_module_read(const NameResolution& res,
                                                  const std::string& prop);
    vortex::support::Result<void> emit_builtin_call(uint16_t dst,
                                                    const char* name,
                                                    const std::vector<Val>& args);
    vortex::support::Result<Val> emit_value_call(const Val& callee,
                                                 const std::vector<Val>& args,
                                                 bool check_callable);

    // ---- naming and resolution --------------------------------------------
    NameResolution resolve_name(const std::string& name);
    EmittedBinding* declare_binding(const std::string& name, uint32_t fn_unit,
                                    bool is_const);
    vortex::support::Result<Val> read_binding(const NameResolution& res,
                                              uint32_t line);
    vortex::support::Result<void> store_local(const NameResolution& res,
                                              uint16_t value_reg);
    vortex::support::Result<uint16_t> capture_box_reg(uint32_t index,
                                                      uint32_t line);
    std::string fresh_label(const std::string& hint);
};

/// Emission driver: lowers the analyzed unit tree into the module (the
/// thin free-function entry point used by compile_lua).
vortex::support::Result<std::vector<FnClassBinding>> emit_program(
    const Chunk& chunk, const ProgramUnits& units,
    vortex::ugb::UGBModule& module);

/// Maps a reserved native global name to its hook (nil when the name is
/// not native). Locals shadow these; the call path checks them only after
/// the scope stack misses.
// CEP:WHAT: Maps a reserved native global name to its runtime hook.
// CEP:WHY: Native globals call hooks directly; locals shadow them because this check runs only after the scope stack misses.
// CEP:STATUS: complete
// CEP:FAILURE: Returns nullptr for names that are not native.
// CEP:ASSUMES: none.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
// CEP:WHAT: Maps a reserved native global name to its runtime hook.
// CEP:WHY: Native globals call hooks directly; locals shadow them because this check runs only after the scope stack misses.
// CEP:STATUS: complete
// CEP:FAILURE: Returns nullptr for names that are not native.
// CEP:ASSUMES: none.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
inline const char* native_hook_for(const std::string& name) {
    if (name == "print") return kBPrint;
    if (name == "type") return kBType;
    if (name == "tostring") return kBTostring;
    if (name == "tonumber") return kBTonumber;
    if (name == "tointeger") return kBTointeger;
    if (name == "assert") return kBAssert;
    if (name == "error") return kBError;
    if (name == "setmetatable") return kBSetMetatable;
    if (name == "getmetatable") return kBGetMetatable;
    if (name == "rawget") return kBRawGet;
    if (name == "rawset") return kBRawSet;
    return nullptr;
}

}  // namespace emit_internal
}  // namespace luavortex
