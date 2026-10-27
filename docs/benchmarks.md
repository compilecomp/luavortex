# luavortex benchmarks

Measured with `bench/run_bench.sh` (best of 3 wall-clock runs, `date +%s%N`
resolution) on the repository's development container: GCC 14.2, Release
lvxc over a Release vortex_core, Linux x86-64, measured 2026-10-28 on this
revision of the source tree. Numbers are
environment-bound; re-run the script rather than quoting these verbatim
(CEP&CC 13.2: benchmark environment is part of the artifact). This
document compares luavortex against its own history only — cross-language
or cross-tier comparisons say nothing actionable about this codebase.

| Workload | What it exercises | Linear-scan runtime | Hash/packed runtime |
|---|---|---|---|
| `bench/fib.lua` | `fib(22)`: recursion, argument passing, integer compares | 30 ms | 30 ms |
| `bench/loop_arith.lua` | 200000-iteration arithmetic loop (`%`, `//`, `-`, `+`) | 103 ms | 102 ms |
| `bench/strings.lua` | 2000 pool-materializations + concats + `rep` | 44 ms | 6 ms |
| `bench/table_ops.lua` | 800 mixed-key pairs, several capacity doublings | 27 ms | 5 ms |

Both columns ran on the same host and build configuration. The first
column is the runtime this repository started with: linear-scan tables,
one byte per tagged slot for strings, and a host `std::string`
materialization per key comparison. The second column is the current
runtime described in [lowering.md](lowering.md): open-addressed hash
tables with stable key hashes and backward-shift deletion, strings packed
seven bytes per slot with cached content hashes, allocation-free lookups
and key compares, and word-granular concat.

Table traffic and string building collapsed accordingly. The
arithmetic- and call-dominated workloads did not move because their cost
is the per-operation `lua.*` builtin dispatch of the T0 interpreter —
engine dispatch machinery, not runtime hook bodies — and the lever for
that is the engine's JIT tiers, not this repository's hooks.

Reproduce:

```sh
sh bench/run_bench.sh ./build/bin/lvxc 3
```
