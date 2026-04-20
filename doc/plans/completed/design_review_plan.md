# C API Design Review — jsdrv (Joulescope Host Driver)

**Scope**: All 11 headers in `include/` (~2,700 lines)
**Version**: 2.0.0
**Date**: 2026-02-23
**Updated**: 2026-04-20 (resolutions applied)

---

## Resolved in this revision

### Completeness
- **`jsdrv_meta_syntax_check` is now implemented** (`src/meta.c`). It parses
  the metadata as JSON and rejects inputs whose root is not an object.
  Tests: `test/meta_test.c`.

### Symbol export (Windows DLL correctness)
- **`JSDRV_API` added** to every public function in `jsdrv/os_event.h`
  (alloc/free/signal/reset) and `jsdrv/os_mutex.h` (all four functions).
- **`JSDRV_CPP_GUARD_START/END` added** around `jsdrv_version_u32_to_str`.

### Doc/code inconsistencies
- `jsdrv_union_eq` now refers to `jsdrv_union_eq_exact` (not the non-existent
  `jsdrv_union_eq_strict`).
- `jsdrv_union_widen` docstring no longer copy-pastes the `equiv` text.
- `jsdrv_statistics_s.rsv3_u8` renamed to `rsv3_u32` to match its type.
- `jsdrv_statistics_s.decimate_factor` docstring no longer claims the value
  is always `2` (it varies per device and downsampling).
- `jsdrv_buffer_info_s.time_range_utc` docstring no longer makes negative
  reference to `time_map`.
- `"length in invalid"` → `"length is invalid"` in `error_code.h`,
  `error_code.c`, and `binding.pyx`.
- `jsdrv_version_u32_to_str` now directs callers to
  `JSDRV_VERSION_STR_LENGTH_MAX` instead of the literal `14`.
- `jsdrv.h` now includes `jsdrv/error_code.h` so callers of the main API
  immediately have access to the `JSDRV_ERROR_*` codes and
  `jsdrv_error_code_name()` without a separate include.

### Type/signature consistency
- `jsdrv_error_code_name` and `_description` now take `int32_t` (not `int`).
- All `jsdrv_cstr_*` functions now return `int32_t` and use `const char *`.
- `jsdrv_log_publish` signature switched to `int8_t level` to match every
  other log-level API and avoid `JSDRV_LOG_LEVEL_OFF = -1` aliasing.
- `jsdrv_thread_create` priority switched to `int32_t`.
- `jsdrv_thread_is_current` parameter now `const jsdrv_thread_t *`.

### Header hygiene
- Include guards `JSDRV_OS_*_H__` renamed to `JSDRV_OS_*_H_` across the five
  `os_*.h` headers to avoid ISO C §7.1.3 reserved identifiers.
- `THREAD_RETURN_TYPE` / `THREAD_ARG_TYPE` / `THREAD_RETURN()` renamed to
  `JSDRV_THREAD_*` to remove global-namespace pollution.

### New API additions
- **`JSDRV_TIMEOUT_MS_ASYNC`** constant added and documented alongside
  `JSDRV_TIMEOUT_MS_DEFAULT` and `JSDRV_TIMEOUT_MS_INIT`, including an
  explicit note that `jsdrv_initialize`, `jsdrv_finalize`, and
  `jsdrv_query` interpret `timeout_ms == 0` as "use default timeout"
  while the remaining entry points treat `0` as async.
- **`jsdrv_union_copy()`** helper added (`union.h` / `src/union.c`) for the
  common "copy union out of callback before it goes out of scope" pattern.
  Tests: `test/union_test.c`.

### Clarified docstrings
- `jsdrv_unsubscribe` now documents the silent `cbk_user_data`-mismatch
  behavior so callers are not surprised when a stale subscription persists.

---

## Still open (require broader decisions)

- **Enum member prefix**: `jsdrv_element_type_e` members still use
  `JSDRV_DATA_TYPE_*`. Left unchanged because the existing names are
  spread across ~40 references in C, Python, and Node.js bindings; a
  coordinated rename remains a separate decision.
- **No synchronous device enumeration**: device discovery still requires
  subscribing to `@/list` or `@/!add`/`@/!remove`.
- **Onboarding quickstart** and a dedicated example header would help
  first-time integrators grasp the PubSub model.
- **tmap enter/exit discipline**: a C++ RAII wrapper or a documented
  `goto cleanup` pattern would reduce the risk of deadlocking the writer.
- **String-typed topic keys**: no compile-time validation for device
  sub-topics assembled by string concatenation.
- **`jsdrv_query` buffer pre-allocation**: for string/JSON/binary types,
  the caller must pre-allocate a buffer inside `jsdrv_union_s` before
  calling.  A helper (or clearer example in the docstring) would make
  first use less error-prone.
- **Manual `jsdrv_union_s` construction**: callers who construct a union
  without using the provided macros must remember to zero `op`, `app`,
  and `size`.  Adding a `jsdrv_union_init()` or emphasising the macros
  more prominently in docs would help.

---

## Design Strengths

- **PubSub architecture**: decouples device communication from application
  logic and enables language bindings.
- **Union type system**: comprehensive, with good convenience macros. The
  `_r` suffix for retained values and `c` prefix for const are nice
  patterns.
- **Thread safety model**: clearly documented — callbacks from driver
  thread, caller responsible for sync.
- **34Q30 time format**: excellent choice for precision measurement with
  good precision-limit documentation.
- **tmap reader/writer pattern**: sound single-writer/multi-reader design.
- **Documentation coverage**: nearly 100% Doxygen coverage across all
  public APIs.
