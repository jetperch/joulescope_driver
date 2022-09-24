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

#define JSDRV_LOG_LEVEL JSDRV_LOG_LEVEL_ALL

#include "jsdrv_prv/pubsub.h"
#include "jsdrv_prv/platform.h"
#include "jsdrv_prv/assert.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/backend.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/log.h"
#include "jsdrv/meta.h"
#include "jsdrv/cstr.h"
#include "jsdrv/error_code.h"

struct subscriber_s {
    struct jsdrv_pubsub_subscriber_s sub;
    struct jsdrv_list_s item;
};

struct topic_s {
    char name[JSDRV_TOPIC_LENGTH_PER_LEVEL];
    struct jsdrvp_msg_s * value;
    struct jsdrvp_msg_s * meta;
    struct topic_s * parent;
    struct jsdrv_list_s item;  // used by parent->children list
    struct jsdrv_list_s children;
    struct jsdrv_list_s subscribers;
};

struct jsdrv_pubsub_s {
    struct jsdrv_context_s * context;
    struct topic_s * root_topic;
    struct jsdrv_list_s subscriber_free;      // of subscriber_s
    struct jsdrv_list_s msg_pend;             // of jsdrvp_msg_s
};

static uint8_t publish(struct topic_s * topic, struct jsdrvp_msg_s * msg, uint8_t flags);

static void topic_str_append(char * topic_str, const char * topic_sub_str) {
    // WARNING: topic_str must be >= TOPIC_LENGTH_MAX
    // topic_sub_str <= TOPIC_LENGTH_PER_LEVEL
    size_t topic_len = 0;
    char * t = topic_str;

    // find the end of topic_str
    while (*t) {
        ++topic_len;
        ++t;
    }
    if (topic_len >= (JSDRV_TOPIC_LENGTH_MAX - 1)) {
        return;
    }

    // add separator
    if (topic_len) {
        *t++ = '/';
        ++topic_len;
    }

    // Copy substring
    while (*topic_sub_str && (topic_len < (JSDRV_TOPIC_LENGTH_MAX - 1))) {
        *t++ = *topic_sub_str++;
        ++topic_len;
    }
    *t = 0;  // null terminate
}

#if 0
static void topic_str_pop(char * topic_str) {
    char * end = topic_str;
    if (!topic_str || !topic_str[0]) {
        return;  // nothing to pop
    }
    for (uint32_t i = 1; i < JSDRV_TOPIC_LENGTH_MAX; ++i) {
        if (!topic_str[i]) {
            *end = topic_str[i - 1];
        }
    }
    while (end >= topic_str) {
        if (*end == '/') {
            *end-- = 0;
            return;
        }
        *end-- = 0;
    }
}
#endif

static struct subscriber_s * subscriber_alloc(struct jsdrv_pubsub_s * self) {
    struct subscriber_s * sub;
    if (!jsdrv_list_is_empty(&self->subscriber_free)) {
        struct jsdrv_list_s * item;
        item = jsdrv_list_remove_head(&self->subscriber_free);
        sub = JSDRV_CONTAINER_OF(item, struct subscriber_s, item);
    } else {
        sub = jsdrv_alloc_clr(sizeof(struct subscriber_s));
        //JSDRV_LOGD3("subscriber alloc: %p", (void *) sub);
    }
    jsdrv_memset(sub, 0, sizeof(*sub));
    jsdrv_list_initialize(&sub->item);
    return sub;
}

static void subscriber_free(struct jsdrv_pubsub_s * self, struct subscriber_s * sub) {
    jsdrv_list_add_tail(&self->subscriber_free, &sub->item);
}

static struct topic_s * topic_alloc(struct jsdrv_pubsub_s * self, const char * name) {
    (void) self;
    struct topic_s * topic = jsdrv_alloc_clr(sizeof(struct topic_s));
    topic->value = NULL;
    jsdrv_list_initialize(&topic->item);
    jsdrv_list_initialize(&topic->children);
    jsdrv_list_initialize(&topic->subscribers);
    JSDRV_ASSERT(0 == jsdrv_cstr_copy(topic->name, name, sizeof(topic->name)));
    JSDRV_LOGD2("topic alloc: %p '%s'", (void *) topic, name);
    return topic;
}

static void topic_free(struct jsdrv_pubsub_s * self, struct topic_s * topic) {
    struct jsdrv_list_s * item;
    struct subscriber_s * subscriber;
    if (!topic) {
        return;
    }
    if (topic->value) {
        jsdrvp_msg_free(self->context, topic->value);
        topic->value = NULL;
    }
    if (topic->meta) {
        jsdrvp_msg_free(self->context, topic->meta);
        topic->meta = NULL;
    }
    jsdrv_list_foreach(&topic->subscribers, item) {
        subscriber = JSDRV_CONTAINER_OF(item, struct subscriber_s, item);
        jsdrv_list_remove(item);
        subscriber_free(self, subscriber);
    }
    struct topic_s * subtopic;
    jsdrv_list_foreach(&topic->children, item) {
        subtopic = JSDRV_CONTAINER_OF(item, struct topic_s, item);
        jsdrv_list_remove(item);
        topic_free(self, subtopic);
    }
    //JSDRV_LOGD3("topic free: %p", (void *)topic);
    jsdrv_free(topic);
}

/**
 * @brief Parse the next subtopic.
 * @param topic[inout] The topic, which is advanced to the next subtopic.
 * @param subtopic[out] The parsed subtopic, which must be
 *      at least JSDRV_TOPIC_LENGTH_PER_LEVEL bytes.
 * @return true on success, false on failure.
 */
static bool subtopic_get_str(const char ** topic, char * subtopic) {
    const char * t = *topic;
    for (uint32_t i = 0; i < JSDRV_TOPIC_LENGTH_PER_LEVEL; ++i) {
        if (*t == 0) {
            *subtopic = 0;
            *topic = t;
            return true;
        } else if (*t == '/') {
            *subtopic = 0;
            t++;
            *topic = t;
            return true;
        } else {
            *subtopic++ = *t++;
        }
    }
    JSDRV_LOGW("subtopic too long: %s", *topic);
    return false;
}

static struct topic_s * subtopic_find(struct topic_s * parent, const char * subtopic_str) {
    struct jsdrv_list_s * item;
    struct topic_s * topic;
    jsdrv_list_foreach(&parent->children, item) {
        topic = JSDRV_CONTAINER_OF(item, struct topic_s, item);
        if (0 == strcmp(subtopic_str, topic->name)) {
            return topic;
        }
    }
    return NULL;
}

static struct topic_s * topic_find(struct jsdrv_pubsub_s * self, const char * topic, bool create) {
    char subtopic_str[JSDRV_TOPIC_LENGTH_PER_LEVEL];
    const char * c = topic;

    struct topic_s * t = self->root_topic;
    struct topic_s * subtopic;
    while (*c != 0) {
        if (!subtopic_get_str(&c, subtopic_str)) {
            return NULL;
        }
        subtopic = subtopic_find(t, subtopic_str);
        if (!subtopic) {
            if (!create) {
                return NULL;
            }
            subtopic = topic_alloc(self, subtopic_str);
            subtopic->parent = t;
            jsdrv_list_add_tail(&t->children, &subtopic->item);
        }
        t = subtopic;
    }
    return t;
}

struct jsdrv_pubsub_s * jsdrv_pubsub_initialize(struct jsdrv_context_s * context) {
    struct jsdrv_pubsub_s * s = jsdrv_alloc_clr(sizeof(struct jsdrv_pubsub_s));
    s->context = context;
    jsdrv_list_initialize(&s->subscriber_free);
    jsdrv_list_initialize(&s->msg_pend);
    s->root_topic = topic_alloc(s, "");
    return s;
}

void jsdrv_pubsub_finalize(struct jsdrv_pubsub_s * self) {
    if (self) {
        while (!jsdrv_list_is_empty(&self->msg_pend)) {
            struct jsdrv_list_s * item = jsdrv_list_remove_head(&self->msg_pend);
            struct jsdrvp_msg_s * m = JSDRV_CONTAINER_OF(item, struct jsdrvp_msg_s, item);
            jsdrvp_msg_free(self->context, m);
        }
        topic_free(self, self->root_topic);
        while (!jsdrv_list_is_empty(&self->subscriber_free)) {
            struct jsdrv_list_s * item = jsdrv_list_remove_head(&self->subscriber_free);
            struct subscriber_s * sub = JSDRV_CONTAINER_OF(item, struct subscriber_s, item);
            //JSDRV_LOGD3("subscriber free: %p", (void *) sub);
            jsdrv_free(sub);
        }
        jsdrv_free(self);
    }
}

int32_t jsdrv_pubsub_publish(struct jsdrv_pubsub_s * self, struct jsdrvp_msg_s * msg) {
    jsdrv_list_add_tail(&self->msg_pend, &msg->item);
    return 0;
}

static int8_t subscriber_call(struct jsdrv_pubsub_subscriber_s * s, struct jsdrvp_msg_s * msg) {
    uint8_t rc = 0;
    if (!s->void_fn) {
        JSDRV_LOGW("skip null subscriber");
    } else if (s->is_internal) {
        rc = s->internal_fn(s->user_data, msg);
    } else {
        if ((msg->value.app == JSDRV_PAYLOAD_TYPE_UNION)
                || (msg->value.app == JSDRV_PAYLOAD_TYPE_STREAM)
                || (msg->value.app == JSDRV_PAYLOAD_TYPE_STATISTICS)) {
            s->external_fn(s->user_data, msg->topic, &msg->value);
        } else if ((msg->value.type == JSDRV_UNION_BIN) && (msg->value.app == JSDRV_PAYLOAD_TYPE_DEVICE)) {
            s->external_fn(s->user_data, msg->topic, &jsdrv_union_str(msg->payload.device.prefix));
        } else {
            JSDRV_LOGW("unsupported value.app type: %d", (int) msg->value.app);
        }
    }

    if (rc) {
        JSDRV_LOGW("subscriber returned %d", (int) rc);
        // todo handle error
    }
    return rc;
}

static void subscribe_traverse(struct topic_s * topic, char * topic_str, struct subscriber_s * sub) {
    size_t topic_str_len = strlen(topic_str);
    char * topic_str_last = topic_str + topic_str_len;
    if ((sub->sub.flags & JSDRV_SFLAG_METADATA_RSP) && topic->meta) {
        subscriber_call(&sub->sub, topic->meta);
    }
    if ((sub->sub.flags & JSDRV_SFLAG_PUB) && topic->value && (topic->value->value.flags & JSDRV_UNION_FLAG_RETAIN)) {
        subscriber_call(&sub->sub, topic->value);
    }
    struct jsdrv_list_s * item;
    struct topic_s * subtopic;
    jsdrv_list_foreach(&topic->children, item) {
        subtopic = JSDRV_CONTAINER_OF(item, struct topic_s, item);
        topic_str_append(topic_str, subtopic->name);
        subscribe_traverse(subtopic, topic_str, sub);
        *topic_str_last = 0;  // reset string to original
    }
}

static void devices_on_sub(struct jsdrv_pubsub_s * self, struct jsdrvp_msg_s * msg) {
    // publish device add for all existing devices (as needed)
    char dev_str[JSDRV_TOPIC_LENGTH_MAX];
    const char * t = msg->payload.sub.topic;
    if ((0 == strcmp(t, "")) || (0 == strcmp(t, "@")) || (0 == strcmp(t, "@/")) || (0 == strcmp(t, JSDRV_MSG_DEVICE_ADD))) {
        struct topic_s * t_dev_list = topic_find(self, JSDRV_MSG_DEVICE_LIST, false);
        if (!t_dev_list || !t_dev_list->value || (t_dev_list->value->value.type != JSDRV_UNION_STR)) {
            return;
        }
        const char * src = t_dev_list->value->value.value.str;
        char * dst = dev_str;
        while (1) {
            if (*src == 0 || *src == ',') {
                *dst = 0;
                if (dev_str[0]) {
                    msg->payload.sub.subscriber.external_fn(msg->payload.sub.subscriber.user_data,
                                                            JSDRV_MSG_DEVICE_ADD,
                                                            &jsdrv_union_str(dev_str));
                }
                dst = dev_str;
                if (!*src) {
                    break;
                }
                ++src;
            } else {
                *dst++ = *src++;
            }
        }
    }
}

static void query_value_copy(const struct jsdrvp_msg_s * src, struct jsdrvp_msg_s * dst) {
    if (!src) {
        *dst->payload.query.value = jsdrv_union_null();
        return;
    }
    size_t sz = src->value.size;
    if (jsdrv_union_is_type_ptr(&src->value)) {
        if (!jsdrv_union_is_type_ptr(dst->payload.query.value)) {
            dst->value = jsdrv_union_i32(JSDRV_ERROR_SYNTAX_ERROR);
            return;
        }
        if (!sz) {
            sz = strlen(src->value.value.str) + 1;
        }
        if (sz > dst->payload.query.value->size) {
            dst->value = jsdrv_union_i32(JSDRV_ERROR_TOO_SMALL);
        } else {
            memcpy((void *) dst->payload.query.value->value.bin, src->value.value.bin, sz);
            dst->payload.query.value->type = src->value.type;
            dst->payload.query.value->size = (uint32_t) sz;
            dst->value = jsdrv_union_i32(0);
        }
    } else {
        *dst->payload.query.value = src->value;
        dst->value = jsdrv_union_i32(0);
    }
}

static void publish_return_code(struct jsdrv_pubsub_s * self, struct jsdrvp_msg_s * msg) {
    size_t sz = strlen(msg->topic);
    switch (msg->topic[sz - 1]) {
        case JSDRV_TOPIC_SUFFIX_METADATA_REQ:    /** intentional fall-through */
        case JSDRV_TOPIC_SUFFIX_METADATA_RSP:    /** intentional fall-through */
        case JSDRV_TOPIC_SUFFIX_QUERY_REQ:       /** intentional fall-through */
        case JSDRV_TOPIC_SUFFIX_QUERY_RSP:       /** intentional fall-through */
        case JSDRV_TOPIC_SUFFIX_RETURN_CODE:     /** intentional fall-through */
            msg->topic[sz - 1] = 0;
            --sz;
            break;
        default:
            break;
    }
    struct topic_s * t = topic_find(self, msg->topic, true);
    if (t) {
        msg->topic[sz] = JSDRV_TOPIC_SUFFIX_RETURN_CODE;
        msg->topic[sz + 1] = 0;
        publish(t, msg, JSDRV_SFLAG_RETURN_CODE);
    }
    jsdrvp_msg_free(self->context, msg);
}

static void publish_return_code_i32(struct jsdrv_pubsub_s * self, const char * topic, int32_t return_code) {
    struct jsdrvp_msg_s * msg = jsdrvp_msg_alloc_value(self->context, topic, &jsdrv_union_i32(return_code));
    publish_return_code(self, msg);
}

static int32_t query(struct jsdrv_pubsub_s * self, struct jsdrvp_msg_s * msg) {
    char topic[JSDRV_TOPIC_LENGTH_MAX];
    jsdrv_cstr_copy(topic, msg->payload.query.topic, sizeof(topic));
    size_t sz = strlen(topic);
    bool is_meta = false;
    if (topic[sz - 1] == JSDRV_TOPIC_SUFFIX_QUERY_REQ) {
        topic[sz - 1] = 0; --sz;
    } else if (topic[sz - 1] == JSDRV_TOPIC_SUFFIX_METADATA_REQ) {
        topic[sz - 1] = 0; --sz;
        is_meta = true;
    }
    struct topic_s * t = topic_find(self, topic, false);
    if (!t) {
        msg->value = jsdrv_union_i32(JSDRV_ERROR_NOT_FOUND);
    } else if (is_meta) {
        if (!t->meta) {
            msg->payload.query.value->type = JSDRV_UNION_NULL;
        } else {
            query_value_copy(t->meta, msg);
        }
    } else {
        query_value_copy(t->value, msg);
        char buf[32];
        jsdrv_union_value_to_str(msg->payload.query.value, buf, sizeof(buf), 1);
        JSDRV_LOGD1("query %s => %s", topic, buf);
    }
    return msg->value.value.i32;
}

static int32_t subscribe(struct jsdrv_pubsub_s * self, struct jsdrvp_msg_s * msg) {
    JSDRV_ASSERT(msg->value.type == JSDRV_UNION_BIN);
    JSDRV_ASSERT(msg->value.value.bin == msg->payload.bin);
    struct topic_s * t = topic_find(self, msg->payload.sub.topic, true);
    if (!t) {
        JSDRV_LOGE("could not find/create subscribe topic");
        return JSDRV_ERROR_NOT_FOUND;
    }

    struct subscriber_s * sub = subscriber_alloc(self);
    sub->sub = msg->payload.sub.subscriber;
    jsdrv_list_add_tail(&t->subscribers, &sub->item);

    if (sub->sub.flags & JSDRV_SFLAG_RETAIN) {
        devices_on_sub(self, msg);
        subscribe_traverse(t, msg->payload.sub.topic, sub);
    }
    return 0;
}

static bool is_same_subscriber(const struct jsdrv_pubsub_subscriber_s * a, const struct jsdrv_pubsub_subscriber_s * b) {
    return ((a->void_fn) &&
            (a->is_internal == b->is_internal) &&
            (a->void_fn == b->void_fn) &&
            (a->user_data == b->user_data));
}

static int32_t unsubscribe(struct jsdrv_pubsub_s * self, struct jsdrvp_msg_s * msg) {
    struct jsdrv_list_s * item;
    struct subscriber_s * s;
    struct topic_s * t = topic_find(self, msg->payload.sub.topic, true);
    int count = 0;
    if (!t) {
        JSDRV_LOGE("could not find/create unsubscribe topic");
        return JSDRV_ERROR_PARAMETER_INVALID;
    }
    jsdrv_list_foreach(&t->subscribers, item) {
        s = JSDRV_CONTAINER_OF(item, struct subscriber_s, item);
        if (is_same_subscriber(&s->sub, &msg->payload.sub.subscriber)) {
            jsdrv_list_remove(item);
            subscriber_free(self, s);
            ++count;
        }
    }
    return count ? 0 : JSDRV_ERROR_NOT_FOUND;
}

static void unsubscribe_traverse(struct jsdrv_pubsub_s * self, struct topic_s * topic, struct jsdrvp_msg_s * msg) {
    struct jsdrv_list_s * item;
    struct topic_s * subtopic;
    struct subscriber_s * s;
    jsdrv_list_foreach(&topic->subscribers, item) {
        s = JSDRV_CONTAINER_OF(item, struct subscriber_s, item);
        if (is_same_subscriber(&s->sub, &msg->payload.sub.subscriber)) {
            jsdrv_list_remove(item);
            subscriber_free(self, s);
        }
    }

    jsdrv_list_foreach(&topic->children, item) {
        subtopic = JSDRV_CONTAINER_OF(item, struct topic_s, item);
        unsubscribe_traverse(self, subtopic, msg);
    }
}

static void unsubscribe_from_all(struct jsdrv_pubsub_s * self, struct jsdrvp_msg_s * msg) {
    unsubscribe_traverse(self, self->root_topic, msg);
}

static uint8_t publish(struct topic_s * topic, struct jsdrvp_msg_s * msg, uint8_t flags) {
    uint8_t status = 0;
    struct jsdrv_list_s * item;
    struct subscriber_s * s;
    while (topic) {
        jsdrv_list_foreach(&topic->subscribers, item) {
            s = JSDRV_CONTAINER_OF(item, struct subscriber_s, item);
            if (is_same_subscriber(&s->sub, &msg->extra.frontend.subscriber)) {
                continue;
            }
            switch (flags) {
                case JSDRV_SFLAG_RETURN_CODE:
                    if (!(s->sub.flags & JSDRV_SFLAG_RETURN_CODE)) {
                        continue;
                    }
                    break;
                case JSDRV_SFLAG_METADATA_RSP:
                    if (!(s->sub.flags & JSDRV_SFLAG_METADATA_RSP)) {
                        continue;
                    }
                    break;
                default:
                    if (!(s->sub.flags & JSDRV_SFLAG_PUB)) {
                        continue;
                    }
                    break;
            }
            uint8_t rv = subscriber_call(&s->sub, msg);
            if (!status && rv) {
                status = rv;
            }
        }
        topic = topic->parent;
    }
    return status;
}

static void publish_meta(struct jsdrv_pubsub_s * self, struct jsdrvp_msg_s * msg) {
    char topic[JSDRV_TOPIC_LENGTH_MAX];
    jsdrv_cstr_copy(topic, msg->topic, sizeof(topic));
    topic[strlen(topic) - 1] = 0;
    struct topic_s * t = topic_find(self, topic, true);
    if (t) {
        if (t->meta) {
            jsdrvp_msg_free(self->context, t->meta);
        }
        if (!msg->value.size) {
            msg->value.size = (uint32_t) (strlen(msg->value.value.str) + 1);
        }
        t->meta = msg;
        publish(t, msg, JSDRV_SFLAG_METADATA_RSP);
    } else {
        jsdrvp_msg_free(self->context, msg);
    }
}

static void local_return_code(struct jsdrv_pubsub_s * self, const char * topic, int32_t return_code) {
    struct topic_s * t = topic_find(self, topic, false);
    if (t) {
        struct jsdrvp_msg_s * rsp = jsdrvp_msg_alloc_value(self->context, "", &jsdrv_union_i32(return_code));
        jsdrv_cstr_join(rsp->topic, topic, "#", sizeof(rsp->topic));
        publish(t, rsp, JSDRV_SFLAG_RETURN_CODE);
        jsdrvp_msg_free(self->context, rsp);
    } else {
        JSDRV_LOGW("local_return_code failed on %s", topic);
    }
}

static void publish_normal(struct jsdrv_pubsub_s * self, struct jsdrvp_msg_s * msg) {
    uint8_t status = 0;
    struct topic_s * t = topic_find(self, msg->topic, true);
    if (t) {
        if (t->meta) {
            status = jsdrv_meta_value(t->meta->value.value.str, &msg->value);
            if (status) {
                char buf[32];
                jsdrv_union_value_to_str(&msg->value, buf, (uint32_t) sizeof(buf), 1);
                JSDRV_LOGW("pubsub validate failed %s %s", msg->topic, buf);
                local_return_code(self, msg->topic, status);
                jsdrvp_msg_free(self->context, msg);
                return;
            }
        }
        if (t->value && jsdrv_union_eq(&t->value->value, &msg->value)) {
            JSDRV_LOGD1("pubsub dedup %s", msg->topic);
            local_return_code(self, msg->topic, 0);
            jsdrvp_msg_free(self->context, msg);
            return;
        }
        if (t->value) {
            jsdrvp_msg_free(self->context, t->value);  // free old value
            t->value = NULL;
        }
        if ((msg->value.flags & JSDRV_UNION_FLAG_RETAIN) && (t->name[0] != '!')) {
            t->value = msg;
        } else {
            t->value = NULL;
        }
        status = publish(t, msg, 0);
        if (status) {
            local_return_code(self, msg->topic, status);
        }
        if (!t->value) {
            jsdrvp_msg_free(self->context, msg);
        }
    } else {
        jsdrvp_msg_free(self->context, msg);
    }
}

static void process_msg(struct jsdrv_pubsub_s * self, struct jsdrvp_msg_s * msg) {
    int32_t rc = 0;
    if (msg->topic[0] == JSDRV_PUBSUB_COMMAND_PREFIX) {
        if (0 == strcmp(JSDRV_PUBSUB_QUERY, msg->topic)) {
            rc = query(self, msg);
        } else if (0 == strcmp(JSDRV_PUBSUB_SUBSCRIBE, msg->topic)) {
            rc = subscribe(self, msg);
        } else if (0 == strcmp(JSDRV_PUBSUB_UNSUBSCRIBE, msg->topic)) {
            rc = unsubscribe(self, msg);
        } else if (0 == strcmp(JSDRV_PUBSUB_UNSUBSCRIBE_ALL, msg->topic)) {
            unsubscribe_from_all(self, msg);
        } else {
            JSDRV_LOGW("unsupported command %s", msg->topic);
            rc = JSDRV_ERROR_NOT_SUPPORTED;
        }
        if (msg->source) {
            JSDRV_LOGD1("publish_return_code_i32(\"%s\", %ld)", msg->topic, rc);
            publish_return_code_i32(self, msg->topic, rc);
        }
        jsdrvp_msg_free(self->context, msg);
    } else {  // publish to topic
        size_t topic_sz = strlen(msg->topic);  // excluding terminator
        if (0 == topic_sz) {
            JSDRV_LOGW("publish to root not allowed");
            if (msg->source) {
                publish_return_code_i32(self, msg->topic, JSDRV_ERROR_NOT_SUPPORTED);
            }
            jsdrvp_msg_free(self->context, msg);
        } else {
            switch (msg->topic[topic_sz - 1]) {
                case '$': publish_meta(self, msg); break;
                case '#': publish_return_code(self, msg); break;
                default: publish_normal(self, msg); break;
            }
        }
    }
}

void jsdrv_pubsub_process(struct jsdrv_pubsub_s * self) {
    while (!jsdrv_list_is_empty(&self->msg_pend)) {
        struct jsdrv_list_s * item = jsdrv_list_remove_head(&self->msg_pend);
        struct jsdrvp_msg_s * msg = JSDRV_CONTAINER_OF(item, struct jsdrvp_msg_s, item);
        if (jsdrv_cstr_ends_with(msg->topic, "!data")) {
            JSDRV_LOGD3("jsdrv_pubsub_process %s", msg->topic);
        } else {
            char buf[32];
            jsdrv_union_value_to_str(&msg->value, buf, sizeof(buf), 1);
            JSDRV_LOGD1("jsdrv_pubsub_process %s => %s", msg->topic, buf);
        }
        process_msg(self, msg);
    }
}