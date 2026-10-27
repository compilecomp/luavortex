# luavortex standard

Version: **0.1**

luavortex adopts **CEP&CC 0.1** (Cycle-Exact Performance & Clean Code,
Psychopathic Tier) as its normative coding standard. The canonical text lives
at <https://github.com/axiomzero0/CEP-CC> (standard.md, Apache-2.0); the
mechanical enforcement tool is `cep_lint`, driven by `.cep/cep_lint.json`.

This document mirrors the lint rule catalog of the standard for this
repository, per Law 8 (no stale documentation) and the `CEP-LINT-DOC-SYNC`
rule. Where this summary and the canonical standard disagree, the canonical
standard wins.

## Scope of enforcement

- Every first-party `.cpp` and `.hpp` file: header block, comment fields,
  naming, constants, macros, and banned-token rules.
- Conformance classes used by this repository: `CEP-1` (deterministic
  compiler and runtime code) and `CEP-2` (offline tooling). No first-party
  file is classified `CEP-0`, `HPC-0`, or `RCS-0`, so the hot-path banned
  token rule has no targets here; it stays in the catalog so the policy
  remains byte-identical to the canonical configuration.
- CI runs both this repository's configuration and the canonical
  configuration of the CEP-CC repository against `src/`, `include/`, and
  `tools/`. Any severity-0 or severity-1 finding fails the build.

## Rule catalog

The rules below are the keys of `.cep/cep_lint.json` under `rules`. Each
entry lists the severity, the governing sections of the canonical standard,
and the mechanical check. Messages and parameters live in the configuration
file; this chapter is the documentation contract the sync rule checks.

### Rule CEP-LINT-FILE-HEADER

**Severity 1. Sections: 35, 10.2. Check: `comment_block_schema` (file header mode).**

Every first-party `.cpp` and `.hpp` file must begin with a CEP file header
block containing `CEP:FILE`, `CEP:WHAT`, `CEP:WHY`, `CEP:CLASS`,
`CEP:STATUS`, `CEP:FAILURE`, `CEP:ASSUMES`, `CEP:COST`, and `CEP:EVIDENCE`.
The declared class must be one of the defined conformance classes. The
header is the first contiguous full-line comment block; content above it is
a violation by design.

### Rule CEP-LINT-FUNC-BLOCK

**Severity 1. Sections: 10.2, 10.3 through 10.9. Check: `function_policy` (block mode).**

Every function definition whose body contains at least two statements must
have a preceding CEP comment block containing `CEP:WHAT`, `CEP:WHY`,
`CEP:STATUS`, `CEP:FAILURE`, `CEP:ASSUMES`, `CEP:COST`, and `CEP:EVIDENCE`.
Lambdas are exempt by this configuration; document nontrivial lambdas in the
enclosing function's block.

### Rule CEP-LINT-STATUS-VALUE

**Severity 2. Section: 10.5. Check: `comment_block_schema` (status value mode).**

Every `CEP:STATUS` value must be one of `complete`, `partial`, `stub`,
`placeholder`.

### Rule CEP-LINT-STUB-DECL

**Severity 1. Sections: 10.5, 10.10. Check: `comment_block_schema` (stub requirements mode).**

Any comment block declaring `CEP:STATUS: stub`, `partial`, or `placeholder`
must contain `CEP:OWNER` and `CEP:TICKET` in the same block. Unowned stubs
and placeholders are banned.

### Rule CEP-LINT-TODO-OWNER

**Severity 1. Section: 10.10. Check: `comment_regex` (line scoped).**

The markers `TODO`, `FIXME`, `XXX`, and `HACK` (case-insensitive) are
violations unless written in the sanctioned form
`CEP:TODO(owner): CEP-<ticket>: text`. Anonymous work markers are banned.

### Rule CEP-LINT-COMMENTED-CODE

**Severity 2. Section: 10.14. Check: `comment_regex` (line scoped).**

Comment lines that parse as code statements are violations. Use version
control for dead code, never comments.

### Rule CEP-LINT-PLACEHOLDER-TEXT

**Severity 1. Sections: 3, 10.5. Check: `string_literal_regex`.**

Marker text (`not implemented`, `unimplemented`, `placeholder`, `fixme`,
`tbd`, `wip`) inside string literals is a violation. Placeholders must fail
loudly through declared statuses, not leak into runtime messages.

### Rule CEP-LINT-PLACEHOLDER-COMMENT

**Severity 1. Sections: 3, 10.5. Check: `comment_regex` (line scoped).**

Marker text in comments is a violation unless the line is a `CEP:` field
line; a declared status is the sanctioned form of unfinished work.

### Rule CEP-LINT-MAGIC-NUMBER

**Severity 2. Sections: Law 7, 11.1 through 11.3, 33.15. Check: `numeric_literal_policy`.**

Numeric literals other than `0` and `1` are violations unless the enclosing
statement is a `constexpr` or `const` declaration, an `enum`, a
`static_assert`, or a preprocessor line. Sanctioned constants use the
`kPascalCase` spelling of section 33.15.

### Rule CEP-LINT-MACRO-PREFIX

**Severity 2. Sections: 33.16, 8.4. Check: `macro_prefix_policy`.**

Object-like macro names must match the `CEP_` prefix pattern or the
include-guard pattern. All other macro definitions are violations.

### Rule CEP-LINT-HOT-BANNED

**Severity 0. Sections: 5.1, 8.25 through 8.29, 9.2 through 9.41, 40.3. Check: `banned_token_policy`.**

In files whose declared `CEP:CLASS` is `CEP-0`, `HPC-0`, or `RCS-0`, the
banned token set of the canonical standard (raw allocation, exceptions,
RTTI, coroutine keywords, node-based containers, stream and formatting
objects, synchronization primitives, C allocation and I/O) is exterminated
on sight. This repository declares no files in those classes; the rule
remains active so the policy matches the canonical configuration exactly.

### Rule CEP-LINT-STUB-BODY

**Severity 1. Sections: 10.5, 19, 20. Check: `function_policy` (stub body mode).**

A function definition with an empty body is a violation unless its comment
block declares `CEP:STATUS: stub`, `partial`, or `placeholder` (which then
requires `CEP:OWNER` and `CEP:TICKET`). Empty special member functions use
`= default` or `= delete`.

### Rule CEP-LINT-EMPTY-CATCH

**Severity 1. Sections: Law 6, 12.2. Check: `flat_code_regex`.**

A `catch` handler with an empty body is a violation. Handle the failure or
propagate it.

### Rule CEP-LINT-EMPTY-BRANCH

**Severity 2. Section: 14.5. Check: `flat_code_regex`.**

`if`, `else`, `for`, `while`, `do`, `switch`, and `try` guarding an empty
block is a violation: the branch does nothing.

### Rule CEP-LINT-CONSTANT-RETURN

**Severity 2. Section: 14.5. Check: `function_policy` (constant return mode).**

A function whose body is a single `return true;`, `return false;`, or
`return nullptr;` statement is a violation: the result must be computed from
inputs. Guard clauses inside larger bodies are not violations.

### Rule CEP-LINT-NAMING-TYPE

**Severity 2. Section: 33.11. Check: `flat_code_regex` with name validation.**

Type names introduced by `struct`, `class`, `enum class`, `enum struct`, and
`using` must be PascalCase without underscores.

### Rule CEP-LINT-NAMING-FUNC

**Severity 2. Section: 33.13. Check: `function_policy` (naming mode).**

Function names must be snake_case. Destructors, operators, and lambdas are
exempt from the mechanical check.

### Rule CEP-LINT-DOC-SYNC

**Severity 1. Sections: Law 8, 16, 34.3. Check: `doc_rule_coverage`.**

This document and the lint configuration must agree: every configured rule
must have a `### Rule` heading here, every documented rule must exist in the
configuration, rule headings must be unique, and the version line of this
document must equal the configuration's `standard.version`. Any drift is a
lint failure.

## Tickets

Repository work items are GitHub issues of this repository; `CEP:TICKET`
fields reference them as `compilecomp/luavortex#<number>`. The open
conformance limitations and their reasons are listed in README.md under
"Documented conformance deviations". `CEP:OWNER` values name the
maintainers group of this repository.
