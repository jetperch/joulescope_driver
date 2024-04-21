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

static Napi::Value union_to_js(Napi::Env env, const struct jsdrv_union_s * value) {
    // https://github.com/nodejs/node-addon-api/blob/main/doc/value.md
    printf("union_to_js type=%d\n", value->type);
    switch (value->type) {
        case JSDRV_UNION_NULL: return env.Null();
        case JSDRV_UNION_STR: return Napi::String::New(env, value->value.str);
        case JSDRV_UNION_JSON: {
            Napi::String json_string = Napi::String::New(env, value->value.str);
            Napi::Object json = env.Global().Get("JSON").As<Napi::Object>();
            Napi::Function parse = json.Get("parse").As<Napi::Function>();
            return parse.Call(json, { json_string }).As<Napi::Object>();
        }
        case JSDRV_UNION_BIN: return env.Undefined();  // todo
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
    printf("query %s, timeout_ms = %d\n", topic_str.c_str(), timeout_ms);
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
    auto callback = [topic_str]( Napi::Env env, Napi::Function jsCallback, const struct jsdrv_union_s * value) {
        jsCallback.Call( {Napi::String::New(env, topic_str), union_to_js(env, value)} );
    };

    context->fn.NonBlockingCall(value, callback);
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
    printf("ThreadSafeFunction construct\n");
    context->fn = Napi::ThreadSafeFunction::New(env, fn, "jsdrv_subscribe_fn", 0, 1);
    printf("ThreadSafeFunction constructed\n");
    int32_t status = jsdrv_subscribe(this->context_, context->topic.c_str(), context->flags,
                                     _subscribe_fn, context, timeout_ms);
    if (status) {
        delete context;
        napi_throw_error(env, NULL, "jsdrv_subscribe failed");
        return env.Undefined();
    }
    printf("Lambdas\n!");
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
