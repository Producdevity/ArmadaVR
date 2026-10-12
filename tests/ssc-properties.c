#include "libssc-sensor.c"

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    SSCSensor *sensor = g_object_new(SSC_TYPE_SENSOR, NULL);
    SSCSensorPrivate *priv = ssc_sensor_get_instance_private(sensor);
    const char *names[] = {SSC_SENSOR_STREAM_TYPE, SSC_SENSOR_AVAILABLE, SSC_SENSOR_SAMPLE_RATE};
#ifdef EXPECT_OLD
    const char *errors[] = {"*G_VALUE_HOLDS_UINT*", "*G_VALUE_HOLDS_BOOLEAN*", "*G_VALUE_HOLDS_FLOAT*"};
    for (unsigned i = 0; i < G_N_ELEMENTS(names); i++) {
        GValue value = G_VALUE_INIT;
        g_test_expect_message("GLib-GObject", G_LOG_LEVEL_CRITICAL, errors[i]);
        g_object_get_property(G_OBJECT(sensor), names[i], &value);
        g_test_assert_expected_messages();
        g_assert_true(G_VALUE_HOLDS_STRING(&value));
        g_assert_null(g_value_get_string(&value));
        g_value_unset(&value);
    }
    g_print("SSC_PROPERTIES_BASELINE_FAILURES 3\n");
#else
    const GType types[] = {G_TYPE_UINT, G_TYPE_BOOLEAN, G_TYPE_FLOAT};
    for (unsigned i = 0; i < G_N_ELEMENTS(names); i++) {
        GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(sensor), names[i]);
        g_assert_nonnull(spec);
        g_assert_cmpuint(G_PARAM_SPEC_VALUE_TYPE(spec), ==, types[i]);
        g_assert_true(spec->flags & G_PARAM_READABLE);
        g_assert_false(spec->flags & G_PARAM_WRITABLE);
    }
    const float rates[] = {0.0f, 1.0f, 72.0f, 480.0f, 1024.0f, G_MAXFLOAT};
    for (unsigned cycle = 0; cycle < 1024; cycle++) {
        priv->stream_type = cycle % 3;
        priv->available = cycle % 2;
        priv->sample_rate = rates[cycle % G_N_ELEMENTS(rates)];
        guint type = 99;
        gboolean available = FALSE;
        gfloat rate = -1.0f;
        g_object_get(sensor, SSC_SENSOR_STREAM_TYPE, &type, SSC_SENSOR_AVAILABLE, &available,
                     SSC_SENSOR_SAMPLE_RATE, &rate, NULL);
        g_assert_cmpuint(type, ==, priv->stream_type);
        g_assert_cmpint(available, ==, priv->available);
        g_assert_cmpfloat(rate, ==, priv->sample_rate);
        for (unsigned i = 0; i < G_N_ELEMENTS(names); i++) {
            GValue value = G_VALUE_INIT;
            g_object_get_property(G_OBJECT(sensor), names[i], &value);
            g_assert_cmpuint(G_VALUE_TYPE(&value), ==, types[i]);
            if (i == 0) g_assert_cmpuint(g_value_get_uint(&value), ==, type);
            if (i == 1) g_assert_cmpint(g_value_get_boolean(&value), ==, available);
            if (i == 2) g_assert_cmpfloat(g_value_get_float(&value), ==, rate);
            g_value_unset(&value);
        }
    }
    g_print("SSC_PROPERTIES_PASS cycles=1024\n");
#endif
    (void)priv;
    g_object_unref(sensor);
    return 0;
}
