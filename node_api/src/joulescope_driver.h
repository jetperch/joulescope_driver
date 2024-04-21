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

#ifndef JOULESCOPE_DRIVER_NODE_H
#define JOULESCOPE_DRIVER_NODE_H

#include <napi.h>
#include "jsdrv.h"

class JoulescopeDriver : public Napi::ObjectWrap<JoulescopeDriver> {
        public:
        static Napi::Object Init(Napi::Env env, Napi::Object exports);
        JoulescopeDriver(const Napi::CallbackInfo& info);

        private:
        Napi::Value publish(const Napi::CallbackInfo& info);
        Napi::Value query(const Napi::CallbackInfo& info);
        Napi::Value subscribe(const Napi::CallbackInfo& info);
        Napi::Value unsubscribe(const Napi::CallbackInfo& info);
        Napi::Value unsubscribe_all(const Napi::CallbackInfo& info);
        Napi::Value finalize(const Napi::CallbackInfo& info);

        struct jsdrv_context_s * context_;
};

#endif  // JOULESCOPE_DRIVER_NODE_H
