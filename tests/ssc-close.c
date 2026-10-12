#define ssc_client_send capture_send
#include "libssc-sensor.c"
#undef ssc_client_send

static unsigned completed;
static gboolean fail_request;

struct request {
    GTask *task;
    GAsyncReadyCallback callback;
    gpointer data;
};

static gboolean reply(gpointer data)
{
    struct request *request = data;
    if (fail_request)
        g_task_return_new_error(request->task, G_IO_ERROR, G_IO_ERROR_CLOSED, "SSC server disappeared");
    else
        g_task_return_boolean(request->task, TRUE);
    request->callback(g_task_get_source_object(request->task), G_ASYNC_RESULT(request->task), request->data);
    g_object_unref(request->task);
    g_free(request);
    return G_SOURCE_REMOVE;
}

void capture_send(SSCClient *client, guint64 high, guint64 low, guint32 id, GArray *payload,
                  GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
    (void)high; (void)low; (void)id; (void)payload;
    struct request *request = g_new0(struct request, 1);
    *request = (struct request){g_task_new(client, cancellable, NULL, NULL), callback, data};
    GSource *source = g_timeout_source_new(1);
    g_source_set_callback(source, reply, request, NULL);
    g_source_attach(source, g_main_context_get_thread_default());
    g_source_unref(source);
}

static void done(GObject *object, GAsyncResult *result, gpointer data)
{
    (void)data;
    GError *error = NULL;
    g_assert_cmpint(ssc_sensor_close_finish(SSC_SENSOR(object), result, &error), ==, !fail_request);
    if (fail_request)
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED);
    else
        g_assert_no_error(error);
    g_clear_error(&error);
    completed++;
}

int main(void)
{
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    SSCSensor *sensor = g_object_new(SSC_TYPE_SENSOR, NULL);
    SSCSensorPrivate *priv = ssc_sensor_get_instance_private(sensor);
    priv->client = g_object_new(SSC_TYPE_CLIENT, NULL);
    for (unsigned i = 0; i < 100; i++) {
        fail_request = i % 2 == 0;
        ssc_sensor_close(sensor, NULL, done, NULL);
        while (completed != i + 1)
            g_main_context_iteration(context, TRUE);
    }
    g_object_unref(sensor);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    g_print("SSC_CLOSE_PASS cycles=100\n");
    return 0;
}
