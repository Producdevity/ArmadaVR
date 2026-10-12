#include <glib.h>
#include <string.h>

#ifdef TEST_CLIENT
#include "libssc-client.c"
#else
#include "libssc-sensor.c"
#endif

static void finalized(gpointer data, GObject *object)
{
    (void)object;
    *(gboolean *)data = TRUE;
}

int main(void)
{
    gboolean freed = FALSE;
#ifdef TEST_CLIENT
    SSCClient *client = g_object_new(SSC_TYPE_CLIENT, NULL);
    SSCClientPrivate *priv = ssc_client_get_instance_private(client);
    priv->bus = g_object_new(QRTR_TYPE_BUS, NULL);
    g_object_weak_ref(G_OBJECT(priv->bus), finalized, &freed);
    g_object_run_dispose(G_OBJECT(client));
    g_assert_true(freed);
    g_assert_null(priv->bus);
    g_object_run_dispose(G_OBJECT(client));
    g_object_unref(client);
    g_print("SSC_LIFETIME_PASS client-partial-init\n");
#else
    SSCSensor *sensor = g_object_new(SSC_TYPE_SENSOR, SSC_SENSOR_DATA_TYPE, "ble_mcu", NULL);
    SSCSensorPrivate *priv = ssc_sensor_get_instance_private(sensor);
    priv->name = g_strdup("Controller");
    priv->vendor = g_strdup("test");
    g_object_weak_ref(G_OBJECT(sensor), finalized, &freed);
    g_object_run_dispose(G_OBJECT(sensor));
    g_object_run_dispose(G_OBJECT(sensor));
    g_assert_null(priv->name);
    g_assert_null(priv->vendor);
    g_assert_null(priv->data_type);
    g_object_unref(sensor);
    g_assert_true(freed);
    g_print("SSC_LIFETIME_PASS sensor-repeated-dispose\n");
#endif
    return 0;
}
