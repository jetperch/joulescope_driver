/*
 * Copyright 2024 Jetperch LLC
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

#include <assert.h>
#include <stdint.h>
#include <cstring>  // memset
#include "joulescope_driver.h"

static const uint32_t _TIMEOUT_MS_INIT = 5000;
static const uint32_t _TIMEOUT_MS = 2000;


Napi::Object JoulescopeDriver::Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func =
            DefineClass(env, "JoulescopeDriver", {
                InstanceMethod("publish", &JoulescopeDriver::publish),
                InstanceMethod("query", &JoulescopeDriver::query),
                InstanceMethod("subscribe", &JoulescopeDriver::subscribe),
                InstanceMethod("finalize", &JoulescopeDriver::finalize)
            });

    Napi::FunctionReference* constructor = new Napi::FunctionReference();
    *constructor = Napi::Persistent(func);
    env.SetInstanceData(constructor);

    exports.Set("JoulescopeDriver", func);
    return exports;
}

JoulescopeDriver::JoulescopeDriver(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<JoulescopeDriver>(info) {
    Napi::Env env = info.Env();
    this->context_ = NULL;
    int32_t status = jsdrv_initialize(&this->context_, NULL, _TIMEOUT_MS_INIT);
    if (status) {
        napi_throw_error(env, NULL, "jsdrv_initialize failed");
        return;
    }
}

static uint32_t parse_timeout(Napi::Env env, Napi::Value value) {
    int64_t timeout_ms = _TIMEOUT_MS;
    if (value.IsNull()) {
        ;  // use default
    } else if (value.IsNumber()) {
        timeout_ms = value.As<Napi::Number>().Int64Value();
        if (timeout_ms > 4294967295LL) {
            timeout_ms = 4294967295LL;
        } else if (timeout_ms < 0) {
            timeout_ms = _TIMEOUT_MS;
        }
    } else {
        Napi::TypeError::New(env, "timeout_ms invalid type").ThrowAsJavaScriptException();
        // use default
    }
    return (uint32_t) timeout_ms;
}

Napi::Value JoulescopeDriver::publish(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() != 3) {
        Napi::TypeError::New(env, "Wrong number of arguments").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    uint32_t timeout_ms = parse_timeout(env, info[2]);

    if (!info[0].IsString()) {
        Napi::TypeError::New(env, "Wrong arguments").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    std::string topic = info[0].As<Napi::String>();
    std::string value_str;

    struct jsdrv_union_s value;
    memset(&value, 0, sizeof(value));
    if (info[1].IsString()) {
        value.type = JSDRV_UNION_STR;
        value.flags = JSDRV_UNION_FLAG_CONST;
        value_str = info[1].As<Napi::String>();
        value.value.str = value_str.c_str();
    } else if (info[1].IsNumber()) {
        value.value.i64 = info[1].As<Napi::Number>().Int64Value();
        if ((value.value.i64 >= 0) && (value.value.i64 < 4294967296LL)) {
            value.type = JSDRV_UNION_U32;
        } else if ((value.value.i64 >= -2147483648LL) && (value.value.i64 < 2147483648LL)) {
            value.type = JSDRV_UNION_I32;
        } else {
            value.type = JSDRV_UNION_I64;
        }
    } else {
        Napi::TypeError::New(env, "unsupported value type").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    if (topic.find_first_of("!") == std::string::npos) {
        value.flags |= JSDRV_UNION_FLAG_RETAIN;
    }

    int32_t status = jsdrv_publish(this->context_, topic.c_str(), &value, timeout_ms);
    if (status) {
        napi_throw_error(env, NULL, "jsdrv_publish failed");
    }
    return env.Undefined();
}

static Napi::Value obj_value1(Napi::Env env, double v1, const char * units) {
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("value", v1);
    obj.Set("units", units);
    return obj;
}

static Napi::Value obj_value2(Napi::Env env, double v1, double v2, const char * units) {
    Napi::Object obj = Napi::Object::New(env);
    Napi::Float64Array value = Napi::Float64Array::New(env, 2);
    value[0] = v1;
    value[1] = v2;
    obj.Set("value", value);
    obj.Set("units", units);
    return obj;
}

static Napi::Value obj_time_map(Napi::Env env, const struct jsdrv_time_map_s * time_map) {
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("offset_time", time_map->offset_time);
    obj.Set("offset_counter", time_map->offset_counter);
    obj.Set("counter_rate", time_map->counter_rate);
    return obj;
}

static double time64_to_ms(int64_t t) {
    int64_t t_ms = JSDRV_TIME_TO_MILLISECONDS(t);
    t_ms += ((int64_t) 1514764800) * 1000;
    return (double) t_ms;
}

static Napi::Value stream_to_js(Napi::Env env, const struct jsdrv_union_s * value) {
    const struct jsdrv_stream_signal_s * s = (const struct jsdrv_stream_signal_s *) value->value.bin;
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("sample_id", s->sample_id);
    obj.Set("utc", time64_to_ms(jsdrv_time_from_counter(&s->time_map, s->sample_id)));
    obj.Set("field_id", s->field_id);
    obj.Set("index", s->index);
    obj.Set("sample_rate", s->sample_rate);
    obj.Set("decimate_factor", s->decimate_factor);
    obj.Set("time_map", obj_time_map(env, &s->time_map));
    if (JSDRV_DATA_TYPE_FLOAT == s->element_type) {
        if (32 == s->element_size_bits) {
            Napi::Float32Array data = Napi::Float32Array::New(env, s->element_count);
            float * data_src = (float *) s->data;
            for (size_t idx = 0; idx < s->element_count; ++idx) {
                data[idx] = data_src[idx];
            }
            obj.Set("data", data);
        } else if (64 == s->element_size_bits) {
            Napi::Float64Array data = Napi::Float64Array::New(env, s->element_count);
            double * data_src = (double *) s->data;
            for (size_t idx = 0; idx < s->element_count; ++idx) {
                data[idx] = data_src[idx];
            }
            obj.Set("data", data);
        }
    } else if (JSDRV_DATA_TYPE_UINT == s->element_type) {
        if (1 == s->element_size_bits) {
            Napi::Uint8Array data = Napi::Uint8Array::New(env, s->element_count);
            uint8_t * data_src = (uint8_t *) s->data;
            for (size_t idx = 0; idx < s->element_count; ++idx) {
                data[idx] = (data_src[idx >> 3] >> (idx & 7)) & 1;
            }
            obj.Set("data", data);
        } else if (4 == s->element_size_bits) {
            Napi::Uint8Array data = Napi::Uint8Array::New(env, s->element_count);
            uint8_t * data_src = (uint8_t *) s->data;
            for (size_t idx = 0; idx < s->element_count; ++idx) {
                data[idx] = (data_src[idx >> 1] >> (4 * (idx & 1))) & 0x0f;
            }
            obj.Set("data", data);
        } else if (8 == s->element_size_bits) {
            Napi::Uint8Array data = Napi::Uint8Array::New(env, s->element_count);
            uint8_t * data_src = (uint8_t *) s->data;
            for (size_t idx = 0; idx < s->element_count; ++idx) {
                data[idx] = data_src[idx];
            }
            obj.Set("data", data);
        }
    } else if (JSDRV_DATA_TYPE_INT == s->element_type) {
        if (16 == s->element_size_bits) {
            Napi::Int16Array data = Napi::Int16Array::New(env, s->element_count);
            int16_t * data_src = (int16_t *) s->data;
            for (size_t idx = 0; idx < s->element_count; ++idx) {
                data[idx] = data_src[idx];
            }
            obj.Set("data", data);
        }
    }
    return obj;
}

static Napi::Value stats_to_js(Napi::Env env, const struct jsdrv_union_s * value) {
    const struct jsdrv_statistics_s * s =(const struct jsdrv_statistics_s *) value->value.bin;
    uint32_t sample_freq = s->sample_freq;
    uint64_t samples_full_rate = s->block_sample_count * (uint64_t) s->decimate_factor;
    uint64_t sample_id_start = s->block_sample_id;
    uint64_t sample_id_end = s->block_sample_id + samples_full_rate;
    uint64_t t_start = sample_id_start / sample_freq;
    uint64_t t_delta = samples_full_rate / sample_freq;

    Napi::Object obj = Napi::Object::New(env);
    Napi::Object time = Napi::Object::New(env);
    time.Set("samples", obj_value2(env, (double) sample_id_start, (double) sample_id_end, "samples"));
    time.Set("sample_freq", obj_value1(env, (double) sample_freq, "Hz"));
    time.Set("utc", obj_value2(
            env,
            time64_to_ms(jsdrv_time_from_counter(&s->time_map, sample_id_start)),
            time64_to_ms(jsdrv_time_from_counter(&s->time_map, sample_id_end)),
            "ms"));
    time.Set("range", obj_value2(env, (double) t_start, (double) (t_start + t_delta), "s"));
    time.Set("delta", obj_value1(env, (double) t_delta, "s"));
    time.Set("decimate_factor", obj_value1(env, (double) s->decimate_factor, "samples"));
    time.Set("decimate_sample_count", obj_value1(env, (double) s->block_sample_count, "samples"));
    time.Set("accum_samples", obj_value2(env, (double) s->accum_sample_id, (double) sample_id_end, "samples"));
    obj.Set("time_map", obj_time_map(env, &s->time_map));

    Napi::Object signals = Napi::Object::New(env);
    obj.Set("signals", signals);

    Napi::Object current = Napi::Object::New(env);
    current.Set("avg", obj_value1(env, s->i_avg, "A"));
    current.Set("std", obj_value1(env, s->i_std, "A"));
    current.Set("min", obj_value1(env, s->i_min, "A"));
    current.Set("max", obj_value1(env, s->i_max, "A"));
    current.Set("p2p", obj_value1(env, s->i_max - s->i_min, "A"));
    current.Set("integral", obj_value1(env, s->i_avg * t_delta, "C"));
    signals.Set("current", current);

    Napi::Object voltage = Napi::Object::New(env);
    voltage.Set("avg", obj_value1(env, s->v_avg, "V"));
    voltage.Set("std", obj_value1(env, s->v_std, "V"));
    voltage.Set("min", obj_value1(env, s->v_min, "V"));
    voltage.Set("max", obj_value1(env, s->v_max, "V"));
    voltage.Set("p2p", obj_value1(env, s->v_max - s->v_min, "V"));
    signals.Set("voltage", voltage);

    Napi::Object power = Napi::Object::New(env);
    power.Set("avg", obj_value1(env, s->p_avg, "W"));
    power.Set("std", obj_value1(env, s->p_std, "W"));
    power.Set("min", obj_value1(env, s->p_min, "W"));
    power.Set("max", obj_value1(env, s->p_max, "W"));
    power.Set("p2p", obj_value1(env, s->p_max - s->p_min, "W"));
    power.Set("integral", obj_value1(env, s->p_avg * t_delta, "J"));
    signals.Set("power", power);

    Napi::Object accumulators = Napi::Object::New(env);
    obj.Set("accumulators", accumulators);
    accumulators.Set("charge", obj_value1(env, s->charge_f64, "C"));
    accumulators.Set("energy", obj_value1(env, s->energy_f64, "J"));
    obj.Set("source", "sensor");

    return obj;
}

static Napi::Value buffer_info_to_js(Napi::Env env, const struct jsdrv_union_s * value) {
    return env.Undefined(); // todo
}

static Napi::Value buffer_rsp_to_js(Napi::Env env, const struct jsdrv_union_s * value) {
    return env.Undefined(); // todo
}

static Napi::Value bin_to_js(Napi::Env env, const struct jsdrv_union_s * value) {
    switch (value->app) {
        case JSDRV_PAYLOAD_TYPE_STREAM: return stream_to_js(env, value);
        case JSDRV_PAYLOAD_TYPE_STATISTICS: return stats_to_js(env, value);
        case JSDRV_PAYLOAD_TYPE_BUFFER_INFO: return buffer_info_to_js(env, value);
        case JSDRV_PAYLOAD_TYPE_BUFFER_RSP: return buffer_rsp_to_js(env, value);
        default:
            return env.Undefined(); // todo
    }
}


static Napi::Value union_to_js(Napi::Env env, const struct jsdrv_union_s * value) {
    // https://github.com/nodejs/node-addon-api/blob/main/doc/value.md
    // printf("union_to_js type=%d\n", value->type);
    switch (value->type) {
        case JSDRV_UNION_NULL: return env.Null();
        case JSDRV_UNION_STR: return Napi::String::New(env, value->value.str);
        case JSDRV_UNION_JSON: {
            Napi::String json_string = Napi::String::New(env, value->value.str);
            Napi::Object json = env.Global().Get("JSON").As<Napi::Object>();
            Napi::Function parse = json.Get("parse").As<Napi::Function>();
            return parse.Call(json, { json_string }).As<Napi::Object>();
        }
        case JSDRV_UNION_BIN: return bin_to_js(env, value);
        case JSDRV_UNION_F32: return Napi::Number::New(env, static_cast<double>(value->value.f32));
        case JSDRV_UNION_F64: return Napi::Number::New(env, static_cast<double>(value->value.f64));
        case JSDRV_UNION_U8:  return Napi::Number::New(env, static_cast<double>(value->value.u8));
        case JSDRV_UNION_U16: return Napi::Number::New(env, static_cast<double>(value->value.u16));
        case JSDRV_UNION_U32: return Napi::Number::New(env, static_cast<double>(value->value.u32));
        case JSDRV_UNION_U64: return Napi::Number::New(env, static_cast<double>(value->value.u64));
        case JSDRV_UNION_I8:  return Napi::Number::New(env, static_cast<double>(value->value.i8));
        case JSDRV_UNION_I16: return Napi::Number::New(env, static_cast<double>(value->value.i16));
        case JSDRV_UNION_I32: return Napi::Number::New(env, static_cast<double>(value->value.i32));
        case JSDRV_UNION_I64: return Napi::Number::New(env, static_cast<double>(value->value.i64));
        default: return env.Undefined();
    }
}

Napi::Value JoulescopeDriver::query(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() != 2) {
        Napi::TypeError::New(env, "Wrong number of arguments").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string topic_str = info[0].As<Napi::String>();
    uint32_t timeout_ms = parse_timeout(env, info[1]);
    struct jsdrv_union_s v;
    char byte_str[1024];
    memset(&v, 0, sizeof(v));
    v.type = JSDRV_UNION_BIN;
    v.size = sizeof(byte_str);
    v.value.str = byte_str;
    int32_t status = jsdrv_query(this->context_, topic_str.c_str(), &v, timeout_ms);
    if (status) {
        napi_throw_error(env, NULL, "jsdrv_query failed");
        return env.Undefined();
    }
    return union_to_js(env, &v);
}

struct subscribe_context {
    public:
    subscribe_context() {}
    std::string topic;
    uint8_t flags;
    Napi::ThreadSafeFunction fn;
};

void _subscribe_fn(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    struct subscribe_context * context = (struct subscribe_context *) user_data;
    std::string topic_str = topic;
    struct jsdrv_union_s * value_cpy = (struct jsdrv_union_s *) malloc(sizeof(*value) + value->size);
    if (NULL == value_cpy) {
        return;
    }
    *value_cpy = *value;

    if (value->size) {
        uint8_t *ptr = (uint8_t *) &value_cpy[1];
        value_cpy->value.bin = ptr;
        memcpy(ptr, value->value.bin, value->size);
    }

    auto callback = [topic_str]( Napi::Env env, Napi::Function jsCallback, const struct jsdrv_union_s * value) {
        jsCallback.Call( {Napi::String::New(env, topic_str), union_to_js(env, value)} );
        free((void *) value);
    };

    context->fn.NonBlockingCall(value_cpy, callback);
}

Napi::Value JoulescopeDriver::subscribe(const Napi::CallbackInfo& info) {  // topic, flags, fn, timeout
    // JSDRV_API int32_t jsdrv_subscribe(struct jsdrv_context_s * context, const char * topic, uint8_t flags,
    //        jsdrv_subscribe_fn cbk_fn, void * cbk_user_data,
    //        uint32_t timeout_ms);
    Napi::Env env = info.Env();
    if (info.Length() != 4) {
        Napi::TypeError::New(env, "Wrong number of arguments").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    subscribe_context * context = new subscribe_context();
    context->topic = info[0].As<Napi::String>();
    context->flags = (uint8_t) (info[1].As<Napi::Number>().Uint32Value());
    Napi::Function fn = info[2].As<Napi::Function>();
    uint32_t timeout_ms = parse_timeout(env, info[3]);
    context->fn = Napi::ThreadSafeFunction::New(env, fn, "jsdrv_subscribe_fn", 0, 1);
    int32_t status = jsdrv_subscribe(this->context_, context->topic.c_str(), context->flags,
                                     _subscribe_fn, context, timeout_ms);
    if (status) {
        delete context;
        napi_throw_error(env, NULL, "jsdrv_subscribe failed");
        return env.Undefined();
    }
    struct jsdrv_context_s * jsdrv_context = this->context_;

    auto unsub_fn = [env, jsdrv_context, context, timeout_ms](const Napi::CallbackInfo& info) -> Napi::Value {
        jsdrv_unsubscribe(jsdrv_context, context->topic.c_str(), _subscribe_fn, context, timeout_ms);
        context->fn.Release();
        delete context;
        return env.Undefined();
    };
    return Napi::Function::New(env, unsub_fn);
}

Napi::Value JoulescopeDriver::finalize(const Napi::CallbackInfo& info) {
    jsdrv_finalize(this->context_, _TIMEOUT_MS_INIT);
    this->context_ = NULL;
    return info.Env().Undefined();
}
