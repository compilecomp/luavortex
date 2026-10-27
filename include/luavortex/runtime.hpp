// CEP:FILE: include/luavortex/runtime.hpp
// CEP:WHAT: The guest runtime hook library: every "lua.*" builtin the
//           lowering references, implementing language semantics over
//           engine heap objects.
// CEP:WHY: The engine deliberately ships no language semantics; this module
//           is the guest runtime half of the port (docs/porting.md Step 3).
// CEP:CLASS: CEP-1
// CEP:STATUS: partial
// CEP:OWNER: luavortex-maintainers
// CEP:TICKET: compilecomp/luavortex#1
// CEP:FAILURE: Semantic errors surface as LuaError exceptions (the guest
//          error path, documented in this header); malformed hook arguments
//          are internal errors and assert. Allocation failure propagates as
//          the engine's out-of-memory path.
// CEP:ASSUMES: Heap objects are 16-byte aligned (gc::Heap invariant); the
//          mutator is single-threaded per Interpreter (engine contract);
//          hooks never hold element pointers across allocations.
// CEP:COST: hook bodies are the guest's hot core (hash-table primitives,
//          packed strings); the registry itself is cold and consulted only
//          at install time. Measured numbers live in docs/benchmarks.md.
// CEP:EVIDENCE: tests/run_tests.sh executes the full corpus through these
//          hooks on the T0 interpreter.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include "vortex/gc/icggc.hpp"
#include "vortex/infra/ffi.hpp"
#include "vortex/support/tagged_value.hpp"
#include "vortex/ugb/module.hpp"
#include "vortex/vm/interpreter.hpp"

namespace luavortex {

/// Guest-visible error: thrown by hooks, caught at the driver boundary.
/// Exceptions are the only unwinding path across guest frames in this
/// conformance target; the policy is documented in docs/lowering.md.
struct LuaError {
    vortex::TaggedValue payload;  // the error object passed to error()
    std::string message;          // rendered text for the driver report
};

/// Shared context handed to every hook (passed as the builtin user pointer).
/// `handles` is the engine's handle-root table, wired by the driver into
/// both the context and the heap's GcRuntimeState so hooks can hold a
/// reference across an allocation the way the gc-icggc 8.8 invariant
/// requires (C++ frames never hold the only reference across a GC point).
struct RuntimeContext {
    vortex::gc::Heap* heap = nullptr;
    const vortex::ugb::UGBModule* module = nullptr;
    vortex::infra::HandleTable* handles = nullptr;
};

/// The hook registry: maps builtin names to native implementations.
class LuaRuntime {
public:
    using Hook = vortex::TaggedValue (*)(std::span<const vortex::TaggedValue>,
                                         void*);

    /// Fills the registry with every default hook. Called once by the
    /// driver before install(); a snake_case free of constructors keeps the
    /// naming rule mechanical.
    void register_default_hooks();

    /// Registers every hook the compiled module declares. Unknown builtin
    /// names are a compile/runtime contract violation and fail loudly.
    vortex::support::Result<void> install(vortex::vm::Interpreter& interp,
                                          RuntimeContext& ctx) const;

    /// True if the name is a registered hook.
    bool has(const std::string& name) const noexcept;

private:
    std::unordered_map<std::string, Hook> hooks_;
};

/// Renders one guest value the way print and tostring do.
std::string render_value(vortex::TaggedValue v, RuntimeContext& ctx);

}  // namespace luavortex
