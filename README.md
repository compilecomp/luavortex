# luavortex

**A Lua 5.4 frontend for [Vortex](https://github.com/compilecomp/vortex)** —
compiles Lua source to Universal Guest Bytecode (UGB) and executes it on the
Vortex engine's T0 speculative register interpreter.

luavortex follows the [CEP&CC](https://github.com/axiomzero0/CEP-CC) coding
standard (Psychopathic Tier): every file and function carries a machine-
parseable CEP comment block, and CI mechanically enforces the lint policy on
every push.

```
fib.lua ──lvxc──▶ UGB module ──vortex──▶ verifier ──▶ T0 ──▶ J1 ──▶ J2 ──▶ J3 ──▶ J4
```

## Quick start

Requirements: GCC 14+ (or Clang 18+) with C++26, CMake >= 3.28, Ninja or Make.

```bash
cmake -S . -B build -G Ninja
cmake --build build -j

./build/bin/lvxc run examples/fib.lua
./build/bin/lvxc run examples/closures.lua
./build/bin/lvxc run examples/metatables.lua

# Emit a binary UGB module the engine's own vx tool can disassemble:
./build/bin/lvxc build examples/fib.lua -o fib.ugb
# (from a vortex build:) vx dis fib.ugb

# Test suite:
ctest --test-dir build --output-on-failure
```

## What works

- **Full Lua 5.4 syntax**: the complete grammar — all operators with exact
  precedence and associativity, long strings/comments, hex numerals,
  goto/labels, method definitions, `<const>` attributes.
- **Values**: nil, booleans, 63-bit integers, doubles, packed-word strings
  (7 bytes per Smi slot), tables, and first-class function values.
- **Closures** with exact Lua capture semantics (shared cells, fresh loop
  variables per iteration) via lambda lifting and explicit environment
  threading.
- **Tables** with the full metatable protocol: `__index` and `__newindex`,
  as both tables and functions; `#`, `pairs`, `ipairs`.
- **Numeric semantics**: string arithmetic coercion, floor division and
  modulo with correct sign handling, `//`, `%`, `^`, bitwise operators with
  integer-conversion rules.
- **Standard members**: `print`, `type`, `tostring`, `tonumber`,
  `tointeger`, `assert`, `error`, `math.*`, `string` methods
  (`sub`/`rep`/`upper`/`lower`/`len`/`reverse`/`byte`/`char`).

## Documented conformance deviations

The T0 conformance target (UGB-Core-Interpreter level) shapes what the
frontend can lower. These deviations are deliberate and ticketed:

| Deviation | Reason | Ticket |
|---|---|---|
| Functions return one value; `return a, b` keeps `a` | UGB calls produce one result register | compilecomp/luavortex#1 |
| The vararg expression `...` is rejected at compile time | no value packs on the T0 target | compilecomp/luavortex#1 |
| `getmetatable("str")` returns nil | string methods dispatch natively instead | compilecomp/luavortex#1 |
| `pcall`/`coroutine`/`string.format` are absent | need interpreter re-entry or varargs | compilecomp/luavortex#1 |
| Integer arithmetic operates on the engine's 63-bit Smi range and traps on overflow | engine canonical arithmetic semantics | compilecomp/luavortex#2 |
| `__eq`/`__lt`/`__le`/`__add` metamethods are not invoked (raw semantics) | arithmetic lowering has no per-site hook path yet | compilecomp/luavortex#2 |
| String literals materialize per read | interning needs a GC-reachable host-side root | compilecomp/luavortex#3 |
| Function objects as table keys hash to one shared home slot | closure layouts are engine-controlled; stable ids need an emitter-reserved slot | compilecomp/luavortex#4 |
| Tables are verified to ~4K live pairs with mixed keys; larger histories hit a GC-era edge in the current M5 substrate (allocation during a hook's growth path) | concurrent evacuation interacting with builtin-frame reentrancy | compilecomp/luavortex#5 |

## How the lowering works

See [docs/lowering.md](docs/lowering.md) for the full contract. The short
version:

- Every function literal lifts to its own UGB method. Non-escaping functions
  receive captured cells as hidden arguments and are called with
  `Call.Direct`; escaping functions become objects of a per-function klass
  whose `call` method resolves through `Call.Virtual`.
- Globals live in an environment table created by the chunk prologue and
  threaded as a hidden parameter — the same shape as Lua 5.2+ `_ENV`.
- Tables are open-addressed hash maps and strings are packed word arrays,
  both GC-managed; all table and string semantics live in the `lua.*`
  runtime hooks registered as engine builtins.
- Static numeric knowledge selects engine-native opcodes (`Add.Any`,
  `Lt.F64`, ...); everything dynamic routes through hooks that implement
  exact Lua semantics.

## Repository layout

```
include/luavortex/   public API: lexer, parser, emitter, runtime hooks
src/                 implementations (CEP-1 compiler, CEP-1 runtime)
tools/lvxc/          the driver: run, build (UGB emission), stats
tests/cases/         executable test cases with expected output
docs/lowering.md     the Lua-to-UGB lowering contract
.cep/cep_lint.json   the lint policy (CEP&CC 0.1 rule catalog)
standard.md          this repository's normative standard document
```

## License

Apache-2.0. Vortex is a project of compilecomp; CEP&CC is the standard of
axiomzero0/CEP-CC.

## Benchmarks

`sh bench/run_bench.sh ./build/bin/lvxc 3` times the compiled workloads in
`bench/` (recursive fib, arithmetic loops, string building, table churn).
Current numbers, the measurement environment, and the runtime's own
before/after history live in [docs/benchmarks.md](docs/benchmarks.md);
per-operation builtin dispatch is the documented trade that the engine's
JIT tiers address.
