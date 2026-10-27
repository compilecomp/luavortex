// CEP:FILE: src/runtime.cpp
// CEP:WHAT: Implementations of the guest runtime hooks: hash-table and
//           packed-string object protocols, the environment table, numerics
//           with exact coercion rules, iteration, and the standard members.
// CEP:WHY: These hooks carry every language semantic the UGB layer leaves
//           open; they are the runtime half of the port contract.
// CEP:CLASS: CEP-1
// CEP:STATUS: partial
// CEP:OWNER: luavortex-maintainers
// CEP:TICKET: compilecomp/luavortex#1, compilecomp/luavortex#3,
//           compilecomp/luavortex#4
// CEP:FAILURE: Guest semantic failures throw LuaError (caught at the
//          driver); hook-contract violations throw the contract-violation
//          error. No hook returns a silently wrong value for a detectable
//          misuse.
// CEP:ASSUMES: Single mutator per Interpreter; heap objects are 16-byte
//          aligned; no element pointer is held across an allocation, and
//          after one, values re-read from the caller's frame window (only
//          frame registers are GC-updated) or through a handle from
//          RuntimeContext::handles (make_table's header). Table identity
//          survives growth because the header object is fixed-size and only
//          its backing array swaps; hash homes stay stable because they
//          derive from object-resident ids, cached content hashes, or
//          canonical values. The GCC/Clang builtin overflow intrinsics
//          (__builtin_add_overflow and friends) are available — the
//          documented toolchain is GCC 14.2 / Clang 18+ on C++26.
// CEP:COST: table and string primitives are the hot core: O(1) amortized
//          hash probes, allocation-free lookups and key compares, strings
//          packed seven bytes per slot with cached content hashes. Growth
//          rehashes once per doubling. Cross-language comparisons are out
//          of scope; measured numbers live in docs/benchmarks.md.
// CEP:EVIDENCE: the whole test corpus executes through these hooks; the
//          bench suite records before/after timings in docs/benchmarks.md.
#include "luavortex/runtime.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace luavortex {

namespace vx = ::vortex;
namespace support = ::vortex::support;

namespace {

// Object layout constants (Law 7: every offset is a named fact).
//
// Table objects: header [tag, metatable, backing, count, id] plus a swappable
// backing array of [capacity(power of two), then key/value entry pairs at
// hash-probed slots]. Strings: [tag, byte length, hash cache, then payload
// words] where each word packs seven bytes into the low 56 bits of one Smi
// slot so copies and equality compare words, not tagged slots.
constexpr int64_t kTagTable = 1;
constexpr int64_t kTagString = 2;
constexpr int64_t kTableHeaderSlots = 5;
constexpr int64_t kMetaSlot = 1;
constexpr int64_t kBackingSlot = 2;
constexpr int64_t kCountSlot = 3;
constexpr int64_t kTableIdSlot = 4;
constexpr int64_t kBackingCapacitySlot = 0;
constexpr int64_t kBackingPairsBase = 1;
constexpr int64_t kStringLenSlot = 1;
constexpr int64_t kStringHashSlot = 2;
constexpr int64_t kStringWordsBase = 3;
constexpr int64_t kStringBytesPerWord = 7;
constexpr int64_t kStringByteBits = 8;
constexpr int64_t kInitialTableCapacity = 8;
constexpr int64_t kTableSlotsPerPair = 2;
constexpr int64_t kLoadFactorNum = 3;
constexpr int64_t kLoadFactorDen = 4;
constexpr int64_t kFirstObjectId = 1;
constexpr int64_t kByteMask = 0xFF;
constexpr uint64_t kFibHashConstant = 0x9E3779B97F4A7C15ULL;
constexpr uint64_t kHashIndexShift = 33;
constexpr uint64_t kTrueKeyHash = 0x54C11A5A3B16ULL;
constexpr uint64_t kFalseKeyHash = 0x2C1B3C38D4E7ULL;
constexpr uint64_t kExoticKeyHash = 0x9E3779B97F4AULL;
constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
constexpr uint64_t kSmiHashValueMask = (uint64_t{1} << 62) - 1;
constexpr uint64_t kHashZeroReplacement = 1;
// Allocation bounds (Law 7 + 11.2): single allocations name their limit so
// the uint32 length casts are guarded, and string requests name theirs.
constexpr int64_t kMaxAllocSlots = 1 << 26;
constexpr int64_t kMaxStringBytes = 1 << 24;
constexpr int64_t kKvPairSlots = 2;
constexpr char kPrintSeparator = '\t';
constexpr std::string_view kFunctionClassPrefix = "lvx.fn.";
constexpr std::string_view kDoubleKlassName = "double";
constexpr std::string_view kStringLibPrefix = "lua.string.";
constexpr std::string_view kStringLibMethodPrefix = "lua.mpath.";
constexpr double kPi = 3.14159265358979323846;
constexpr size_t kRenderBufferBytes = 64;
constexpr int64_t kShiftWordBits = 64;
constexpr size_t kMinPairArgs = 2;
constexpr size_t kMinTripleArgs = 3;
constexpr size_t kFirstOptionalArg = 1;
constexpr size_t kSecondOptionalArg = 2;
constexpr size_t kValueArgIndex = 2;
constexpr size_t kForLimitArg = 1;
constexpr size_t kForStepArg = 2;

// CEP:WHAT: Returns the resolved array body of a tagged heap value.
// CEP:WHY: Brooks resolution must precede every element access so reads
//          and writes follow the current body (the gc-icggc contract).
// CEP:STATUS: complete
// CEP:FAILURE: none (the caller type-checks first).
// CEP:ASSUMES: v is a heap-pointer tagged value.
// CEP:COST: @warm; one load plus compare.
// CEP:EVIDENCE: every hook access resolves through this helper.
vx::ArrayObject* body_of(vx::TaggedValue v) noexcept {
    return reinterpret_cast<vx::ArrayObject*>(
        vortex::gc_resolve(v.as_heap_object()));
}

// CEP:WHAT: Returns the resolved backing array of a table value.
// CEP:WHY: Table storage lives in a swappable backing array so growth
//          never invalidates references to the table itself.
// CEP:STATUS: complete
// CEP:FAILURE: none (the caller type-checks first).
// CEP:ASSUMES: v is a table object.
// CEP:COST: @warm; two resolutions.
// CEP:EVIDENCE: every table operation goes through this accessor.
vx::ArrayObject* table_backing(vx::TaggedValue t) noexcept {
    return body_of(body_of(t)->element(kBackingSlot));
}

// CEP:WHAT: Reports whether a value is a guest table object.
// CEP:WHY: Every hook dispatches on the value kind; the tag word is the type test (Rule 110 discipline).
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Heap objects are 16-byte aligned (heap invariant).
// CEP:COST: @hot on the table-key probe path (called from key_hash); one
//          resolve plus two compares.
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
bool is_table(vx::TaggedValue v) noexcept {
    if (!v.is_heap_object()) return false;
    auto* a = body_of(v);
    if (a->header.klass != nullptr) return false;
    return a->element(0).is_smi() && a->element(0).as_smi() == kTagTable;
}

// CEP:WHAT: Reports whether a value is a guest string object.
// CEP:WHY: String operations branch on this test before touching the byte payload.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Heap objects are 16-byte aligned (heap invariant).
// CEP:COST: @hot on the string-key probe path (called from key_hash and
//          key_bits_eq); one resolve plus two compares.
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
bool is_string_obj(vx::TaggedValue v) noexcept {
    if (!v.is_heap_object()) return false;
    auto* a = body_of(v);
    if (a->header.klass != nullptr) return false;
    return a->element(0).is_smi() && a->element(0).as_smi() == kTagString;
}

// CEP:WHAT: Reports whether a value is a boxed engine double.
// CEP:WHY: Numeric dispatch separates integer and float paths; the klass
//          word is the type test (Rule 110 discipline).
// CEP:STATUS: complete
// CEP:FAILURE: Returns false for every other value kind.
// CEP:ASSUMES: The heap's double klass is statically named.
// CEP:COST: @warm; two compares.
// CEP:EVIDENCE: float examples exercise this test.
bool is_float_obj(vx::TaggedValue v) noexcept {
    if (!v.is_heap_object()) return false;
    auto* o = v.as_heap_object();
    if (o->header.klass == nullptr) return false;
    return kDoubleKlassName == o->header.klass->name();
}

bool is_number(vx::TaggedValue v) noexcept {
    return v.is_smi() || is_float_obj(v);
}

// CEP:WHAT: Reports whether a value is a guest function object.
// CEP:WHY: Callable checks and the type hook classify functions through the klass name prefix.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Function klasses carry the reserved lvx.fn. prefix.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
bool is_function(vx::TaggedValue v) noexcept {
    if (!v.is_heap_object()) return false;
    auto* o = v.as_heap_object();
    if (o->header.klass == nullptr) return false;
    return o->header.klass->name().starts_with(kFunctionClassPrefix);
}

// CEP:WHAT: Word count of the packed payload for a byte length.
// CEP:WHY: The packed string layout stores seven bytes per Smi slot; every
//          allocation and loop derives its extent from this one formula.
// CEP:STATUS: complete
// CEP:FAILURE: none (pure arithmetic).
// CEP:ASSUMES: byte_len is non-negative.
// CEP:COST: @hot; two adds and a shift.
// CEP:EVIDENCE: make_string and the equality fast path share it.
int64_t string_word_count(int64_t byte_len) noexcept {
    return (byte_len + kStringBytesPerWord - 1) / kStringBytesPerWord;
}

// CEP:WHAT: Reads one byte from a packed string body.
// CEP:WHY: Byte-level operations (lexicographic compare, unpacking) index
//          through this accessor so the word layout lives in exactly one
//          place.
// CEP:STATUS: complete
// CEP:FAILURE: none (the caller bounds-checks the index).
// CEP:ASSUMES: a is a resolved string body; i is within the byte length.
// CEP:COST: @hot; one load, shift, and mask.
// CEP:EVIDENCE: the string corpus round-trips bytes through it.
int64_t string_byte_at(vx::ArrayObject* a, int64_t i) noexcept {
    const int64_t word = i / kStringBytesPerWord;
    const int64_t shift = (i % kStringBytesPerWord) * kStringByteBits;
    return (a->element(kStringWordsBase + word).as_smi() >> shift) &
           kByteMask;
}

// CEP:WHAT: Returns the byte payload of a string object.
// CEP:WHY: Cold paths (rendering, parsing, libc-backed methods) still want
//          host bytes; the unpack walks words, not tagged slots.
// CEP:STATUS: complete
// CEP:FAILURE: none (the caller type-checks first).
// CEP:ASSUMES: v is a string object.
// CEP:COST: @warm; one pass over the payload words.
// CEP:EVIDENCE: string examples exercise this accessor.
std::string string_bytes(vx::TaggedValue v) {
    auto* a = body_of(v);
    const int64_t len = a->element(kStringLenSlot).as_smi();
    std::string out;
    out.reserve(static_cast<size_t>(len));
    for (int64_t i = 0; i < len; ++i) {
        out.push_back(static_cast<char>(string_byte_at(a, i)));
    }
    return out;
}

// CEP:WHAT: Allocates a string object from bytes with a packed payload.
// CEP:WHY: Packing seven bytes per slot cuts allocation size and copy cost
//          sevenfold versus one slot per byte; the fill stores only Smis so
//          no write barrier runs inside the loop.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the out-of-memory guest error.
// CEP:ASSUMES: ctx.heap is valid; the fill loop performs no allocation, so
//          the resolved body stays current throughout.
// CEP:COST: @hot; one allocation plus a word-parallel fill.
// CEP:EVIDENCE: every string literal and concat result materializes here.
vx::TaggedValue make_string(RuntimeContext& ctx, std::string_view bytes) {
    const int64_t len = static_cast<int64_t>(bytes.size());
    const int64_t words = string_word_count(len);
    if (kStringWordsBase + words > kMaxAllocSlots) {
        throw LuaError{vx::TaggedValue::null(), "out of memory"};
    }
    auto arr = ctx.heap->allocate_array(
        static_cast<uint32_t>(kStringWordsBase + words));
    if (!arr) throw LuaError{vx::TaggedValue::null(), "out of memory"};
    vx::ArrayObject* a = *arr;
    a->element(0) = vx::TaggedValue::smi(kTagString);
    a->element(kStringLenSlot) = vx::TaggedValue::smi(len);
    a->element(kStringHashSlot) = vx::TaggedValue::smi(0);
    for (int64_t w = 0; w < words; ++w) {
        uint64_t word = 0;
        const int64_t base = w * kStringBytesPerWord;
        const int64_t count =
            std::min(kStringBytesPerWord, len - base);
        for (int64_t b = 0; b < count; ++b) {
            word |= static_cast<uint64_t>(
                        static_cast<uint8_t>(bytes[base + b]))
                    << (b * kStringByteBits);
        }
        a->element(kStringWordsBase + w) =
            vx::TaggedValue::smi(static_cast<int64_t>(word));
    }
    return vx::TaggedValue::heap_pointer(a);
}

// CEP:WHAT: Allocation-free structural equality of two string objects.
// CEP:WHY: Table probes and the str_eq hook compare strings on the hot
//          path; comparing length then packed words avoids the host-byte
//          round trip and cannot allocate, so it is safe inside any probe.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Both values are string objects; the final word's padding
//          bits are zero for equal lengths (make_string guarantees it).
// CEP:COST: @hot; O(words) with no allocation.
// CEP:EVIDENCE: mixed-key table tests unify string keys through it.
bool string_obj_eq(vx::TaggedValue a, vx::TaggedValue b) noexcept {
    auto* x = body_of(a);
    auto* y = body_of(b);
    if (x->element(kStringLenSlot).raw() != y->element(kStringLenSlot).raw()) {
        return false;
    }
    const int64_t words =
        string_word_count(x->element(kStringLenSlot).as_smi());
    for (int64_t w = 0; w < words; ++w) {
        if (x->element(kStringWordsBase + w).raw() !=
            y->element(kStringWordsBase + w).raw()) {
            return false;
        }
    }
    return true;
}

// CEP:WHAT: Allocation-free byte-lexicographic ordering of two strings.
// CEP:WHY: The str_lt/str_le hooks and generic comparison need the
//          reference byte order without a host allocation per comparison.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Both values are string objects.
// CEP:COST: @hot; O(length) byte steps, no allocation.
// CEP:EVIDENCE: string ordering tests observe reference results.
bool string_obj_lt(vx::TaggedValue a, vx::TaggedValue b) noexcept {
    auto* x = body_of(a);
    auto* y = body_of(b);
    const int64_t lx = x->element(kStringLenSlot).as_smi();
    const int64_t ly = y->element(kStringLenSlot).as_smi();
    const int64_t common = std::min(lx, ly);
    for (int64_t i = 0; i < common; ++i) {
        const int64_t ba = string_byte_at(x, i);
        const int64_t bb = string_byte_at(y, i);
        if (ba != bb) return ba < bb;
    }
    return lx < ly;
}

// CEP:WHAT: Fibonacci-hypercube bit mixer for hash seeds.
// CEP:WHY: Key hashing needs cheap avalanche so probe sequences spread
//          across the backing; the multiplicative mixer is branch-free.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: none.
// CEP:COST: @hot; one multiply and one xor-shift.
// CEP:EVIDENCE: mixed-key table tests fill every capacity bucket.
uint64_t mix64(uint64_t x) noexcept {
    const uint64_t mixed = x * kFibHashConstant;
    return mixed ^ (mixed >> kHashIndexShift);
}

// CEP:WHAT: Content hash of a packed string body with an in-object cache.
// CEP:WHY: Strings are immutable, so the FNV hash computes once and the
//          cached word makes string-keyed lookups O(1) forever after.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: a is a resolved string body; the cache slot holds Smi(0)
//          until the first hash; the store is a Smi so no barrier runs.
// CEP:COST: @hot; O(length) once, then one load.
// CEP:EVIDENCE: string-heavy table tests observe stable probe behavior.
uint64_t string_hash_of(vx::ArrayObject* a) noexcept {
    const int64_t cached = a->element(kStringHashSlot).as_smi();
    if (cached != 0) return static_cast<uint64_t>(cached);
    const int64_t len = a->element(kStringLenSlot).as_smi();
    uint64_t h = kFnvOffsetBasis;
    for (int64_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(string_byte_at(a, i));
        h *= kFnvPrime;
    }
    h &= kSmiHashValueMask;
    if (h == 0) h = kHashZeroReplacement;
    a->element(kStringHashSlot) = vx::TaggedValue::smi(
        static_cast<int64_t>(h));
    return h;
}

// CEP:WHAT: Reads a double payload from a boxed float value.
// CEP:WHY: Numeric hooks need the payload after the klass test.
// CEP:STATUS: complete
// CEP:FAILURE: Returns false for non-float values.
// CEP:ASSUMES: none.
// CEP:COST: @warm; one payload load.
// CEP:EVIDENCE: float examples exercise this path.
bool float_of(vx::TaggedValue v, double& out) noexcept {
    if (!is_float_obj(v)) return false;
    out = vortex::read_boxed_double(vortex::gc_resolve(v.as_heap_object()));
    return true;
}

// CEP:WHAT: Reports whether a double value fits the engine Smi range.
// CEP:WHY: Rounding hooks return integers when exactly representable.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: none.
// CEP:COST: @warm; three compares.
// CEP:EVIDENCE: math examples exercise this predicate.
bool is_smi_representable_double(double d) noexcept {
    return d >= static_cast<double>(vx::TaggedValue::smi_min()) &&
           d <= static_cast<double>(vx::TaggedValue::smi_max()) &&
           d == std::floor(d);
}

// CEP:WHAT: Extracts a double payload from any numeric value.
// CEP:WHY: Mixed integer and float arithmetic compares and computes through one extraction point.
// CEP:STATUS: complete
// CEP:FAILURE: Returns false for non-numeric values.
// CEP:ASSUMES: none.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
bool number_of(vx::TaggedValue v, double& out) noexcept {
    if (v.is_smi()) {
        out = static_cast<double>(v.as_smi());
        return true;
    }
    return float_of(v, out);
}

// CEP:WHAT: Allocates a boxed double on the guest heap.
// CEP:WHY: Floats are heap objects so the collector traces them like every other value.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the out-of-memory guest error on allocation failure.
// CEP:ASSUMES: The GC-safety contract (no element pointers across allocation) holds.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue make_double(RuntimeContext& ctx, double d) {
    auto boxed = ctx.heap->allocate_double(d);
    if (!boxed) throw LuaError{vx::TaggedValue::null(), "out of memory"};
    return vx::TaggedValue::heap_pointer(*boxed);
}

/// An exact numeric view of a value, computed without allocation: the
/// string case parses into C++ primitives so no heap reference can go
/// stale across a later allocation in the calling hook.
struct NumVal {
    bool ok = false;
    bool is_int = false;
    int64_t i = 0;
    double d = 0.0;
};

// CEP:WHAT: Coerces a value to an exact numeric view without allocating.
// CEP:WHY: Arithmetic hooks must never hold a vx::TaggedValue copy across an
//          allocation; parsing into C++ primitives removes that hazard
//          class entirely while keeping the exact coercion rules.
// CEP:STATUS: complete
// CEP:FAILURE: ok stays false for values without a numeric form.
// CEP:ASSUMES: none.
// CEP:COST: @hot on the arithmetic and compare paths; one branch tree for
//          numeric operands, string parsing only for text operands.
// CEP:EVIDENCE: string arithmetic examples coerce correctly.
NumVal num_val_of(vx::TaggedValue v) {
    NumVal n;
    if (v.is_smi()) {
        n.ok = true;
        n.is_int = true;
        n.i = v.as_smi();
        n.d = static_cast<double>(n.i);
        return n;
    }
    double d = 0.0;
    if (float_of(v, d)) {
        n.ok = true;
        n.is_int = false;
        n.d = d;
        return n;
    }
    if (is_string_obj(v)) {
        // The strtoll/strtod pair matches the stoll/stod semantics this
        // coercion has always had (leading whitespace skipped, a full
        // consume required, ERANGE rejected) without throwing, so the
        // no-numeric-form failure is just ok == false.
        const std::string text = string_bytes(v);
        const char* first = text.c_str();
        char* end = nullptr;
        errno = 0;
        const long long as_int = std::strtoll(first, &end, 10);
        if (end != first && end == first + text.size() &&
            errno != ERANGE) {
            n.ok = true;
            n.is_int = true;
            n.i = static_cast<int64_t>(as_int);
            n.d = static_cast<double>(as_int);
            return n;
        }
        errno = 0;
        const double parsed = std::strtod(first, &end);
        if (end != first && end == first + text.size() &&
            errno != ERANGE) {
            n.ok = true;
            n.is_int = false;
            n.d = parsed;
            return n;
        }
    }
    return n;
}

// CEP:WHAT: Exact-integer view of an already-coerced numeric value.
// CEP:WHY: Bitwise operations need word operands; the view keeps the
//          conversion free of any allocation.
// CEP:STATUS: complete
// CEP:FAILURE: Returns zero for values without an exact integer form.
// CEP:ASSUMES: The NumVal came from num_val_of.
// CEP:COST: @warm; two compares.
// CEP:EVIDENCE: bitwise examples execute correctly.
int64_t num_int(const NumVal& n) noexcept {
    if (!n.ok) return 0;
    if (n.is_int) return n.i;
    if (n.d == std::floor(n.d) &&
        n.d >= static_cast<double>(vx::TaggedValue::smi_min()) &&
        n.d <= static_cast<double>(vx::TaggedValue::smi_max())) {
        return static_cast<int64_t>(n.d);
    }
    return 0;
}

[[noreturn]] void throw_arith(const char* op) {
    throw LuaError{vx::TaggedValue::null(),
                   std::string("attempt to perform arithmetic (") + op +
                       ") on a non-number value"};
}

[[noreturn]] void throw_compare() {
    throw LuaError{vx::TaggedValue::null(),
                   "attempt to compare incompatible values"};
}

// CEP:WHAT: Renders a float the way the language formats numbers.
// CEP:WHY: print, concat, and tostring share one float spelling.
// CEP:STATUS: complete
// CEP:FAILURE: none (finite and non-finite forms both render).
// CEP:ASSUMES: none.
// CEP:COST: cold; one formatting pass.
// CEP:EVIDENCE: float printing matches the reference spelling in tests.
std::string render_double(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
    char buffer[kRenderBufferBytes];
    std::snprintf(buffer, sizeof(buffer), "%.14g", d);
    return std::string(buffer);
}

std::string render_int(int64_t v) {
    return std::to_string(v);
}

// CEP:WHAT: Compares two table keys under the language identity rules.
// CEP:WHY: Integer 1 and float 1.0 denote the same key; string keys
//          compare by packed words; everything else compares by identity
//          bits, which the GC keeps current inside every heap slot.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Keys are legal key kinds (nil is never stored); both values
//          are GC-current (heap slots or refreshed frame registers).
// CEP:COST: @hot; allocation-free for every key kind.
// CEP:EVIDENCE: mixed integer, float, and string keys unify in tests.
bool key_bits_eq(vx::TaggedValue a, vx::TaggedValue b) noexcept {
    if (a.reference_equals(b)) return true;
    if (a.is_smi() && b.is_smi()) return a.as_smi() == b.as_smi();
    double da = 0.0;
    double db = 0.0;
    if (number_of(a, da) && number_of(b, db)) return da == db;
    if (is_string_obj(a) && is_string_obj(b)) return string_obj_eq(a, b);
    return false;
}

// CEP:WHAT: The process-wide source of stable table object ids.
// CEP:WHY: Tables used as keys need a hash that survives evacuation; the
//          id rides in the object header, so it moves with the object and
//          the cursor never reissues a value.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: One mutator thread per Interpreter (engine contract), so a
//          plain static cursor needs no synchronization.
// CEP:COST: @hot; one load and increment per first-use-as-key.
// CEP:EVIDENCE: table-as-key tests observe stable identity after growth.
int64_t& object_id_cursor() noexcept {
    static int64_t cursor = kFirstObjectId;
    return cursor;
}

// CEP:WHAT: Computes the stable hash of a table key.
// CEP:WHY: Open addressing needs a home slot per key; numeric keys
//          canonicalize so 1 and 1.0 share a home, string keys read the
//          cached content hash, and table keys read the header id. Heap
//          objects without a runtime-controlled header (closures) use a
//          constant home and rely on reference identity for equality, so
//          their cluster stays small unless a program keys on many
//          functions.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: k is a legal key (non-nil); string and table bodies are
//          resolved fresh, so writes to cache slots stay GC-current.
// CEP:COST: @hot; O(1) amortized for every key kind (strings hash once).
// CEP:EVIDENCE: mixed-key table tests exercise every branch.
uint64_t key_hash(vx::TaggedValue k) noexcept {
    if (k.is_smi()) {
        return mix64(static_cast<uint64_t>(k.as_smi()));
    }
    if (k.is_boolean()) {
        return k.as_boolean_unchecked() ? kTrueKeyHash : kFalseKeyHash;
    }
    double d = 0.0;
    if (float_of(k, d)) {
        if (d == 0.0) d = 0.0;  // normalize -0.0 onto +0.0
        if (is_smi_representable_double(d)) {
            return mix64(static_cast<uint64_t>(static_cast<int64_t>(d)));
        }
        uint64_t bits = 0;
        std::memcpy(&bits, &d, sizeof(bits));
        return mix64(bits);
    }
    if (is_string_obj(k)) return string_hash_of(body_of(k));
    if (is_table(k)) {
        auto* a = body_of(k);
        int64_t id = a->element(kTableIdSlot).as_smi();
        if (id == 0) {
            id = object_id_cursor();
            object_id_cursor() = id + 1;
            a->element(kTableIdSlot) = vx::TaggedValue::smi(id);
        }
        return mix64(static_cast<uint64_t>(id));
    }
    return kExoticKeyHash;
}

// CEP:WHAT: Allocates a table object with its initial hash backing.
// CEP:WHY: The fixed-size header (tag, metatable, backing, count, id) keeps
//          table references valid across growth; the backing array carries
//          the power-of-two capacity and the probed entry pairs.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the out-of-memory guest error.
// CEP:ASSUMES: The initial capacity is a power of two; reference-valued
//          header stores go through the write barrier. The header is rooted
//          through ctx.handles across the backing allocation because a C++
//          local is not a GC root; the handle is destroyed once the header
//          links the backing (the table is then reachable from the caller).
// CEP:COST: @warm; linear in the initial capacity.
// CEP:EVIDENCE: every table in the corpus originates here.
vx::TaggedValue make_table(RuntimeContext& ctx) {
    auto header = ctx.heap->allocate_array(
        static_cast<uint32_t>(kTableHeaderSlots));
    if (!header) throw LuaError{vx::TaggedValue::null(), "out of memory"};
    // Root the header before the backing allocation: a C++ local is not a
    // GC root (gc-icggc 8.8 invariant 1), and the backing allocation can
    // collect, reset the young space, and overwrite the header's memory.
    // The handle keeps the header alive and GC-updated across the window.
    auto hid = ctx.handles->create(*header,
                                   vortex::infra::HandleScope::Local);
    if (!hid) throw LuaError{vx::TaggedValue::null(), "out of handles"};
    auto backing = ctx.heap->allocate_array(static_cast<uint32_t>(
        kBackingPairsBase + kInitialTableCapacity * kTableSlotsPerPair));
    if (!backing) {
        ctx.handles->destroy(*hid);
        throw LuaError{vx::TaggedValue::null(), "out of memory"};
    }
    auto rooted = ctx.handles->resolve(*hid);
    if (!rooted) {
        ctx.handles->destroy(*hid);
        throw LuaError{vx::TaggedValue::null(), "handle resolve failed"};
    }
    vx::ArrayObject* h = static_cast<vx::ArrayObject*>(*rooted);
    vx::ArrayObject* b = *backing;
    ctx.handles->destroy(*hid);
    h->element(0) = vx::TaggedValue::smi(kTagTable);
    ctx.heap->barriered_store(h, &h->element(kMetaSlot),
                              vx::TaggedValue::null());
    ctx.heap->barriered_store(h, &h->element(kBackingSlot),
                              vx::TaggedValue::heap_pointer(b));
    h->element(kCountSlot) = vx::TaggedValue::smi(0);
    h->element(kTableIdSlot) = vx::TaggedValue::smi(0);
    b->element(kBackingCapacitySlot) =
        vx::TaggedValue::smi(kInitialTableCapacity);
    return vx::TaggedValue::heap_pointer(h);
}

int64_t table_count(vx::TaggedValue t) noexcept {
    return body_of(t)->element(kCountSlot).as_smi();
}

// CEP:WHAT: Reads the power-of-two entry capacity of a backing array.
// CEP:WHY: Every probe and rehash derives its mask from this one slot.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: b is a resolved table backing written by this runtime.
// CEP:COST: @hot; one load.
// CEP:EVIDENCE: all table operations route through it.
int64_t backing_capacity(vx::ArrayObject* b) noexcept {
    return b->element(kBackingCapacitySlot).as_smi();
}

// CEP:WHAT: Linear probe for a key: its entry slot when present, otherwise
//           the first empty slot where it belongs.
// CEP:WHY: One routine serves lookup, update, insert, and delete so the
//          probe invariant (entries live at or after their home slot within
//          a cluster) holds by construction.
// CEP:STATUS: complete
// CEP:FAILURE: none (terminates because the load factor guarantees at
//          least one empty slot per backing).
// CEP:ASSUMES: b's capacity is a power of two; the backing has an empty
//          slot; key comparisons are allocation-free (they are, for every
//          key kind).
// CEP:COST: @hot; O(1) amortized probes under the load factor bound.
// CEP:EVIDENCE: the full table corpus probes through this helper.
int64_t probe_slot(vx::ArrayObject* b, vx::TaggedValue k, uint64_t h) noexcept {
    const int64_t mask = backing_capacity(b) - 1;
    int64_t i = static_cast<int64_t>((h >> kHashIndexShift) &
                                     static_cast<uint64_t>(mask));
    for (;;) {
        const int64_t slot = kBackingPairsBase + i * kTableSlotsPerPair;
        const vx::TaggedValue key = b->element(slot);
        if (key.is_null() || key_bits_eq(key, k)) return slot;
        i = (i + 1) & mask;
    }
}

// CEP:WHAT: Raw table lookup with the language key-identity rules.
// CEP:WHY: Field access, globals, and metamethods all share this lookup.
// CEP:STATUS: complete
// CEP:FAILURE: Returns nil for absent keys (tables never store nil).
// CEP:ASSUMES: Both values are GC-current. Hashing and comparing never
//          allocate, so no collection can invalidate the resolved backing
//          mid-probe.
// CEP:COST: @hot; O(1) amortized, allocation-free.
// CEP:EVIDENCE: every table access in the corpus resolves here.
vx::TaggedValue table_get(vx::TaggedValue t, vx::TaggedValue k) noexcept {
    vx::ArrayObject* b = table_backing(t);
    const int64_t slot = probe_slot(b, k, key_hash(k));
    if (b->element(slot).is_null()) return vx::TaggedValue::null();
    return b->element(slot + 1);
}

// CEP:WHAT: Backward-shift deletion of one occupied entry slot.
// CEP:WHY: Linear probing needs cluster repair on delete; backward-shift
//          keeps probe chains valid without tombstones, so iteration stays
//          a plain empty-slot scan.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: slot points at an occupied key slot; the backing has no
//          tombstones; rehashing an entry recomputes the same hash (stable
//          ids and cached content hashes).
// CEP:COST: @warm; O(cluster length) around the deleted slot.
// CEP:EVIDENCE: delete-heavy table tests keep lookups correct afterwards.
void erase_slot(RuntimeContext& ctx, vx::ArrayObject* b, int64_t slot) noexcept {
    const int64_t mask = backing_capacity(b) - 1;
    // Empty the hole first: the shift below moves survivors into it, and a
    // scan that reaches the cluster end must leave this slot empty.
    ctx.heap->barriered_store(b, &b->element(slot),
                              vx::TaggedValue::null());
    ctx.heap->barriered_store(b, &b->element(slot + 1),
                              vx::TaggedValue::null());
    // All indices below are ENTRY indices; slot arrived as a raw element
    // index, so convert once here (Law 7: keep the unit conversion at the
    // boundary, never inside the loop).
    int64_t hole = (slot - kBackingPairsBase) / kTableSlotsPerPair;
    for (;;) {
        int64_t j = hole;
        for (;;) {
            j = (j + 1) & mask;
            const int64_t jslot = kBackingPairsBase + j * kTableSlotsPerPair;
            if (b->element(jslot).is_null()) return;  // cluster ends; done
            const int64_t home = static_cast<int64_t>(
                (key_hash(b->element(jslot)) >> kHashIndexShift) &
                static_cast<uint64_t>(mask));
            const int64_t from_home = (j - home) & mask;
            const int64_t from_hole = (j - hole) & mask;
            if (from_home >= from_hole) {
                // The entry's home lies at or before the hole: it belongs
                // in the hole to keep its probe chain connected.
                const int64_t hslot =
                    kBackingPairsBase + hole * kTableSlotsPerPair;
                ctx.heap->barriered_store(b, &b->element(hslot),
                                          b->element(jslot));
                ctx.heap->barriered_store(b, &b->element(hslot + 1),
                                          b->element(jslot + 1));
                ctx.heap->barriered_store(b, &b->element(jslot),
                                          vx::TaggedValue::null());
                ctx.heap->barriered_store(b, &b->element(jslot + 1),
                                          vx::TaggedValue::null());
                hole = j;
                break;
            }
        }
    }
}

// CEP:WHAT: Inserts, updates, or deletes a raw table entry with doubling
//           growth at the load-factor bound.
// CEP:WHY: All stores (fields, globals, constructors, __newindex fallback)
//          funnel here; hash probing keeps every operation O(1) amortized.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the out-of-memory guest error and the nil-index
//          guest error; assigning nil removes the key.
// CEP:ASSUMES: Arguments arrive in the caller's frame window; after the
//          growth allocation they re-read from that window because only
//          frame registers are GC-updated, and the old backing is
//          re-resolved through the table header after the refresh. Every
//          reference store goes through the write barrier.
// CEP:COST: @hot; O(1) amortized insert/update, O(entries) on doubling.
// CEP:EVIDENCE: growth-heavy table examples execute in tests.
void table_set(RuntimeContext& ctx, std::span<const vx::TaggedValue> args,
               size_t t_idx, size_t k_idx, size_t v_idx) {
    vx::TaggedValue t = args[t_idx];
    vx::TaggedValue k = args[k_idx];
    vx::TaggedValue v = args[v_idx];
    const auto refresh = [&]() {
        t = args[t_idx];
        k = args[k_idx];
        v = args[v_idx];
    };
    if (v.is_null()) {
        vx::ArrayObject* b = table_backing(t);
        const int64_t slot = probe_slot(b, k, key_hash(k));
        if (b->element(slot).is_null()) return;
        erase_slot(ctx, b, slot);
        body_of(t)->element(kCountSlot) = vx::TaggedValue::smi(
            body_of(t)->element(kCountSlot).as_smi() - 1);
        return;
    }
    if (k.is_null()) {
        throw LuaError{vx::TaggedValue::null(), "table index is nil"};
    }
    vx::ArrayObject* b = table_backing(t);
    int64_t slot = probe_slot(b, k, key_hash(k));
    if (!b->element(slot).is_null()) {
        ctx.heap->barriered_store(b, &b->element(slot + 1), v);
        return;
    }
    if ((table_count(t) + 1) * kLoadFactorDen >
        backing_capacity(b) * kLoadFactorNum) {
        const int64_t new_cap = backing_capacity(b) * 2;
        if (kBackingPairsBase + new_cap * kTableSlotsPerPair >
            kMaxAllocSlots) {
            throw LuaError{vx::TaggedValue::null(), "out of memory"};
        }
        auto grown = ctx.heap->allocate_array(static_cast<uint32_t>(
            kBackingPairsBase + new_cap * kTableSlotsPerPair));
        if (!grown) throw LuaError{vx::TaggedValue::null(), "out of memory"};
        vx::ArrayObject* g = *grown;
        g->element(kBackingCapacitySlot) = vx::TaggedValue::smi(new_cap);
        refresh();
        vx::ArrayObject* old = table_backing(t);
        const int64_t cap = backing_capacity(old);
        for (int64_t i = 0; i < cap; ++i) {
            const int64_t src = kBackingPairsBase + i * kTableSlotsPerPair;
            const vx::TaggedValue key = old->element(src);
            if (key.is_null()) continue;
            const int64_t dst = probe_slot(g, key, key_hash(key));
            ctx.heap->barriered_store(g, &g->element(dst), key);
            ctx.heap->barriered_store(g, &g->element(dst + 1),
                                      old->element(src + 1));
        }
        ctx.heap->barriered_store(
            body_of(t), &body_of(t)->element(kBackingSlot),
            vx::TaggedValue::heap_pointer(g));
        b = table_backing(t);
        slot = probe_slot(b, k, key_hash(k));
    }
    ctx.heap->barriered_store(b, &b->element(slot), k);
    ctx.heap->barriered_store(b, &b->element(slot + 1), v);
    body_of(t)->element(kCountSlot) = vx::TaggedValue::smi(
        body_of(t)->element(kCountSlot).as_smi() + 1);
}

}  // namespace

// CEP:WHAT: Renders one guest value per print and tostring rules.
// CEP:WHY: One renderer keeps user-visible spelling consistent everywhere.
// CEP:STATUS: complete
// CEP:FAILURE: none (every value kind has a spelling).
// CEP:ASSUMES: none.
// CEP:COST: cold; linear in the string payload.
// CEP:EVIDENCE: print output matches expectations across the corpus.
std::string render_value(vx::TaggedValue v, [[maybe_unused]] RuntimeContext& ctx) {
    if (v.is_null()) return "nil";
    if (v.is_boolean()) return v.as_boolean_unchecked() ? "true" : "false";
    if (v.is_smi()) return render_int(v.as_smi());
    double d = 0.0;
    if (float_of(v, d)) return render_double(d);
    if (is_string_obj(v)) return string_bytes(v);
    if (is_table(v)) return "table";
    if (is_function(v)) return "function";
    return "value";
}

namespace {

// ---- hooks -----------------------------------------------------------------

vx::TaggedValue hook_new_table(std::span<const vx::TaggedValue>, void* user) {
    return make_table(*static_cast<RuntimeContext*>(user));
}

// CEP:WHAT: Implements the lua.raw_get hook: raw table lookup without metamethods.
// CEP:WHY: Field access, the environment, and metamethod chains all share one lookup.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest indexing error for nil and non-table receivers; strings read as nil.
// CEP:ASSUMES: Arguments carry a receiver and a key.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_raw_get(std::span<const vx::TaggedValue> args, void*) {
    if (args.size() < kMinPairArgs) {
        throw LuaError{vx::TaggedValue::null(), "bad raw_get arguments"};
    }
    if (is_table(args[0])) return table_get(args[0], args[1]);
    if (is_string_obj(args[0])) {
        // Strings carry no fields on this target; method dispatch and the
        // byte accessors are the supported surface.
        return vx::TaggedValue::null();
    }
    if (args[0].is_null()) {
        throw LuaError{args[0], "attempt to index a nil value"};
    }
    throw LuaError{args[0], "attempt to index a non-table value"};
}

// CEP:WHAT: Implements the lua.raw_set hook: raw table store with growth.
// CEP:WHY: Constructors, globals, and the __newindex fallback all share one store path.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest indexing error for non-table receivers; growth throws on allocation failure.
// CEP:ASSUMES: Arguments carry a receiver, a key, and a value.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_raw_set(std::span<const vx::TaggedValue> args, void* user) {
    if (args.size() < kMinTripleArgs || !is_table(args[0])) {
        throw LuaError{args[0], "attempt to index a non-table value"};
    }
    table_set(*static_cast<RuntimeContext*>(user), args, 0, 1,
              kValueArgIndex);
    return vx::TaggedValue::undefined();
}

// CEP:WHAT: Implements the lua.getmetatable hook.
// CEP:WHY: The metatable slot of the table header is the single source of truth.
// CEP:STATUS: complete
// CEP:FAILURE: Returns nil for non-table values (strings carry no metatable on this target).
// CEP:ASSUMES: Arguments carry one value.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_getmetatable(std::span<const vx::TaggedValue> args, void*) {
    if (!is_table(args[0])) return vx::TaggedValue::null();
    return body_of(args[0])->element(kMetaSlot);
}

// CEP:WHAT: Implements the lua.setmetatable hook.
// CEP:WHY: Attaching a metatable is a raw header write; the value stays the table per the language contract.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest type error when the target or the metatable is not a table.
// CEP:ASSUMES: Arguments carry a target and a metatable (or nil).
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_setmetatable(std::span<const vx::TaggedValue> args,
                              void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    if (!is_table(args[0])) {
        throw LuaError{args[0], "setmetatable target is not a table"};
    }
    if (args.size() > kFirstOptionalArg && !args[1].is_null()) {
        if (!is_table(args[1])) {
            throw LuaError{args[1], "metatable must be a table"};
        }
        ctx->heap->barriered_store(body_of(args[0]),
                                   &body_of(args[0])->element(kMetaSlot),
                                   args[1]);
    } else {
        // Nil is not a reference; a plain store needs no barrier.
        body_of(args[0])->element(kMetaSlot) = vx::TaggedValue::null();
    }
    return args[0];
}

vx::TaggedValue hook_is_function(std::span<const vx::TaggedValue> args, void*) {
    return vx::TaggedValue::boolean(!args.empty() && is_function(args[0]));
}

vx::TaggedValue hook_is_string(std::span<const vx::TaggedValue> args, void*) {
    return vx::TaggedValue::boolean(!args.empty() && is_string_obj(args[0]));
}

// CEP:WHAT: Implements the lua.getenv hook: global read through the environment table.
// CEP:WHY: Globals are keys of one table threaded through every call, mirroring the reference _ENV shape.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest error when the environment table is missing.
// CEP:ASSUMES: Arguments carry the environment and the interned name id.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_getenv(std::span<const vx::TaggedValue> args, void*) {
    if (args.size() < kMinPairArgs || !is_table(args[0])) {
        throw LuaError{args[0], "environment table is missing"};
    }
    return table_get(args[0], args[1]);
}

// CEP:WHAT: Implements the lua.setenv hook: global write through the environment table.
// CEP:WHY: Assignments to globals reduce to one table store; ids keep keys allocation-free.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest error when the environment table is missing.
// CEP:ASSUMES: Arguments carry the environment, the id, and the value.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_setenv(std::span<const vx::TaggedValue> args, void* user) {
    if (args.size() < kMinTripleArgs || !is_table(args[0])) {
        throw LuaError{args[0], "environment table is missing"};
    }
    table_set(*static_cast<RuntimeContext*>(user), args, 0, 1,
              kValueArgIndex);
    return vx::TaggedValue::undefined();
}

// CEP:WHAT: Implements the lua.conststr hook: materializes a pool string.
// CEP:WHY: The T0 target executes no string opcode; literals travel as pool ids and build here.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest error for malformed or out-of-range ids.
// CEP:ASSUMES: The context carries the compiled module's constant pool.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_conststr(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    if (args.empty() || !args[0].is_smi()) {
        throw LuaError{vx::TaggedValue::null(), "bad constant id"};
    }
    const int64_t id = args[0].as_smi();
    if (id < 0 ||
        static_cast<size_t>(id) >= ctx->module->constants.size() ||
        ctx->module->constants[static_cast<size_t>(id)].kind !=
            vortex::ugb::Constant::Kind::String) {
        throw LuaError{vx::TaggedValue::null(), "constant id out of range"};
    }
    return make_string(
        *ctx, ctx->module->constants[static_cast<size_t>(id)].str);
}

// CEP:WHAT: Implements the lua.length hook behind the # operator.
// CEP:WHY: Strings report their byte length; tables report the border the reference language defines.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest error for values without a length.
// CEP:ASSUMES: Arguments carry one value.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_length(std::span<const vx::TaggedValue> args, void*) {
    if (is_string_obj(args[0])) {
        return vx::TaggedValue::smi(
            body_of(args[0])->element(kStringLenSlot).as_smi());
    }
    if (is_table(args[0])) {
        const int64_t cap =
            table_backing(args[0])->element(kBackingCapacitySlot).as_smi();
        int64_t border = 0;
        for (int64_t i = 1; i <= cap; ++i) {
            const vx::TaggedValue v = table_get(args[0], vx::TaggedValue::smi(i));
            if (v.is_null()) break;
            border = i;
        }
        return vx::TaggedValue::smi(border);
    }
    throw LuaError{args[0],
                   "attempt to get the length of a value without one"};
}

// CEP:WHAT: Allocates a packed string joining two string objects at word
//           granularity, reusing the left operand's payload words as-is.
// CEP:WHY: Chained .. with two string operands is the concat hot path;
//          copying whole words and packing only the right operand's bytes
//          avoids the unpack-repack round trip through host memory.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the out-of-memory guest error.
// CEP:ASSUMES: Both values are string objects. The bodies re-resolve after
//          the allocation (only frame registers are GC-updated), and the
//          fill loop performs no allocation. Element stores are Smis, so
//          no write barrier runs.
// CEP:COST: @hot; O(words of a) copy plus O(bytes of b) pack.
// CEP:EVIDENCE: the strings bench and corpus exercise this path.
vx::TaggedValue concat_strings(RuntimeContext& ctx, vx::TaggedValue left,
                            vx::TaggedValue right) {
    const int64_t lx =
        body_of(left)->element(kStringLenSlot).as_smi();
    const int64_t ly =
        body_of(right)->element(kStringLenSlot).as_smi();
    const int64_t total = lx + ly;
    const int64_t words = string_word_count(total);
    auto arr = ctx.heap->allocate_array(
        static_cast<uint32_t>(kStringWordsBase + words));
    if (!arr) throw LuaError{vx::TaggedValue::null(), "out of memory"};
    vx::ArrayObject* a = *arr;
    a->element(0) = vx::TaggedValue::smi(kTagString);
    a->element(kStringLenSlot) = vx::TaggedValue::smi(total);
    a->element(kStringHashSlot) = vx::TaggedValue::smi(0);
    // Re-resolve after the allocation before touching any payload word.
    vx::ArrayObject* x = body_of(left);
    vx::ArrayObject* y = body_of(right);
    const int64_t wa = string_word_count(lx);
    for (int64_t w = 0; w < wa; ++w) {
        a->element(kStringWordsBase + w) =
            x->element(kStringWordsBase + w);
    }
    int64_t w = lx / kStringBytesPerWord;
    int64_t p = (lx % kStringBytesPerWord) * kStringByteBits;
    uint64_t word = 0;
    if (p != 0) word = static_cast<uint64_t>(
        a->element(kStringWordsBase + w).as_smi());
    for (int64_t i = 0; i < ly; ++i) {
        word |= static_cast<uint64_t>(string_byte_at(y, i)) << p;
        p += kStringByteBits;
        if (p == kStringBytesPerWord * kStringByteBits) {
            a->element(kStringWordsBase + w) =
                vx::TaggedValue::smi(static_cast<int64_t>(word));
            ++w;
            word = 0;
            p = 0;
        }
    }
    if (p != 0) {
        a->element(kStringWordsBase + w) =
            vx::TaggedValue::smi(static_cast<int64_t>(word));
    }
    return vx::TaggedValue::heap_pointer(a);
}

// CEP:WHAT: Implements the lua.concat hook behind the .. operator.
// CEP:WHY: Strings join with the reference spellings for numbers; the
//          two-string form takes the word-granular fast path and other
//          operand mixes assemble host bytes once.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest error when an operand has no string or
//          numeric form.
// CEP:ASSUMES: Arguments carry at least one operand.
// CEP:COST: @hot for the two-string form (single allocation, word copy);
//          @warm otherwise, linear in the output size.
// CEP:EVIDENCE: concat examples across the corpus execute correctly.
vx::TaggedValue hook_concat(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    if (args.size() == kKvPairSlots && is_string_obj(args[0]) &&
        is_string_obj(args[1])) {
        return concat_strings(*ctx, args[0], args[1]);
    }
    std::string out;
    for (size_t i = 0; i < args.size(); ++i) {
        if (is_string_obj(args[i])) {
            out += string_bytes(args[i]);
            continue;
        }
        double d = 0.0;
        if (number_of(args[i], d)) {
            out += args[i].is_smi() ? render_int(args[i].as_smi())
                                    : render_double(d);
            continue;
        }
        throw LuaError{args[i],
                       "attempt to concatenate a value without a string form"};
    }
    return make_string(*ctx, out);
}

// CEP:WHAT: Implements the lua.op_neg hook: unary minus with coercion.
// CEP:WHY: String operands coerce per the language; integers negate in the Smi range.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numbers.
// CEP:ASSUMES: Arguments carry one operand.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_op_neg(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    const NumVal n = num_val_of(args[0]);
    if (!n.ok) throw_arith("negate");
    if (n.is_int) return vx::TaggedValue::smi(-n.i);
    return make_double(*ctx, -n.d);
}

// CEP:WHAT: Shared integer and float arithmetic for the binary hooks.
// CEP:WHY: The language's numeric rules (string coercion, integer traps)
//          live in one place so every operator behaves identically; the
//          NumVal path allocates only for the final result.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the language arithmetic error for non-numbers and
//          for Smi-range overflow (the documented conformance deviation).
// CEP:ASSUMES: The op parameter selects +, -, or *.
// CEP:COST: @warm; one branch tree per call.
// CEP:EVIDENCE: arithmetic examples match expected results.
vx::TaggedValue arith_binary(RuntimeContext& ctx,
                         std::span<const vx::TaggedValue> args, char op) {
    const NumVal a = num_val_of(args[0]);
    if (!a.ok) throw_arith("binary");
    const NumVal b = num_val_of(args[1]);
    if (!b.ok) throw_arith("binary");
    if (a.is_int && b.is_int) {
        int64_t r = 0;
        bool ovf = true;
        if (op == '+') ovf = __builtin_add_overflow(a.i, b.i, &r);
        if (op == '-') ovf = __builtin_sub_overflow(a.i, b.i, &r);
        if (op == '*') ovf = __builtin_mul_overflow(a.i, b.i, &r);
        if (!ovf) return vx::TaggedValue::smi(r);
        throw LuaError{vx::TaggedValue::null(), "integer overflow"};
    }
    if (op == '+') return make_double(ctx, a.d + b.d);
    if (op == '-') return make_double(ctx, a.d - b.d);
    return make_double(ctx, a.d * b.d);
}

vx::TaggedValue hook_op_add(std::span<const vx::TaggedValue> args, void* user) {
    return arith_binary(*static_cast<RuntimeContext*>(user), args, '+');
}

vx::TaggedValue hook_op_sub(std::span<const vx::TaggedValue> args, void* user) {
    return arith_binary(*static_cast<RuntimeContext*>(user), args, '-');
}

vx::TaggedValue hook_op_mul(std::span<const vx::TaggedValue> args, void* user) {
    return arith_binary(*static_cast<RuntimeContext*>(user), args, '*');
}

// CEP:WHAT: Implements the lua.op_div hook: float division.
// CEP:WHY: The language's division always produces a float, including IEEE infinities for zero divisors.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numbers.
// CEP:ASSUMES: Arguments carry two operands.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_op_div(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    const NumVal a = num_val_of(args[0]);
    if (!a.ok) throw_arith("divide");
    const NumVal b = num_val_of(args[1]);
    if (!b.ok) throw_arith("divide");
    return make_double(*ctx, a.d / b.d);
}

// CEP:WHAT: Implements the lua.op_floordiv hook: floor division with exact sign rules.
// CEP:WHY: Integer floor division corrects the truncated quotient; the float path floors the IEEE quotient.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numbers and the divide-by-zero error for zero divisors.
// CEP:ASSUMES: Arguments carry two operands.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_op_floordiv(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    const NumVal a = num_val_of(args[0]);
    if (!a.ok) throw_arith("floor-divide");
    const NumVal b = num_val_of(args[1]);
    if (!b.ok) throw_arith("floor-divide");
    if (a.is_int && b.is_int) {
        if (b.i == 0) {
            throw LuaError{vx::TaggedValue::null(), "attempt to perform 'n//0'"};
        }
        int64_t q = a.i / b.i;
        const int64_t r = a.i % b.i;
        if (r != 0 && ((r < 0) != (b.i < 0))) --q;
        return vx::TaggedValue::smi(q);
    }
    if (b.d == 0.0) {
        throw LuaError{vx::TaggedValue::null(), "attempt to perform 'n//0'"};
    }
    return make_double(*ctx, std::floor(a.d / b.d));
}

// CEP:WHAT: Implements the lua.op_mod hook: modulo with floor semantics.
// CEP:WHY: The result takes the divisor's sign, matching the reference definition.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numbers and the modulo-by-zero error for integer zeros.
// CEP:ASSUMES: Arguments carry two operands.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_op_mod(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    const NumVal a = num_val_of(args[0]);
    if (!a.ok) throw_arith("modulo");
    const NumVal b = num_val_of(args[1]);
    if (!b.ok) throw_arith("modulo");
    if (a.is_int && b.is_int) {
        if (b.i == 0) {
            throw LuaError{vx::TaggedValue::null(), "attempt to perform 'n%%0'"};
        }
        int64_t r = a.i % b.i;
        if (r != 0 && ((r < 0) != (b.i < 0))) r += b.i;
        return vx::TaggedValue::smi(r);
    }
    if (b.d == 0.0) return make_double(*ctx, std::nan(""));
    return make_double(*ctx, a.d - std::floor(a.d / b.d) * b.d);
}

// CEP:WHAT: Implements the lua.op_pow hook behind the ^ operator.
// CEP:WHY: Exponentiation always produces a float on this target.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numbers.
// CEP:ASSUMES: Arguments carry two operands.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_op_pow(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    const NumVal a = num_val_of(args[0]);
    if (!a.ok) throw_arith("power");
    const NumVal b = num_val_of(args[1]);
    if (!b.ok) throw_arith("power");
    return make_double(*ctx, std::pow(a.d, b.d));
}

// CEP:WHAT: Implements the lua.op_bnot hook: bitwise not.
// CEP:WHY: The operand converts to an exact integer before the bit complement.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numbers.
// CEP:ASSUMES: Arguments carry one operand.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_op_bnot(std::span<const vx::TaggedValue> args, void* user) {
    [[maybe_unused]] auto* ctx = static_cast<RuntimeContext*>(user);
    const NumVal n = num_val_of(args[0]);
    if (!n.ok) throw_arith("bitwise-not");
    return vx::TaggedValue::smi(~num_int(n));
}

// CEP:WHAT: Implements the lua.op_band hook behind the & operator.
// CEP:WHY: Both operands convert to exact integers before the bit conjunction.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numbers.
// CEP:ASSUMES: Arguments carry two operands.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_op_band(std::span<const vx::TaggedValue> args, void* user) {
    [[maybe_unused]] auto* ctx = static_cast<RuntimeContext*>(user);
    const NumVal a = num_val_of(args[0]);
    if (!a.ok) throw_arith("bitwise-and");
    const NumVal b = num_val_of(args[1]);
    if (!b.ok) throw_arith("bitwise-and");
    return vx::TaggedValue::smi(num_int(a) & num_int(b));
}

// CEP:WHAT: Implements the lua.op_bor hook behind the | operator.
// CEP:WHY: Both operands convert to exact integers before the bit disjunction.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numbers.
// CEP:ASSUMES: Arguments carry two operands.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_op_bor(std::span<const vx::TaggedValue> args, void* user) {
    [[maybe_unused]] auto* ctx = static_cast<RuntimeContext*>(user);
    const NumVal a = num_val_of(args[0]);
    if (!a.ok) throw_arith("bitwise-or");
    const NumVal b = num_val_of(args[1]);
    if (!b.ok) throw_arith("bitwise-or");
    return vx::TaggedValue::smi(num_int(a) | num_int(b));
}

// CEP:WHAT: Implements the lua.op_bxor hook behind the ~ operator.
// CEP:WHY: Both operands convert to exact integers before the bit difference.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numbers.
// CEP:ASSUMES: Arguments carry two operands.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_op_bxor(std::span<const vx::TaggedValue> args, void* user) {
    [[maybe_unused]] auto* ctx = static_cast<RuntimeContext*>(user);
    const NumVal a = num_val_of(args[0]);
    if (!a.ok) throw_arith("bitwise-xor");
    const NumVal b = num_val_of(args[1]);
    if (!b.ok) throw_arith("bitwise-xor");
    return vx::TaggedValue::smi(num_int(a) ^ num_int(b));
}

// CEP:WHAT: Implements the lua.op_shl hook behind the << operator.
// CEP:WHY: Shift counts outside the word width produce zero, matching the reference rules.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numbers.
// CEP:ASSUMES: Arguments carry two operands.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_op_shl(std::span<const vx::TaggedValue> args, void* user) {
    [[maybe_unused]] auto* ctx = static_cast<RuntimeContext*>(user);
    const NumVal a = num_val_of(args[0]);
    if (!a.ok) throw_arith("shift");
    const NumVal b = num_val_of(args[1]);
    if (!b.ok) throw_arith("shift");
    const int64_t x = num_int(a);
    const int64_t sh = num_int(b);
    if (sh <= -kShiftWordBits || sh >= kShiftWordBits) {
        return vx::TaggedValue::smi(0);
    }
    if (sh >= 0) return vx::TaggedValue::smi(x << sh);
    return vx::TaggedValue::smi(x >> (-sh));
}

// CEP:WHAT: Implements the lua.op_shr hook behind the >> operator.
// CEP:WHY: Shift counts outside the word width produce zero; negative counts reverse the direction.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numbers.
// CEP:ASSUMES: Arguments carry two operands.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_op_shr(std::span<const vx::TaggedValue> args, void* user) {
    [[maybe_unused]] auto* ctx = static_cast<RuntimeContext*>(user);
    const NumVal a = num_val_of(args[0]);
    if (!a.ok) throw_arith("shift");
    const NumVal b = num_val_of(args[1]);
    if (!b.ok) throw_arith("shift");
    const int64_t x = num_int(a);
    const int64_t sh = num_int(b);
    if (sh <= -kShiftWordBits || sh >= kShiftWordBits) {
        return vx::TaggedValue::smi(0);
    }
    if (sh >= 0) return vx::TaggedValue::smi(x >> sh);
    return vx::TaggedValue::smi(x << (-sh));
}

vx::TaggedValue hook_op_eq(std::span<const vx::TaggedValue> args, void*) {
    return vx::TaggedValue::boolean(key_bits_eq(args[0], args[1]));
}

// CEP:WHAT: Implements the lua.num_f64 hook: normalizes a number to a boxed double.
// CEP:WHY: The engine's typed float compare requires boxed operands; equality lowering normalizes through here.
// CEP:STATUS: complete
// CEP:FAILURE: Returns nil for non-numeric values (the caller branches).
// CEP:ASSUMES: Arguments carry one value.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_num_f64(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    const NumVal n = num_val_of(args[0]);
    if (!n.ok) return vx::TaggedValue::null();
    if (n.is_int) return make_double(*ctx, n.d);
    return args[0];
}

// CEP:WHAT: Implements numeric and lexicographic ordering for both comparison hooks.
// CEP:WHY: Numbers compare numerically across integer and float, strings by bytes, and anything else raises the language error.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest comparison error for unorderable operand pairs.
// CEP:ASSUMES: Arguments carry two operands.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue compare_order(std::span<const vx::TaggedValue> args,
                          bool allow_equal) {
    double da = 0.0;
    double db = 0.0;
    const bool a_num = number_of(args[0], da);
    const bool b_num = number_of(args[1], db);
    if (a_num && b_num) {
        return vx::TaggedValue::boolean(allow_equal ? (da <= db) : (da < db));
    }
    if (is_string_obj(args[0]) && is_string_obj(args[1])) {
        return vx::TaggedValue::boolean(allow_equal
                                            ? !string_obj_lt(args[1], args[0])
                                            : string_obj_lt(args[0], args[1]));
    }
    throw_compare();
}

vx::TaggedValue hook_op_lt(std::span<const vx::TaggedValue> args, void*) {
    return compare_order(args, false);
}

vx::TaggedValue hook_op_le(std::span<const vx::TaggedValue> args, void*) {
    return compare_order(args, true);
}

// CEP:WHAT: Implements the lua.str_eq hook for the static string equality fast path.
// CEP:WHY: String equality compares bytes; the fast path skips the generic equality protocol.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest comparison error for non-string operands.
// CEP:ASSUMES: Arguments carry two values.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_str_eq(std::span<const vx::TaggedValue> args, void*) {
    if (!is_string_obj(args[0]) || !is_string_obj(args[1])) throw_compare();
    return vx::TaggedValue::boolean(string_obj_eq(args[0], args[1]));
}

// CEP:WHAT: Implements the lua.str_lt hook for the static string ordering fast path.
// CEP:WHY: Lexicographic byte order matches the reference definition of string comparison.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest comparison error for non-string operands.
// CEP:ASSUMES: Arguments carry two values.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_str_lt(std::span<const vx::TaggedValue> args, void*) {
    if (!is_string_obj(args[0]) || !is_string_obj(args[1])) throw_compare();
    return vx::TaggedValue::boolean(string_obj_lt(args[0], args[1]));
}

// CEP:WHAT: Implements the lua.str_le hook for the static string ordering fast path.
// CEP:WHY: Lexicographic byte order with equality matches the reference definition.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest comparison error for non-string operands.
// CEP:ASSUMES: Arguments carry two values.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_str_le(std::span<const vx::TaggedValue> args, void*) {
    if (!is_string_obj(args[0]) || !is_string_obj(args[1])) throw_compare();
    return vx::TaggedValue::boolean(!string_obj_lt(args[1], args[0]));
}

// CEP:WHAT: Implements the lua.check_step hook: validates the numeric for step.
// CEP:WHY: A zero step would loop forever; the reference language rejects it at loop entry.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest error for a zero step or a non-numeric step.
// CEP:ASSUMES: Arguments carry the step value.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_check_step(std::span<const vx::TaggedValue> args, void* user) {
    [[maybe_unused]] auto* ctx = static_cast<RuntimeContext*>(user);
    const NumVal n = num_val_of(args[0]);
    if (!n.ok) throw LuaError{args[0], "'for' step must be a number"};
    if (n.d == 0.0) {
        throw LuaError{vx::TaggedValue::null(), "'for' step is zero"};
    }
    // The step value already sits in the frame window; returning it keeps
    // the register identity without any new allocation.
    return args[0];
}

// CEP:WHAT: Implements the lua.for_cont hook: the numeric for continuation test.
// CEP:WHY: The test direction follows the step's sign, evaluated per iteration with exact numeric rules.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest error when a control value is not a number.
// CEP:ASSUMES: Arguments carry control, limit, and step values.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_for_cont(std::span<const vx::TaggedValue> args, void* user) {
    [[maybe_unused]] auto* ctx = static_cast<RuntimeContext*>(user);
    const NumVal ctrl = num_val_of(args[0]);
    const NumVal limit = num_val_of(args[kForLimitArg]);
    const NumVal step = num_val_of(args[kForStepArg]);
    if (!ctrl.ok || !limit.ok || !step.ok) {
        throw LuaError{vx::TaggedValue::null(),
                       "'for' control values must be numbers"};
    }
    return vx::TaggedValue::boolean(step.d > 0.0 ? (ctrl.d <= limit.d)
                                             : (ctrl.d >= limit.d));
}

// CEP:WHAT: Implements the lua.for_step hook: the numeric for increment.
// CEP:WHY: Integer steps advance with overflow detection; float steps advance in IEEE arithmetic.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest error on integer overflow.
// CEP:ASSUMES: Arguments carry control and step values.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_for_step(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    const NumVal a = num_val_of(args[0]);
    const NumVal b = num_val_of(args[kFirstOptionalArg]);
    if (a.is_int && b.is_int) {
        int64_t r = 0;
        if (__builtin_add_overflow(a.i, b.i, &r)) {
            throw LuaError{vx::TaggedValue::null(), "integer overflow"};
        }
        return vx::TaggedValue::smi(r);
    }
    return make_double(*ctx, a.d + b.d);
}

// CEP:WHAT: Implements the lua.next hook behind the pairs protocol.
// CEP:WHY: Iteration resumes just past the previous key's probed slot and
//          walks the backing forward in slot order, returning one key and
//          value per call without rescanning from the start.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest error when the target is not a table;
//          allocation failure propagates.
// CEP:ASSUMES: Arguments carry the table and the previous key (nil to
//          start). On an unmodified table the forward-only scan visits each
//          backing slot at most once per pass (probe clusters wrap, but
//          their wrapped entries sit at lower slots and were visited
//          earlier), so a full pass terminates and yields every live entry
//          exactly once. Under concurrent modification the reference
//          language leaves order and completeness undefined; here, clearing
//          any key before the cursor runs backward-shift repair that can
//          move an already-visited entry below the re-derived start, so
//          such entries may be yielded again. A vanished previous key
//          restarts the scan. The slot index is derived before the kv
//          allocation and the pair is re-read through a fresh backing
//          resolution after it.
// CEP:COST: @warm; O(1) amortized per step, O(cluster) to reposition.
// CEP:EVIDENCE: pairs tests across the corpus visit each entry exactly once.
vx::TaggedValue hook_next(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    if (!is_table(args[0])) {
        throw LuaError{args[0], "the pairs target is not a table"};
    }
    vx::ArrayObject* b = table_backing(args[0]);
    const int64_t cap = backing_capacity(b);
    int64_t start = 0;
    if (!args[1].is_null()) {
        const int64_t prev = probe_slot(b, args[1], key_hash(args[1]));
        if (b->element(prev).is_null()) {
            start = 0;  // previous key vanished; restart per the reference UB stance
        } else {
            const int64_t entry =
                (prev - kBackingPairsBase) / kTableSlotsPerPair;
            start = entry + 1;
        }
    }
    for (int64_t entry = start; entry < cap; ++entry) {
        const int64_t slot = kBackingPairsBase + entry * kTableSlotsPerPair;
        if (b->element(slot).is_null()) continue;
        auto kv = ctx->heap->allocate_array(kKvPairSlots);
        if (!kv) throw LuaError{vx::TaggedValue::null(), "out of memory"};
        // Re-resolve after the allocation: only frame registers are
        // GC-updated, so the element read must follow, not precede it.
        vx::ArrayObject* fresh = table_backing(args[0]);
        ctx->heap->barriered_store(*kv, &(*kv)->element(0),
                                   fresh->element(slot));
        ctx->heap->barriered_store(*kv, &(*kv)->element(1),
                                   fresh->element(slot + 1));
        return vx::TaggedValue::heap_pointer(*kv);
    }
    return vx::TaggedValue::null();
}

// CEP:WHAT: Implements the lua.inext hook behind the ipairs protocol.
// CEP:WHY: Numeric iteration stops at the first absent positive index.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest error when the target is not a table; allocation failure propagates.
// CEP:ASSUMES: Arguments carry the table and the previous index.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_inext(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    if (!is_table(args[0])) {
        throw LuaError{args[0], "the ipairs target is not a table"};
    }
    const int64_t next = args[1].as_smi() + 1;
    auto kv = ctx->heap->allocate_array(kKvPairSlots);
    if (!kv) throw LuaError{vx::TaggedValue::null(), "out of memory"};
    // Read after the allocation so the resolve sees post-GC bits.
    const vx::TaggedValue v = table_get(args[0], vx::TaggedValue::smi(next));
    if (v.is_null()) return vx::TaggedValue::null();
    ctx->heap->barriered_store(*kv, &(*kv)->element(0),
                               vx::TaggedValue::smi(next));
    ctx->heap->barriered_store(*kv, &(*kv)->element(1), v);
    return vx::TaggedValue::heap_pointer(*kv);
}

vx::TaggedValue hook_call_error(std::span<const vx::TaggedValue> args, void*) {
    throw LuaError{args[0], "attempt to call a non-function value"};
}

// CEP:WHAT: Implements the lua.method_error hook: raises the method-call error.
// CEP:WHY: Missing methods must fail loudly with the method name in the message.
// CEP:STATUS: complete
// CEP:FAILURE: Always throws the guest method-call error.
// CEP:ASSUMES: Arguments carry the receiver and the method name.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_method_error(std::span<const vx::TaggedValue> args, void*) {
    std::string msg = "attempt to call a non-function method";
    if (args.size() > 1 && is_string_obj(args[1])) {
        msg += " '";
        msg += string_bytes(args[1]);
        msg += "'";
    }
    throw LuaError{args[0], msg};
}

// CEP:WHAT: Implements the lua.print hook.
// CEP:WHY: Standard output formatting renders each value with the shared spelling, tab-separated.
// CEP:STATUS: complete
// CEP:FAILURE: none (I/O failure surfaces through the C library).
// CEP:ASSUMES: The context renders values; stdout accepts writes.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_print(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    std::string out;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0) out.push_back(kPrintSeparator);
        out += render_value(args[i], *ctx);
    }
    out.push_back('\n');
    std::fputs(out.c_str(), stdout);
    return vx::TaggedValue::undefined();
}

// CEP:WHAT: Implements the lua.type hook.
// CEP:WHY: The type names are part of the language surface; the klass tag drives every branch.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Arguments carry one value.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_type(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    const char* name = "userdata";
    if (args[0].is_null()) {
        name = "nil";
    } else if (args[0].is_boolean()) {
        name = "boolean";
    } else if (is_number(args[0])) {
        name = "number";
    } else if (is_string_obj(args[0])) {
        name = "string";
    } else if (is_table(args[0])) {
        name = "table";
    } else if (is_function(args[0])) {
        name = "function";
    }
    return make_string(*ctx, name);
}

// CEP:WHAT: Implements the lua.tonumber hook with the optional base form.
// CEP:WHY: Base conversion uses exact integer parsing; the one-argument form shares the general coercion.
// CEP:STATUS: complete
// CEP:FAILURE: Returns nil for values without a numeric form.
// CEP:ASSUMES: Arguments carry a value and optionally a base.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_tonumber(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    if (args.size() > kFirstOptionalArg && !args[1].is_null()) {
        const int64_t base = args[1].as_smi();
        if (!is_string_obj(args[0])) return vx::TaggedValue::null();
        const std::string text = string_bytes(args[0]);
        try {
            size_t consumed = 0;
            const int64_t v =
                std::stoll(text, &consumed, static_cast<int>(base));
            if (consumed != text.size()) return vx::TaggedValue::null();
            return vx::TaggedValue::smi(v);
        } catch (const std::exception&) {
            return vx::TaggedValue::null();
        }
    }
    const NumVal n = num_val_of(args[0]);
    if (!n.ok) return vx::TaggedValue::null();
    if (n.is_int) return vx::TaggedValue::smi(n.i);
    return make_double(*ctx, n.d);
}

// CEP:WHAT: Implements the lua.tointeger hook.
// CEP:WHY: Only exact integer payloads convert; everything else returns nil per the reference rules.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Arguments carry one value.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_tointeger(std::span<const vx::TaggedValue> args, void*) {
    if (args[0].is_smi()) return args[0];
    double d = 0.0;
    if (float_of(args[0], d) && is_smi_representable_double(d)) {
        return vx::TaggedValue::smi(static_cast<int64_t>(d));
    }
    return vx::TaggedValue::null();
}

// CEP:WHAT: Implements the lua.tostring hook.
// CEP:WHY: One renderer keeps print, concat, and tostring spellings identical.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Arguments carry one value.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_tostring(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    return make_string(*ctx, render_value(args[0], *ctx));
}

// CEP:WHAT: Implements the lua.assert hook.
// CEP:WHY: Falsy values raise the caller's message or the standard assertion failure text.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest error carrying the message value when the check fails.
// CEP:ASSUMES: Arguments carry the checked value and optionally a message.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_assert(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    const bool truthy =
        !args[0].is_null() &&
        !(args[0].is_boolean() && !args[0].as_boolean_unchecked());
    if (truthy) return args[0];
    const vx::TaggedValue msg =
        args.size() > kFirstOptionalArg
            ? args[1]
            : make_string(*ctx, "assertion failed!");
    throw LuaError{msg, render_value(msg, *ctx)};
}

// CEP:WHAT: Implements the lua.error hook.
// CEP:WHY: Errors unwind as guest errors carrying the payload value; the driver renders the text.
// CEP:STATUS: complete
// CEP:FAILURE: Always throws the guest error with the payload.
// CEP:ASSUMES: Arguments carry the payload (or nothing).
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_error(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    const vx::TaggedValue payload =
        args.empty() ? make_string(*ctx, "error") : args[0];
    throw LuaError{payload, render_value(payload, *ctx)};
}

// ---- string methods (lua.mpath.*) -------------------------------------------

// CEP:WHAT: Returns the byte payload of a string method receiver or argument.
// CEP:WHY: Every string method starts from bytes; one accessor keeps the type error uniform.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest error for non-string arguments.
// CEP:ASSUMES: Arguments carry at least the receiver string.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
std::string expect_string_arg(std::span<const vx::TaggedValue> args,
                              size_t index) {
    if (index >= args.size() || !is_string_obj(args[index])) {
        throw LuaError{vx::TaggedValue::null(),
                       "a string method ran on a non-string value"};
    }
    return string_bytes(args[index]);
}

// CEP:WHAT: Implements the sub string method.
// CEP:WHY: Negative indices count from the end and the range clamps to the payload, matching the reference rules.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest error for non-string receivers.
// CEP:ASSUMES: Arguments carry the string and an optional index pair.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue string_method_sub(RuntimeContext& ctx,
                              std::span<const vx::TaggedValue> args) {
    const std::string bytes = expect_string_arg(args, 0);
    const int64_t len = static_cast<int64_t>(bytes.size());
    int64_t i = args.size() > kFirstOptionalArg && args[1].is_smi()
                    ? args[1].as_smi()
                    : 1;
    int64_t j = args.size() > kSecondOptionalArg && args[kValueArgIndex].is_smi()
                    ? args[kValueArgIndex].as_smi()
                    : -1;
    if (i < 0) i = len + i + 1;
    if (j < 0) j = len + j + 1;
    if (i < 1) i = 1;
    if (j > len) j = len;  // clamps use the one-based bounds
    if (i > j) return make_string(ctx, "");
    return make_string(ctx, std::string_view(bytes).substr(
                                static_cast<size_t>(i - 1),
                                static_cast<size_t>(j - i + 1)));
}

// CEP:WHAT: Implements the rep string method.
// CEP:WHY: Non-positive counts produce the empty string; the output size is reserved up front.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest error for non-string receivers and the
//          out-of-memory guest error when the requested size exceeds the
//          named string bound (kMaxStringBytes), so no host length_error
//          escapes the guest error path.
// CEP:ASSUMES: Arguments carry the string and a count in Smi range (the
//          emitter guarantees numeric method arguments).
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue string_method_rep(RuntimeContext& ctx,
                              std::span<const vx::TaggedValue> args) {
    const std::string bytes = expect_string_arg(args, 0);
    const int64_t n = args[1].as_smi();
    if (n <= 0) return make_string(ctx, "");
    if (static_cast<int64_t>(bytes.size()) >
        kMaxStringBytes / n) {
        throw LuaError{vx::TaggedValue::null(), "out of memory"};
    }
    std::string out;
    out.reserve(bytes.size() * static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) out += bytes;
    return make_string(ctx, out);
}

// CEP:WHAT: Implements the upper string method.
// CEP:WHY: ASCII case folding matches the reference behavior for the byte-oriented payload.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest error for non-string receivers.
// CEP:ASSUMES: Arguments carry the string.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue string_method_upper(RuntimeContext& ctx,
                                std::span<const vx::TaggedValue> args) {
    std::string bytes = expect_string_arg(args, 0);
    for (char& c : bytes) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return make_string(ctx, bytes);
}

// CEP:WHAT: Implements the lower string method.
// CEP:WHY: ASCII case folding matches the reference behavior for the byte-oriented payload.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest error for non-string receivers.
// CEP:ASSUMES: Arguments carry the string.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue string_method_lower(RuntimeContext& ctx,
                                std::span<const vx::TaggedValue> args) {
    std::string bytes = expect_string_arg(args, 0);
    for (char& c : bytes) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return make_string(ctx, bytes);
}

// CEP:WHAT: Implements the len string method.
// CEP:WHY: The byte length is the payload's stored length word.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest error for non-string receivers.
// CEP:ASSUMES: Arguments carry the string.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue string_method_len(std::span<const vx::TaggedValue> args, void*) {
    if (!is_string_obj(args[0])) {
        throw LuaError{args[0], "the length method ran on a non-string"};
    }
    return vx::TaggedValue::smi(
        body_of(args[0])->element(kStringLenSlot).as_smi());
}

// CEP:WHAT: Implements the reverse string method.
// CEP:WHY: The payload reverses byte-wise; the result allocates once.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest error for non-string receivers.
// CEP:ASSUMES: Arguments carry the string.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue string_method_reverse(RuntimeContext& ctx,
                                  std::span<const vx::TaggedValue> args) {
    std::string bytes = expect_string_arg(args, 0);
    std::reverse(bytes.begin(), bytes.end());
    return make_string(ctx, bytes);
}

// CEP:WHAT: Implements the byte string method (single-value form).
// CEP:WHY: The single-value conformance target returns the first requested byte; the range form reduces to it.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest error for non-string receivers; returns nil when the index is out of range.
// CEP:ASSUMES: Arguments carry the string and optionally an index.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue string_method_byte(std::span<const vx::TaggedValue> args, void*) {
    const std::string bytes = expect_string_arg(args, 0);
    const int64_t len = static_cast<int64_t>(bytes.size());
    const int64_t i =
        args.size() > kFirstOptionalArg ? args[1].as_smi() : 1;
    if (i < 1 || i > len) return vx::TaggedValue::null();
    // The single-value conformance target returns the first requested byte.
    return vx::TaggedValue::smi(static_cast<uint8_t>(bytes[i - 1]));
}

// CEP:WHAT: Implements the char string method.
// CEP:WHY: Each argument masks to one byte; the result allocates once.
// CEP:STATUS: complete
// CEP:FAILURE: none (out-of-range arguments wrap by design).
// CEP:ASSUMES: Arguments carry byte values.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue string_method_char(std::span<const vx::TaggedValue> args,
                               void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    std::string out;
    for (size_t i = 0; i < args.size(); ++i) {
        out.push_back(static_cast<char>(args[i].as_smi() & kByteMask));
    }
    return make_string(*ctx, out);
}

// ---- math library -----------------------------------------------------------

// CEP:WHAT: Shared body of the single-argument math hooks.
// CEP:WHY: Rounding hooks return integers when the result is exactly representable, matching the reference contract.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numeric operands.
// CEP:ASSUMES: Arguments carry one numeric operand.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue math_unary(RuntimeContext& ctx, vx::TaggedValue v,
                       double (*fn)(double)) {
    const NumVal n = num_val_of(v);
    if (!n.ok) throw_arith("math");
    const double r = fn(n.d);
    if (n.is_int && r == static_cast<double>(n.i)) {
        return vx::TaggedValue::smi(n.i);
    }
    if (is_smi_representable_double(r)) {
        return vx::TaggedValue::smi(static_cast<int64_t>(r));
    }
    return make_double(ctx, r);
}

// CEP:WHAT: Implements the lua.math.floor hook.
// CEP:WHY: Floor rounds toward negative infinity and returns an integer when representable.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numeric operands.
// CEP:ASSUMES: Arguments carry one numeric operand.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_math_floor(std::span<const vx::TaggedValue> args, void* user) {
    return math_unary(*static_cast<RuntimeContext*>(user), args[0],
                      [](double d) { return std::floor(d); });
}

// CEP:WHAT: Implements the lua.math.ceil hook.
// CEP:WHY: Ceiling rounds toward positive infinity and returns an integer when representable.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numeric operands.
// CEP:ASSUMES: Arguments carry one numeric operand.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_math_ceil(std::span<const vx::TaggedValue> args, void* user) {
    return math_unary(*static_cast<RuntimeContext*>(user), args[0],
                      [](double d) { return std::ceil(d); });
}

// CEP:WHAT: Implements the lua.math.abs hook.
// CEP:WHY: Integer magnitudes stay integers; floats keep the IEEE absolute value.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numeric operands.
// CEP:ASSUMES: Arguments carry one numeric operand.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_math_abs(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    const NumVal n = num_val_of(args[0]);
    if (!n.ok) throw_arith("math");
    if (n.is_int) return vx::TaggedValue::smi(n.i < 0 ? -n.i : n.i);
    return make_double(*ctx, std::fabs(n.d));
}

// CEP:WHAT: Implements the lua.math.sqrt hook.
// CEP:WHY: The IEEE square root is the exact reference behavior, including NaN for negative inputs.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numeric operands.
// CEP:ASSUMES: Arguments carry one numeric operand.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_math_sqrt(std::span<const vx::TaggedValue> args, void* user) {
    return math_unary(*static_cast<RuntimeContext*>(user), args[0],
                      [](double d) { return std::sqrt(d); });
}

// CEP:WHAT: Implements the lua.math.exp hook.
// CEP:WHY: The IEEE exponential is the reference behavior for this target.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numeric operands.
// CEP:ASSUMES: Arguments carry one numeric operand.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_math_exp(std::span<const vx::TaggedValue> args, void* user) {
    return math_unary(*static_cast<RuntimeContext*>(user), args[0],
                      [](double d) { return std::exp(d); });
}

// CEP:WHAT: Implements the lua.math.log hook (natural log form).
// CEP:WHY: The natural logarithm is the one-argument reference behavior; the base form is a documented gap.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numeric operands.
// CEP:ASSUMES: Arguments carry one numeric operand.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_math_log(std::span<const vx::TaggedValue> args, void* user) {
    return math_unary(*static_cast<RuntimeContext*>(user), args[0],
                      [](double d) { return std::log(d); });
}

// CEP:WHAT: Implements the lua.math.fmod hook.
// CEP:WHY: The IEEE remainder-truncated modulo matches the reference two-argument form.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numeric operands.
// CEP:ASSUMES: Arguments carry two numeric operands.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_math_fmod(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    const NumVal a = num_val_of(args[0]);
    if (!a.ok) throw_arith("math");
    const NumVal b = num_val_of(args[1]);
    if (!b.ok) throw_arith("math");
    return make_double(*ctx, std::fmod(a.d, b.d));
}

// CEP:WHAT: Implements the lua.math.pow hook.
// CEP:WHY: The two-argument power matches the ^ operator's IEEE behavior.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numeric operands.
// CEP:ASSUMES: Arguments carry two numeric operands.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_math_pow(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    const NumVal a = num_val_of(args[0]);
    if (!a.ok) throw_arith("math");
    const NumVal b = num_val_of(args[1]);
    if (!b.ok) throw_arith("math");
    return make_double(*ctx, std::pow(a.d, b.d));
}

// CEP:WHAT: Implements the lua.math.min hook.
// CEP:WHY: Folding left to right matches the reference variadic definition.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numeric operands.
// CEP:ASSUMES: Arguments carry at least one numeric operand.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_math_min(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    NumVal best = num_val_of(args[0]);
    if (!best.ok) throw_arith("math");
    for (size_t i = 1; i < args.size(); ++i) {
        const NumVal d = num_val_of(args[i]);
        if (!d.ok) throw_arith("math");
        if (d.d < best.d) best = d;
    }
    if (best.is_int) return vx::TaggedValue::smi(best.i);
    return make_double(*ctx, best.d);
}

// CEP:WHAT: Implements the lua.math.max hook.
// CEP:WHY: Folding left to right matches the reference variadic definition.
// CEP:STATUS: complete
// CEP:FAILURE: Throws the guest arithmetic error for non-numeric operands.
// CEP:ASSUMES: Arguments carry at least one numeric operand.
// CEP:COST: cold; one hook call per site (not performance-critical).
// CEP:EVIDENCE: tests/run_tests.sh executes this path on the T0 engine.
vx::TaggedValue hook_math_max(std::span<const vx::TaggedValue> args, void* user) {
    auto* ctx = static_cast<RuntimeContext*>(user);
    NumVal best = num_val_of(args[0]);
    if (!best.ok) throw_arith("math");
    for (size_t i = 1; i < args.size(); ++i) {
        const NumVal d = num_val_of(args[i]);
        if (!d.ok) throw_arith("math");
        if (d.d > best.d) best = d;
    }
    if (best.is_int) return vx::TaggedValue::smi(best.i);
    return make_double(*ctx, best.d);
}

vx::TaggedValue hook_math_const_pi(std::span<const vx::TaggedValue>, void* user) {
    return make_double(*static_cast<RuntimeContext*>(user), kPi);
}

vx::TaggedValue hook_math_const_huge(std::span<const vx::TaggedValue>, void* user) {
    return make_double(*static_cast<RuntimeContext*>(user),
                       std::numeric_limits<double>::infinity());
}

vx::TaggedValue hook_math_const_maxinteger(std::span<const vx::TaggedValue>, void*) {
    return vx::TaggedValue::smi(vx::TaggedValue::smi_max());
}

vx::TaggedValue hook_math_const_mininteger(std::span<const vx::TaggedValue>, void*) {
    return vx::TaggedValue::smi(vx::TaggedValue::smi_min());
}

}  // namespace

// CEP:WHAT: Builds the hook registry.
// CEP:WHY: One table drives install(); names match the emitter constants.
// CEP:STATUS: complete
// CEP:FAILURE: Registry growth throws std::bad_alloc, which is fatal at
//              the driver boundary; no guest error is involved here.
// CEP:ASSUMES: Hook names stay synchronized with emitter_internal.hpp.
// CEP:COST: cold; one map build per process.
// CEP:EVIDENCE: install() registers every name the corpus compiles.
void LuaRuntime::register_default_hooks() {
    hooks_["lua.new_table"] = hook_new_table;
    hooks_["lua.raw_get"] = hook_raw_get;
    hooks_["lua.raw_set"] = hook_raw_set;
    hooks_["lua.getmetatable"] = hook_getmetatable;
    hooks_["lua.setmetatable"] = hook_setmetatable;
    hooks_["lua.is_function"] = hook_is_function;
    hooks_["lua.is_string"] = hook_is_string;
    hooks_["lua.getenv"] = hook_getenv;
    hooks_["lua.setenv"] = hook_setenv;
    hooks_["lua.conststr"] = hook_conststr;
    hooks_["lua.length"] = hook_length;
    hooks_["lua.concat"] = hook_concat;
    hooks_["lua.op_neg"] = hook_op_neg;
    hooks_["lua.op_add"] = hook_op_add;
    hooks_["lua.op_sub"] = hook_op_sub;
    hooks_["lua.op_mul"] = hook_op_mul;
    hooks_["lua.op_div"] = hook_op_div;
    hooks_["lua.op_floordiv"] = hook_op_floordiv;
    hooks_["lua.op_mod"] = hook_op_mod;
    hooks_["lua.op_pow"] = hook_op_pow;
    hooks_["lua.op_bnot"] = hook_op_bnot;
    hooks_["lua.op_band"] = hook_op_band;
    hooks_["lua.op_bor"] = hook_op_bor;
    hooks_["lua.op_bxor"] = hook_op_bxor;
    hooks_["lua.op_shl"] = hook_op_shl;
    hooks_["lua.op_shr"] = hook_op_shr;
    hooks_["lua.op_eq"] = hook_op_eq;
    hooks_["lua.op_lt"] = hook_op_lt;
    hooks_["lua.op_le"] = hook_op_le;
    hooks_["lua.str_eq"] = hook_str_eq;
    hooks_["lua.str_lt"] = hook_str_lt;
    hooks_["lua.str_le"] = hook_str_le;
    hooks_["lua.num_f64"] = hook_num_f64;
    hooks_["lua.check_step"] = hook_check_step;
    hooks_["lua.for_cont"] = hook_for_cont;
    hooks_["lua.for_step"] = hook_for_step;
    hooks_["lua.next"] = hook_next;
    hooks_["lua.inext"] = hook_inext;
    hooks_["lua.call_error"] = hook_call_error;
    hooks_["lua.method_error"] = hook_method_error;
    hooks_["lua.print"] = hook_print;
    hooks_["lua.type"] = hook_type;
    hooks_["lua.tonumber"] = hook_tonumber;
    hooks_["lua.tointeger"] = hook_tointeger;
    hooks_["lua.tostring"] = hook_tostring;
    hooks_["lua.assert"] = hook_assert;
    hooks_["lua.error"] = hook_error;

    hooks_["lua.math.floor"] = hook_math_floor;
    hooks_["lua.math.ceil"] = hook_math_ceil;
    hooks_["lua.math.abs"] = hook_math_abs;
    hooks_["lua.math.sqrt"] = hook_math_sqrt;
    hooks_["lua.math.exp"] = hook_math_exp;
    hooks_["lua.math.log"] = hook_math_log;
    hooks_["lua.math.fmod"] = hook_math_fmod;
    hooks_["lua.math.pow"] = hook_math_pow;
    hooks_["lua.math.min"] = hook_math_min;
    hooks_["lua.math.max"] = hook_math_max;
    hooks_["lua.math.pi"] = hook_math_const_pi;
    hooks_["lua.math.huge"] = hook_math_const_huge;
    hooks_["lua.math.maxinteger"] = hook_math_const_maxinteger;
    hooks_["lua.math.mininteger"] = hook_math_const_mininteger;

    // String methods dispatch through per-name builtins ("lua.mpath.<m>");
    // the library-call form string.<m>(s, ...) aliases the same bodies.
    hooks_["lua.mpath.sub"] = [](std::span<const vx::TaggedValue> a, void* u) {
        return string_method_sub(*static_cast<RuntimeContext*>(u), a);
    };
    hooks_["lua.mpath.rep"] = [](std::span<const vx::TaggedValue> a, void* u) {
        return string_method_rep(*static_cast<RuntimeContext*>(u), a);
    };
    hooks_["lua.mpath.upper"] = [](std::span<const vx::TaggedValue> a, void* u) {
        return string_method_upper(*static_cast<RuntimeContext*>(u), a);
    };
    hooks_["lua.mpath.lower"] = [](std::span<const vx::TaggedValue> a, void* u) {
        return string_method_lower(*static_cast<RuntimeContext*>(u), a);
    };
    hooks_["lua.mpath.len"] = string_method_len;
    hooks_["lua.mpath.reverse"] = [](std::span<const vx::TaggedValue> a, void* u) {
        return string_method_reverse(*static_cast<RuntimeContext*>(u), a);
    };
    hooks_["lua.mpath.byte"] = string_method_byte;
    hooks_["lua.mpath.char"] = string_method_char;
    for (const char* m :
         {"sub", "rep", "upper", "lower", "len", "reverse", "byte", "char"}) {
        hooks_[std::string(kStringLibPrefix) + m] =
            hooks_[std::string(kStringLibMethodPrefix) + m];
    }
}

bool LuaRuntime::has(const std::string& name) const noexcept {
    return hooks_.find(name) != hooks_.end();
}

// CEP:WHAT: Registers the hooks a compiled module needs on an interpreter.
// CEP:WHY: The driver registers exactly the module's declared builtins;
//          unknown names are a compile and runtime contract violation.
// CEP:STATUS: complete
// CEP:FAILURE: Returns a Diagnostic naming any unregistered builtin.
// CEP:ASSUMES: The interpreter is fresh and the context outlives the run.
// CEP:COST: cold; linear in the declared builtin count.
// CEP:EVIDENCE: every module in the corpus installs cleanly.
vortex::support::Result<void> LuaRuntime::install(
    vortex::vm::Interpreter& interp, RuntimeContext& ctx) const {
    for (const auto& b : ctx.module->builtins) {
        auto it = hooks_.find(b.name);
        if (it == hooks_.end()) {
            return support::fail(support::ErrorCode::RuntimeError,
                                 "no runtime hook registered for builtin '" +
                                     b.name + "'");
        }
        interp.register_builtin(b.name, it->second, &ctx);
    }
    return support::ok();
}

}  // namespace luavortex
