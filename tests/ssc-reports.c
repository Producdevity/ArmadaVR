#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libssc-client.c"
#include "pico-controller.h"

GType ssc_sensor_get_type(void)
{
    return G_TYPE_OBJECT;
}

static const guint64 high = UINT64_C(0xfedcba9876543210);
static const guint64 low = UINT64_C(0x0123456789abcdef);
static const guint64 ticks[] = {0, UINT64_C(0x0123456789abcdef), UINT64_MAX};
static const guint32 ids[] = {1024, 1024, 128};
static GBytes *retained[3];
static unsigned legacy_count, timed_count;
static guint8 payload[104];

static void legacy_report(SSCClient *client, guint id, guint64 uid_high,
                          guint64 uid_low, GArray *data, gpointer unused)
{
    g_assert_nonnull(client);
    g_assert_cmpuint(legacy_count, <, 3);
    g_assert_cmpuint(id, ==, ids[legacy_count]);
    g_assert_cmpuint(uid_high, ==, high);
    g_assert_cmpuint(uid_low, ==, low);
    g_assert_cmpmem(data->data, data->len, payload, sizeof(payload));
    memset(data->data, 0x66, data->len);
    legacy_count++;
}

#ifndef EXPECT_OLD
static void timed_report(SSCClient *client, guint id, guint64 uid_high,
                         guint64 uid_low, guint64 timestamp, GBytes *data,
                         gpointer unused)
{
    g_assert_nonnull(client);
    g_assert_cmpuint(timed_count, <, 3);
    g_assert_cmpuint(id, ==, ids[timed_count]);
    g_assert_cmpuint(uid_high, ==, high);
    g_assert_cmpuint(uid_low, ==, low);
    g_assert_cmpuint(timestamp, ==, ticks[timed_count]);
    gsize length;
    const void *bytes = g_bytes_get_data(data, &length);
    g_assert_cmpmem(bytes, length, payload, sizeof(payload));
    retained[timed_count++] = g_bytes_ref(data);
}
#endif

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    const char *record =
        "a5011fb068b2e3bff317f3f217dd1beb514b0fa2f6d41558960c3021ab435effd07d87a8d36ae1f4a0a339a913880e260239"
        "dff4c8a96dcaa94b5653a49b475e5d1c55fb80c669e503b856c1006759a4867df1cfc1a13b3f00fe8c03e29e7b9f8e9a5c1a";
    payload[0] = 0x0a; payload[1] = 100; payload[102] = 0x10; payload[103] = 3;
    for (unsigned i = 0; i < 100; i++) {
        unsigned value;
        g_assert_cmpint(sscanf(record + 2 * i, "%2x", &value), ==, 1);
        payload[i + 2] = (guint8)value;
    }
    SSCClient *client = g_object_new(SSC_TYPE_CLIENT, NULL);
    g_signal_connect(client, "report", G_CALLBACK(legacy_report), NULL);
#ifdef EXPECT_OLD
    g_assert_cmpuint(g_signal_lookup("report-timestamped", SSC_TYPE_CLIENT), ==, 0);
#else
    g_signal_connect(client, "report-timestamped", G_CALLBACK(timed_report), NULL);
#endif
    SscUid uid = SSC_UID__INIT;
    uid.high = high; uid.low = low;
    SscClientResponse message = SSC_CLIENT_RESPONSE__INIT;
    SscClientResponseBody bodies[3], *pointers[3];
    for (unsigned i = 0; i < 3; i++) {
        ssc_client_response_body__init(&bodies[i]);
        bodies[i].msg_id = ids[i]; bodies[i].timestamp = ticks[i];
        bodies[i].msg.data = payload; bodies[i].msg.len = sizeof(payload);
        pointers[i] = &bodies[i];
    }
    message.uid = &uid; message.n_response = 3; message.response = pointers;
    gsize length = ssc_client_response__get_packed_size(&message);
    GArray *wire = g_array_sized_new(FALSE, FALSE, 1, length);
    g_array_set_size(wire, length);
    g_assert_cmpuint(ssc_client_response__pack(&message, (guint8 *)wire->data), ==, length);
    const char *wire_output = g_getenv("SSC_TEST_WIRE_OUTPUT");
    if (wire_output) g_assert_true(g_file_set_contents(wire_output, wire->data, wire->len, NULL));
    handle_report(client, wire);
    memset(wire->data, 0x77, wire->len);
    g_array_free(wire, TRUE);
    g_assert_cmpuint(legacy_count, ==, 3);

    GArray *malformed = g_array_new(FALSE, FALSE, 1);
    const guint8 missing_fields[] = {0x0a, 0};
    g_array_append_vals(malformed, missing_fields, sizeof(missing_fields));
    g_test_expect_message("libssc", G_LOG_LEVEL_WARNING, "Failed to unpack message");
    handle_report(client, malformed);
    g_test_assert_expected_messages();
    g_array_free(malformed, TRUE);
    g_assert_cmpuint(legacy_count, ==, 3);
    g_object_unref(client);
#ifdef EXPECT_OLD
    g_assert_cmpuint(timed_count, ==, 0);
    g_assert_null(retained[0]);
    puts("3 reports delivered without timestamps");
#else
    g_assert_cmpuint(timed_count, ==, 3);
    for (unsigned i = 0; i < 3; i++) {
        gsize bytes;
        const void *data = g_bytes_get_data(retained[i], &bytes);
        g_assert_cmpmem(data, bytes, payload, sizeof(payload));
        struct pico_controller_sample sample;
        unsigned accuracy;
        g_assert_true(pico_controller_decode_ssc(data, bytes, 1, 1, FALSE, &sample, &accuracy));
        g_assert_cmpuint(sample.hand, ==, 2);
        g_assert_cmpuint(sample.trigger, ==, 0xb0);
        g_assert_cmpuint(accuracy, ==, 3);
        g_bytes_unref(retained[i]);
    }
    puts("3 timestamps and retained payloads verified; legacy reports unchanged");
#endif
    return 0;
}
