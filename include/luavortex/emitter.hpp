// CEP:FILE: include/luavortex/emitter.hpp
// CEP:WHAT: The Lua-to-UGB lowering pass: whole-program scope analysis,
//           lambda lifting with explicit environment threading, and UGB
//           emission through the engine's MethodBuilder.
// CEP:WHY: The engine executes only UGB, so every Lua semantic (closures,
//           tables, metatables, globals) must lower into T0-executable
//           opcodes plus runtime hooks; this module owns that contract.
// CEP:CLASS: CEP-1
// CEP:STATUS: partial
// CEP:OWNER: luavortex-maintainers
// CEP:TICKET: compilecomp/luavortex#1
// CEP:FAILURE: Returns a Diagnostic with line information for unsupported
//          constructs (value-pack varargs, to-be-closed variables) and for
//          names that resolve to nothing. Container growth throws
//          std::bad_alloc, which is fatal at the driver boundary.
// CEP:ASSUMES: The AST outlives the emission; the engine's T0 dispatch table
//          covers the opcode subset documented in docs/lowering.md.
// CEP:COST: cold; one analysis pass plus one emission pass per compile.
// CEP:EVIDENCE: tests/run_tests.sh executes the compiled example corpus on
//          the T0 interpreter end to end.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "luavortex/parser.hpp"
#include "vortex/support/result.hpp"
#include "vortex/ugb/module.hpp"

namespace luavortex {

/// Sentinel unit id: no parent, no function binding.
inline constexpr uint32_t kNoUnit = 0xFFFFFFFFu;
/// Lower bound of the uniform call window: receiver, self, one argument,
/// and one padding slot.
inline constexpr uint32_t kMinUniformArity = 4;

/// One lifted function: a Lua function literal compiled to its own UGB
/// method with an explicit environment convention.
struct FnUnit {
    uint32_t id = 0;
    std::string method_name;      // unique UGB method name
    const Expr* fn = nullptr;     // the Function literal being lifted
    uint32_t parent = kNoUnit;
    bool escaping = false;        // value used anywhere -> object convention
    bool is_chunk = false;        // the chunk body unit ("main")

    /// Captures: ancestor bindings this unit needs, in emission order.
    struct Capture {
        uint32_t owner_unit = kNoUnit;
        uint32_t local_index = 0;
    };
    std::vector<Capture> captures;

    /// Locals declared directly by this unit, in declaration order.
    struct LocalInfo {
        std::string name;
        bool boxed = false;       // captured by any descendant unit
    };
    std::vector<LocalInfo> locals;

    uint32_t param_count = 0;     // declared parameters
    bool has_vararg = false;
    uint32_t reserved_slot_count = 0;  // loop hidden slots (3 per loop form)
};

/// Whole-program analysis result feeding emission.
struct ProgramUnits {
    std::vector<FnUnit> units;
    std::map<const Expr*, uint32_t> fn_units;  // function literal -> unit id
    uint32_t root_unit = 0;
    /// Uniform declared arity for value calls and escaping functions: the
    /// verifier requires every call to match its callee's declared argument
    /// count, so value calls (callee unknown until runtime) pad to one
    /// program-wide count; a dummy "call" method carries that count for
    /// the token resolution check.
    uint32_t uniform_arity = kMinUniformArity;
};

/// One compiled function class binding handed to the host runtime: an
/// escaping function's klass carries the "call" method used by Call.Virtual
/// to invoke the function value.
struct FnClassBinding {
    uint32_t class_token = 0;
    std::string method_name;
};

/// The result of a whole compile: the UGB module plus the function-class
/// bindings the host must register before execution.
struct CompiledLua {
    vortex::ugb::UGBModule module;
    std::vector<FnClassBinding> fn_classes;
};

/// Compiles Lua source text into a UGB module. The module carries the
/// "lua" language id and every builtin token the lowering references; the
/// host registers the matching natives (see luavortex/runtime.hpp).
vortex::support::Result<CompiledLua> compile_lua(
    std::string_view source, std::string_view chunk_name);

/// Runs the analysis pass alone (exposed for tests).
vortex::support::Result<ProgramUnits> analyze_units(const Chunk& chunk);

}  // namespace luavortex
