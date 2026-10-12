#define ssc_client_send capture_send
#define ssc_client_new_finish capture_client_finish
#include "libssc-sensor.c"
#undef ssc_client_send
#undef ssc_client_new_finish

static struct {
    SSCClient *client;
    GAsyncReadyCallback callback;
    gpointer data;
} request;
static unsigned completions;
static GError *completion_error;

void capture_send(SSCClient *client, guint64 high, guint64 low, guint32 id, GArray *payload,
                  GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
    (void)high;
    (void)low;
    (void)id;
    (void)payload;
    (void)cancellable;
    g_assert_null(request.callback);
    request.client = g_object_ref(client);
    request.callback = callback;
    request.data = data;
}

static void reply(gboolean success)
{
    SSCClient *client = request.client;
    gpointer data = request.data;
    GTask *task = g_task_new(client, NULL, NULL, NULL);
    if (success)
        g_task_return_boolean(task, TRUE);
    else
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "test transport failure");
    GAsyncReadyCallback callback = request.callback;
    memset(&request, 0, sizeof(request));
    callback(G_OBJECT(client), G_ASYNC_RESULT(task), data);
    g_object_unref(task);
    g_object_unref(client);
}

SSCClient *capture_client_finish(GAsyncResult *result, GError **error)
{
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void done(GObject *object, GAsyncResult *result, gpointer data)
{
    (void)data;
    g_assert_cmpuint(completions, ==, 0);
    ssc_sensor_open_finish(SSC_SENSOR(object), result, &completion_error);
    completions++;
}

static void drain(GMainContext *context)
{
    while (g_main_context_iteration(context, FALSE)) {}
}

static void measurement(SSCSensorPrivate *priv)
{
    GArray *data = g_array_new(FALSE, FALSE, 1);
    g_signal_emit_by_name(priv->client, "report", 1024u, priv->uid_high, priv->uid_low, data);
    g_array_unref(data);
}

static void suid(SSCSensorPrivate *priv, const char *name)
{
    SscSuidResponse response = SSC_SUID_RESPONSE__INIT;
    SscUid uid = SSC_UID__INIT;
    SscUid *uids[] = {&uid};
    uid.high = 1;
    uid.low = 2;
    response.data_type = (char *)name;
    response.n_uid = 1;
    response.uid = uids;
    GArray *data = g_array_new(FALSE, FALSE, 1);
    g_array_set_size(data, ssc_suid_response__get_packed_size(&response));
    ssc_suid_response__pack(&response, (uint8_t *)data->data);
    g_signal_emit_by_name(priv->client, "report", SSC_MSG_RESPONSE_SUID,
                          SSC_SENSOR_UID_SUID_HIGH, SSC_SENSOR_UID_SUID_LOW, data);
    g_array_unref(data);
}

static void busy_done(GObject *object, GAsyncResult *result, gpointer data)
{
    GError *error = NULL;
    g_assert_false(ssc_sensor_open_finish(SSC_SENSOR(object), result, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PENDING);
    g_clear_error(&error);
    *(gboolean *)data = TRUE;
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, ==, 2);
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    SSCSensor *sensor = g_object_new(SSC_TYPE_SENSOR, SSC_SENSOR_DATA_TYPE, "ble_mcu", NULL);
    SSCSensorPrivate *priv = ssc_sensor_get_instance_private(sensor);
    priv->available = TRUE;
    priv->stream_type = SSC_STREAM_TYPE_CONTINUOUS;
    priv->sample_rate = 480;
    priv->uid_high = 1;
    priv->uid_low = 2;
    GCancellable *cancel = g_cancellable_new();
    gboolean discovery = g_str_has_prefix(argv[1], "registry-") || g_str_has_prefix(argv[1], "suid-") ||
                         g_str_has_prefix(argv[1], "attribute-");
    if (discovery) {
        GTask *task = g_task_new(sensor, cancel, done, NULL);
        GTask *client_task = g_task_new(NULL, NULL, NULL, NULL);
        g_task_return_pointer(client_task, g_object_new(SSC_TYPE_CLIENT, NULL), g_object_unref);
        client_ready(NULL, G_ASYNC_RESULT(client_task), task);
        g_object_unref(client_task);
        if (!g_str_has_prefix(argv[1], "registry-")) {
            reply(TRUE);
            suid(priv, "registry");
        }
        if (g_str_has_prefix(argv[1], "attribute-")) {
            reply(TRUE);
            suid(priv, "ble_mcu");
        }
    } else {
        priv->client = g_object_new(SSC_TYPE_CLIENT, NULL);
        ssc_sensor_open(sensor, cancel, done, NULL);
    }
    g_assert_nonnull(request.callback);
    if (g_str_has_suffix(argv[1], "error")) {
        reply(FALSE);
        drain(context);
        g_assert_cmpuint(completions, ==, 1);
        g_assert_error(completion_error, G_IO_ERROR, G_IO_ERROR_FAILED);
        measurement(priv);
    } else if (!strcmp(argv[1], "report-before-ack")) {
        measurement(priv);
        drain(context);
        g_assert_cmpuint(completions, ==, 1);
        g_assert_no_error(completion_error);
        reply(TRUE);
    } else if (g_str_has_suffix(argv[1], "cancel")) {
        g_cancellable_cancel(cancel);
        drain(context);
        g_assert_cmpuint(completions, ==, 1);
        g_assert_error(completion_error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        reply(FALSE);
        measurement(priv);
    } else if (g_str_has_suffix(argv[1], "timeout")) {
        g_assert_nonnull(priv->discovery_timeout);
        g_source_set_ready_time(priv->discovery_timeout, 0);
        drain(context);
        g_assert_cmpuint(completions, ==, 1);
        g_assert_nonnull(completion_error);
        reply(FALSE);
        measurement(priv);
    } else if (!strcmp(argv[1], "busy")) {
        gboolean refused = FALSE;
        ssc_sensor_open(sensor, cancel, busy_done, &refused);
        drain(context);
        g_assert_true(refused);
        g_assert_cmpuint(completions, ==, 0);
        reply(TRUE);
        measurement(priv);
    } else {
        g_assert_not_reached();
    }
    drain(context);
    g_assert_cmpuint(completions, ==, 1);
    g_assert_cmpuint(priv->report_id, ==, 0);
    g_assert_null(priv->discovery_timeout);
    g_clear_error(&completion_error);
    g_object_unref(cancel);
    g_object_unref(sensor);
    drain(context);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    g_print("SSC_OPERATION_PASS %s\n", argv[1]);
    return 0;
}
