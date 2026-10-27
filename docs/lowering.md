# docs/lowering.md — the Lua-to-UGB lowering contract

This document is the auditable contract between the Lua frontend and the
Vortex engine. It lists the representation choices, the calling
conventions, and the exact opcode subset the frontend emits (the T0
executable set).

## Value representation

| Lua value | Engine representation |
|---|---|
| nil | `TaggedValue::null()` (`Const.Null`) |
| true / false | `Const.True` / `Const.False` |
| integers | Smis (`Const.I32` / `Const.I64`), 63-bit signed |
| floats | boxed doubles (`Const.F64`, heap) |
| strings | packed word arrays (`ArrayObject`, 7 bytes per Smi slot), built by the `lua.conststr` hook from the module constant pool |
| tables | fixed-size header array + swappable backing array, all access through `lua.*` hooks |
| functions | per-function klass objects created by `New.Object`, invoked through `Call.Virtual` |

The frontend never emits `Const.String`: the T0 conformance target does not
execute it. String literals travel as constant-pool integer ids and
materialize through the `lua.conststr` hook.

## Method lifting and environments

Every function literal lifts to a UGB method named `lvx.fn.<id>.<hint>`.
The chunk body compiles to the method `main`.

Two calling conventions exist, decided by the analysis pass:

- **Non-escaping** (the function value is never used, only called directly
  from its owning scope): args are `v[0..p-1]` declared parameters,
  `v[p]` the environment table, `v[p+1..]` one cell per captured variable.
  Calls go through `Call.Direct` with the full hidden window.
- **Escaping** (any value use): `v[0]` is the function object. Its klass is
  `lvx.fn.<method name>` with declared fields: `lvx.env.table` (the
  environment) and `lvx.box.<i>` (one cell per capture). The host registers
  the `call` method entry on each klass after module load; calls go through
  `Call.Virtual`, and the callee reads its environment from the receiver.

Captured variables always live in one-slot array cells (`New.Array` +
`Array.Get/Set`), created at declaration, so upvalue sharing semantics match
the reference implementation exactly. Numeric for-loop variables that are
captured get a fresh cell per iteration.

Globals are keys of an environment table created by the `main` prologue
(`lua.new_table`) and threaded through every call, mirroring the `_ENV`
shape of the reference language. Global ids are interned integer keys.

## The T0 opcode subset the frontend emits

`Const.*`, `Move`, `Add.Any/I32`, `Sub.Any/F64`, `Mul.Any/F64`,
`Div.F64`, `Add.F64/Sub.F64/Mul.F64`, `Neg.F64`, `Eq.I32/I64/Ref/Null/F64`,
`Ne.I64`, `Lt/Le/Gt/Ge.S.I64`, `Lt/Le/Gt/Ge.F64`, `I64ToF64`,
`Jump/JumpTrue/JumpFalse`, `Return/ReturnUnit`, `Call.Direct`,
`Call.Virtual`, `Call.Builtin`, `New.Object`, `New.Array`,
`Get/Set.Field`, `Array.Length/Get/Set`, `Debug.Srcpos`.

## Fast paths versus hooks

The emitter tracks conservative static types (Int, Float, Str, Bool):

- Static Int+Int arithmetic and comparisons emit engine opcodes directly.
- Mixed Int/Float converts the integer side with `I64ToF64` and emits the
  F64 form.
- Everything dynamic routes through `lua.*` builtins that implement the
  language semantics: string arithmetic coercion, floor division and modulo
  sign rules, lexicographic string comparison, cross-type numeric equality,
  and the runtime type errors.

Condition positions normalize through the language truthiness rule (only
nil and false are falsy) because the engine's branch truthiness treats Smi
zero as falsy.

## Table and string object layouts

Table header (fixed, 5 slots):
`[tag=1, metatable, backing, count, id]`. Backing (swappable):
`[capacity (power of two), then key/value entry pairs at hash-probed
slots]`. String: `[tag=2, byte length, hash cache, payload words]` where
each word packs seven bytes into the low 56 bits of one Smi slot.

Keys hash to stable homes: numbers canonicalize so `1` and `1.0` collide
and compare equal; strings carry a lazily computed, cached content hash;
tables take a stable id from the header slot (assigned once, travels with
the object through evacuation); remaining heap objects (closures) share
one constant home slot and compare by identity bits, so their cluster
stays small unless a program keys on many functions
(compilecomp/luavortex#4). Lookup, insert, and delete are O(1) amortized:
linear probing, doubling growth at a 75% load factor, and backward-shift
deletion keep the backing tombstone-free. Growth swaps the backing into
the header, so references to the table stay valid; lookups and key
compares never allocate.

## The `lua.*` builtin surface

`new_table`, `raw_get`, `raw_set`, `getmetatable`, `setmetatable`,
`is_function`, `is_string`, `getenv`, `setenv`, `conststr`, `length`,
`concat`, `op_neg/add/sub/mul/div/floordiv/mod/pow/bnot/band/bor/bxor/shl/shr`,
`op_eq/lt/le`, `str_eq/lt/le`, `num_f64`, `check_step`, `for_cont`,
`for_step`, `next`, `inext`, `call_error`, `method_error`, `print`, `type`,
`tonumber`, `tointeger`, `tostring`, `assert`, `error`,
`math.*` (floor, ceil, abs, sqrt, exp, log, fmod, pow, min, max, pi, huge,
maxinteger, mininteger), `mpath.<method>` for native string methods.

## Verified scale and named limits

Tables are exercised to roughly 4K live pairs with mixed integer and
string keys, spanning several capacity doublings plus collections driven
by per-iteration string allocation. Beyond that scale the current M5
substrate can crash during a hook's growth path (an allocation inside a
builtin call that itself doubles the backing array interacts with the
concurrent evacuation's frame-update contract). The gap is filed as
compilecomp/luavortex#5; the bench suite stays inside the verified
envelope until it closes.

## Error handling

Guest errors (`error`, `assert`, type errors from hooks) throw a C++
`LuaError` carrying the payload; the driver catches it at the run boundary.
The exception unwinds guest frames through the engine's guarded frame
management (depth and frame guards unwind correctly by construction). The
engine's own `Result` diagnostics surface unchanged for verifier, loader,
and T0 runtime failures.

## Indexing protocols

`t[k]` lowers to: raw lookup; on nil, walk the metatable chain
(`__index`), calling function metamethods through `Call.Virtual` and
rebinding the receiver for table metamethods. `t[k] = v` mirrors it with
`__newindex` and falls back to the raw store. Method calls `o:m(...)`
resolve through the index protocol first; on nil with a string receiver
they dispatch to the native `lua.mpath.<m>` hook.
