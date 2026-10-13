#define ssc_client_send capture_send
#include "libssc-sensor.c"
#undef ssc_client_send

static SSCClient *pending_client;
static GAsyncReadyCallback pending_callback;
static gpointer pending_data;
static unsigned requests, attributes, completions;
static guint64 selected_high, selected_low;
static gboolean default_only, succeeded;
static GError *completion_error;

void capture_send(SSCClient *client, guint64 high, guint64 low, guint32 id, GArray *payload,
                  GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
    (void)cancel;
    g_assert_null(pending_callback);
    if (id == SSC_MSG_REQUEST_SUID) {
        SscSuidRequest *request = ssc_suid_request__unpack(NULL, payload->len, (const guint8 *)payload->data);
        g_assert_nonnull(request);
        g_assert_cmpstr(request->data_type, ==, "accel");
        g_assert_true(request->has_only_default_values);
        g_assert_true(request->has_enable_updates);
        g_assert_false(request->enable_updates);
        default_only = request->only_default_values;
        requests++;
        ssc_suid_request__free_unpacked(request, NULL);
    } else {
        g_assert_cmpuint(id, ==, SSC_MSG_REQUEST_GET_ATTRIBUTES);
        attributes++;
        selected_high = high; selected_low = low;
    }
    pending_client = g_object_ref(client);
    pending_callback = callback;
    pending_data = data;
}

static void acknowledge(void)
{
    GTask *task = g_task_new(pending_client, NULL, NULL, NULL);
    g_task_return_boolean(task, TRUE);
    pending_callback(G_OBJECT(pending_client), G_ASYNC_RESULT(task), pending_data);
    g_object_unref(task);
    g_clear_object(&pending_client);
    pending_callback = NULL;
}

static void completed(GObject *object, GAsyncResult *result, gpointer data)
{
    (void)object; (void)data;
    g_assert_cmpuint(completions, ==, 0);
    succeeded = g_task_propagate_boolean(G_TASK(result), &completion_error);
    completions++;
}

static void emit(SSCSensorPrivate *priv, SscSuidResponse *response)
{
    GArray *payload = g_array_sized_new(FALSE, FALSE, 1, ssc_suid_response__get_packed_size(response));
    g_array_set_size(payload, ssc_suid_response__get_packed_size(response));
    ssc_suid_response__pack(response, (guint8 *)payload->data);
    g_signal_emit_by_name(priv->client, "report", SSC_MSG_RESPONSE_SUID,
                         SSC_SENSOR_UID_SUID_HIGH, SSC_SENSOR_UID_SUID_LOW, payload);
    g_array_unref(payload);
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
        unsigned count = 8;
#endif
        for (unsigned mode = 0; mode < count; mode++) {
            requests = attributes = completions = 0;
            selected_high = selected_low = 0;
            succeeded = FALSE;
            guint64 wanted_high = mode == 5 ? 0 : G_MAXUINT64;
            guint64 wanted_low = mode == 6 ? 0 : G_MAXUINT64 - 1;
#ifdef EXPECT_OLD
            SSCSensor *sensor = g_object_new(SSC_TYPE_SENSOR, SSC_SENSOR_DATA_TYPE, "accel", NULL);
#else
            SSCSensor *sensor = g_object_new(SSC_TYPE_SENSOR, SSC_SENSOR_DATA_TYPE, "accel",
                "requested-uid-high", mode == 7 ? (guint64)0 : wanted_high,
                "requested-uid-low", mode == 7 ? (guint64)0 : wanted_low, NULL);
            const char *names[] = {"requested-uid-high", "requested-uid-low"};
            for (unsigned i = 0; i < G_N_ELEMENTS(names); i++) {
                GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(sensor), names[i]);
                g_assert_nonnull(spec);
                g_assert_cmpuint(G_PARAM_SPEC_VALUE_TYPE(spec), ==, G_TYPE_UINT64);
                g_assert_true(spec->flags & G_PARAM_CONSTRUCT_ONLY);
                g_assert_true(spec->flags & G_PARAM_WRITABLE);
            }
#endif
            SSCSensorPrivate *priv = ssc_sensor_get_instance_private(sensor);
            priv->client = g_object_new(SSC_TYPE_CLIENT, NULL);
            priv->service_available = TRUE;
            GTask *task = g_task_new(sensor, NULL, completed, NULL);
            ReportReceivedContext *operation = operation_new(sensor, task);
            discover(sensor, operation);
            g_assert_cmpuint(requests, ==, 1);
#ifdef EXPECT_OLD
            g_assert_true(default_only);
#else
            g_assert_cmpint(default_only, ==, mode == 7);
#endif
            acknowledge();
            SscUid values[32];
            SscUid *pointers[32];
            for (unsigned i = 0; i < G_N_ELEMENTS(values); i++) {
                ssc_uid__init(&values[i]); values[i].high = i + 1; values[i].low = i + 2;
                pointers[i] = &values[i];
            }
            unsigned selected = mode == 1 ? 0 : 31;
            if (mode != 2) { values[selected].high = wanted_high; values[selected].low = wanted_low; }
            if (mode == 3) { values[30].high = wanted_high; values[30].low = wanted_low; }
            SscSuidResponse response = SSC_SUID_RESPONSE__INIT;
            response.data_type = "gyro"; response.n_uid = mode == 4 ? 0 : 32; response.uid = pointers;
            emit(priv, &response);
            g_assert_cmpuint(attributes, ==, 0);
            g_assert_nonnull(priv->operation);
            response.data_type = "accel";
            emit(priv, &response);
#ifdef EXPECT_OLD
            g_assert_cmpuint(attributes, ==, 1);
            g_assert_cmpuint(selected_high, ==, 1);
            g_assert_cmpuint(selected_low, ==, 2);
#else
            gboolean found = mode < 2 || mode >= 5;
            g_assert_cmpuint(attributes, ==, found);
            if (found) {
                g_assert_cmpuint(selected_high, ==, mode == 7 ? 1 : wanted_high);
                g_assert_cmpuint(selected_low, ==, mode == 7 ? 2 : wanted_low);
            }
#endif
            if (attributes) {
                acknowledge();
                operation_finish(priv->operation, TRUE, NULL);
            }
            while (g_main_context_iteration(context, FALSE)) {}
            g_assert_cmpuint(completions, ==, 1);
            g_assert_cmpint(succeeded, ==, attributes != 0);
            if (attributes) g_assert_no_error(completion_error);
            else g_assert_error(completion_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
            g_clear_error(&completion_error);
            g_assert_null(priv->operation);
            g_assert_null(priv->discovery_timeout);
            g_assert_cmpuint(priv->report_id, ==, 0);
            g_object_unref(sensor);
            while (g_main_context_iteration(context, FALSE)) {}
            cases++;
        }
    }
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
#ifdef EXPECT_OLD
    g_print("SSC_SELECTION_BASELINE_DEFAULT_ONLY cases=%u\n", cases);
#else
    g_print("SSC_SELECTION_PASS cases=%u\n", cases);
#endif
    return 0;
}
