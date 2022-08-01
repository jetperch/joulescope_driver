/*
* Copyright 2022 Jetperch LLC
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

#include "jsdrv_prv/js110_cal.h"
#include "jsdrv_prv/json.h"
#include "jsdrv/error_code.h"
#include "jsdrv_prv/log.h"
#include <string.h>


static const uint8_t CALIBRATION_MAGIC[] = "\xd3tagfmt \r\n \n  \x1a\x1c";

struct tl_s {
    uint32_t tag;
    uint32_t length;
};

struct parse_s {
    uint8_t depth;
    int8_t idx[3];
    uint8_t consume[3];
    double value[2][2][9];  // current/voltage, offset/gain
};


static int32_t json_cbk(void * user_data, const struct jsdrv_union_s * token) {
    struct parse_s * s = (struct parse_s *) user_data;

    switch (token->op) {
        case JSDRV_JSON_VALUE:
            if ((s->depth == 3) && (s->consume[1])) {
                struct jsdrv_union_s t = *token;
                int32_t rv = jsdrv_union_as_type(&t, JSDRV_UNION_F64);
                if (rv) {
                    JSDRV_LOGW("could not convert type to f32");
                    return rv;
                }
                s->value[s->idx[0]][s->idx[1]][s->idx[2]++] = t.value.f64;
            }
            break;
        case JSDRV_JSON_KEY:
            if (s->depth == 1) {
                if (0 == jsdrv_json_strcmp("current", token)) {
                    s->consume[0] = 1;
                    s->idx[0] = 0;
                } else if (0 == jsdrv_json_strcmp("voltage", token)) {
                    s->consume[0] = 1;
                    s->idx[0] = 1;
                } else {
                    s->consume[0] = 0;
                }
            } else if ((s->depth == 2) && (s->consume[0] == 1)) {
                if (0 == jsdrv_json_strcmp("offset", token)) {
                    s->consume[1] = 1;
                    s->idx[1] = 0;
                } else if (0 == jsdrv_json_strcmp("gain", token)) {
                    s->consume[1] = 1;
                    s->idx[1] = 1;
                } else {
                    s->consume[1] = 0;
                }
            }
            break;
        case JSDRV_JSON_OBJ_START: s->depth++; break;
        case JSDRV_JSON_OBJ_END: s->depth--; s->consume[0] = 0; break;
        case JSDRV_JSON_ARRAY_START: s->depth++; s->idx[2] = 0; break;
        case JSDRV_JSON_ARRAY_END: s->depth--; s->consume[1] = 0; break;
        default: break;
    }

    return 0;
}

int32_t js110_cal_parse(const uint8_t * data, double cal[2][2][9]) {
    struct parse_s state;
    struct tl_s * tl;
    struct js110_cal_header_s * hdr = (struct js110_cal_header_s *) data;
    if (!data || !cal) {
        return JSDRV_ERROR_PARAMETER_INVALID;
    }
    if (0 != memcmp(data, hdr->magic, sizeof(hdr->magic))) {
        return JSDRV_ERROR_PARAMETER_INVALID;
    }

    memset(&state, 0, sizeof(state));
    uint32_t offset = sizeof(*hdr);
    while (offset < hdr->length) {
        tl = (struct tl_s *) (&data[offset]);
        uint32_t tlv_length = sizeof(struct tl_s) + tl->length + 4;
        if (tlv_length & 0x7) {
            tlv_length += 8 - (tlv_length & 0x7);
        }
        if ((tl->tag & 0x00FFFFFFU) == 0x534A41) {
            char const * json = (char const *) (tl + 1);
            JSDRV_LOGI("%s", "Parse JSON calibration record");
            jsdrv_json_parse(json, json_cbk, &state);
        }
        offset += tlv_length;
    }

    memcpy(cal, state.value, sizeof(state.value));
    return 0;
}
