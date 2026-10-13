/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "libssc-client-private.h"
#include "ssc-sensor-suid.pb-c.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_SENSORS 64
#define MAX_VALUES 4096

struct exchange {
    SSCClient *client;
    GCancellable *cancel;
    guint64 high, low;
    guint response;
    gboolean acknowledged, expired;
    GError *error;
    GBytes *payload;
};

static void received(SSCClient *client, guint id, guint64 high, guint64 low,
                     guint64 ticks, GBytes *payload, gpointer data)
{
    (void)client; (void)ticks;
    struct exchange *x = data;
    if (id == x->response && high == x->high && low == x->low && !x->payload)
        x->payload = g_bytes_ref(payload);
}

static void acknowledged(GObject *object, GAsyncResult *result, gpointer data)
{
    struct exchange *x = data;
    ssc_client_send_finish(SSC_CLIENT(object), result, &x->error);
    x->acknowledged = TRUE;
}

static gboolean expired(gpointer data)
{
    struct exchange *x = data;
    x->expired = TRUE;
    g_cancellable_cancel(x->cancel);
    return G_SOURCE_REMOVE;
}

static GBytes *exchange(SSCClient *client, guint64 high, guint64 low, guint request,
                        guint response, const ProtobufCMessage *message)
{
    struct exchange x = {.client = client, .cancel = g_cancellable_new(),
                         .high = high, .low = low, .response = response};
    GArray *body = g_array_sized_new(FALSE, FALSE, 1, protobuf_c_message_get_packed_size(message));
    g_array_set_size(body, protobuf_c_message_get_packed_size(message));
    protobuf_c_message_pack(message, (guint8 *)body->data);
    gulong handler = g_signal_connect(client, "report-timestamped", G_CALLBACK(received), &x);
    guint timeout = g_timeout_add(3000, expired, &x);
    ssc_client_send(client, high, low, request, body, x.cancel, acknowledged, &x);
    g_array_unref(body);
    while (!x.acknowledged || (!x.payload && !x.error && !x.expired))
        g_main_context_iteration(NULL, TRUE);
    g_signal_handler_disconnect(client, handler);
    if (!x.expired) g_source_remove(timeout);
    if (x.error || x.expired) {
        g_printerr("SSC metadata request failed: %s\n", x.error ? x.error->message : "report timeout");
        g_clear_pointer(&x.payload, g_bytes_unref);
    }
    g_clear_error(&x.error);
    g_object_unref(x.cancel);
    return x.payload;
}

static gboolean json_string(GString *out, const char *text)
{
    if (!text || strlen(text) > 4096 || !g_utf8_validate(text, -1, NULL)) return FALSE;
    g_string_append_c(out, '"');
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < 32) g_string_append_printf(out, "\\u%04x", *p);
        else {
            if (*p == '"' || *p == '\\') g_string_append_c(out, '\\');
            g_string_append_c(out, *p);
        }
    }
    g_string_append_c(out, '"');
    return TRUE;
}

static gboolean json_value(GString *out, const SscAttrValue *v, unsigned depth, unsigned *count)
{
    if (!v || depth > 8 || ++*count > MAX_VALUES ||
        (!!v->a + !!v->s + !!v->has_f + !!v->has_i + !!v->has_b) != 1) return FALSE;
    if (v->s) return json_string(out, v->s);
    if (v->has_f) {
        if (!isfinite(v->f)) return FALSE;
        char number[G_ASCII_DTOSTR_BUF_SIZE];
        g_string_append(out, g_ascii_dtostr(number, sizeof(number), v->f));
    } else if (v->has_i) {
        /* Keep fixed64 values exact in JSON consumers with 53-bit numbers. */
        g_string_append_printf(out, "{\"fixed64\":\"%" G_GUINT64_FORMAT "\"}", v->i);
    } else if (v->has_b) g_string_append(out, v->b ? "true" : "false");
    else {
        if (v->a->n_element > MAX_VALUES) return FALSE;
        g_string_append_c(out, '[');
        for (size_t i = 0; i < v->a->n_element; i++) {
            if (i) g_string_append_c(out, ',');
            if (!json_value(out, v->a->element[i], depth + 1, count)) return FALSE;
        }
        g_string_append_c(out, ']');
    }
    return TRUE;
}

static gboolean attributes(GString *out, GBytes *payload)
{
    gsize length;
    const guint8 *bytes = g_bytes_get_data(payload, &length);
    if (!length || length > 65536) return FALSE;
    SscAttrResponse *reply = ssc_attr_response__unpack(NULL, length, bytes);
    gboolean valid = reply && reply->n_attr && reply->n_attr <= 128;
    unsigned count = 0;
    g_string_append_c(out, '[');
    for (size_t i = 0; valid && i < reply->n_attr; i++) {
        const SscAttr *a = reply->attr[i];
        valid = a->id >= 0 && a->value_array->n_v <= MAX_VALUES;
        for (size_t j = 0; j < i; j++) valid &= a->id != reply->attr[j]->id;
        if (i) g_string_append_c(out, ',');
        g_string_append_printf(out, "{\"id\":%d,\"values\":[", a->id);
        for (size_t j = 0; valid && j < a->value_array->n_v; j++) {
            if (j) g_string_append_c(out, ',');
            valid = json_value(out, a->value_array->v[j], 0, &count);
        }
        g_string_append(out, "]}");
    }
    g_string_append_c(out, ']');
    if (reply) ssc_attr_response__free_unpacked(reply, NULL);
    return valid;
}

static SscSuidResponse *identities(GBytes *payload, const char *type)
{
    gsize length;
    const guint8 *bytes = g_bytes_get_data(payload, &length);
    if (!length || length > 65536) return NULL;
    SscSuidResponse *reply = ssc_suid_response__unpack(NULL, length, bytes);
    gboolean valid = reply && !strcmp(type, reply->data_type) && reply->n_uid <= MAX_SENSORS;
    for (size_t i = 0; valid && i < reply->n_uid; i++) {
        const SscUid *u = reply->uid[i];
        valid = (u->high || u->low) &&
                !(u->high == SSC_SENSOR_UID_SUID_HIGH && u->low == SSC_SENSOR_UID_SUID_LOW);
        for (size_t j = 0; j < i; j++)
            valid &= u->high != reply->uid[j]->high || u->low != reply->uid[j]->low;
    }
    if (!valid && reply) { ssc_suid_response__free_unpacked(reply, NULL); reply = NULL; }
    return reply;
}

static SscSuidResponse *discover(SSCClient *client, const char *type)
{
    SscSuidRequest request = SSC_SUID_REQUEST__INIT;
    request.data_type = (char *)type;
    request.has_enable_updates = request.has_only_default_values = TRUE;
    request.enable_updates = request.only_default_values = FALSE;
    GBytes *payload = exchange(client, SSC_SENSOR_UID_SUID_HIGH, SSC_SENSOR_UID_SUID_LOW,
                               SSC_MSG_REQUEST_SUID, SSC_MSG_RESPONSE_SUID, &request.base);
    if (!payload) return NULL;
    SscSuidResponse *reply = identities(payload, type);
    g_bytes_unref(payload);
    return reply;
}

static void connected(GObject *object, GAsyncResult *result, gpointer data)
{
    (void)object;
    struct exchange *x = data;
    x->client = ssc_client_new_finish(result, &x->error);
    x->acknowledged = TRUE;
}

static gboolean platform_permitted(const char *board, size_t size, const char *line)
{
    gboolean pico = FALSE, virt = FALSE, qrtr = FALSE, test = FALSE;
    unsigned pico_keys = 0, test_keys = 0;
    for (size_t pos = 0; pos < size;) {
        size_t n = strnlen(board + pos, size - pos);
        if (n == size - pos) return FALSE;
        pico |= !strcmp(board + pos, "qcom,kona");
        virt |= !strcmp(board + pos, "linux,dummy-virt");
        pos += n + 1;
    }
    gchar **tokens = g_strsplit_set(line, " \t\r\n", -1);
    for (size_t i = 0; tokens[i]; i++) {
        pico_keys += g_str_has_prefix(tokens[i], "reframed.pico=");
        test_keys += g_str_has_prefix(tokens[i], "reframed.ssc_test=");
        qrtr |= !strcmp(tokens[i], "reframed.pico=qrtr");
        test |= !strcmp(tokens[i], "reframed.ssc_test=1");
    }
    g_strfreev(tokens);
    return (pico && qrtr && !virt && pico_keys == 1 && !test_keys) ||
           (virt && test && !pico && test_keys == 1 && !pico_keys);
}

static gboolean permitted(void)
{
    gchar *board = NULL, *line = NULL;
    gsize size = 0;
    gboolean result = g_file_get_contents("/proc/device-tree/compatible", &board, &size, NULL) &&
                      g_file_get_contents("/proc/cmdline", &line, NULL, NULL) &&
                      platform_permitted(board, size, line);
    g_free(board); g_free(line);
    return result;
}

int main(int argc, char **argv)
{
    const char *types[] = {"accel", "gyro", "accel_cal", "gyro_cal", "ble_mcu", "ndi_fpga"};
    gboolean known = FALSE;
    if (argc == 3 && !strcmp(argv[1], "--collect"))
        for (size_t i = 0; i < G_N_ELEMENTS(types); i++) known |= !strcmp(argv[2], types[i]);
    if (!known || !permitted()) {
        g_printerr("Requires --collect TYPE on an explicitly enabled Pico QRTR boot or isolated SSC VM\n");
        return 2;
    }
    alarm(60);
    struct exchange connection = {.cancel = g_cancellable_new()};
    guint timeout = g_timeout_add(3000, expired, &connection);
    ssc_client_new(connection.cancel, connected, &connection);
    while (!connection.acknowledged) g_main_context_iteration(NULL, TRUE);
    if (!connection.expired) g_source_remove(timeout);
    gboolean valid = connection.client && !connection.expired && !connection.error;
    GString *out = g_string_new("{\"schema\":1,\"data_type\":");
    json_string(out, argv[2]);
    g_string_append(out, ",\"sensors\":[");
    SscSuidResponse *registry = valid ? discover(connection.client, "registry") : NULL;
    valid &= registry && registry->n_uid > 0;
    if (registry) ssc_suid_response__free_unpacked(registry, NULL);
    SscSuidResponse *sensors = valid ? discover(connection.client, argv[2]) : NULL;
    valid &= sensors != NULL;
    for (size_t i = 0; valid && i < sensors->n_uid; i++) {
        const SscUid *uid = sensors->uid[i];
        SscAttrRequest request = SSC_ATTR_REQUEST__INIT;
        request.has_enable_updates = TRUE;
        request.enable_updates = FALSE;
        GBytes *payload = exchange(connection.client, uid->high, uid->low,
                                   SSC_MSG_REQUEST_GET_ATTRIBUTES, SSC_MSG_RESPONSE_GET_ATTRIBUTES, &request.base);
        if (i) g_string_append_c(out, ',');
        g_string_append_printf(out, "{\"suid_high\":\"%016" G_GINT64_MODIFIER "x\",\"suid_low\":\"%016" G_GINT64_MODIFIER "x\",\"attributes\":", uid->high, uid->low);
        valid = payload && attributes(out, payload);
        g_string_append_c(out, '}');
        if (payload) g_bytes_unref(payload);
    }
    if (sensors) ssc_suid_response__free_unpacked(sensors, NULL);
    g_string_append(out, "]}\n");
    g_clear_object(&connection.client);
    g_clear_error(&connection.error);
    g_object_unref(connection.cancel);
    if (valid) valid = fwrite(out->str, 1, out->len, stdout) == out->len && fflush(stdout) == 0;
    else g_printerr("SSC inventory incomplete; no sensor configuration was selected\n");
    g_string_free(out, TRUE);
    alarm(0);
    return valid ? 0 : 1;
}
