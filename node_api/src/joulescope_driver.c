#include <assert.h>
#include <node_api.h>
#include <stdint.h>
#include "jsdrv.h"

static const uint32_t _TIMEOUT_MS_INIT = 5000;

static napi_value initialize(napi_env env, napi_callback_info info) {
    struct jsdrv_context_s * context = NULL;
    int32_t status = jsdrv_initialize(&context, NULL, _TIMEOUT_MS_INIT);
    if (status) {
        napi_throw_error(env, NULL, "Failed to initialize jsdrv");
        return NULL;
    }

    // Wrap the context pointer in a JavaScript object
    napi_value n_context;
    napi_status n_status = napi_create_external(env, context, NULL, NULL, &n_context);  // NULLs for finalize callback & hint
    if (n_status != napi_ok) {
        // Make sure to finalize context since we won't return it
        jsdrv_finalize(context, _TIMEOUT_MS_INIT);
        napi_throw_error(env, NULL, "Failed to wrap jsdrv context");
        return NULL;
    }

    return n_context;
}

static napi_value finalize(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    struct jsdrv_context_s * context;
    napi_unwrap(env, args[0], (void**) &context);

    if (!context) {
        napi_throw_error(env, NULL, "Invalid context");
        return NULL;
    }

    jsdrv_finalize(context, _TIMEOUT_MS_INIT);
    return NULL;
}



#define DECLARE_NAPI_METHOD(name, func)                                        \
  { name, 0, func, 0, 0, 0, napi_default, 0 }

static napi_value Init(napi_env env, napi_value exports) {
    napi_status status;
    napi_property_descriptor desc[] = {
            DECLARE_NAPI_METHOD("initialize", initialize),
    };
    status = napi_define_properties(env, exports, sizeof(desc) / sizeof(*desc), desc);
    assert(status == napi_ok);
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
