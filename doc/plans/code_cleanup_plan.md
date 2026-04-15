# Design Review: Race Conditions & Memory Management

## Status (2026-04-15)

ISSUES 1, 2, and 3 were fixed in commit `a5b111c` ("Fix use-after-free
during forced device removal via barrier shutdown") using a closed-loop
sentinel protocol (LL_TERMINATED → DEVICE_REMOVE barriers) that replaces
timeout-based teardown.  `msg_queue_finalize` now takes a context and
properly frees heap payloads / decrements tmap refs.

**Still open:** ISSUE 4 (POSIX list remove without lock), ISSUE 5
(atomics for `do_exit`), ISSUE 7 (tmap reader protection and snapshot
on publish), and the Part 2 items (pool high-water marks, reference
counting).  ISSUE 6 was determined to be a false positive.

## Prompt

This folder contains a user-space driver for Joulescope products.  doc/architecture.svg contains a block diagram.  I have a few
  things that I would like to investigate.
  1. race conditions, especially when tearing down devices on removal or error.  I suspect that we may refer to queues and/or
  messages that are no longer valid.
  2. Improving allocation & free for messages and queues.  Perhaps use reference counting?

  Please investigate, perform a design review, audit the existing implementation and make recommendations.


## Context

This is an audit and design review of the Joulescope user-space driver, focusing on:
1. Race conditions during device teardown/removal/error paths
2. Message and queue allocation/free patterns, with consideration of reference counting

The driver uses a multi-threaded architecture with message-passing between threads:
- **API thread(s)**: Any thread calling the public API
- **Frontend thread**: Single thread running pubsub dispatch (`src/jsdrv.c:709`)
- **UL device threads**: One per device, JS220/JS110 protocol handling (`src/js220_usb.c`)
- **LL device threads**: One per device, USB I/O (`src/backend/winusb/backend.c:595`)
- **Backend discovery thread**: Device hotplug monitoring (`src/backend/winusb/backend.c:896`)

---

## Part 1: Race Condition Audit

### ISSUE 1 (HIGH) — FIXED in a5b111c: Use-after-free in `device_subscriber` during device removal

**Files:** `src/jsdrv.c:426-432`, `src/jsdrv.c:531-542`

**Root cause:** When a device is removed, `device_remove()` frees the `frontend_dev_s` immediately, but the pubsub unsubscribe is deferred (enqueued to `msg_pend`). Any messages already in `msg_pend` for this device's topics will invoke `device_subscriber` with a dangling `user_data` pointer.

**Scenario:**
1. Device sends data/status messages via `jsdrvp_backend_send` → queued to `msg_backend`
2. Device disconnect detected → backend sends `DEVICE_REMOVE` → also queued to `msg_backend`
3. Frontend processes backend messages in order:
   - Processes data messages → `jsdrv_pubsub_publish` adds them to `msg_pend`
   - Processes `DEVICE_REMOVE` → `device_remove()` frees `frontend_dev_s`, enqueues unsubscribe to `msg_pend`
4. `jsdrv_pubsub_process` runs:
   - First processes data messages (added earlier) → calls `device_subscriber(freed_ptr, msg)` → **USE-AFTER-FREE**
   - Then processes unsubscribe (added later)

**Recommendation:** Unsubscribe the device **synchronously** before freeing, or defer the free until after pubsub processing. Simplest fix: in `device_remove()`, call `jsdrv_pubsub_process(c->pubsub)` between `device_sub(d, UNSUBSCRIBE)` and `jsdrv_free(d)` to flush the unsubscribe. Alternatively, process the unsubscribe directly rather than via `msg_pend`.

### ISSUE 2 (MEDIUM) — FIXED in a5b111c: LL device thread may write to freed `rsp_q`

**Files:** `src/backend/winusb/backend.c:637-639`, `src/backend/winusb/backend.c:790-801`

**Root cause:** When the LL device thread exits, its last action is `jsdrvp_send_finalize_msg(d->context, d->device.rsp_q, ...)`. If `device_thread_stop()` times out after 1000ms, `device_free()` proceeds to `msg_queue_finalize(d->device.rsp_q)`, destroying the queue. Meanwhile, the LL thread may still be running `device_close()` or pushing to the now-freed `rsp_q`.

**Scenario:** A device doing heavy I/O may take >1000ms in `device_close()` → `bulk_in_finalize()` → `WinUsb_GetOverlappedResult(... TRUE)` (blocking wait). After timeout, `device_free` frees the queue; the LL thread then pushes to freed memory.

**Recommendation:**
- Increase the join timeout or make it infinite (the LL thread WILL exit eventually once USB I/O completes or errors)
- Or: NULL-check `rsp_q` before pushing and use an atomic for the pointer
- Or: use a separate "thread exited" event that the join waits on, and only free resources after confirmed exit

### ISSUE 3 (MEDIUM) — FIXED in a5b111c: `msg_queue_finalize` leaks message payloads

**Files:** `src/backend/winusb/msg_queue.c:61-82`, `src/backend/libusb/msg_queue.c:65-84`

**Root cause:** `msg_queue_finalize` calls `jsdrv_free(msg)` directly instead of `jsdrvp_msg_free(context, msg)`. This means:
- Messages with `JSDRV_UNION_FLAG_HEAP_MEMORY` have their heap-allocated payloads leaked
- Messages with `JSDRV_PAYLOAD_TYPE_BUFFER_RSP` don't decrement their tmap reference count

**Recommendation:** Pass the context to `msg_queue_finalize` and use `jsdrvp_msg_free` for proper cleanup. Or add a cleanup callback parameter.

### ISSUE 4 (LOW-MEDIUM): POSIX `msg_queue_push` modifies list without lock

**File:** `src/backend/libusb/msg_queue.c:94-101`

**Root cause:** `jsdrv_list_remove(&msg->item)` is called BEFORE acquiring the mutex. If the message is somehow still in a list (e.g., a different queue due to a bug), this modifies that list without holding its lock. The Windows version has the same issue (the remove is under the destination lock, but not the source lock).

**Note:** In normal operation, messages should not be in a queue when pushed to another. This is a defensive measure. But if it does happen, it silently corrupts a list.

**Recommendation:** Either remove this defensive `jsdrv_list_remove` and replace with an assertion (`JSDRV_ASSERT(jsdrv_list_is_empty(&msg->item))`), or document that it's a best-effort safety net.

### ISSUE 5 (LOW): `volatile bool` insufficient for non-x86 architectures

**Files:** `src/jsdrv.c:90`, `src/js220_usb.c:286`, `src/js110_usb.c:473`,
`src/backend/libusb/backend.c:113`, `src/js320_fwup_mgr.c:141-143`,
`src/mb_device.c:158`

**Root cause:** `volatile bool do_exit` provides sufficient ordering
on x86 (strong memory model) but is technically insufficient on
ARM/other architectures. Without a memory barrier, a thread may not
see the updated value promptly.

**Note:** The WinUSB backend (`src/backend/winusb/backend.c:106`)
already migrated to plain `bool` (not `volatile`), but the remaining
files listed above still use `volatile bool`. ARM targets
(Raspberry Pi, Apple Silicon) would need proper atomics.

**Recommendation:** Use `_Atomic bool` (C11 atomics) or
platform-specific atomic operations. Low priority if only targeting
x86.

### ISSUE 6 (LOW): `device_scan` removal during iteration

**File:** `src/backend/winusb/backend.c:860-870` (removal at ~868)

**Analysis:** Initially appears unsafe, but `jsdrv_list_foreach` caches `next__` before the loop body (`list.h:336-339`), so removing the current item is safe. **This is NOT a bug** — the safe iterator handles it correctly.

### ISSUE 7 (HIGH): Mutable tmap shared with consumers

**Files:** `src/buffer_signal.c:133-157`, `src/buffer_signal.c:193-194`,
`include/jsdrv/tmap.h`, `src/tmap.c`

**Root cause:** The API shares a **mutable** tmap pointer with
consumers via `jsdrv_buffer_info_s.tmap`. The device/buffer thread
continues to mutate this tmap (adding entries, expiring old ones)
while consumers hold a reference to the same object. This creates
three concrete problems:

**Problem A — Missing reader protection in buffer_signal.c:**
`samples_to_utc()` and `utc_to_samples()` (`buffer_signal.c:133-157`)
call tmap query functions **without** `jsdrv_tmap_reader_enter/exit`.
These are called from `jsdrv_bufsig_info()` (line 184) and
`jsdrv_bufsig_process_request()` (line 842). The writer thread can
modify tmap concurrently, causing undefined behavior.

**Problem B — Consumer holds mutable pointer across threads:**
When a `JSDRV_PAYLOAD_TYPE_BUFFER_RSP` message is delivered to an
API consumer's callback, the embedded `info.tmap` pointer references
the live, mutable tmap. The consumer must follow the reader_enter/exit
protocol, but:
- This discipline is easy to forget (no compile-time enforcement)
- Forgetting `reader_exit` silently blocks all writer updates
  indefinitely
- No `jsdrv_union_copy()`-style helper exists for safe snapshotting
- The C API documentation doesn't prominently warn about this

**Problem C — Stale tmap after buffer clear/restart:**
If the buffer is cleared (`jsdrv_bufsig_clear` at line 257) or
the device restarts, the old tmap is freed via `jsdrv_tmap_ref_decr`.
Any consumer that hasn't yet decremented its reference still holds
a valid (ref-counted) pointer, but the data is stale and no longer
corresponds to the active stream. There is no notification mechanism
for this.

**Mitigation options (from least to most invasive):**

1. **Fix immediate bugs** — Add `reader_enter/exit` calls in
   `buffer_signal.c` around `samples_to_utc()` and `utc_to_samples()`.
   Small fix, addresses Problem A only.

2. **Snapshot on publish** — When building a buffer response, create
   an immutable snapshot of the tmap (`jsdrv_tmap_snapshot()` — new
   function). Consumers receive a frozen copy they can read without
   locking. Writer continues mutating the original. Addresses A and B.
   Cost: one allocation + memcpy of the entry array per response.

3. **Copy-on-write tmap** — tmap internally uses a COW entry array.
   Writer creates a new array on mutation; readers hold a reference
   to the old one. No reader_enter/exit needed. Addresses A, B, and C
   but significantly increases implementation complexity.

**Recommendation:** Option 2 (snapshot on publish) is the best
tradeoff. It eliminates the shared-mutable-state problem at the API
boundary while keeping the internal writer path simple. The snapshot
cost is small relative to the 256KB data buffers already being
copied. Fix Problem A immediately as a prerequisite.

---

## Part 2: Message Allocation & Free Review

### Current Design

The system uses a **two-tier free-pool** pattern:
- `msg_free`: Pool of normal-sized messages (~1.2KB each)
- `msg_free_data`: Pool of data messages (~256KB each)
- Heap fallback: `jsdrv_alloc` if pool is empty
- Return to pool: `jsdrvp_msg_free` returns messages to the appropriate pool
- During `do_exit`: messages are freed to heap instead of pooled

### Problem: Unbounded Pool Growth

Messages allocated during peak activity are returned to the free pool and never released until `jsdrv_finalize`. For data messages at 256KB each, this is significant. A burst of 4 concurrent USB transfers creates 4 data messages (~1MB) that persist forever in the pool.

**Recommendation:** Add a high-water mark to the free pools. When returning a message, if the pool exceeds the limit, `jsdrv_free` the message instead of pooling it. Suggested limits: ~16 for normal messages, ~4 for data messages.

### On Reference Counting

The current system has implicit ownership transfer: a message is owned by exactly one entity at a time (the allocator → a queue → the processor → the free pool). This is clean and avoids the complexity of reference counting.

**Where reference counting could help:**

1. **Pubsub fan-out:** When a message is published and multiple subscribers need it, the current design either:
   - Passes the same message pointer to each subscriber (subscribers must not modify or free it)
   - Clones the message for the `device_subscriber` path (`jsdrvp_msg_clone`)

   For data messages (256KB), cloning is expensive. Reference counting would allow sharing the same buffer across subscribers.

2. **tmap sharing:** Already implemented ad-hoc in `jsdrvp_msg_free` for `JSDRV_PAYLOAD_TYPE_BUFFER_RSP` / `jsdrv_tmap_ref_decr`.

**Recommendation:** If you want reference counting, the cleanest approach is to add it **to the message struct itself**, not to the payload:

```c
struct jsdrvp_msg_s {
    struct jsdrv_list_s item;
    _Atomic int32_t ref_count;  // NEW: starts at 1
    uint32_t inner_msg_type;
    // ... rest unchanged
};
```

With helper functions:
```c
void jsdrvp_msg_incref(struct jsdrvp_msg_s * msg);      // atomic increment
void jsdrvp_msg_decref(struct jsdrv_context_s * ctx, struct jsdrvp_msg_s * msg);  // atomic decrement, free when 0
```

The existing `jsdrvp_msg_free` becomes `jsdrvp_msg_decref`. For the common single-owner case, behavior is identical (ref_count goes 1→0, message freed). For shared cases (pubsub fan-out, data message sharing), callers `incref` before sharing.

**Caveat:** Reference counting adds complexity and creates subtle bugs if counts are mismanaged. The current clone-based approach is safer and simpler. I'd only recommend reference counting if profiling shows `jsdrvp_msg_clone` of data messages is a measurable bottleneck.

### Specific Allocation Improvements

1. **Pool size limits** (described above) — simple, high value
2. **`msg_queue_finalize` cleanup** (ISSUE 3) — use `jsdrvp_msg_free` instead of `jsdrv_free`
3. **Optional: pre-allocate pool** — allocate N messages at init time to avoid heap allocation during streaming. Currently the first N messages always hit `jsdrv_alloc`.

---

## Summary of Recommended Changes (Priority Order)

| Priority | Issue | Files to Modify | Effort |
|----------|-------|-----------------|--------|
| ~~P0~~ DONE | ~~Fix use-after-free in device_subscriber (ISSUE 1)~~ | `src/jsdrv.c` | a5b111c |
| **P0** | Fix missing tmap reader protection (ISSUE 7A) | `src/buffer_signal.c` | Small |
| **P1** | Tmap snapshot on publish (ISSUE 7B) | `src/tmap.c`, `src/buffer_signal.c` | Medium |
| ~~P1~~ DONE | ~~Fix msg_queue_finalize payload leak (ISSUE 3)~~ | `msg_queue.h`, both `msg_queue.c`, `jsdrv.c` | a5b111c |
| ~~P1~~ DONE | ~~Fix LL thread timeout → freed rsp_q (ISSUE 2)~~ | `src/backend/winusb/backend.c` | a5b111c |
| **P2** | Add free-pool high-water marks | `src/jsdrv.c` | Small |
| **P3** | Fix POSIX list remove without lock (ISSUE 4) | `src/backend/libusb/msg_queue.c` | Trivial |
| **P3** | C11 atomics for do_exit flags (ISSUE 5) | Multiple | Small |
| **P4** | Reference counting on messages | `frontend.h`, `jsdrv.c` | Medium-Large |

## Verification

- Run existing test suite
- Stress test: rapidly connect/disconnect devices while streaming
- Valgrind/AddressSanitizer on Linux to detect use-after-free
- Long-running test to verify no memory growth from pool accumulation
