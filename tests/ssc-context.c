#include "libssc-common-private.h"

#ifdef TEST_SENSOR
#include "libssc-sensor.c"
#endif

static gboolean foreign_context(gpointer data)
{
    (void)data;
    g_error("SSC_CONTEXT_WRONG_DEFAULT_DISPATCH");
    return G_SOURCE_REMOVE;
}

static gboolean complete(gpointer data)
{
    GTask *task = data;
    g_assert_true(g_main_context_is_owner(g_task_get_context(task)));
    g_task_return_boolean(task, TRUE);
    return G_SOURCE_REMOVE;
}

#ifdef TEST_SENSOR
static void timeout_done(GObject *object, GAsyncResult *result, gpointer data)
{
    GError *error = NULL;
    (void)object;
    g_assert_false(g_task_propagate_boolean(G_TASK(result), &error));
    g_assert_error(error, ssc_sensor_error_quark(), SSC_SENSOR_ERROR_NO_SERVICE);
    g_clear_error(&error);
    (*(unsigned *)data)++;
}
#endif

int main(void)
{
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    GSource *foreign = g_idle_source_new();
    g_source_set_callback(foreign, foreign_context, NULL, NULL);
    g_source_attach(foreign, g_main_context_default());
    for (unsigned index = 0; index < 128; index++) {
        SyncContext sync;
        ssc_common_init_sync_context(&sync);
        GTask *task = g_task_new(NULL, NULL, ssc_common_callback_sync_context, &sync);
        GSource *source = g_idle_source_new();
        g_source_set_callback(source, complete, task, NULL);
        g_source_attach(source, context);
        ssc_common_wait_sync_context(&sync);
        g_assert_true(g_task_propagate_boolean(G_TASK(sync.result), NULL));
        ssc_common_clear_sync_context(&sync);
        g_source_unref(source);
        g_object_unref(task);
    }
#ifndef EXPECT_OLD
    SyncContext sync;
    ssc_common_init_sync_context(&sync);
    GTask *task = g_task_new(NULL, NULL, ssc_common_callback_sync_context, &sync);
    GSource *source = ssc_common_timeout_source(0, complete, task);
    g_assert_true(g_source_get_context(source) == context);
    ssc_common_wait_sync_context(&sync);
    g_assert_true(g_source_is_destroyed(source));
    ssc_common_clear_source(source);
    ssc_common_clear_sync_context(&sync);
    g_object_unref(task);
#endif
#ifdef TEST_SENSOR
    SSCSensor *sensor = g_object_new(SSC_TYPE_SENSOR, SSC_SENSOR_DATA_TYPE, "ble_mcu", NULL);
    SSCSensorPrivate *priv = ssc_sensor_get_instance_private(sensor);
    unsigned finished = 0;
    priv->client = g_object_new(SSC_TYPE_CLIENT, NULL);
    operation_new(sensor, g_task_new(sensor, NULL, timeout_done, &finished));
    g_source_set_ready_time(priv->discovery_timeout, 0);
    while (!finished)
        g_main_context_iteration(context, TRUE);
    g_assert_null(priv->discovery_timeout);
    g_assert_cmpuint(finished, ==, 1);
    priv->discovery_timeout = ssc_common_timeout_source(0, foreign_context, NULL);
    source = g_source_ref(priv->discovery_timeout);
    g_object_run_dispose(G_OBJECT(sensor));
    g_assert_true(g_source_is_destroyed(source));
    g_assert_null(priv->discovery_timeout);
    g_source_unref(source);
    g_object_unref(sensor);
#endif
    g_source_destroy(foreign);
    g_source_unref(foreign);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    g_print("SSC_CONTEXT_PASS private-context cycles=128\n");
    return 0;
}
