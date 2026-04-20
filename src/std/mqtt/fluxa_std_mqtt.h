#ifndef FLUXA_STD_MQTT_H
#define FLUXA_STD_MQTT_H

/*
 * std.mqtt — MQTT client for Fluxa-lang
 *
 * Backed by libmosquitto. All calls must be inside danger{}.
 * Connection state lives in a dyn cursor (use prst dyn for persistence).
 *
 * API:
 *   mqtt.connect(host, port, client_id)  → dyn cursor
 *   mqtt.connect_auth(host, port, id, user, pass) → dyn cursor
 *   mqtt.disconnect(cursor)              → nil
 *   mqtt.publish(cursor, topic, payload) → nil
 *   mqtt.publish_qos(cursor, topic, payload, qos) → nil
 *   mqtt.subscribe(cursor, topic)        → nil
 *   mqtt.subscribe_qos(cursor, topic, qos) → nil
 *   mqtt.loop(cursor, timeout_ms)        → nil  (process one event)
 *   mqtt.connected(cursor)               → bool
 */

#include <stdlib.h>
#include <string.h>
#include <mosquitto.h>
#include "../../scope.h"
#include "../../err.h"

/* ── Cursor ──────────────────────────────────────────────────────── */
typedef struct {
    struct mosquitto *mosq;
    int               connected;
} MqttClient;

/* ── Value helpers ───────────────────────────────────────────────── */
static inline Value mqtt_nil(void)          { Value v; v.type = VAL_NIL;  return v; }
static inline Value mqtt_bool(int b)        { Value v; v.type = VAL_BOOL; v.as.boolean = b; return v; }

static inline Value mqtt_wrap(MqttClient *c) {
    FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
    d->cap = 1; d->count = 1;
    d->items = (Value *)malloc(sizeof(Value));
    d->items[0].type   = VAL_PTR;
    d->items[0].as.ptr = c;
    Value v; v.type = VAL_DYN; v.as.dyn = d;
    return v;
}

static inline MqttClient *mqtt_unwrap(const Value *v, ErrStack *err,
                                       int *had_error, int line,
                                       const char *fn) {
    char errbuf[280];
    if (v->type != VAL_DYN || !v->as.dyn || v->as.dyn->count < 1 ||
        v->as.dyn->items[0].type != VAL_PTR || !v->as.dyn->items[0].as.ptr) {
        snprintf(errbuf, sizeof(errbuf),
            "mqtt.%s: invalid client cursor — use mqtt.connect() first", fn);
        errstack_push(err, ERR_FLUXA, errbuf, "mqtt", line);
        *had_error = 1;
        return NULL;
    }
    return (MqttClient *)v->as.dyn->items[0].as.ptr;
}

/* ── Dispatch ────────────────────────────────────────────────────── */
static inline Value fluxa_std_mqtt_call(const char *fn_name,
                                         const Value *args, int argc,
                                         ErrStack *err, int *had_error,
                                         int line) {
    char errbuf[280];

#define MQTT_ERR(msg) do { \
    char _eb[512]; \
    snprintf(_eb, sizeof(_eb), "mqtt.%s (line %d): %.400s", \
             fn_name, line, (msg)); \
    errstack_push(err, ERR_FLUXA, _eb, "mqtt", line); \
    *had_error = 1; return mqtt_nil(); \
} while(0)

#define NEED(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "mqtt.%s: expected %d arg(s), got %d", fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "mqtt", line); \
        *had_error = 1; return mqtt_nil(); \
    } \
} while(0)

#define GET_STR(idx, var) \
    if (args[(idx)].type != VAL_STRING || !args[(idx)].as.string) \
        MQTT_ERR("expected str argument"); \
    const char *(var) = args[(idx)].as.string;

#define GET_INT(idx, var) \
    if (args[(idx)].type != VAL_INT) MQTT_ERR("expected int argument"); \
    long (var) = args[(idx)].as.integer;

#define GET_CLIENT(idx) \
    MqttClient *client = mqtt_unwrap(&args[(idx)], err, had_error, line, fn_name); \
    if (!client) return mqtt_nil();

    /* mqtt.connect(host, port, client_id) → dyn */
    if (strcmp(fn_name, "connect") == 0) {
        NEED(3);
        GET_STR(0, host); GET_INT(1, port); GET_STR(2, client_id);

        mosquitto_lib_init();
        struct mosquitto *mosq = mosquitto_new(client_id, true, NULL);
        if (!mosq) MQTT_ERR("failed to create mosquitto client");

        int rc = mosquitto_connect(mosq, host, (int)port, 60);
        if (rc != MOSQ_ERR_SUCCESS) {
            snprintf(errbuf, sizeof(errbuf),
                "mqtt.connect: %s", mosquitto_strerror(rc));
            mosquitto_destroy(mosq);
            errstack_push(err, ERR_FLUXA, errbuf, "mqtt", line);
            *had_error = 1;
            return mqtt_nil();
        }

        MqttClient *c = (MqttClient *)malloc(sizeof(MqttClient));
        c->mosq      = mosq;
        c->connected = 1;
        return mqtt_wrap(c);
    }

    /* mqtt.connect_auth(host, port, client_id, username, password) → dyn */
    if (strcmp(fn_name, "connect_auth") == 0) {
        NEED(5);
        GET_STR(0, host); GET_INT(1, port); GET_STR(2, client_id);
        GET_STR(3, username); GET_STR(4, password);

        mosquitto_lib_init();
        struct mosquitto *mosq = mosquitto_new(client_id, true, NULL);
        if (!mosq) MQTT_ERR("failed to create mosquitto client");

        mosquitto_username_pw_set(mosq, username, password);
        int rc = mosquitto_connect(mosq, host, (int)port, 60);
        if (rc != MOSQ_ERR_SUCCESS) {
            snprintf(errbuf, sizeof(errbuf),
                "mqtt.connect_auth: %s", mosquitto_strerror(rc));
            mosquitto_destroy(mosq);
            errstack_push(err, ERR_FLUXA, errbuf, "mqtt", line);
            *had_error = 1;
            return mqtt_nil();
        }

        MqttClient *c = (MqttClient *)malloc(sizeof(MqttClient));
        c->mosq      = mosq;
        c->connected = 1;
        return mqtt_wrap(c);
    }

    /* mqtt.disconnect(cursor) → nil */
    if (strcmp(fn_name, "disconnect") == 0) {
        NEED(1); GET_CLIENT(0);
        if (client->connected && client->mosq) {
            mosquitto_disconnect(client->mosq);
            mosquitto_destroy(client->mosq);
            client->mosq      = NULL;
            client->connected = 0;
        }
        free(client);
        if (args[0].type == VAL_DYN && args[0].as.dyn &&
            args[0].as.dyn->count >= 1)
            args[0].as.dyn->items[0].as.ptr = NULL;
        return mqtt_nil();
    }

    /* mqtt.publish(cursor, topic, payload) → nil */
    if (strcmp(fn_name, "publish") == 0) {
        NEED(3); GET_CLIENT(0);
        GET_STR(1, topic); GET_STR(2, payload);
        if (!client->connected || !client->mosq)
            MQTT_ERR("not connected — call mqtt.connect() first");
        int rc = mosquitto_publish(client->mosq, NULL, topic,
                                   (int)strlen(payload), payload, 0, false);
        if (rc != MOSQ_ERR_SUCCESS) {
            snprintf(errbuf, sizeof(errbuf),
                "mqtt.publish: %s", mosquitto_strerror(rc));
            MQTT_ERR(errbuf);
        }
        return mqtt_nil();
    }

    /* mqtt.publish_qos(cursor, topic, payload, qos) → nil */
    if (strcmp(fn_name, "publish_qos") == 0) {
        NEED(4); GET_CLIENT(0);
        GET_STR(1, topic); GET_STR(2, payload); GET_INT(3, qos);
        if (!client->connected || !client->mosq)
            MQTT_ERR("not connected");
        if (qos < 0 || qos > 2) MQTT_ERR("qos must be 0, 1, or 2");
        int rc = mosquitto_publish(client->mosq, NULL, topic,
                                   (int)strlen(payload), payload,
                                   (int)qos, false);
        if (rc != MOSQ_ERR_SUCCESS) {
            snprintf(errbuf, sizeof(errbuf),
                "mqtt.publish_qos: %s", mosquitto_strerror(rc));
            MQTT_ERR(errbuf);
        }
        return mqtt_nil();
    }

    /* mqtt.subscribe(cursor, topic) → nil */
    if (strcmp(fn_name, "subscribe") == 0) {
        NEED(2); GET_CLIENT(0); GET_STR(1, topic);
        if (!client->connected || !client->mosq)
            MQTT_ERR("not connected");
        int rc = mosquitto_subscribe(client->mosq, NULL, topic, 0);
        if (rc != MOSQ_ERR_SUCCESS) {
            snprintf(errbuf, sizeof(errbuf),
                "mqtt.subscribe: %s", mosquitto_strerror(rc));
            MQTT_ERR(errbuf);
        }
        return mqtt_nil();
    }

    /* mqtt.subscribe_qos(cursor, topic, qos) → nil */
    if (strcmp(fn_name, "subscribe_qos") == 0) {
        NEED(3); GET_CLIENT(0); GET_STR(1, topic); GET_INT(2, qos);
        if (!client->connected || !client->mosq)
            MQTT_ERR("not connected");
        if (qos < 0 || qos > 2) MQTT_ERR("qos must be 0, 1, or 2");
        int rc = mosquitto_subscribe(client->mosq, NULL, topic, (int)qos);
        if (rc != MOSQ_ERR_SUCCESS) {
            snprintf(errbuf, sizeof(errbuf),
                "mqtt.subscribe_qos: %s", mosquitto_strerror(rc));
            MQTT_ERR(errbuf);
        }
        return mqtt_nil();
    }

    /* mqtt.loop(cursor, timeout_ms) → nil  (process network events) */
    if (strcmp(fn_name, "loop") == 0) {
        NEED(2); GET_CLIENT(0); GET_INT(1, timeout_ms);
        if (!client->connected || !client->mosq)
            MQTT_ERR("not connected");
        int rc = mosquitto_loop(client->mosq, (int)timeout_ms, 1);
        if (rc == MOSQ_ERR_CONN_LOST) {
            client->connected = 0;
            MQTT_ERR("connection lost");
        }
        if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_NO_CONN) {
            snprintf(errbuf, sizeof(errbuf),
                "mqtt.loop: %s", mosquitto_strerror(rc));
            MQTT_ERR(errbuf);
        }
        return mqtt_nil();
    }

    /* mqtt.connected(cursor) → bool */
    if (strcmp(fn_name, "connected") == 0) {
        NEED(1); GET_CLIENT(0);
        return mqtt_bool(client->connected && client->mosq != NULL);
    }

#undef MQTT_ERR
#undef NEED
#undef GET_STR
#undef GET_INT
#undef GET_CLIENT

    snprintf(errbuf, sizeof(errbuf), "mqtt.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "mqtt", line);
    *had_error = 1;
    return mqtt_nil();
}

/* ── Lib descriptor ──────────────────────────────────────────────── */
FLUXA_LIB_EXPORT(
    name      = "mqtt",
    toml_key  = "std.mqtt",
    owner     = "mqtt",
    call      = fluxa_std_mqtt_call,
    rt_aware  = 0,
    cfg_aware = 0
)

#endif /* FLUXA_STD_MQTT_H */
