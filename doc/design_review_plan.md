# C API Design Review — jsdrv (Joulescope Host Driver)

**Scope**: All 11 headers in `include/` (~2,700 lines)
**Version**: 2.0.0
**Date**: 2026-02-23

---

## 1. Correctness Issues

### [FIXED] BUG — `JSDRV_API` dllimport was unreachable
**`cmacro_inc.h:65-68`**

The `#elif` condition was identical to the `#if` (`WIN32 && JSDRV_EXPORT`), making `dllimport` dead code.
Removed the unreachable branch. Note: if DLL consumers on Windows need `__declspec(dllimport)`,
a separate `#elif defined(WIN32)` branch should be added in the future.

### [FIXED] BUG — `min`/`max` doc comments swapped
**`jsdrv.h` — `jsdrv_summary_entry_s`**

The `min` field doc said "maximum" and vice versa. Corrected.

### [FIXED] BUG — Query/Response suffix documentation mismatch
**`jsdrv.h:93-95`**

The prose documentation was inconsistent with the `#define` values.
Documentation updated to match the actual defines: `&` = query request, `?` = query response.

### [FIXED] WARNING — `JSDRV_MSG_TIMEOUT` copy-paste doc error
**`jsdrv.h:185`**

Doc said "UnhandledDriver version: subscribe only JSDRV version (u32)" — a leftover from
`JSDRV_MSG_VERSION`. Updated to describe the actual timeout command.

### [FIXED] WARNING — `jsdrv_cstr_hex_to_u4` return range
**`cstr.h:239`**

Doc said "0 to 16" — corrected to "0 to 15" (4-bit nibble max is 0xF).

### [FIXED] WARNING — Typos in `tmap.h` function references
**`tmap.h:67,70,71`**

Documentation referenced `jsdrv_tmp_ref_incr` / `jsdrv_tmp_ref_decr` — corrected to
`jsdrv_tmap_ref_incr` / `jsdrv_tmap_ref_decr`.

### [FIXED] WARNING — `_r` retained macros used wrong union member
**`union.h:110-133`**

The retained variants for smaller integer types (u8, u16, u32, i8, i16, i32) wrote through
`.u64`/`.i64` without explicit narrowing casts, while non-retained variants used the matching
member (`.u8`, `.u16`, etc.). This was inconsistent and only correct on little-endian.

All macros now consistently write through the widest member (`.u64` for unsigned, `.i64` for
signed) with explicit narrowing casts to ensure proper zero-extension or sign-extension.
A compile-time endianness check was added to enforce the little-endian assumption.

---

## 2. Consistency & Convention Issues (remaining)

### WARNING — Missing `JSDRV_API` on public functions

Several public functions lack the `JSDRV_API` export macro, meaning they won't be
DLL-exported on Windows:

- **`cstr.h:162-173`**: `jsdrv_cstr_to_u64()` and `jsdrv_cstr_to_i64()`
- **`tmap.h`**: Nearly every function — `jsdrv_tmap_alloc`, `jsdrv_tmap_ref_incr/decr`,
  `jsdrv_tmap_clear`, `jsdrv_tmap_length`, `jsdrv_tmap_add`,
  `jsdrv_tmap_expire_by_sample_id`, `jsdrv_tmap_reader_enter/exit`,
  `jsdrv_tmap_sample_id_to_timestamp`, `jsdrv_tmap_timestamp_to_sample_id`, `jsdrv_tmap_get`

### SUGGESTION — Include guard naming inconsistency

Mixed conventions: double underscore (`JSDRV_TIME_H__`), single (`JSDRV_CSTR_H_`),
and custom (`JSDRV_LOG_INCLUDE_H_`). Single underscore is more C-standards-correct
since double underscores are technically reserved.

### SUGGESTION — Enum prefix mismatch

`enum jsdrv_element_type_e` members use `JSDRV_DATA_TYPE_*` prefix instead of
`JSDRV_ELEMENT_TYPE_*`, mismatching the enum name.

### SUGGESTION — Return type `int` vs `int32_t`

`cstr.h` functions return `int` while the rest of the API uses `int32_t`.
`int32_t` is more explicit for a cross-platform C library.

### SUGGESTION — Doxygen tag style mismatch

`version.h` uses `\brief`, `\param`, `\return` while all other files use
`@brief`, `@param`, `@return`.

---

## 3. Documentation Quality (remaining)

**Overall assessment**: Excellent. Nearly 100% Doxygen coverage.

| Severity | Location | Issue |
|----------|----------|-------|
| SUGGESTION | `jsdrv.h:134` | Typo: "topis" should be "topics" in `@defgroup` |
| SUGGESTION | `jsdrv.h:174` | Typo: "commad" should be "command" |
| WARNING | `time.h:162,188` | "IEEE 747" should be "IEEE 754" |
| WARNING | `cstr.h:100` | Parameter named `prefix` but it represents a suffix (`jsdrv_cstr_ends_with`) |
| WARNING | `jsdrv.h:510` | References `JSDRV_PUBSUB_SFLAG_PUB` and `JSDRV_PUBSUB_SFLAG_METADATA_RSP` — should be `JSDRV_SFLAG_PUB` and `JSDRV_SFLAG_METADATA_RSP` |
| WARNING | `tmap.h:149,196` | References `jsdrv_tmap_size()` — should be `jsdrv_tmap_length()` |
| WARNING | `jsdrv.h:717-720` | Doxygen `@param` syntax wrong: `msg[in]` should be `@param[in] msg` |
| SUGGESTION | `time.h:321,345,349` | `@param x` says "32-bit unsigned" but macros accept any integer |

---

## 4. API Design Observations

- **PubSub architecture**: Well-designed — decouples device communication from application
  logic and enables easy language binding.
- **Union type system**: Comprehensive with good convenience macros. The `_r` suffix for
  retained and `c` prefix for const are nice patterns.
- **Thread safety model**: Clearly documented — callbacks from driver thread, caller
  responsible for sync.
- **34Q30 time format**: Excellent choice for precision measurement with good precision
  limit documentation.
- **tmap reader/writer pattern**: Sound single-writer/multi-reader design.
- **`jsdrv_topic_s` fixed size**: 64-byte topic + 1 byte length is fine for a
  stack-allocated utility type.

---

## 5. Safety & Portability (remaining)

### WARNING — Empty parameter lists in C
**`log.h:169,183,190`**

`jsdrv_log_level_get()`, `jsdrv_log_initialize()`, and `jsdrv_log_finalize()` use `()`
instead of `(void)`. In C, `()` means unspecified parameters. Should be `(void)`.

### SUGGESTION — MSVC packed struct support

`JSDRV_STRUCT_PACKED` uses `__attribute__((packed))` for GCC and is empty for MSVC.
If packed structs are needed on MSVC, add `#pragma pack` support.

### SUGGESTION — `JSDRV_CPP_GUARD_END` trailing semicolon

`cmacro_inc.h:53`: `};` includes a semicolon after `extern "C" { }`. Harmless but
unnecessary.

---

## 6. API Usability Analysis

### Onboarding complexity
The PubSub model is powerful but has a steep learning curve. A consumer must understand
topics, suffixes, retained values, metadata JSON, and the callback threading model before
writing useful code. A quickstart example in `jsdrv.h` or a separate example header would help.

### String-typed topic keys — silent failure mode
Topics are `const char *` with no compile-time validation. Typos produce silent runtime
failures. The `JSDRV_MSG_*` constants help for system topics, but device-specific topics
must be string-concatenated by the caller.

### `jsdrv_query` buffer pre-allocation trap
For string/JSON/binary types, the caller must pre-allocate a buffer inside `jsdrv_union_s`
before calling `jsdrv_query()`. Passing an uninitialized union for a string topic is a
common first-use mistake.

### Callback value lifetime
The `jsdrv_subscribe_fn` callback says "value only remains valid for the duration of the
callback" and "binary or string data must be copied." No `jsdrv_union_copy()` helper is
provided — adding one would prevent common use-after-free bugs.

### `jsdrv_unsubscribe` matching
Requires both `cbk_fn` AND `cbk_user_data` to match. A mismatch produces no error — the
subscription silently remains active.

### Timeout semantics inconsistency
- `jsdrv_publish(timeout_ms=0)` means async (no wait)
- `jsdrv_initialize(timeout_ms=0)` means "use default timeout" (blocking)

Consider named constants like `JSDRV_TIMEOUT_ASYNC` vs `JSDRV_TIMEOUT_DEFAULT` to
make intent explicit.

### No `error_code.h` include from `jsdrv.h`
The main header returns `int32_t` error codes but doesn't include `error_code.h`.
Callers must separately include it to get `jsdrv_error_code_name()`.

### `jsdrv_union_s` manual construction
Callers who manually construct a `jsdrv_union_s` (instead of using macros) must remember
to zero `op`, `app`, and `size`. Partial initialization is a common C mistake.

### tmap enter/exit discipline
Forgetting `jsdrv_tmap_reader_exit()` silently blocks all writer updates indefinitely.
For C++ consumers, a RAII wrapper would help. For C, documenting a `goto cleanup` pattern
would be valuable.

### No synchronous device enumeration
Device discovery requires subscribing to `@/list` or `@/!add`/`@/!remove`. There's no
simple "give me the current device list" synchronous function — the caller must set up
PubSub infrastructure first.
