# Cross-Platform OS Library & minibitty CLI Examples

## Context

The `example/minibitty/` CLI tools only build on Windows due to
direct Win32 API usage.  The jsdrv library has custom
cross-platform abstractions (`jsdrv_os_event_t`,
`jsdrv_os_mutex_t`, `jsdrv_thread_t`) in
`include_private/jsdrv_prv/` with implementations in
`src/backend/{windows,posix}.c`.  The CLI also needs
semaphores, atomics, and event-wait which are not currently
abstracted.

An initial migration attempt revealed that the existing
abstractions have subtle behavioral differences between
platforms (e.g., manual-reset vs auto-reset events) and
the metadata fetch during device open interacts poorly with
the CLI's response callbacks.

**Goal:** Promote the OS primitives from private to a public
cross-platform library in `include/jsdrv/`, fix POSIX parity
gaps, add the missing primitives, write thorough tests, then
migrate the CLI.

## Decision Record

- **API exposure:** Public headers in `include/jsdrv/`
  so downstream consumers (examples, Node bindings,
  third-party code) can use them directly.
- **POSIX parity:** Full parity with Windows, including
  macOS quirks (no `sem_timedwait`,
  no `pthread_timedjoin_np`).
- **Atomics:** Thin `jsdrv_os_*` inline wrappers over
  `<stdatomic.h>` with a compiler-intrinsic fallback for
  older MSVC (pre-VS2022 17.5).  Never expose `_Atomic`
  types to Cython -- wrap in plain C functions.

Options B (CThreads) and C (C11 threads) from the original
plan are ruled out.  CThreads adds an external dependency
for something we control better ourselves.  C11 `<threads.h>`
has no semaphore, incomplete MSVC support, and still needs
an event abstraction.  We are going with a hybrid of
Options A and D: extend the existing jsdrv abstractions with
thin native wrappers and a comprehensive test suite.

## Current Win32 Dependencies (10 files)

| API | Purpose | Files |
|-----|---------|-------|
| `CreateSemaphore/ReleaseSemaphore/WaitForSingleObject` | Pipeline depth | fpga_mem, mem, state_get |
| `CreateEvent/SetEvent/WaitForSingleObject` | Async completion | firmware, fpga_mem, loopback, pubsub_info |
| `InterlockedIncrement/Decrement/Exchange` | Atomic counters | fpga_mem, mem, state_get, pubsub_info |
| `Sleep()` | Delays | adapter, loopback, stream, throughput, pubsub_test |

## Existing Abstractions & Gaps

| Primitive | Current Header | Win32 | POSIX | Gap |
|-----------|---------------|-------|-------|-----|
| Event | `jsdrv_prv/event.h` | HANDLE (manual-reset) | pipe+poll | **No wait-with-timeout** |
| Mutex | `jsdrv_prv/mutex.h` | HANDLE + timeout | pthread_mutex | **POSIX: no timeout, FATALs instead** |
| Thread | `jsdrv_prv/thread.h` | CreateThread | pthread | **POSIX: join ignores timeout, priority ignored** |
| Semaphore | -- | -- | -- | **Missing entirely** |
| Atomics | -- | -- | -- | **Missing entirely** |
| Sleep | `jsdrv_prv/thread.h` | Sleep() | nanosleep | OK (already cross-platform) |

## Public API Design

All new headers go in `include/jsdrv/`.  Types use opaque
pointers or small structs with platform-conditional members.

### include/jsdrv/os_event.h

Moved from `jsdrv_prv/event.h`.  Add wait-with-timeout.

```c
// Existing (relocated)
jsdrv_os_event_t jsdrv_os_event_alloc(void);
void jsdrv_os_event_free(jsdrv_os_event_t ev);
void jsdrv_os_event_signal(jsdrv_os_event_t ev);
void jsdrv_os_event_reset(jsdrv_os_event_t ev);

// New
int32_t jsdrv_os_event_wait(jsdrv_os_event_t ev,
                             uint32_t timeout_ms);
// Returns 0 on signaled, JSDRV_ERROR_TIMED_OUT on timeout.
```

**Windows:** `WaitForSingleObject(ev, timeout_ms)`.
**POSIX:** `poll()` on the pipe read fd with timeout.

### include/jsdrv/os_mutex.h

Moved from `jsdrv_prv/mutex.h`.  Fix POSIX timeout.

```c
jsdrv_os_mutex_t jsdrv_os_mutex_alloc(const char * name);
void jsdrv_os_mutex_free(jsdrv_os_mutex_t m);
int32_t jsdrv_os_mutex_lock(jsdrv_os_mutex_t m,
                             uint32_t timeout_ms);
void jsdrv_os_mutex_unlock(jsdrv_os_mutex_t m);
```

**POSIX fix:** Use `pthread_mutex_timedlock()` (available on
Linux and macOS 10.12+).  Returns `JSDRV_ERROR_TIMED_OUT`
instead of FATAL on timeout.  Keep FATAL for unexpected
pthread errors.

### include/jsdrv/os_thread.h

Moved from `jsdrv_prv/thread.h`.  Fix POSIX gaps.

```c
int32_t jsdrv_thread_create(jsdrv_thread_t * t,
    jsdrv_thread_fn fn, THREAD_ARG_TYPE arg, int priority);
int32_t jsdrv_thread_join(jsdrv_thread_t * t,
                           uint32_t timeout_ms);
bool    jsdrv_thread_is_current(jsdrv_thread_t const * t);
void    jsdrv_thread_sleep_ms(uint32_t duration_ms);
```

**POSIX join timeout:** Spawn a helper that does
`pthread_join` and signals an event; main thread waits on
event with timeout.  Or use `pthread_tryjoin_np` in a loop
with short sleeps (Linux-only; macOS needs the helper
approach).

**POSIX priority:** Use `pthread_setschedparam` with
`SCHED_OTHER` and adjusted niceness, or `SCHED_RR` where
available.  Document that elevated priority may require
`CAP_SYS_NICE` on Linux.

### include/jsdrv/os_sem.h (new)

```c
typedef struct jsdrv_os_sem_s * jsdrv_os_sem_t;

jsdrv_os_sem_t jsdrv_os_sem_alloc(int32_t initial_count,
                                   int32_t max_count);
void    jsdrv_os_sem_free(jsdrv_os_sem_t sem);
int32_t jsdrv_os_sem_wait(jsdrv_os_sem_t sem,
                           uint32_t timeout_ms);
void    jsdrv_os_sem_release(jsdrv_os_sem_t sem);
```

**Windows:** `CreateSemaphore` / `WaitForSingleObject` /
`ReleaseSemaphore`.

**Linux:** `sem_init` (unnamed) + `sem_timedwait`.

**macOS:** `sem_timedwait` is not available.  Use
`dispatch_semaphore_create` / `dispatch_semaphore_wait`
with `dispatch_time()` for timeout.  Alternatively,
condition-variable + mutex emulation.

### include/jsdrv/os_atomic.h (new)

```c
#include <stdint.h>

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) \
    && !defined(__STDC_NO_ATOMICS__)
  #include <stdatomic.h>
  typedef atomic_int_fast32_t jsdrv_os_atomic_t;
  #define JSDRV_OS_ATOMIC_INC(p)    (atomic_fetch_add((p), 1) + 1)
  #define JSDRV_OS_ATOMIC_DEC(p)    (atomic_fetch_sub((p), 1) - 1)
  #define JSDRV_OS_ATOMIC_SET(p, v) atomic_store((p), (v))
  #define JSDRV_OS_ATOMIC_GET(p)    atomic_load((p))
#elif defined(_WIN32)
  typedef volatile LONG jsdrv_os_atomic_t;
  #define JSDRV_OS_ATOMIC_INC(p)    InterlockedIncrement((p))
  #define JSDRV_OS_ATOMIC_DEC(p)    InterlockedDecrement((p))
  #define JSDRV_OS_ATOMIC_SET(p, v) InterlockedExchange((p), (v))
  #define JSDRV_OS_ATOMIC_GET(p)    InterlockedCompareExchange((p), 0, 0)
#else
  // GCC/Clang builtins (pre-C11 fallback)
  typedef volatile int32_t jsdrv_os_atomic_t;
  #define JSDRV_OS_ATOMIC_INC(p)    (__sync_add_and_fetch((p), 1))
  #define JSDRV_OS_ATOMIC_DEC(p)    (__sync_sub_and_fetch((p), 1))
  #define JSDRV_OS_ATOMIC_SET(p, v) __sync_lock_test_and_set((p), (v))
  #define JSDRV_OS_ATOMIC_GET(p)    __sync_add_and_fetch((p), 0)
#endif
```

Header-only, no .c file needed.  The three-tier fallback
ensures it works on all target compilers.  Cython bindings
should never include this header directly; wrap in plain C
functions if Python-side access is ever needed.

## Internal Migration

Once the public headers exist, update the private code:

- `include_private/jsdrv_prv/event.h` → thin include of
  `jsdrv/os_event.h` (or remove and update all includes)
- `include_private/jsdrv_prv/mutex.h` → same
- `include_private/jsdrv_prv/thread.h` → same
- `src/backend/windows.c` and `posix.c` → add semaphore
  and event-wait implementations, fix mutex/thread gaps

## Test Plan

Create `test/os_test.c` (or split per-primitive) using the
existing cmocka test infrastructure.

### Event tests
- signal then wait → immediate return
- wait with no signal → timeout returns JSDRV_ERROR_TIMED_OUT
- reset then wait → timeout
- signal from thread A, wait in thread B
- multiple signal/reset cycles

### Mutex tests
- lock/unlock from same thread
- lock from thread A, attempt lock from thread B → timeout
- recursive lock attempt → expected behavior documented
- lock/unlock interleaved between two threads

### Thread tests
- create and join → returns 0
- join with timeout → thread finishes in time
- join with timeout → thread does NOT finish, returns timeout
- priority levels (smoke test, verify no crash)
- is_current → true in created thread, false elsewhere

### Semaphore tests
- init with count=1, wait → immediate
- init with count=0, wait → timeout
- release then wait → immediate
- producer/consumer: N threads produce, 1 consumes
- pipeline pattern: match the fpga_mem usage
  (PIPELINE_MAX concurrent, release on callback)

### Atomic tests
- single-threaded inc/dec/set/get
- N threads increment same counter → final value == N * iters
- N threads decrement same counter → final value correct
- set from one thread, get from another

**All tests must pass on Windows, Linux, and macOS.**
Add a CI step or document manual verification for all three.

## CLI Migration

After the OS library is tested, migrate the 10 example files:

| File | Changes |
|------|---------|
| adapter.c | `Sleep()` → `jsdrv_thread_sleep_ms()` |
| firmware.c | `CreateEvent/SetEvent/WaitForSingleObject/CloseHandle` → `jsdrv_os_event_*` |
| fpga_mem.c | `CreateSemaphore/WaitForSingleObject/ReleaseSemaphore` → `jsdrv_os_sem_*`; `InterlockedIncrement/Decrement` → `JSDRV_OS_ATOMIC_INC/DEC`; `CreateEvent` → `jsdrv_os_event_*` |
| loopback.c | `CreateEvent/ResetEvent/WaitForSingleObject/Sleep` → `jsdrv_os_event_*` + `jsdrv_thread_sleep_ms` |
| mem.c | `CreateSemaphore/InterlockedIncrement/Decrement` → `jsdrv_os_sem_*` + `JSDRV_OS_ATOMIC_*` |
| pubsub_info.c | `CreateEvent/SetEvent/WaitForSingleObject/CloseHandle/InterlockedIncrement/Sleep` → os library |
| pubsub_test.c | `Sleep()` → `jsdrv_thread_sleep_ms()` |
| state_get.c | `CreateSemaphore/InterlockedIncrement/Decrement/Exchange` → `jsdrv_os_sem_*` + `JSDRV_OS_ATOMIC_*` |
| stream.c | `Sleep()` → `jsdrv_thread_sleep_ms()` |
| throughput.c | `Sleep()` → `jsdrv_thread_sleep_ms()` |

Also update `minibitty_exe_prv.h` to remove any remaining
Windows-specific includes.

## Implementation Order

Each step is a separate commit (or small PR).  Do not
proceed to the next step until tests pass.

### Step 1: Public headers (3 files changed)
Create the four new public headers:
- `include/jsdrv/os_event.h`
- `include/jsdrv/os_mutex.h`
- `include/jsdrv/os_thread.h`
- `include/jsdrv/os_sem.h`
- `include/jsdrv/os_atomic.h`

Keep the private headers as thin redirects for now to
avoid a massive include-path change in one commit.

### Step 2: Fix POSIX parity (1-2 files changed)
In `src/backend/posix.c`:
- Mutex: replace `pthread_mutex_lock` with
  `pthread_mutex_timedlock`
- Thread join: implement timeout via helper thread or
  timed polling
- Thread priority: `pthread_setschedparam` best-effort

### Step 3: Add semaphore implementation (2 files changed)
- `src/backend/windows.c`: add `jsdrv_os_sem_*` using
  Win32 semaphore API
- `src/backend/posix.c`: add `jsdrv_os_sem_*` using
  `sem_timedwait` (Linux) / `dispatch_semaphore` (macOS)

### Step 4: Add event wait (2 files changed)
- `src/backend/windows.c`: `WaitForSingleObject`
- `src/backend/posix.c`: `poll()` on pipe fd

### Step 5: Tests (1-2 new files)
- `test/os_test.c` covering all primitives
- Verify on Windows, Linux, macOS

### Step 6: Migrate CLI examples (10 files changed)
- Replace Win32 calls per the migration table above
- Build and run on all three platforms
- Remove `#include <windows.h>` from example code

### Step 7: Update CMakeLists.txt and CI
- Ensure public headers are installed
- Add os_test to CTest
- Verify CI runs tests on all platforms
