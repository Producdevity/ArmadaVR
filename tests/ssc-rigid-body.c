#include "libssc-sensor.c"

#ifndef EXPECT_OLD
static void attributes(SSCSensor *sensor, int body, unsigned fault)
{
    SSCSensorPrivate *priv = ssc_sensor_get_instance_private(sensor);
    SscAttrValue value = SSC_ATTR_VALUE__INIT;
    value.has_i = TRUE; value.i = (guint64)body;
    SscAttrValue *values[] = {&value, &value};
    SscAttrArrayValue array = SSC_ATTR_ARRAY_VALUE__INIT;
    array.n_v = 1; array.v = values;
    SscAttr attribute = SSC_ATTR__INIT;
    attribute.id = 19; attribute.value_array = &array;
    SscAttr *list[] = {&attribute, &attribute};
    SscAttrResponse response = SSC_ATTR_RESPONSE__INIT;
    response.n_attr = 1; response.attr = list;
    if (fault == 1) response.n_attr = 0;
    if (fault == 2) response.n_attr = 2;
    if (fault == 3) array.n_v = 0;
    if (fault == 4) array.n_v = 2;
    if (fault == 5) value.has_i = FALSE;
    if (fault == 6) value.has_b = TRUE;
    if (fault == 7) value.has_f = TRUE;
    if (fault == 8) value.s = "0";
    SscAttrValueArray nested = SSC_ATTR_VALUE_ARRAY__INIT;
    if (fault == 9) value.a = &nested;
    GArray *bytes = g_array_sized_new(FALSE, FALSE, 1, ssc_attr_response__get_packed_size(&response));
    g_array_set_size(bytes, ssc_attr_response__get_packed_size(&response));
    ssc_attr_response__pack(&response, (guint8 *)bytes->data);
    ReportReceivedContext context = {.sensor = sensor};
    report_received(NULL, SSC_MSG_RESPONSE_GET_ATTRIBUTES, priv->uid_high,
                    priv->uid_low + (fault == 10), bytes, &context);
    g_array_unref(bytes);
}

static gint read_body(SSCSensor *sensor)
{
    gint body = 99;
    g_object_get(sensor, "rigid-body", &body, NULL);
    return body;
}
#endif

int main(void)
{
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
    SSCSensor *sensor = g_object_new(SSC_TYPE_SENSOR, SSC_SENSOR_DATA_TYPE, "accel", NULL);
    GParamSpec *property = g_object_class_find_property(G_OBJECT_GET_CLASS(sensor), "rigid-body");
#ifdef EXPECT_OLD
    g_assert_null(property);
    g_print("SSC_RIGID_BODY_BASELINE_MISSING\n");
#else
    g_assert_nonnull(property);
    g_assert_cmpuint(G_PARAM_SPEC_VALUE_TYPE(property), ==, G_TYPE_INT);
    g_assert_true(property->flags & G_PARAM_READABLE);
    g_assert_false(property->flags & G_PARAM_WRITABLE);
    g_assert_cmpint(read_body(sensor), ==, -1);
    SSCSensorPrivate *priv = ssc_sensor_get_instance_private(sensor);
    priv->uid_high = 1; priv->uid_low = 2;
    for (unsigned cycle = 0; cycle < 128; cycle++) {
        for (gint body = 0; body <= 2; body++) {
            attributes(sensor, body, 0);
            g_assert_cmpint(read_body(sensor), ==, body);
            attributes(sensor, (body + 1) % 3, 10);
            g_assert_cmpint(read_body(sensor), ==, body);
            for (unsigned fault = 1; fault <= 9; fault++) {
                attributes(sensor, body, 0);
                attributes(sensor, body, fault);
                g_assert_cmpint(read_body(sensor), ==, -1);
            }
        }
        attributes(sensor, -1, 0); g_assert_cmpint(read_body(sensor), ==, -1);
        attributes(sensor, 3, 0); g_assert_cmpint(read_body(sensor), ==, -1);
        attributes(sensor, G_MAXINT, 0); g_assert_cmpint(read_body(sensor), ==, -1);
    }
    g_print("SSC_RIGID_BODY_PASS cycles=128\n");
#endif
    g_object_unref(sensor);
    return 0;
}
