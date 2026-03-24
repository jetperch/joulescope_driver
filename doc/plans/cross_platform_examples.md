# Cross-Platform minibitty CLI Examples

## Context

The `example/minibitty/` CLI tools only build on Windows due to
direct Win32 API usage.  The jsdrv library has custom
cross-platform abstractions (`jsdrv_os_event_t`,
`jsdrv_os_mutex_t`, `jsdrv_thread_t`) in `src/backend/`.
The CLI needs semaphores, atomics, and event-wait which are
not currently abstracted.

An initial migration attempt revealed that the existing
abstractions have subtle behavioral differences between
platforms (e.g., manual-reset vs auto-reset events) and
the metadata fetch during device open interacts poorly with
the CLI's response callbacks.  We need to define and
thoroughly test the cross-platform threading API before
migrating the CLI.

## Current Win32 Dependencies (10 files)

| API | Purpose | Files |
|-----|---------|-------|
| `CreateSemaphore/ReleaseSemaphore/WaitForSingleObject` | Pipeline depth control | fpga_mem, mem, state_get |
| `CreateEvent/SetEvent/WaitForSingleObject` | Async completion | firmware, fpga_mem, loopback, pubsub_info |
| `InterlockedIncrement/Decrement/Exchange` | Atomic counters | fpga_mem, mem, state_get, pubsub_info |
| `Sleep()` | Delays | adapter, loopback, stream, throughput, pubsub_test |

## Existing jsdrv Abstractions

| Primitive | Header | Win32 | POSIX | Notes |
|-----------|--------|-------|-------|-------|
| Event | `jsdrv_prv/event.h` | `HANDLE` (manual-reset) | pipe + poll | No wait function |
| Mutex | `jsdrv_prv/mutex.h` | `HANDLE` | pthread_mutex | With timeout |
| Thread | `jsdrv_prv/thread.h` | `CreateThread` | pthread | Sleep included |

**Missing**: semaphore, atomic ops, event wait with timeout.

## Options to Evaluate

### Option A: Extend jsdrv's Custom Abstractions

Add semaphore, atomics, and event-wait to the existing
`jsdrv_prv/` headers and `src/backend/{windows,posix}.c`.

**Pros:**
- Consistent with existing codebase
- No new dependencies
- Full control over behavior

**Cons:**
- Must implement and test each primitive per-platform
- Subtle behavioral differences (manual vs auto-reset,
  named vs unnamed semaphores, macOS sem_timedwait)
- More code to maintain

### Option B: Use CThreads Library

[CThreads](https://github.com/PerformanC/CThreads) provides
a cross-platform C threading API.

**Evaluate:**
- Does it cover semaphores? (Yes: `cthreads_sem_*`)
- Does it cover atomics?
- License compatibility (MIT — compatible with Apache 2.0)
- maturity / platform support (Windows, Linux, macOS)
- Integration complexity (header-only? static lib?)

**Pros:**
- Pre-built, tested cross-platform primitives
- Reduces maintenance burden
- Community-maintained

**Cons:**
- External dependency
- May not match jsdrv's exact needs
- Need to evaluate quality and coverage

### Option C: C11 Threads + Compiler Builtins

Use C11 `<threads.h>` for threads/mutexes and compiler
builtins for atomics (`__sync_*` or `<stdatomic.h>`).

**Pros:**
- Standard C — no external dependency
- Atomics are well-defined across compilers

**Cons:**
- MSVC C11 thread support is incomplete
- No semaphore in C11 threads
- Event abstraction still needed

### Option D: Minimal Wrapper Using POSIX + Win32

Write a thin `jsdrv_os_*` wrapper that maps directly to
native APIs per-platform, with a comprehensive test suite.

**Pros:**
- Thinnest possible abstraction
- No behavioral surprises — just maps to native
- Each platform uses its optimal implementation

**Cons:**
- Must handle platform quirks (macOS deprecated unnamed
  semaphores, Windows manual vs auto-reset events)
- More `#ifdef` blocks

## Recommended Approach

1. **Evaluate CThreads** — check if it covers our needs
   (semaphore with timeout, atomic inc/dec, event wait).
   Write a small test program on all 3 platforms.

2. **Define the API** — regardless of implementation, define
   the exact API the CLI needs:
   ```c
   // Semaphore
   jsdrv_os_sem_t jsdrv_os_sem_alloc(int32_t initial, int32_t max);
   void jsdrv_os_sem_free(jsdrv_os_sem_t sem);
   int32_t jsdrv_os_sem_wait(jsdrv_os_sem_t sem, uint32_t timeout_ms);
   void jsdrv_os_sem_release(jsdrv_os_sem_t sem);

   // Atomic
   typedef volatile int32_t jsdrv_atomic_t;
   int32_t jsdrv_atomic_inc(jsdrv_atomic_t * p);
   int32_t jsdrv_atomic_dec(jsdrv_atomic_t * p);
   void jsdrv_atomic_set(jsdrv_atomic_t * p, int32_t v);

   // Event wait (add to existing event.h)
   int32_t jsdrv_os_event_wait(jsdrv_os_event_t ev,
                                uint32_t timeout_ms);
   ```

3. **Write tests** — create `test/os_primitives_test.c` that
   exercises each primitive:
   - Semaphore: producer/consumer with multiple threads
   - Atomics: concurrent increment from N threads
   - Event: signal/wait/reset/timeout
   - Build and run on Windows, Linux, macOS

4. **Migrate CLI** — only after tests pass on all platforms

## Implementation Order

1. Define API headers
2. Implement for all platforms
3. Write and run cross-platform tests
4. Migrate example/minibitty files
5. Build and test on all platforms
