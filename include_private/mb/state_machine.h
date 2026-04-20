/*
 * SPDX-FileCopyrightText: Copyright 2014-2024 Jetperch LLC
 * SPDX-License-Identifier: Apache-2.0
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

/**
 * @file
 *
 * @brief The OS state machine base implementation.
 */

#ifndef MB_SM_H_
#define MB_SM_H_

#include "mb/cdef.h"
#include "mb/event.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @ingroup mb_core
 * @defgroup mb_sm State machine
 *
 * @brief The OS state machine base implementation.
 *
 * @{
 */

MB_CPP_GUARD_START

struct mb_sm_s;  /* forward declaration */

/**
 * @brief A state machine function.
 *
 * @param self The state machine instance.
 * @param event The event to handle.
 *      For on_enter, MB_EV_START.
 *      For on_exit, MB_EV_STOP.
 * @return For guards, return true to accept the transition, False otherwise.
 *      For on_enter and on_exit, the state machine ignores the return value.
 *
 * If the guard returns false, then the guard MUST not perform any
 * actions or mutate self.  If the guard returns true, it may
 * perform other actions or mutate self, but do so with care.
 * Both the current state's on_exit and the next state's on_enter
 * are called AFTER the guard.
 */
typedef bool (*mb_sm_fn)(struct mb_sm_s * self, uint8_t event);

/**
 * @brief Define a single transition (edge) for the state machine.
 *
 * The state machine engine searches transitions in order for the
 * first matching transition.  When the transition event is
 * MB_EV_NULL, then the engine stops searching with no match found.
 *
 * If transition's event matches and the guard is zero, then the
 * engine transitions the state machine to state_next.  When the guard
 * is not zero, then the engine invokes the corresponding guard function.
 * The engine only transitions to state_next if the guard function
 * returns true.
 *
 * If the event does not match or the guard returns true, then the
 * engine will continue searching for a match with the next transition.
 */
struct mb_sm_transition_s {
    /**
     * @brief The event to match for this transition.
     *
     * The last transition in the list must be MB_EV_NULL.
     */
    uint8_t event;

    /**
     * @brief The next state.
     *
     * If guard == 0, then always transition to state_next.
     * Otherwise, call the guard corresponding to this index and
     * only transition if it returns true.
     */
    uint8_t state_next;

    /**
     * @brief The guard function index into mb_sm_s->guards.
     *
     * 0 is reserved for NULL guard.
     */
    uint16_t guard;
};

/**
 * @brief Define a single state in the state machine.
 */
struct mb_sm_state_s {
    /**
     * @brief The function index called when entering the state.
     *
     * If 0, then no function will be called on enter.
     */
    uint16_t on_enter;

    /**
     * @brief The function index called when exiting the state.
     *
     * If 0, then no function will be call on exit.
     * Use this on_exit feature with care since
     * it will be called AFTER the matching transition guard is evaluated.
     */
    uint16_t on_exit;

    /**
     * @brief The transitions (edges) for this state.
     *
     * The final transition must have event MB_EV_NULL (== 0).
     * NULL if child is not NULL.
     */
    struct mb_sm_transition_s const * const transitions;

    /**
     * @brief The child hierarchical state machine.
     *
     * NULL for a normal state.
     */
    struct mb_sm_s * child;
};

/**
 * @brief The state machine instance.
 *
 * This structure contains an abstract class definition for all state machine
 * instances.  Individual state machine instances may provide additional data
 * by allocated a structure with an mb_sm_s field as the first element.
 */
struct mb_sm_s {
    const uint16_t id;      ///< The id for this instance.
    uint8_t state;          ///< The current state index.
    uint8_t rsv;            // reserved, keep 0
    void * context;         ///< Optional, arbitrary state context.

    /**
     * @brief The states in the state machine.
     *
     * NOTE: The first state (index 0) is reserved for global transitions,
     * which should at least include MB_EV_RESET.
     */
    struct mb_sm_state_s const * states;

    /**
     * @brief The functions used by the state machine in a lookup array.
     */
    mb_sm_fn const * functions;
};

/**
 * @brief Provide an event to the state machine.
 *
 * @param self The state machine instance.
 * @param event The event to handle.
 */
MB_API void mb_sm_event(struct mb_sm_s * self, uint8_t event);

MB_CPP_GUARD_END

/** @} */

#endif /* MB_SM_H_ */
