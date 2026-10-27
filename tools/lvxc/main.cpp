// CEP:FILE: tools/lvxc/main.cpp
// CEP:WHAT: The lvxc driver: compiles Lua source into UGB and executes it
//           on the engine's T0 interpreter, or emits the binary module.
// CEP:WHY: The engine's vx driver owns the generic frontdoor; lvxc is the
//           language-specific one that registers the guest runtime hooks and
//           the function-value classes before execution.
// CEP:CLASS: CEP-2
// CEP:STATUS: complete
// CEP:FAILURE: Exits 1 with a diagnostic for compile, load, and runtime
//          errors; exits 2 for usage and I/O errors. Guest errors surface
//          as LuaError exceptions caught at this boundary.
// CEP:ASSUMES: The runtime context and heap outlive the interpreter run.
// CEP:COST: cold; driver overhead is irrelevant to the conformance goals.
// CEP:EVIDENCE: tests/run_tests.sh drives every case through this tool.
#include <cstring>
#include <exception>
#include <map>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "luavortex/emitter.hpp"
#include "luavortex/runtime.hpp"
#include "vortex/ugb/module.hpp"
#include "vortex/vm/interpreter.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;
constexpr int kScriptArgPos = 2;      // argv[2] is the input file
constexpr int kFirstOptionPos = 3;    // argv[3] is the first option

// CEP:WHAT: Reads a whole file into a string.
// CEP:WHY: Compile inputs arrive from the filesystem; one reader keeps the
//          error reporting uniform.
// CEP:STATUS: complete
// CEP:FAILURE: Returns false for unreadable files; the caller reports.
// CEP:ASSUMES: The file fits in memory.
// CEP:COST: cold; linear in the file size.
// CEP:EVIDENCE: every test case reads its source through this helper.
bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// CEP:WHAT: Prints a frontend diagnostic with position information.
// CEP:WHY: Compiler users need file, line, and column; one formatter keeps
//          every failure report consistent.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: none.
// CEP:COST: cold.
// CEP:EVIDENCE: failure cases in the test suite assert this shape.
void print_diagnostic(const std::string& path,
                      const vortex::support::Diagnostic& d) {
    std::cerr << "lvxc: " << path << ":" << d.line << ":" << d.column << ": "
              << d.message << "\n";
}

// CEP:WHAT: Prints the module runtime stats after a run.
// CEP:WHY: Observability of the conformance target needs the instruction
//          counters; the run command prints them after every execution.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: The stats struct layout matches the engine's.
// CEP:COST: cold.
// CEP:EVIDENCE: the test driver parses these lines for regression checks.
void print_stats(const vortex::vm::InterpStats& st) {
    std::cout << "tier T0 | instructions=" << st.instructions_executed
              << " calls=" << st.calls << " allocations=" << st.allocations
              << " max_depth=" << st.max_call_depth << "\n";
}

// CEP:WHAT: Registers each escaping function's call method on its klass.
// CEP:WHY: Call.Virtual resolves function values through the receiver's
//          klass method table; the host owns that registration step.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic if a method name fails to resolve.
// CEP:ASSUMES: build_module_runtime already created the klass handles.
// CEP:COST: cold; linear in the escaping function count.
// CEP:EVIDENCE: function-value examples invoke through this registration.
vortex::support::Result<void> register_function_classes(
    vortex::ugb::UGBModule& module,
    const std::vector<luavortex::FnClassBinding>& bindings,
    vortex::vm::Interpreter& interp) {
    for (const auto& b : bindings) {
        const int32_t method_id = module.find_method(b.method_name);
        if (method_id < 0) {
            return vortex::support::fail(
                vortex::support::ErrorCode::InternalError,
                "function class '" + b.method_name + "' has no method");
        }
        vortex::MethodEntry entry;
        entry.name = "call";
        entry.kind = vortex::MethodImplementationKind::Bytecode;
        entry.ugb_method_id = static_cast<uint32_t>(method_id);
        interp.klass_registry().at(b.class_token)->methods().push_back(entry);
    }
    return vortex::support::ok();
}

// CEP:WHAT: Compiles and executes a Lua chunk on the T0 interpreter.
// CEP:WHY: This is the run command's core: compile, install hooks, register
//          function classes, execute, and report.
// CEP:STATUS: complete
// CEP:FAILURE: Exits through the return code; guest errors report the
//          LuaError payload text.
// CEP:ASSUMES: The entry method exists (the chunk body compiles to main).
// CEP:COST: cold; dominated by interpretation.
// CEP:EVIDENCE: the whole corpus runs through this command.
int cmd_run(const std::string& path, bool show_stats) {
    std::string source;
    if (!read_file(path, source)) {
        std::cerr << "lvxc: cannot read '" << path << "'\n";
        return kExitUsage;
    }
    auto compiled = luavortex::compile_lua(source, path);
    if (!compiled) {
        print_diagnostic(path, compiled.error());
        return kExitError;
    }
    vortex::ugb::UGBModule& module = compiled->module;

    vortex::gc::Heap heap;
    // The handle table is a GC root set for native code (docs/gc-icggc.md
    // 8.6): hooks that allocate while holding a reference root it here.
    vortex::infra::HandleTable handles;
    heap.gc_state().handles = &handles;
    vortex::vm::Interpreter interp(heap);
    luavortex::RuntimeContext ctx;
    ctx.heap = &heap;
    ctx.handles = &handles;
    ctx.module = &module;
    luavortex::LuaRuntime runtime;
    runtime.register_default_hooks();
    auto installed = runtime.install(interp, ctx);
    if (!installed) {
        std::cerr << "lvxc: runtime: " << installed.error().message << "\n";
        return kExitError;
    }

    auto ready = interp.build_module_runtime(module);
    if (!ready) {
        std::cerr << "lvxc: load: " << ready.error().format() << "\n";
        return kExitError;
    }
    auto classes = register_function_classes(module, compiled->fn_classes,
                                             interp);
    if (!classes) {
        std::cerr << "lvxc: load: " << classes.error().message << "\n";
        return kExitError;
    }

    try {
        auto result = interp.run(module, "main", {});
        if (!result) {
            std::cerr << "lvxc: runtime: " << result.error().format() << "\n";
            return kExitError;
        }
        std::cout << "=> " << result->value.to_string() << "\n";
        if (show_stats) print_stats(result->stats);
        return kExitOk;
    } catch (const luavortex::LuaError& e) {
        std::cerr << "lvxc: lua error: " << e.message << "\n";
        return kExitError;
    } catch (const std::exception& e) {
        std::cerr << "lvxc: internal: " << e.what() << "\n";
        return kExitError;
    }
}

// CEP:WHAT: Compiles a Lua chunk and writes the binary UGB module.
// CEP:WHY: The build command produces artifacts the engine's vx tool can
//          disassemble and inspect.
// CEP:STATUS: complete
// CEP:FAILURE: Exits 1 on compile errors and 2 on file errors.
// CEP:ASSUMES: The output path is writable.
// CEP:COST: cold; dominated by compilation.
// CEP:EVIDENCE: the build command feeds vx dis in CI.
int cmd_build(const std::string& in, const std::string& out_path) {
    std::string source;
    if (!read_file(in, source)) {
        std::cerr << "lvxc: cannot read '" << in << "'\n";
        return kExitUsage;
    }
    auto compiled = luavortex::compile_lua(source, in);
    if (!compiled) {
        print_diagnostic(in, compiled.error());
        return kExitError;
    }
    const std::vector<uint8_t> binary =
        vortex::ugb::encode_module(compiled->module);
    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        std::cerr << "lvxc: cannot write '" << out_path << "'\n";
        return kExitUsage;
    }
    out.write(reinterpret_cast<const char*>(binary.data()),
              static_cast<std::streamsize>(binary.size()));
    std::cout << "wrote " << binary.size() << " bytes to " << out_path << "\n";
    return kExitOk;
}

void usage() {
    std::cout
        << "lvxc — the luavortex Lua-to-UGB compiler and runner\n\n"
        << "usage:\n"
        << "  lvxc run <file.lua>\n"
        << "  lvxc build <in.lua> [-o out.ugb]\n"
        << "  lvxc version\n";
}

}  // namespace

// CEP:WHAT: The driver entry point and command dispatch.
// CEP:WHY: Command routing stays trivial so failures are always explicit.
// CEP:STATUS: complete
// CEP:FAILURE: Exits with the usage code for unknown commands.
// CEP:ASSUMES: none.
// CEP:COST: cold.
// CEP:EVIDENCE: the CI workflow drives every command.
int main(int argc, char** argv) {
    if (argc < kScriptArgPos) {
        usage();
        return kExitUsage;
    }
    const std::string cmd = argv[1];
    if (cmd == "version") {
        std::cout << "lvxc 0.1.0 (luavortex; Lua 5.4 frontend on vortex)\n";
        return kExitOk;
    }
    if (cmd == "help" || cmd == "--help") {
        usage();
        return kExitOk;
    }
    if (cmd == "run" && argc >= kScriptArgPos + 1) {
        return cmd_run(argv[kScriptArgPos], false);
    }
    if (cmd == "stats" && argc >= kScriptArgPos + 1) {
        return cmd_run(argv[kScriptArgPos], true);
    }
    if (cmd == "build" && argc >= kScriptArgPos + 1) {
        std::string out_path = "a.ugb";
        for (int i = kFirstOptionPos; i < argc; ++i) {
            if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out_path = argv[++i];
            }
        }
        return cmd_build(argv[kScriptArgPos], out_path);
    }
    usage();
    return kExitUsage;
}
