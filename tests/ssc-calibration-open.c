#define ssc_client_send capture_send
#include "libssc-sensor.c"
#undef ssc_client_send

static SSCClient *pending_client;
static GAsyncReadyCallback pending_callback;
static gpointer pending_data;
static unsigned completions;
static gboolean succeeded;
static GError *completion_error;

void capture_send(SSCClient *client, guint64 high, guint64 low, guint32 id, GArray *payload,
                  GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
    (void)cancel;
    g_assert_null(pending_callback);
    g_assert_cmpuint(high, ==, 1);
    g_assert_cmpuint(low, ==, 2);
    g_assert_cmpuint(id, ==, 514);
    g_assert_null(payload);
    pending_client = g_object_ref(client);
    pending_callback = callback;
    pending_data = data;
}

static void reply(gboolean success)
{
    GTask *task = g_task_new(pending_client, NULL, NULL, NULL);
    if (success) g_task_return_boolean(task, TRUE);
    else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "late transport failure");
    pending_callback(G_OBJECT(pending_client), G_ASYNC_RESULT(task), pending_data);
    g_object_unref(task);
    g_clear_object(&pending_client);
    pending_callback = NULL;
}

static void completed(GObject *object, GAsyncResult *result, gpointer data)
{
    (void)data;
    g_assert_cmpuint(completions, ==, 0);
    succeeded = ssc_sensor_open_finish(SSC_SENSOR(object), result, &completion_error);
    completions++;
}

static void drain(GMainContext *context)
{
    while (g_main_context_iteration(context, FALSE)) {}
}

static void emit(SSCSensorPrivate *priv, guint32 id, guint64 high, guint64 low)
{
    const guint8 event[] = {10, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 3};
    GArray *data = g_array_sized_new(FALSE, FALSE, 1, sizeof(event));
    g_array_append_vals(data, event, sizeof(event));
    g_signal_emit_by_name(priv->client, "report", id, high, low, data);
    g_array_unref(data);
}

int main(void)
{
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    const char *types[] = {"accel_cal", "gyro_cal", "unrelated"};
    unsigned pending = 0;
    for (unsigned cycle = 0; cycle < 128; cycle++) {
        for (unsigned type = 0; type < G_N_ELEMENTS(types); type++) {
            completions = 0; succeeded = FALSE;
            SSCSensor *sensor = g_object_new(SSC_TYPE_SENSOR, SSC_SENSOR_DATA_TYPE, types[type], NULL);
            SSCSensorPrivate *priv = ssc_sensor_get_instance_private(sensor);
            priv->client = g_object_new(SSC_TYPE_CLIENT, NULL);
            priv->available = TRUE;
            priv->stream_type = SSC_STREAM_TYPE_ON_CHANGE;
            priv->uid_high = 1; priv->uid_low = 2;
            GCancellable *cancel = g_cancellable_new();
            ssc_sensor_open(sensor, cancel, completed, NULL);
            g_assert_nonnull(pending_callback);
            emit(priv, 1022, 1, 3);
            emit(priv, 1234, 1, 2);
            drain(context);
            g_assert_cmpuint(completions, ==, 0);
            if (cycle & 1) reply(TRUE);
            emit(priv, 1022, 1, 2);
            drain(context);
            gboolean recognized = type < 2;
#ifdef EXPECT_OLD
            recognized = FALSE;
#endif
            if (recognized) {
                g_assert_cmpuint(completions, ==, 1);
                g_assert_true(succeeded);
                g_assert_no_error(completion_error);
            } else {
                g_assert_cmpuint(completions, ==, 0);
                g_assert_nonnull(priv->operation);
                pending += type < 2;
                g_cancellable_cancel(cancel);
                drain(context);
                g_assert_cmpuint(completions, ==, 1);
                g_assert_false(succeeded);
                g_assert_error(completion_error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
            }
            if (!(cycle & 1)) reply((cycle & 2) != 0);
            emit(priv, 1022, 1, 2);
            drain(context);
            g_assert_cmpuint(completions, ==, 1);
            g_assert_null(priv->operation);
            g_assert_cmpuint(priv->report_id, ==, 0);
            g_assert_null(priv->discovery_timeout);
            g_clear_error(&completion_error);
            g_object_unref(cancel); g_object_unref(sensor);
            drain(context);
        }
    }
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
#ifdef EXPECT_OLD
    g_assert_cmpuint(pending, ==, 256);
    g_print("SSC_CALIBRATION_OPEN_BASELINE_PENDING 256\n");
#else
    g_assert_cmpuint(pending, ==, 0);
    g_print("SSC_CALIBRATION_OPEN_PASS cycles=384\n");
#endif
    return 0;
}
