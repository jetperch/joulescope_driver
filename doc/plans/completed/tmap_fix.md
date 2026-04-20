# tmap fix — copy on publish, drop reference counting

**Status**: proposed
**Date**: 2026-04-20

## Problem

`jsdrv_tmap_s` is published from the buffer/device thread (writer) to
API consumers (readers) via `jsdrv_buffer_info_s.tmap`. The current
design keeps the writer and readers looking at the *same* instance,
coordinated by:

- A `ref_count` so consumers and the writer can independently hold the
  instance.
- A `reader_count` with `reader_enter/exit` so writes can be deferred
  while any reader is active.
- A mutex guarding both counters and deferred updates.

The existing code review (`doc/plans/completed/design_review_plan.md`)
flagged three problems with this pattern:

1. Forgetting `jsdrv_tmap_reader_exit()` silently blocks all writer
   updates indefinitely, with no compile-time enforcement.
2. `samples_to_utc()` / `utc_to_samples()` in `src/buffer_signal.c`
   already call tmap query functions without entering a reader section.
3. After a buffer clear or device restart, a consumer that still holds
   a reference points at a tmap whose data no longer corresponds to the
   active stream, with no notification mechanism.

## Decision

**Copy on publish. Remove reference counting and the reader/writer
discipline entirely.**

Every `jsdrv_tmap_s` instance becomes single-owner. At publish time the
writer allocates a fresh tmap, copies its entries into it, and attaches
the copy to `jsdrv_buffer_info_s.tmap`. The consumer receives an
immutable snapshot it can query freely with no locking. When the
consumer is done, it calls `jsdrv_tmap_free()`.

This trades a small per-publish allocation + `memcpy` (bounded by the
entry-array size, typically a few KB) for:
- No mutex. No deferred update plumbing. No `reader_count`, no
  `ref_count`, no `time_map_update_pending`, no `tail_update_pending`.
- The snapshot is stable regardless of subsequent writer activity
  (fixes Problem 3 above).
- No discipline required of consumers (fixes Problem 1). The writer's
  own internal queries in `samples_to_utc()` / `utc_to_samples()` are
  already on the writer thread, so they are safe without any protection
  (fixes Problem 2).

This is the `jsdrv` host driver only. 2.0 is not yet released; break the
API.

## API changes (`include/jsdrv/tmap.h`)

Remove:
- `jsdrv_tmap_ref_incr()`
- `jsdrv_tmap_ref_decr()`
- `jsdrv_tmap_reader_enter()`
- `jsdrv_tmap_reader_exit()`

Add:
- `jsdrv_tmap_free(struct jsdrv_tmap_s * self)` — frees the entry array
  and the struct. Safe to pass `NULL`.
- `struct jsdrv_tmap_s * jsdrv_tmap_copy(const struct jsdrv_tmap_s * src)`
  — allocates a new instance with the same entries as `src`. The new
  instance is independent of `src`.

Keep (behaviour stays, doc updated):
- `jsdrv_tmap_alloc()`
- `jsdrv_tmap_clear()`
- `jsdrv_tmap_length()`
- `jsdrv_tmap_add()`
- `jsdrv_tmap_expire_by_sample_id()`
- `jsdrv_tmap_sample_id_to_timestamp()`
- `jsdrv_tmap_timestamp_to_sample_id()`
- `jsdrv_tmap_get()`

The module-level doc becomes: "Single-owner time map. The writer holds
one instance; each published response carries an independent copy
produced by `jsdrv_tmap_copy()`. There is no sharing and no locking."
Drop the reference-counted note.

## Struct changes (`src/tmap.c`)

`struct jsdrv_tmap_s` simplifies to:

```c
struct jsdrv_tmap_s {
    size_t alloc_size;  // always a power of 2
    size_t head;
    size_t tail;
    struct jsdrv_time_map_s * entry;
};
```

Removed fields: `ref_count`, `reader_count`, `mutex`,
`time_map_update_pending`, `time_map_update`, `tail_update_pending`,
`tail_update`.

`jsdrv_tmap_add()` collapses: the `defer_add()` path becomes
unconditional (no `reader_count` gate, no deferred buffer).
`jsdrv_tmap_expire_by_sample_id()` updates `self->tail` directly.
`jsdrv_tmap_clear()` is now a two-field reset with no mutex.

`jsdrv_tmap_copy()` allocates a new tmap sized to `max(tmap_size(src),
initial_default)`, then `memcpy`s the entry array in logical order
(handling the ring wrap), resetting `head`/`tail` in the copy to
`[0, length)`.

## Writer update (`src/buffer_signal.c`)

Current publish site (lines 188-196):

```c
info->tmap = NULL;
if (NULL != self->tmap) {
    size_t tmap_sz = jsdrv_tmap_length(self->tmap);
    if (tmap_sz) {
        jsdrv_tmap_get(self->tmap, tmap_sz - 1, &info->time_map);
        jsdrv_tmap_ref_incr(self->tmap);
        info->tmap = self->tmap;
    }
}
```

Becomes:

```c
info->tmap = NULL;
if (NULL != self->tmap) {
    size_t tmap_sz = jsdrv_tmap_length(self->tmap);
    if (tmap_sz) {
        jsdrv_tmap_get(self->tmap, tmap_sz - 1, &info->time_map);
        info->tmap = jsdrv_tmap_copy(self->tmap);
    }
}
```

Writer finalize (line 129): `jsdrv_tmap_ref_decr(self->tmap)` →
`jsdrv_tmap_free(self->tmap)`.

## Message teardown (`src/jsdrv.c`)

Line 799:

```c
jsdrv_tmap_ref_decr(rsp->info.tmap);
```

Becomes:

```c
jsdrv_tmap_free(rsp->info.tmap);
```

## Public docstring for `jsdrv_buffer_info_s.tmap` (`include/jsdrv.h`)

Replace the reference-counting paragraph with:

> This instance is a snapshot owned by the consumer. Call
> `jsdrv_tmap_free()` when done. No locking is required for queries.

## Python binding (`pyjoulescope_driver/binding.pyx`)

- `TimeMap.factory()` no longer calls `jsdrv_tmap_ref_incr`. It takes
  ownership of the passed pointer directly.
- `__del__` calls `jsdrv_tmap_free` instead of `jsdrv_tmap_ref_decr`.
- `__copy__` / `__deepcopy__` call `jsdrv_tmap_copy` and wrap the
  result (previously they aliased via `ref_incr`, which was wrong
  semantically — `copy.copy(tm)` should give an independent instance).
- `__enter__` / `__exit__` become no-ops and can be kept as
  backward-compatible shims or removed. Removing them is preferred
  (the `_reader_active` counter in the binding also goes away).
- `query()` stops auto-entering a reader section — the wrapping
  `with self:` block in `sample_id_to_timestamp` / `timestamp_to_sample_id`
  just goes away.
- `c_jsdrv.pxd` drops the four removed symbols and adds the two new
  ones.

## Tests

`test/tmap_test.c`:
- Replace `test_concurrency` (which exercises `reader_enter/exit` and
  deferred writes) with a `test_copy` that verifies:
  - A copy has the same length and entries as the source.
  - Writing to the source after copy does not affect the copy.
  - Writing to the copy after copy does not affect the source.
  - Freeing the source does not invalidate the copy.
- Replace `jsdrv_tmap_ref_decr(s)` cleanup at the end of every test
  with `jsdrv_tmap_free(s)`.
- Add `test_free_null` verifying `jsdrv_tmap_free(NULL)` is safe.

`test/buffer_signal_test.c` (line 145) already queries directly on the
internal writer tmap and needs no change.

## Risk / compatibility

- Public ABI breaks at the tmap boundary. Acceptable: 2.0 has not
  shipped; see `CHANGELOG.md` for similar tmap signature changes in
  this release window.
- `pyjoulescope_driver` needs rebuild. Cython binding regen is part of
  the normal build.
- Any third-party consumers that reach directly into `jsdrv_tmap_s`
  (shouldn't exist — struct is opaque) would break.
- Memory: one extra allocation + entry-array `memcpy` per published
  buffer response. Entry count is bounded by `expire_by_sample_id`
  policy; typical sizes are ≤ a few hundred entries (each ~24 B), so
  ≤ a few KB per publish. Negligible against the 256 KB data payloads
  already in flight.

## Sequencing

Task list in implementation order (each is a small, reviewable step):

1. Update `include/jsdrv/tmap.h`: remove 4 ref/reader functions, add
   `jsdrv_tmap_free` and `jsdrv_tmap_copy`, rewrite module-level doc.
2. Update `include/jsdrv.h`: rewrite the `jsdrv_buffer_info_s.tmap`
   field docstring.
3. Rewrite `src/tmap.c`: drop the 7 struct fields and the mutex; drop
   the 4 functions; collapse `defer_add` into `jsdrv_tmap_add`;
   simplify `clear` / `expire_by_sample_id`; implement
   `jsdrv_tmap_free` and `jsdrv_tmap_copy`.
4. Update `src/buffer_signal.c`: replace `ref_incr` with `copy` at the
   publish site; replace `ref_decr` with `free` at the finalize site.
5. Update `src/jsdrv.c`: replace `ref_decr` with `free` in
   `jsdrvp_msg_free`.
6. Update `test/tmap_test.c`: replace all `ref_decr` cleanups with
   `free`, replace `test_concurrency` with `test_copy`, add
   `test_free_null`.
7. Update `pyjoulescope_driver/c_jsdrv.pxd`: drop removed symbols, add
   `jsdrv_tmap_free` and `jsdrv_tmap_copy`.
8. Update `pyjoulescope_driver/binding.pyx`: fix `TimeMap.factory`,
   `__del__`, `__copy__`, `__deepcopy__`; remove reader-context
   plumbing and `_reader_active`.
9. Build the C library and run `ctest` — all 27 tests plus the new
   copy/free tests should pass.
10. Rebuild the Python extension and run its test suite.
11. Add `CHANGELOG.md` entry under the 2.0 section summarising the
    tmap API change.
