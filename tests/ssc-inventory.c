#define main collector_main
#include "../tools/ssc-inventory.c"
#undef main

static GBytes *packed(const ProtobufCMessage *message)
{
    size_t size = protobuf_c_message_get_packed_size(message);
    void *buffer = g_malloc(size);
    protobuf_c_message_pack(message, buffer);
    return g_bytes_new_take(buffer, size);
}

int main(void)
{
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
    const char pico[] = "qcom,kona", virt[] = "linux,dummy-virt";
    g_assert_true(platform_permitted(pico, sizeof(pico), "ro reframed.pico=qrtr\n"));
    g_assert_true(platform_permitted(virt, sizeof(virt), "reframed.ssc_test=1 ro"));
    g_assert_false(platform_permitted(pico, sizeof(pico) - 1, "reframed.pico=qrtr"));
    g_assert_false(platform_permitted(pico, sizeof(pico), "reframed.pico=qrtr reframed.pico=no"));
    g_assert_false(platform_permitted(pico, sizeof(pico), "reframed.pico=qrtr reframed.pico=qrtr"));
    g_assert_false(platform_permitted(pico, sizeof(pico), "reframed.pico=qrtr-extra"));
    g_assert_false(platform_permitted(pico, sizeof(pico), "reframed.ssc_test=1"));
    g_assert_false(platform_permitted(virt, sizeof(virt), "reframed.pico=qrtr"));
    g_assert_false(platform_permitted(virt, sizeof(virt), "reframed.ssc_test=1 reframed.ssc_test=0"));
    g_assert_false(platform_permitted(virt, sizeof(virt), "reframed.ssc_test=1 reframed.pico=qrtr"));
    for (unsigned cycle = 0; cycle < 100; cycle++) {
        SscUid uid = SSC_UID__INIT, other = SSC_UID__INIT;
        uid.high = 1; uid.low = UINT64_MAX;
        other.high = 2; other.low = 4;
        SscUid *uids[] = {&uid, &other};
        SscSuidResponse r = SSC_SUID_RESPONSE__INIT;
        r.data_type = "accel"; r.n_uid = 2; r.uid = uids;
        for (unsigned scenario = 0; scenario < 5; scenario++) {
            if (scenario == 1) other = uid;
            if (scenario == 2) { other.high = 0; other.low = 0; }
            if (scenario == 3) other.high = other.low = SSC_SENSOR_UID_SUID_HIGH;
            if (scenario == 4) r.data_type = "gyro";
            GBytes *wire = packed(&r.base);
            SscSuidResponse *decoded = identities(wire, "accel");
            g_assert_cmpint(decoded != NULL, ==, scenario == 0);
            if (decoded) ssc_suid_response__free_unpacked(decoded, NULL);
            g_bytes_unref(wire);
        }
        r.data_type = "accel"; r.n_uid = 0;
        GBytes *wire = packed(&r.base);
        SscSuidResponse *decoded = identities(wire, "accel");
        g_assert_nonnull(decoded); g_assert_cmpuint(decoded->n_uid, ==, 0);
        ssc_suid_response__free_unpacked(decoded, NULL); g_bytes_unref(wire);

        SscAttrValue value = SSC_ATTR_VALUE__INIT;
        SscAttrValue *values[] = {&value};
        SscAttrArrayValue array = SSC_ATTR_ARRAY_VALUE__INIT;
        array.n_v = 1; array.v = values;
        SscAttr a = SSC_ATTR__INIT;
        a.id = 6; a.value_array = &array;
        SscAttr *attrs[] = {&a, &a};
        SscAttrResponse response = SSC_ATTR_RESPONSE__INIT;
        response.n_attr = 1; response.attr = attrs;
        for (unsigned scenario = 0; scenario < 9; scenario++) {
            ssc_attr_value__init(&value);
            if (scenario == 0) value.s = "line\n\"\\\té";
            if (scenario == 1) { value.has_f = TRUE; value.f = 1023.9f; }
            if (scenario == 2) { value.has_i = TRUE; value.i = UINT64_MAX; }
            if (scenario == 3) { value.has_b = TRUE; value.b = FALSE; }
            if (scenario == 4) { value.has_f = TRUE; value.f = NAN; }
            if (scenario == 5) { value.has_f = value.has_i = TRUE; }
            if (scenario == 7) value.s = "\xff";
            if (scenario == 8) { value.has_i = TRUE; response.n_attr = 2; }
            wire = packed(&response.base);
            GString *out = g_string_new(NULL);
            g_assert_cmpint(attributes(out, wire), ==, scenario < 4);
            if (scenario < 4 && !cycle) puts(out->str);
            g_string_free(out, TRUE); g_bytes_unref(wire);
        }
        GString *out = g_string_new(NULL);
        unsigned count = 0;
        SscAttrValueArray nested = SSC_ATTR_VALUE_ARRAY__INIT;
        ssc_attr_value__init(&value); value.a = &nested;
        nested.n_element = 1; nested.element = values;
        g_assert_false(json_value(out, &value, 0, &count));
        nested.n_element = MAX_VALUES + 1;
        g_assert_false(json_value(out, &value, 0, &count));
        g_string_free(out, TRUE);
        wire = g_bytes_new_static("\xff", 1);
        g_assert_null(identities(wire, "accel"));
        out = g_string_new(NULL);
        g_assert_false(attributes(out, wire));
        g_string_free(out, TRUE); g_bytes_unref(wire);
    }
    puts("SSC_INVENTORY_UNIT_PASS cycles=100");
    return 0;
}
