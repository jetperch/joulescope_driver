# C API Design Review — jsdrv (Joulescope Host Driver)

**Scope**: All 11 headers in `include/` (~2,700 lines)
**Version**: 2.0.0
**Date**: 2026-02-23

---

## Open Items

### Consistency

- **Enum prefix mismatch** — `enum jsdrv_element_type_e` members use
  `JSDRV_DATA_TYPE_*` prefix instead of `JSDRV_ELEMENT_TYPE_*`. ~40 references
  across C, Python, and Node.js bindings. The existing names are well-established.

- **Return type `int` vs `int32_t`** — `cstr.h` functions return `int` while the
  rest of the API uses `int32_t`. Blast radius across `cstr.c` and all callers.

### Usability Improvements

- **Onboarding complexity** — The PubSub model requires understanding topics,
  suffixes, retained values, metadata JSON, and the callback threading model.
  A quickstart example in `jsdrv.h` or a separate example header would help.

- **`jsdrv_query` buffer pre-allocation trap** — For string/JSON/binary types,
  the caller must pre-allocate a buffer inside `jsdrv_union_s` before calling
  `jsdrv_query()`. Passing an uninitialized union for a string topic is a common
  first-use mistake.

- **Callback value lifetime** — `jsdrv_subscribe_fn` says "value only remains
  valid for the duration of the callback" and "binary or string data must be
  copied." No `jsdrv_union_copy()` helper is provided — adding one would prevent
  common use-after-free bugs.

- **`jsdrv_unsubscribe` matching** — Requires both `cbk_fn` AND `cbk_user_data`
  to match. A mismatch produces no error — the subscription silently remains active.

- **Timeout semantics inconsistency** — `jsdrv_publish(timeout_ms=0)` means async,
  but `jsdrv_initialize(timeout_ms=0)` means "use default timeout" (blocking).
  Consider named constants like `JSDRV_TIMEOUT_ASYNC` vs `JSDRV_TIMEOUT_DEFAULT`.

- **No `error_code.h` include from `jsdrv.h`** — The main header returns `int32_t`
  error codes but doesn't include `error_code.h`. Callers must separately include
  it to get `jsdrv_error_code_name()`.

- **`jsdrv_union_s` manual construction** — Callers who manually construct a
  `jsdrv_union_s` (instead of using macros) must remember to zero `op`, `app`,
  and `size`. Partial initialization is a common C mistake.

- **tmap enter/exit discipline** — Forgetting `jsdrv_tmap_reader_exit()` silently
  blocks all writer updates indefinitely. For C++ consumers, a RAII wrapper would
  help. For C, documenting a `goto cleanup` pattern would be valuable.

- **No synchronous device enumeration** — Device discovery requires subscribing to
  `@/list` or `@/!add`/`@/!remove`. No simple synchronous "get device list" function
  exists.

- **String-typed topic keys** — Topics are `const char *` with no compile-time
  validation. Typos produce silent runtime failures. `JSDRV_MSG_*` constants help
  for system topics, but device-specific topics must be string-concatenated.

---

## Design Strengths

- **PubSub architecture**: Well-designed — decouples device communication from
  application logic and enables easy language binding.
- **Union type system**: Comprehensive with good convenience macros. The `_r` suffix
  for retained and `c` prefix for const are nice patterns.
- **Thread safety model**: Clearly documented — callbacks from driver thread, caller
  responsible for sync.
- **34Q30 time format**: Excellent choice for precision measurement with good
  precision limit documentation.
- **tmap reader/writer pattern**: Sound single-writer/multi-reader design.
- **Documentation coverage**: Nearly 100% Doxygen coverage across all public APIs.
