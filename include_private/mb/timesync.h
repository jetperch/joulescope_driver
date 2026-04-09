/*
 * SPDX-FileCopyrightText: Copyright 2023-2026 Jetperch LLC
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

#ifndef MB_TIMESYNC_H__
#define MB_TIMESYNC_H__

#include "mb/cdef.h"
#include "mb/stdmsg.h"
#include <stdint.h>


MB_CPP_GUARD_START

/**
 * @ingroup mb
 * @defgroup mb_ts Timesync
 *
 * @brief Messages to synchronize the local monotonic counter with UTC wall-clock time.
 *
 * These timesync MiniBitty standard messages allow a MiniBitty instance
 * to map a local monotonic counter mb_time_counter_u64()
 * to UTC wall-clock time.  The module implements a synchronization algorithm
 * similar to [NTP](https://en.wikipedia.org/wiki/Network_Time_Protocol).
 *
 * @{
 */


/**
 * @brief The body of a time synchronization stdmsg.
 *
 * Sent as the payload of a MB_VALUE_STDMSG with type MB_STDMSG_TIMESYNC_SYNC
 * (see mb/stdmsg.h).  The body does NOT include the mb_stdmsg_header_s;
 * dispatch on the header type and then access the body via (hdr + 1).
 *
 * A client initiates a time synchronization by publishing
 * this message to topic '!req' with only count_start populated.
 * The host receives this, populates utc_recv on receive and
 * utc_send on send (can be the same), and publishes to topic '!rsp'.
 * The client then populates count_end and processes the time
 * synchronization event.
 */
struct mb_timesync_sync_v1_s {
    uint64_t count_start;       ///< The client's counter when sent
    int64_t utc_recv;           ///< The MB_TIME 34Q30 UTC timestamp when received by host
    int64_t utc_send;           ///< The MB_TIME 34Q30 UTC timestamp when transmitted by host
    uint64_t count_end;         ///< The client's counter when received
};

/**
 * @brief The body of a time synchronization map announcement stdmsg.
 *
 * Sent as the payload of a MB_VALUE_STDMSG with type MB_STDMSG_TIMESYNC_MAP
 * (see mb/stdmsg.h).  The body does NOT include the mb_stdmsg_header_s;
 * dispatch on the header type and then access the body via (hdr + 1).
 *
 * After computing the time map from mb_timesync_sync_v1_s messages,
 * the client announces its time map.  The most-recent sync exchange is
 * embedded so subscribers can derive RTT and host-vs-device skew without
 * also subscribing to the !req / !rsp topics:
 *
 *   RTT_seconds = (sync_count_end - sync_count_start) * (2^32 / counter_rate)
 *   host_skew   = (sync_utc_recv + sync_utc_send) / 2 - utc_at(count_mid)
 *
 *   where count_mid = (sync_count_start + sync_count_end) / 2 and
 *         utc_at(c) = utc + (c - counter) * (2^32 / counter_rate)
 */
struct mb_timesync_map_v1_s {
    int64_t  utc;               ///< The MB_TIME 34Q30 UTC timestamp at counter
    uint64_t counter;           ///< The counter tick reference
    uint64_t counter_rate;      ///< Best estimate in ticks per second (Hz) as 32Q32.
    uint64_t counter_ppb;       ///< The rated counter accuracy in parts per billion.
    // Most-recent sync exchange used to update this map:
    uint64_t sync_count_start;  ///< Device counter when !req was sent
    uint64_t sync_count_end;    ///< Device counter when !rsp was received
    int64_t  sync_utc_recv;     ///< Host MB_TIME 34Q30 UTC when !req received
    int64_t  sync_utc_send;     ///< Host MB_TIME 34Q30 UTC when !rsp sent
    uint32_t update_counter;    ///< Monotonic count of map updates (since boot)
    uint32_t rsv;               ///< Reserved for alignment, set to 0
};


MB_CPP_GUARD_END

/** @} */

#endif // MB_TIMESYNC_H__
