/*
* Copyright 2025 Jetperch LLC
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

#define THREAD_ID (1)


#include "adapter_tracy.h"
#include "adapter.h"
//#include "tracy/tracy/TracyC.h"
//#include "tracy/client/TracyProfiler.hpp"
//#include "tracy/common/TracyColor.hpp"
#include "tracy/common/tracy_lz4.hpp"
#include "tracy/common/TracySocket.hpp"
#include "tracy/common/TracyProtocol.hpp"
#include "tracy/common/TracyQueue.hpp"
#include "tracy/client/TracyThread.hpp"
#include <chrono>
#include <thread>

#include "jsdrv.h"
#include "jsdrv/error_code.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/log.h"
#include "jsdrv_prv/msg_queue.h"
#include <assert.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <map>
#include <jsdrv_prv/frontend.h>

struct time_parts_s {
    uint32_t l;
    uint32_t u;
};

union time_u {
    struct time_parts_s parts;
    int64_t time;
};

#if 0
struct adapter_tracy_s {
    TracyCZoneCtx task_zones[256];
    TracyCZoneCtx isr_zones[256];
    std::map<uint32_t, uint64_t> srcloc;
    union time_u time;
    uint64_t srcloc_task;
    uint64_t srcloc_isr;
};

#endif

namespace tracy
{
class Profiler {

public:
    Profiler(jsdrv_context_s * context);
    ~Profiler();
    void StartWorker();
    void on_trace(const struct jsdrv_union_s * value);

private:
    static void LaunchWorker( void* ptr ) { ((Profiler*)ptr)->Worker(); }
    void Worker();
    void ProcessTraceMessage(jsdrvp_msg_s * msg);

    bool HandleServerQuery();
    void HandleParameter( uint64_t payload );
    void HandleSymbolCodeQuery( uint64_t symbol, uint32_t size );
    void HandleSourceCodeQuery( char* data, char* image, uint32_t id );
    void AckServerQuery();
    void AckSymbolCodeNotAvailable();

    void buf_header(QueueType type) {
        *m_buf_ptr++ = (uint8_t) type;
    }

    void buf_reftime(int64_t time) {
        int64_t dt = time - m_reftime;
        m_reftime = time;
        memcpy(m_buf_ptr, &dt, sizeof(dt));
        m_buf_ptr += sizeof(dt);
    }

    void buf_u64(uint64_t val) {
        memcpy(m_buf_ptr, &val, sizeof(val));
        m_buf_ptr += sizeof(val);
    }

    void buf_u32(uint32_t val) {
        memcpy(m_buf_ptr, &val, sizeof(val));
        m_buf_ptr += sizeof(val);
    }

    void buf_u16(uint16_t val) {
        memcpy(m_buf_ptr, &val, sizeof(val));
        m_buf_ptr += sizeof(val);
    }

    void buf_u8(uint8_t val) {
        memcpy(m_buf_ptr, &val, sizeof(val));
        m_buf_ptr += sizeof(val);
    }

    void buf_ptr(const void * ptr, uint32_t size) {
        memcpy(m_buf_ptr, ptr, size);
        m_buf_ptr += size;
    }

    void buf_color(uint32_t color) {
        memcpy(m_buf_ptr, &color, 3);
        m_buf_ptr += 3;
    }

    void buf_string_transfer(QueueType type, uint64_t ptr, const char * str) {
        size_t sz = strlen(str);
        assert( sz <= 0xffff );
        buf_header(type);
        buf_u64(ptr);
        auto sz_u16 = (uint16_t) sz;
        buf_u16(sz_u16);
        buf_ptr(str, sz_u16);
    }

    void buf_source_location(uint64_t srcloc) {
        buf_header(QueueType::SourceLocation);
        buf_u64(4);  // name
        buf_u64(8);  // function
        buf_u64(12); // file
        buf_u32(srcloc & 0xffff); // line
        buf_color(0x000000);
    }

    bool SendData() {
        int sz = (int) (m_buf_ptr - m_buf_start);  // buffer not empty
        if (m_sock && sz) {
            lz4sz_t lz4sz = LZ4_compress_fast_continue( m_stream, (char *) m_buf_start, (char *) (m_lz4 + sizeof( lz4sz_t )), sz, LZ4Size, 1 );
            memcpy( m_lz4, &lz4sz, sizeof( lz4sz ) );  // frame starts with length
            lz4sz += sizeof(lz4sz);
            if ((m_buf_ptr - m_buf) >= (TargetFrameSize * 2 )) {
                m_buf_ptr = m_buf;  // reset to start of buffer
            }
            m_buf_start = m_buf_ptr;
            return m_sock->Send( m_lz4, lz4sz) != -1;
        }
        return true;
    }

    uint64_t m_epoch;
    Socket* m_sock;
    UdpBroadcast* m_broadcast;
    Thread * m_thread;
    LZ4_stream_t * m_stream;
    msg_queue_s * m_queue;
    jsdrv_context_s * m_context;
    uint8_t m_buf[TargetFrameSize * 3];     // total buffer size
    uint8_t * m_buf_start;  // current buffer block start, need to preserve 64 kB for LZ4_compress_fast_continue
    uint8_t * m_buf_ptr;    // current buffer insertion location
    uint8_t m_lz4[LZ4Size + sizeof(lz4sz_t)];
    time_u m_time;
    int64_t m_reftime;
    bool m_quit;
    uint32_t m_zone_id;
    uint32_t m_task_zone_id[256];
    uint32_t m_isr_zone_id[256];
};

Profiler::Profiler(jsdrv_context_s * context)
    : m_epoch( std::chrono::duration_cast<std::chrono::seconds>( std::chrono::system_clock::now().time_since_epoch() ).count() )
    , m_sock( nullptr )
    , m_broadcast( nullptr )
    , m_thread(nullptr)
    , m_stream(LZ4_createStream())
    , m_queue(msg_queue_init())
    , m_context(context)
    , m_time()
    , m_reftime(0)
    , m_quit(false)
    , m_zone_id(1)
{
    memset(m_buf, 0x55, sizeof(m_buf));
    memset(m_lz4, 0x00, sizeof(m_lz4));
    m_buf_start = m_buf;
    m_buf_ptr = m_buf;
    m_time.time = 0;
}

Profiler::~Profiler() {
    m_quit = true;
    if (m_thread) {
        delete m_thread;
        m_thread = nullptr;
    }

    if ( m_sock ) {
        m_sock->~Socket();
        free( m_sock );
    }
    if ( m_broadcast ) {
        m_broadcast->~UdpBroadcast();
        free( m_broadcast );
    }
    if (m_queue) {
        msg_queue_finalize(m_queue);
        m_queue = nullptr;
    }
    if (m_stream) {
        LZ4_freeStream(m_stream);
        m_stream = nullptr;
    }
}

void Profiler::StartWorker() {
    m_thread = new tracy::Thread(tracy::Profiler::LaunchWorker, this);
}

__attribute__((used))
static BroadcastMessage& GetBroadcastMessage( const char* procname, size_t pnsz, int& len, int port ) {
    static BroadcastMessage msg;

    msg.broadcastVersion = BroadcastVersion;
    msg.protocolVersion = ProtocolVersion;
    msg.listenPort = port;
    msg.pid = 1111;

    memcpy( msg.programName, procname, pnsz );
    memset( msg.programName + pnsz, 0, WelcomeMessageProgramNameSize - pnsz );

    len = int( offsetof( BroadcastMessage, programName ) + pnsz + 1 );
    return msg;
}

void Profiler::on_trace(const struct jsdrv_union_s * value) {
    assert(value->type == JSDRV_UNION_BIN);
    assert(value->size <= JSDRV_PAYLOAD_LENGTH_MAX);
    jsdrvp_msg_s * msg = jsdrvp_msg_alloc(m_context);
    msg->value.type = JSDRV_UNION_BIN;
    msg->value.op = 0;
    msg->value.flags = 0;
    msg->value.app = 0;
    msg->value.value.bin = msg->payload.bin;
    msg->value.size = value->size;
    memcpy(msg->payload.bin, value->value.bin, value->size);
    msg_queue_push(m_queue, msg);
}

void Profiler::Worker() {
    ThreadExitHandler threadExitHandler;
    uint32_t dataPort = 8086;
    const auto broadcastPort = 8086;
    const char * programName = "minibitty";

    WelcomeMessage welcome;
    memset(&welcome, 0, sizeof(welcome));
    welcome.timerMul = 10.0;  // scale to 1 GHz
    welcome.initBegin = 1;
    welcome.initEnd = 2;
    welcome.delay = 16;
    welcome.resolution = 16;  // units?
    welcome.epoch = m_epoch;
    welcome.exectime = m_epoch;
    welcome.pid = 0;
    welcome.samplingPeriod = 0;
    welcome.flags = WelcomeFlag::CombineSamples;
    welcome.cpuArch = 0;
    welcome.cpuManufacturer[0] = 0;
    welcome.cpuId = 0;
    memcpy( welcome.programName, programName, strlen( programName ) );
    welcome.hostInfo[0] = 0;

    ListenSocket listen;
    bool isListening = false;
    for( uint32_t i = 0; i < 20; i++ ) {
        if( listen.Listen( dataPort + i, 4 ) ) {
            dataPort += i;
            isListening = true;
            break;
        }
    }
    if( !isListening ) {
        return;
    }
    m_broadcast = new UdpBroadcast();
    const char* addr = "127.255.255.255";  // localhost
    //const char* addr = "255.255.255.255";  // global
    if( !m_broadcast->Open( addr, broadcastPort ) ) {
        delete m_broadcast;
        m_broadcast = nullptr;
        return;
    }

    int broadcastLen = 0;
    auto& broadcastMsg = GetBroadcastMessage( programName, strlen( programName ), broadcastLen, dataPort );
    uint64_t lastBroadcast = 0;

    // Connections loop.
    // Each iteration of the loop handles whole connection. Multiple iterations will only
    // happen in the on-demand mode or when handshake fails.
    for(;;) {
        for(;;) {
            m_sock = listen.Accept();
            if (m_sock) {
                break;
            }
            const auto t = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            if ( t - lastBroadcast > 3000000000 ) { // 3s
                lastBroadcast = t;
                const auto ts = std::chrono::duration_cast<std::chrono::seconds>( std::chrono::system_clock::now().time_since_epoch() ).count();
                broadcastMsg.activeTime = int32_t( ts - m_epoch );
                m_broadcast->Send( broadcastPort, &broadcastMsg, broadcastLen );
            }
        }

        lastBroadcast = 0;
        broadcastMsg.activeTime = -1;
        m_broadcast->Send( broadcastPort, &broadcastMsg, broadcastLen );

        // Handshake
        {
            char shibboleth[HandshakeShibbolethSize];
            auto res = m_sock->ReadRaw( shibboleth, HandshakeShibbolethSize, 2000 );
            if( !res || memcmp( shibboleth, HandshakeShibboleth, HandshakeShibbolethSize ) != 0 ) {
                delete m_sock;
                m_sock = nullptr;
                continue;
            }

            uint32_t protocolVersion;
            res = m_sock->ReadRaw( &protocolVersion, sizeof( protocolVersion ), 2000 );
            if( !res ) {
                delete m_sock;
                m_sock = nullptr;
                continue;
            }

            if( protocolVersion != ProtocolVersion ) {
                HandshakeStatus status = HandshakeProtocolMismatch;
                m_sock->Send( &status, sizeof( status ) );
                delete m_sock;
                m_sock = nullptr;
                continue;
            }
        }

        HandshakeStatus handshake = HandshakeWelcome;
        m_sock->Send( &handshake, sizeof( handshake ) );

        LZ4_resetStream( m_stream );
        m_sock->Send( &welcome, sizeof( welcome ) );

        buf_header(QueueType::ThreadContext);
        buf_u32(THREAD_ID);

        int keepAlive = 0;
        while (!m_quit) {
            bool idle = true;

            // process trace message queue
            while ((m_buf_ptr - m_buf) < (TargetFrameSize * 2)) {
                idle = false;
                jsdrvp_msg_s * msg = nullptr;
                if (JSDRV_ERROR_TIMED_OUT == msg_queue_pop(m_queue, &msg, 0)) {
                    break;
                }
                ProcessTraceMessage(msg);
            }
            if (!SendData()) {
                break;
            }

            // handle received messages
            bool connActive = true;
            while( m_sock && m_sock->HasData() ) {
                idle = false;
                connActive = HandleServerQuery();
                if( !connActive ) {
                    break;  // out of inner handle loop
                }
            }
            if( !connActive ) {
                break;  // out of main loop
            }

            if (idle) {
                keepAlive++;
                if( keepAlive >= 500 ) {
                    buf_header(QueueType::KeepAlive);
                    keepAlive = 0;
                } else {
                    std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
                }
            } else {
                keepAlive = 0;
            }
        }

        // Send client termination notice to the server
        buf_header(QueueType::Terminate);
        SendData();

        delete m_sock;
        m_sock = nullptr;
    }
}

#define COUNTER_FMT  "%u "

void Profiler::ProcessTraceMessage(jsdrvp_msg_s * msg) {
    const uint8_t * p8 = msg->payload.bin;
    const uint32_t * p32 = (const uint32_t *) p8;
    const uint32_t * p32_end = p32 + ((msg->value.size + 3) >> 2);

    while (p32 < p32_end) {
        if (MB_TRACE_SOF != (p32[0] & 0xff)) {
            JSDRV_LOGW("trace: invalid SOF");
            while (MB_TRACE_SOF != (p32[0] & 0xff)) {
                ++p32;
                if (p32 >= p32_end) {
                    return;
                }
            }
        }
        uint8_t length = (p32[0] >> 8) & 0x0f;
        uint8_t type = (p32[0] >> 12) & 0x0f;
        uint16_t metadata = p32[0] >> 16;
        uint32_t obj_type = (metadata >> 12) & 0x000f;
        const char * obj_name = MB_OBJ_NAME[obj_type];
        uint32_t obj_id = metadata & 0x0fff;
        uint32_t counter = p32[1];  // 100 MHz = 10 ns, rollover every 43 seconds
        uint64_t srcloc;
        if (counter < m_time.parts.l) {
            ++m_time.parts.u;  // todo estimate using wall-clock time?
            m_time.parts.l = counter;
        } else {
            m_time.parts.l = counter;
        }
        p32 += 2;
        uint32_t file_id = 0;
        uint32_t line = 0;
        if (length) {
            file_id = (p32[0] >> 16) & 0x0000ffff;
            line = p32[0] & 0x0000ffff;
        }

        switch (type) {
            case MB_TRACE_TYPE_INVALID:
                JSDRV_LOGW("trace type invalid");
                break;
            case MB_TRACE_TYPE_READY:
                printf(COUNTER_FMT "%s.%d ready\n", counter, obj_name, obj_id);
                break;
            case MB_TRACE_TYPE_ENTER: {
                if (obj_type == MB_OBJ_TASK) {
                    srcloc = (1LLU << 32) | obj_id;
                    m_task_zone_id[obj_id] = m_zone_id;
                } else if (obj_type == MB_OBJ_ISR) {
                    srcloc = (2LLU << 32) | obj_id;
                    m_isr_zone_id[obj_id] = m_zone_id;
                } else {
                    JSDRV_LOGW("enter type invalid");
                    break;
                }
                buf_header(QueueType::ZoneValidation);
                buf_u32(m_zone_id++);
                buf_header(QueueType::ZoneBegin);
                buf_reftime(m_time.time);
                buf_u64(srcloc);
                break;
            }
            case MB_TRACE_TYPE_EXIT:
                if (length == 0) {
                    uint32_t zone_id;
                    if (obj_type == MB_OBJ_TASK) {
                        zone_id = m_task_zone_id[obj_id];
                    } else if (obj_type == MB_OBJ_ISR) {
                        zone_id = m_isr_zone_id[obj_id];
                    } else {
                        JSDRV_LOGW("exit type invalid");
                        break;
                    }
                    buf_header(QueueType::ZoneValidation);
                    buf_u32(zone_id);
                    buf_header(QueueType::ZoneEnd);
                    buf_reftime(m_time.time);
                } else if (length == 1) {
                    if (obj_type == MB_OBJ_ISR) {
                        uint32_t zone_id = m_zone_id++;
                        buf_header(QueueType::ZoneValidation);
                        buf_u32(zone_id);
                        buf_header(QueueType::ZoneBegin);
                        srcloc = (2LLU << 32) | obj_id;
                        buf_reftime(m_time.time - p32[0]);
                        buf_u64(srcloc);
                        buf_header(QueueType::ZoneValidation);
                        buf_u32(zone_id);
                        buf_header(QueueType::ZoneEnd);
                        buf_reftime(m_time.time);
                    }
                } else {
                    JSDRV_LOGW("exit length invalid");
                }
                break;
            case MB_TRACE_TYPE_ALLOC:
                printf(COUNTER_FMT "%s.%d alloc @ %d.%d\n", counter, obj_name, obj_id, file_id, line);
                break;
            case MB_TRACE_TYPE_FREE:
                printf(COUNTER_FMT "%s.%d free @ %d.%d\n", counter, obj_name, obj_id, file_id, line);
                break;
            case MB_TRACE_TYPE_RSV6: break;
            case MB_TRACE_TYPE_RSV7: break;
            case MB_TRACE_TYPE_TIMESYNC: break;
            case MB_TRACE_TYPE_TIMEMAP: break;
            case MB_TRACE_TYPE_FAULT: break;
            case MB_TRACE_TYPE_VALUE: break;
            case MB_TRACE_TYPE_LOG:
                //printf(COUNTER_FMT "LOG @ %d.%d\n", counter, file_id, line);
                    break;
            case MB_TRACE_TYPE_RSV13: break;
            case MB_TRACE_TYPE_RSV14: break;
            case MB_TRACE_TYPE_OVERFLOW:
                //printf(COUNTER_FMT "OVERFLOW %d\n", counter, metadata);
                    break;
        }
        p32 += length;
    }
    jsdrvp_msg_free(m_context, msg);
}

bool Profiler::HandleServerQuery() {
    ServerQueryPacket payload;
    if ( !m_sock->Read( &payload, sizeof( payload ), 10 ) ) {
        return false;
    }

    switch( payload.type ) {
        case ServerQueryString:
            if (payload.ptr == 0) {
                buf_string_transfer(QueueType::StringData, payload.ptr, ""); // todo
            } else {
                buf_string_transfer(QueueType::StringData, payload.ptr, "hello"); // todo
            }
            break;
        case ServerQueryThreadString:
            buf_string_transfer(QueueType::ThreadName, payload.ptr, "main");
            break;
        case ServerQuerySourceLocation:
            buf_source_location( payload.ptr );
            break;
        case ServerQueryPlotName:
            buf_string_transfer(QueueType::PlotName, payload.ptr, "plot");  // todo
            break;
        case ServerQueryTerminate:
            return false;
        case ServerQueryDisconnect:
            m_quit = true;
            return false;
        case ServerQuerySymbol:
            buf_header(QueueType::AckServerQueryNoop);
            //buf_header(QueueType::AckSymbolCodeNotAvailable);
            break;
        case ServerQuerySourceCode:
            buf_header(QueueType::AckServerQueryNoop);
            //buf_header(QueueType::AckSourceCodeNotAvailable);
            break;
        case ServerQueryDataTransfer:
            buf_header(QueueType::AckServerQueryNoop);
            break;
        case ServerQueryDataTransferPart:
            buf_header(QueueType::AckServerQueryNoop);
            break;
        default:
            assert( false );
            break;
    }
    return true;
}

} // namespace tracy

void adapter_tracy_on_trace(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) topic;
    auto * self = (tracy::Profiler *) user_data;
    self->on_trace(value);
}

void * adapter_tracy_initialize(struct jsdrv_context_s * context) {
    auto * profiler = new tracy::Profiler(context);
    profiler->StartWorker();
    return profiler;
}

void adapter_tracy_finalize(void * self) {
    delete (tracy::Profiler *) self;
}
