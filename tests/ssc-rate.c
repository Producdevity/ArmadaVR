#define ssc_client_send capture_send
#include "libssc-sensor.c"
#undef ssc_client_send

#include <math.h>

static SSCClient *pending_client;
static GAsyncReadyCallback pending_callback;
static gpointer pending_data;
static unsigned sends, completions;
static gfloat sent_rate;
static gboolean succeeded;
static GError *completion_error;

void capture_send(SSCClient *client, guint64 high, guint64 low, guint32 id, GArray *payload,
                  GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
    (void)cancel;
    g_assert_null(pending_callback);
    g_assert_cmpuint(high, ==, 1); g_assert_cmpuint(low, ==, 2);
    g_assert_cmpuint(id, ==, SSC_MSG_REQUEST_ENABLE_REPORT_CONTINUOUS);
    SscEnableConfigRequest *request = ssc_enable_config_request__unpack(NULL, payload->len, (const guint8 *)payload->data);
    g_assert_nonnull(request);
    sent_rate = request->sample_rate;
    ssc_enable_config_request__free_unpacked(request, NULL);
    sends++;
    pending_client = g_object_ref(client);
    pending_callback = callback;
    pending_data = data;
}

static void completed(GObject *object, GAsyncResult *result, gpointer data)
{
    (void)data;
    g_assert_cmpuint(completions, ==, 0);
    succeeded = ssc_sensor_open_finish(SSC_SENSOR(object), result, &completion_error);
    completions++;
}

static void attributes(SSCSensor *sensor, unsigned fault)
{
    SSCSensorPrivate *priv = ssc_sensor_get_instance_private(sensor);
    SscAttrValue values[3]; SscAttrValue *pointers[3];
    const float rates[] = {5.0f, 1000.0f, 2000.0f};
    for (unsigned i = 0; i < 3; i++) {
        ssc_attr_value__init(&values[i]); values[i].has_f = TRUE; values[i].f = rates[i];
        pointers[i] = &values[i];
    }
    SscAttrArrayValue array = SSC_ATTR_ARRAY_VALUE__INIT;
    array.n_v = 3; array.v = pointers;
    SscAttr attr = SSC_ATTR__INIT;
    attr.id = SSC_ATTRIBUTE_SAMPLE_RATE; attr.value_array = &array;
    SscAttr *attrs[] = {&attr, &attr};
    SscAttrResponse response = SSC_ATTR_RESPONSE__INIT;
    response.n_attr = 1; response.attr = attrs;
    if (fault == 1) { array.n_v = 1; array.v = &pointers[1]; }
    if (fault == 2) values[1].f = 999.0f;
    if (fault == 3 || fault == 13) response.n_attr = 0;
    if (fault == 4) array.n_v = 0;
    if (fault == 5) response.n_attr = 2;
    if (fault == 6) values[1].has_f = FALSE;
    if (fault == 7) values[0].f = NAN;
    if (fault == 8) values[0].f = INFINITY;
    if (fault == 9) values[0].f = -1.0f;
    if (fault == 10) values[0].f = 0.0f;
    if (fault == 11) values[1].has_i = TRUE;
    if (fault == 15) values[0].f = 1000.0f;
    GArray *bytes = g_array_sized_new(FALSE, FALSE, 1, ssc_attr_response__get_packed_size(&response));
    g_array_set_size(bytes, ssc_attr_response__get_packed_size(&response));
    ssc_attr_response__pack(&response, (guint8 *)bytes->data);
    ReportReceivedContext context = {.sensor = sensor};
    report_received(NULL, SSC_MSG_RESPONSE_GET_ATTRIBUTES, priv->uid_high,
                    priv->uid_low + (fault == 12), bytes, &context);
    g_array_unref(bytes);
}

int main(void)
{
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    unsigned cases = 0;
    for (unsigned cycle = 0; cycle < 128; cycle++) {
#ifdef EXPECT_OLD
        unsigned count = 1;
#else
        unsigned count = 16;
#endif
        for (unsigned mode = 0; mode < count; mode++) {
            sends = completions = 0; succeeded = FALSE; sent_rate = 0;
#ifdef EXPECT_OLD
            SSCSensor *sensor = g_object_new(SSC_TYPE_SENSOR, SSC_SENSOR_DATA_TYPE, "accel", NULL);
            gfloat expected = 5.0f;
#else
            SSCSensor *sensor = g_object_new(SSC_TYPE_SENSOR, SSC_SENSOR_DATA_TYPE, "accel",
                "requested-sample-rate", mode == 14 ? 0.0 : 1000.0, NULL);
            GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(sensor), "requested-sample-rate");
            g_assert_nonnull(spec);
            g_assert_cmpuint(G_PARAM_SPEC_VALUE_TYPE(spec), ==, G_TYPE_FLOAT);
            g_assert_true(spec->flags & G_PARAM_CONSTRUCT_ONLY);
            g_assert_true(spec->flags & G_PARAM_WRITABLE);
            gfloat expected = mode == 14 ? 5.0f : (mode < 2 || mode == 12 || mode == 15) ? 1000.0f : 0.0f;
#endif
            SSCSensorPrivate *priv = ssc_sensor_get_instance_private(sensor);
            priv->client = g_object_new(SSC_TYPE_CLIENT, NULL);
            priv->uid_high = 1; priv->uid_low = 2;
            priv->available = TRUE; priv->stream_type = SSC_STREAM_TYPE_CONTINUOUS;
            attributes(sensor, 0);
            attributes(sensor, mode);
            gfloat rate = -1.0f;
            g_object_get(sensor, SSC_SENSOR_SAMPLE_RATE, &rate, NULL);
            g_assert_cmpfloat(rate, ==, expected);
            ssc_sensor_open(sensor, NULL, completed, NULL);
            g_assert_cmpuint(sends, ==, expected > 0);
            if (sends) {
                g_assert_cmpfloat(sent_rate, ==, expected);
                GTask *task = g_task_new(pending_client, NULL, NULL, NULL);
                g_task_return_boolean(task, TRUE);
                pending_callback(G_OBJECT(pending_client), G_ASYNC_RESULT(task), pending_data);
                g_object_unref(task); g_clear_object(&pending_client); pending_callback = NULL;
                operation_finish(priv->operation, TRUE, NULL);
            }
            while (g_main_context_iteration(context, FALSE)) {}
            g_assert_cmpuint(completions, ==, 1);
            g_assert_cmpint(succeeded, ==, expected > 0);
            if (expected > 0) g_assert_no_error(completion_error);
            else g_assert_error(completion_error, ssc_sensor_error_quark(), SSC_SENSOR_ERROR_SAMPLE_RATE_UNAVAILABLE);
            g_clear_error(&completion_error);
            g_assert_null(priv->operation);
            g_object_unref(sensor);
            while (g_main_context_iteration(context, FALSE)) {}
            cases++;
        }
    }
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
#ifdef EXPECT_OLD
    g_print("SSC_RATE_BASELINE_FIRST_ONLY cases=%u\n", cases);
#else
    g_print("SSC_RATE_PASS cases=%u\n", cases);
#endif
    return 0;
}
