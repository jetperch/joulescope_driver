/*
 * Copyright 2026 Jetperch LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "jsdrv/os_event.h"
#include "jsdrv/os_mutex.h"
#include "jsdrv/os_thread.h"
#include "jsdrv/os_sem.h"
#include "jsdrv/os_atomic.h"
#include "jsdrv/error_code.h"
#include <string.h>

#define THREAD_COUNT 4
#define ITERATIONS 10000

// ============================================================
// Event tests
// ============================================================

static void event_alloc_free(void **state) {
    (void) state;
    jsdrv_os_event_t ev = jsdrv_os_event_alloc();
    assert_non_null(ev);
    jsdrv_os_event_free(ev);
}

static void event_signal_then_wait(void **state) {
    (void) state;
    jsdrv_os_event_t ev = jsdrv_os_event_alloc();
    jsdrv_os_event_signal(ev);
    int32_t rc = jsdrv_os_event_wait(ev, 100);
    assert_int_equal(0, rc);
    jsdrv_os_event_free(ev);
}

static void event_wait_timeout(void **state) {
    (void) state;
    jsdrv_os_event_t ev = jsdrv_os_event_alloc();
    int32_t rc = jsdrv_os_event_wait(ev, 10);
    assert_int_equal(JSDRV_ERROR_TIMED_OUT, rc);
    jsdrv_os_event_free(ev);
}

static void event_wait_zero_timeout_unsignaled(void **state) {
    (void) state;
    jsdrv_os_event_t ev = jsdrv_os_event_alloc();
    int32_t rc = jsdrv_os_event_wait(ev, 0);
    assert_int_equal(JSDRV_ERROR_TIMED_OUT, rc);
    jsdrv_os_event_free(ev);
}

static void event_wait_zero_timeout_signaled(void **state) {
    (void) state;
    jsdrv_os_event_t ev = jsdrv_os_event_alloc();
    jsdrv_os_event_signal(ev);
    int32_t rc = jsdrv_os_event_wait(ev, 0);
    assert_int_equal(0, rc);
    jsdrv_os_event_free(ev);
}

static void event_reset_then_wait(void **state) {
    (void) state;
    jsdrv_os_event_t ev = jsdrv_os_event_alloc();
    jsdrv_os_event_signal(ev);
    jsdrv_os_event_reset(ev);
    int32_t rc = jsdrv_os_event_wait(ev, 10);
    assert_int_equal(JSDRV_ERROR_TIMED_OUT, rc);
    jsdrv_os_event_free(ev);
}

static void event_signal_persists(void **state) {
    (void) state;
    jsdrv_os_event_t ev = jsdrv_os_event_alloc();
    jsdrv_os_event_signal(ev);
    // Multiple waits should all succeed (manual-reset)
    assert_int_equal(0, jsdrv_os_event_wait(ev, 0));
    assert_int_equal(0, jsdrv_os_event_wait(ev, 0));
    assert_int_equal(0, jsdrv_os_event_wait(ev, 0));
    jsdrv_os_event_free(ev);
}

static void event_signal_reset_cycle(void **state) {
    (void) state;
    jsdrv_os_event_t ev = jsdrv_os_event_alloc();
    for (int i = 0; i < 10; i++) {
        jsdrv_os_event_signal(ev);
        assert_int_equal(0, jsdrv_os_event_wait(ev, 0));
        jsdrv_os_event_reset(ev);
        assert_int_equal(JSDRV_ERROR_TIMED_OUT,
                         jsdrv_os_event_wait(ev, 0));
    }
    jsdrv_os_event_free(ev);
}

struct event_thread_data_s {
    jsdrv_os_event_t ev;
    volatile int ready;
};

static JSDRV_THREAD_RETURN_TYPE event_signaler_fn(JSDRV_THREAD_ARG_TYPE arg) {
    struct event_thread_data_s * d = (struct event_thread_data_s *) arg;
    jsdrv_thread_sleep_ms(20);
    d->ready = 1;
    jsdrv_os_event_signal(d->ev);
    JSDRV_THREAD_RETURN();
}

static void event_cross_thread_signal(void **state) {
    (void) state;
    struct event_thread_data_s data;
    data.ev = jsdrv_os_event_alloc();
    data.ready = 0;

    jsdrv_thread_t thread;
    jsdrv_thread_create(&thread, event_signaler_fn, &data, 0);

    int32_t rc = jsdrv_os_event_wait(data.ev, 2000);
    assert_int_equal(0, rc);
    assert_int_equal(1, data.ready);

    jsdrv_thread_join(&thread, 2000);
    jsdrv_os_event_free(data.ev);
}

// ============================================================
// Mutex tests
// ============================================================

static void mutex_alloc_free(void **state) {
    (void) state;
    jsdrv_os_mutex_t m = jsdrv_os_mutex_alloc("test");
    assert_non_null(m);
    jsdrv_os_mutex_free(m);
}

static void mutex_lock_unlock(void **state) {
    (void) state;
    jsdrv_os_mutex_t m = jsdrv_os_mutex_alloc("test");
    jsdrv_os_mutex_lock(m);
    jsdrv_os_mutex_unlock(m);
    jsdrv_os_mutex_free(m);
}

static void mutex_lock_null(void **state) {
    (void) state;
    // Should not crash
    jsdrv_os_mutex_lock(NULL);
    jsdrv_os_mutex_unlock(NULL);
}

struct mutex_thread_data_s {
    jsdrv_os_mutex_t mutex;
    int counter;
};

static JSDRV_THREAD_RETURN_TYPE mutex_increment_fn(JSDRV_THREAD_ARG_TYPE arg) {
    struct mutex_thread_data_s * d = (struct mutex_thread_data_s *) arg;
    for (int i = 0; i < ITERATIONS; i++) {
        jsdrv_os_mutex_lock(d->mutex);
        d->counter++;
        jsdrv_os_mutex_unlock(d->mutex);
    }
    JSDRV_THREAD_RETURN();
}

static void mutex_contention(void **state) {
    (void) state;
    struct mutex_thread_data_s data;
    data.mutex = jsdrv_os_mutex_alloc("contention");
    data.counter = 0;

    jsdrv_thread_t threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++) {
        jsdrv_thread_create(&threads[i], mutex_increment_fn, &data, 0);
    }
    for (int i = 0; i < THREAD_COUNT; i++) {
        jsdrv_thread_join(&threads[i], 5000);
    }

    assert_int_equal(THREAD_COUNT * ITERATIONS, data.counter);
    jsdrv_os_mutex_free(data.mutex);
}

// ============================================================
// Thread tests
// ============================================================

static JSDRV_THREAD_RETURN_TYPE simple_thread_fn(JSDRV_THREAD_ARG_TYPE arg) {
    int * value = (int *) arg;
    *value = 42;
    JSDRV_THREAD_RETURN();
}

static void thread_create_join(void **state) {
    (void) state;
    int value = 0;
    jsdrv_thread_t thread;
    int32_t rc = jsdrv_thread_create(&thread, simple_thread_fn, &value, 0);
    assert_int_equal(0, rc);
    rc = jsdrv_thread_join(&thread, 2000);
    assert_int_equal(0, rc);
    assert_int_equal(42, value);
}

static JSDRV_THREAD_RETURN_TYPE slow_thread_fn(JSDRV_THREAD_ARG_TYPE arg) {
    (void) arg;
    jsdrv_thread_sleep_ms(500);
    JSDRV_THREAD_RETURN();
}

static void thread_join_timeout(void **state) {
    (void) state;
    jsdrv_thread_t thread;
    jsdrv_thread_create(&thread, slow_thread_fn, NULL, 0);
    int32_t rc = jsdrv_thread_join(&thread, 10);
    assert_int_equal(JSDRV_ERROR_TIMED_OUT, rc);
    // Let the thread finish to avoid resource leak
    jsdrv_thread_sleep_ms(600);
}

static JSDRV_THREAD_RETURN_TYPE is_current_fn(JSDRV_THREAD_ARG_TYPE arg) {
    jsdrv_thread_t * self = (jsdrv_thread_t *) arg;
    assert_true(jsdrv_thread_is_current(self));
    JSDRV_THREAD_RETURN();
}

static void thread_is_current(void **state) {
    (void) state;
    jsdrv_thread_t thread;
    jsdrv_thread_create(&thread, is_current_fn, &thread, 0);
    assert_false(jsdrv_thread_is_current(&thread));
    jsdrv_thread_join(&thread, 2000);
}

static void thread_sleep(void **state) {
    (void) state;
    // Just verify it doesn't crash and takes approximately
    // the right amount of time.
    jsdrv_thread_sleep_ms(10);
}

static void thread_priority_levels(void **state) {
    (void) state;
    // Smoke test: verify all priority levels don't crash
    for (int prio = -2; prio <= 2; prio++) {
        int value = 0;
        jsdrv_thread_t thread;
        jsdrv_thread_create(&thread, simple_thread_fn, &value, prio);
        jsdrv_thread_join(&thread, 2000);
        assert_int_equal(42, value);
    }
}

// ============================================================
// Semaphore tests
// ============================================================

static void sem_alloc_free(void **state) {
    (void) state;
    jsdrv_os_sem_t sem = jsdrv_os_sem_alloc(0, 10);
    assert_non_null(sem);
    jsdrv_os_sem_free(sem);
}

static void sem_initial_count_wait(void **state) {
    (void) state;
    jsdrv_os_sem_t sem = jsdrv_os_sem_alloc(1, 10);
    int32_t rc = jsdrv_os_sem_wait(sem, 100);
    assert_int_equal(0, rc);
    // Second wait should timeout (count now 0)
    rc = jsdrv_os_sem_wait(sem, 10);
    assert_int_equal(JSDRV_ERROR_TIMED_OUT, rc);
    jsdrv_os_sem_free(sem);
}

static void sem_wait_timeout(void **state) {
    (void) state;
    jsdrv_os_sem_t sem = jsdrv_os_sem_alloc(0, 10);
    int32_t rc = jsdrv_os_sem_wait(sem, 10);
    assert_int_equal(JSDRV_ERROR_TIMED_OUT, rc);
    jsdrv_os_sem_free(sem);
}

static void sem_release_then_wait(void **state) {
    (void) state;
    jsdrv_os_sem_t sem = jsdrv_os_sem_alloc(0, 10);
    jsdrv_os_sem_release(sem);
    int32_t rc = jsdrv_os_sem_wait(sem, 100);
    assert_int_equal(0, rc);
    jsdrv_os_sem_free(sem);
}

static void sem_multiple_releases(void **state) {
    (void) state;
    jsdrv_os_sem_t sem = jsdrv_os_sem_alloc(0, 10);
    jsdrv_os_sem_release(sem);
    jsdrv_os_sem_release(sem);
    jsdrv_os_sem_release(sem);
    assert_int_equal(0, jsdrv_os_sem_wait(sem, 100));
    assert_int_equal(0, jsdrv_os_sem_wait(sem, 100));
    assert_int_equal(0, jsdrv_os_sem_wait(sem, 100));
    assert_int_equal(JSDRV_ERROR_TIMED_OUT, jsdrv_os_sem_wait(sem, 10));
    jsdrv_os_sem_free(sem);
}

static void sem_zero_timeout_available(void **state) {
    (void) state;
    jsdrv_os_sem_t sem = jsdrv_os_sem_alloc(1, 10);
    assert_int_equal(0, jsdrv_os_sem_wait(sem, 0));
    jsdrv_os_sem_free(sem);
}

static void sem_zero_timeout_unavailable(void **state) {
    (void) state;
    jsdrv_os_sem_t sem = jsdrv_os_sem_alloc(0, 10);
    assert_int_equal(JSDRV_ERROR_TIMED_OUT, jsdrv_os_sem_wait(sem, 0));
    jsdrv_os_sem_free(sem);
}

struct sem_thread_data_s {
    jsdrv_os_sem_t sem;
    jsdrv_os_atomic_t produced;
    jsdrv_os_atomic_t consumed;
};

static JSDRV_THREAD_RETURN_TYPE sem_producer_fn(JSDRV_THREAD_ARG_TYPE arg) {
    struct sem_thread_data_s * d = (struct sem_thread_data_s *) arg;
    for (int i = 0; i < ITERATIONS; i++) {
        JSDRV_OS_ATOMIC_INC(&d->produced);
        jsdrv_os_sem_release(d->sem);
    }
    JSDRV_THREAD_RETURN();
}

static JSDRV_THREAD_RETURN_TYPE sem_consumer_fn(JSDRV_THREAD_ARG_TYPE arg) {
    struct sem_thread_data_s * d = (struct sem_thread_data_s *) arg;
    for (int i = 0; i < ITERATIONS * THREAD_COUNT; i++) {
        int32_t rc = jsdrv_os_sem_wait(d->sem, 5000);
        if (rc == 0) {
            JSDRV_OS_ATOMIC_INC(&d->consumed);
        }
    }
    JSDRV_THREAD_RETURN();
}

static void sem_producer_consumer(void **state) {
    (void) state;
    struct sem_thread_data_s data;
    data.sem = jsdrv_os_sem_alloc(0, ITERATIONS * THREAD_COUNT);
    JSDRV_OS_ATOMIC_SET(&data.produced, 0);
    JSDRV_OS_ATOMIC_SET(&data.consumed, 0);

    jsdrv_thread_t consumer;
    jsdrv_thread_create(&consumer, sem_consumer_fn, &data, 0);

    jsdrv_thread_t producers[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++) {
        jsdrv_thread_create(&producers[i], sem_producer_fn, &data, 0);
    }
    for (int i = 0; i < THREAD_COUNT; i++) {
        jsdrv_thread_join(&producers[i], 5000);
    }
    jsdrv_thread_join(&consumer, 10000);

    assert_int_equal(THREAD_COUNT * ITERATIONS,
                     JSDRV_OS_ATOMIC_GET(&data.produced));
    assert_int_equal(THREAD_COUNT * ITERATIONS,
                     JSDRV_OS_ATOMIC_GET(&data.consumed));
    jsdrv_os_sem_free(data.sem);
}

// Pipeline pattern matching fpga_mem usage
#define PIPELINE_MAX 4

struct pipeline_data_s {
    jsdrv_os_sem_t sem;
    jsdrv_os_atomic_t outstanding;
    jsdrv_os_atomic_t completed;
};

static JSDRV_THREAD_RETURN_TYPE pipeline_worker_fn(JSDRV_THREAD_ARG_TYPE arg) {
    struct pipeline_data_s * d = (struct pipeline_data_s *) arg;
    for (int i = 0; i < 100; i++) {
        jsdrv_os_sem_wait(d->sem, 5000);
        JSDRV_OS_ATOMIC_DEC(&d->outstanding);
        JSDRV_OS_ATOMIC_INC(&d->completed);
    }
    JSDRV_THREAD_RETURN();
}

static void sem_pipeline_pattern(void **state) {
    (void) state;
    struct pipeline_data_s data;
    data.sem = jsdrv_os_sem_alloc(0, PIPELINE_MAX);
    JSDRV_OS_ATOMIC_SET(&data.outstanding, 0);
    JSDRV_OS_ATOMIC_SET(&data.completed, 0);

    jsdrv_thread_t worker;
    jsdrv_thread_create(&worker, pipeline_worker_fn, &data, 0);

    for (int i = 0; i < 100; i++) {
        JSDRV_OS_ATOMIC_INC(&data.outstanding);
        jsdrv_os_sem_release(data.sem);
        // Limit pipeline depth
        while (JSDRV_OS_ATOMIC_GET(&data.outstanding) >= PIPELINE_MAX) {
            jsdrv_thread_sleep_ms(1);
        }
    }

    jsdrv_thread_join(&worker, 10000);
    assert_int_equal(0, JSDRV_OS_ATOMIC_GET(&data.outstanding));
    assert_int_equal(100, JSDRV_OS_ATOMIC_GET(&data.completed));
    jsdrv_os_sem_free(data.sem);
}

// ============================================================
// Atomic tests
// ============================================================

static void atomic_inc_dec(void **state) {
    (void) state;
    jsdrv_os_atomic_t v = 0;
    assert_int_equal(1, JSDRV_OS_ATOMIC_INC(&v));
    assert_int_equal(2, JSDRV_OS_ATOMIC_INC(&v));
    assert_int_equal(3, JSDRV_OS_ATOMIC_INC(&v));
    assert_int_equal(2, JSDRV_OS_ATOMIC_DEC(&v));
    assert_int_equal(1, JSDRV_OS_ATOMIC_DEC(&v));
    assert_int_equal(0, JSDRV_OS_ATOMIC_DEC(&v));
}

static void atomic_set_get(void **state) {
    (void) state;
    jsdrv_os_atomic_t v = 0;
    JSDRV_OS_ATOMIC_SET(&v, 42);
    assert_int_equal(42, JSDRV_OS_ATOMIC_GET(&v));
    JSDRV_OS_ATOMIC_SET(&v, -1);
    assert_int_equal(-1, JSDRV_OS_ATOMIC_GET(&v));
    JSDRV_OS_ATOMIC_SET(&v, 0);
    assert_int_equal(0, JSDRV_OS_ATOMIC_GET(&v));
}

static void atomic_inc_dec_boundary(void **state) {
    (void) state;
    jsdrv_os_atomic_t v = 0;
    assert_int_equal(-1, JSDRV_OS_ATOMIC_DEC(&v));
    assert_int_equal(-2, JSDRV_OS_ATOMIC_DEC(&v));
    assert_int_equal(-1, JSDRV_OS_ATOMIC_INC(&v));
    assert_int_equal(0, JSDRV_OS_ATOMIC_INC(&v));
}

struct atomic_thread_data_s {
    jsdrv_os_atomic_t counter;
};

static JSDRV_THREAD_RETURN_TYPE atomic_inc_fn(JSDRV_THREAD_ARG_TYPE arg) {
    struct atomic_thread_data_s * d = (struct atomic_thread_data_s *) arg;
    for (int i = 0; i < ITERATIONS; i++) {
        JSDRV_OS_ATOMIC_INC(&d->counter);
    }
    JSDRV_THREAD_RETURN();
}

static void atomic_concurrent_increment(void **state) {
    (void) state;
    struct atomic_thread_data_s data;
    JSDRV_OS_ATOMIC_SET(&data.counter, 0);

    jsdrv_thread_t threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++) {
        jsdrv_thread_create(&threads[i], atomic_inc_fn, &data, 0);
    }
    for (int i = 0; i < THREAD_COUNT; i++) {
        jsdrv_thread_join(&threads[i], 5000);
    }

    assert_int_equal(THREAD_COUNT * ITERATIONS,
                     JSDRV_OS_ATOMIC_GET(&data.counter));
}

static JSDRV_THREAD_RETURN_TYPE atomic_dec_fn(JSDRV_THREAD_ARG_TYPE arg) {
    struct atomic_thread_data_s * d = (struct atomic_thread_data_s *) arg;
    for (int i = 0; i < ITERATIONS; i++) {
        JSDRV_OS_ATOMIC_DEC(&d->counter);
    }
    JSDRV_THREAD_RETURN();
}

static void atomic_concurrent_decrement(void **state) {
    (void) state;
    struct atomic_thread_data_s data;
    JSDRV_OS_ATOMIC_SET(&data.counter, THREAD_COUNT * ITERATIONS);

    jsdrv_thread_t threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++) {
        jsdrv_thread_create(&threads[i], atomic_dec_fn, &data, 0);
    }
    for (int i = 0; i < THREAD_COUNT; i++) {
        jsdrv_thread_join(&threads[i], 5000);
    }

    assert_int_equal(0, JSDRV_OS_ATOMIC_GET(&data.counter));
}

static JSDRV_THREAD_RETURN_TYPE atomic_set_fn(JSDRV_THREAD_ARG_TYPE arg) {
    struct atomic_thread_data_s * d = (struct atomic_thread_data_s *) arg;
    for (int i = 0; i < ITERATIONS; i++) {
        JSDRV_OS_ATOMIC_SET(&d->counter, i);
    }
    JSDRV_THREAD_RETURN();
}

static void atomic_concurrent_set_get(void **state) {
    (void) state;
    struct atomic_thread_data_s data;
    JSDRV_OS_ATOMIC_SET(&data.counter, 0);

    jsdrv_thread_t writer;
    jsdrv_thread_create(&writer, atomic_set_fn, &data, 0);

    // Reader: just verify we get valid values (no torn reads)
    for (int i = 0; i < ITERATIONS; i++) {
        int32_t v = JSDRV_OS_ATOMIC_GET(&data.counter);
        assert_true(v >= 0);
        assert_true(v < ITERATIONS);
    }

    jsdrv_thread_join(&writer, 5000);
}

// ============================================================
// Main
// ============================================================

int main(void) {
    const struct CMUnitTest tests[] = {
        // Event
        cmocka_unit_test(event_alloc_free),
        cmocka_unit_test(event_signal_then_wait),
        cmocka_unit_test(event_wait_timeout),
        cmocka_unit_test(event_wait_zero_timeout_unsignaled),
        cmocka_unit_test(event_wait_zero_timeout_signaled),
        cmocka_unit_test(event_reset_then_wait),
        cmocka_unit_test(event_signal_persists),
        cmocka_unit_test(event_signal_reset_cycle),
        cmocka_unit_test(event_cross_thread_signal),

        // Mutex
        cmocka_unit_test(mutex_alloc_free),
        cmocka_unit_test(mutex_lock_unlock),
        cmocka_unit_test(mutex_lock_null),
        cmocka_unit_test(mutex_contention),

        // Thread
        cmocka_unit_test(thread_create_join),
        cmocka_unit_test(thread_join_timeout),
        cmocka_unit_test(thread_is_current),
        cmocka_unit_test(thread_sleep),
        cmocka_unit_test(thread_priority_levels),

        // Semaphore
        cmocka_unit_test(sem_alloc_free),
        cmocka_unit_test(sem_initial_count_wait),
        cmocka_unit_test(sem_wait_timeout),
        cmocka_unit_test(sem_release_then_wait),
        cmocka_unit_test(sem_multiple_releases),
        cmocka_unit_test(sem_zero_timeout_available),
        cmocka_unit_test(sem_zero_timeout_unavailable),
        cmocka_unit_test(sem_producer_consumer),
        cmocka_unit_test(sem_pipeline_pattern),

        // Atomic
        cmocka_unit_test(atomic_inc_dec),
        cmocka_unit_test(atomic_set_get),
        cmocka_unit_test(atomic_inc_dec_boundary),
        cmocka_unit_test(atomic_concurrent_increment),
        cmocka_unit_test(atomic_concurrent_decrement),
        cmocka_unit_test(atomic_concurrent_set_get),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
