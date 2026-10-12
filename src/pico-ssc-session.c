#include "pico-ssc-session.h"

#include <libssc-sensor.h>
#include <os/os_time.h>
#include <string.h>

struct pico_ssc_session {
    struct pico_ssc_session_config config;
    struct pico_analog_calibration calibration[2];
    struct pico_monado_producer *producer;
    struct pico_controller_stream stream;
    GMutex lock;
    struct pico_ssc_session_status status;
    GThread *thread;
    GMainContext *context;
    GCancellable *cancel;
    SSCSensor *sensor;
    GObject *client;
    unsigned live_objects;
    int64_t last_report_ns;
    bool pending, stopping, enable_attempted, close_attempted, released;
};

static void stop_worker(struct pico_ssc_session *session, const char *error)
{
    if (session->stopping)
        return;
    session->stopping = true;
    pico_controller_stream_invalidate(&session->stream);
    pico_monado_producer_disconnect(session->producer);
    g_mutex_lock(&session->lock);
    session->status.streaming = false;
    session->status.stopping = true;
    session->status.failed = error != NULL;
    if (error)
        g_strlcpy(session->status.error, error, sizeof(session->status.error));
    g_mutex_unlock(&session->lock);
    g_cancellable_cancel(session->cancel);
}

static gboolean cancelled(GCancellable *cancel, gpointer data)
{
    (void)cancel;
    stop_worker(data, NULL);
    return G_SOURCE_REMOVE;
}

static void finalized(gpointer data, GObject *object)
{
    (void)object;
    struct pico_ssc_session *session = data;
    session->live_objects--;
    g_main_context_wakeup(session->context);
}

static void watch(struct pico_ssc_session *session, GObject *object)
{
    if (object) {
        session->live_objects++;
        g_object_weak_ref(object, finalized, session);
    }
}

static void closed(GObject *object, GAsyncResult *result, gpointer data)
{
    struct pico_ssc_session *session = data;
    GError *error = NULL;
    if (!ssc_sensor_close_finish(SSC_SENSOR(object), result, &error)) {
        g_mutex_lock(&session->lock);
        if (!session->status.failed) {
            session->status.failed = true;
            g_strlcpy(session->status.error, error->message, sizeof(session->status.error));
        }
        g_mutex_unlock(&session->lock);
    }
    g_clear_error(&error);
    session->pending = false;
}

static void report(GObject *client, guint id, guint64 high, guint64 low,
                   guint64 timestamp, GBytes *payload, gpointer data)
{
    (void)client;
    struct pico_ssc_session *session = data;
    if (session->stopping || g_cancellable_is_cancelled(session->cancel))
        return;
    gsize length;
    const void *bytes = g_bytes_get_data(payload, &length);
    struct pico_controller_event event;
    int64_t now = (int64_t)os_monotonic_get_ns();
    bool accepted = pico_controller_stream_report(&session->stream, session->stream.generation,
                                                   id, high, low, timestamp, bytes, length, now, &event) &&
                    pico_monado_producer_push(session->producer, &event);
    if (accepted)
        session->last_report_ns = now;
    g_mutex_lock(&session->lock);
    if (accepted)
        session->status.accepted++;
    else
        session->status.rejected++;
    g_mutex_unlock(&session->lock);
}

static void opened(GObject *object, GAsyncResult *result, gpointer data)
{
    struct pico_ssc_session *session = data;
    GError *error = NULL;
    bool success = ssc_sensor_open_finish(SSC_SENSOR(object), result, &error);
    session->pending = false;
    if (g_cancellable_is_cancelled(session->cancel))
        stop_worker(session, NULL);
    else if (!success)
        stop_worker(session, error->message);
    else {
        g_mutex_lock(&session->lock);
        session->status.streaming = true;
        g_mutex_unlock(&session->lock);
    }
    g_clear_error(&error);
}

static void discovered(GObject *object, GAsyncResult *result, gpointer data)
{
    struct pico_ssc_session *session = data;
    GError *error = NULL;
    watch(session, object);
    g_object_get(object, SSC_SENSOR_CLIENT, &session->client, NULL);
    watch(session, session->client);
    session->sensor = ssc_sensor_new_finish(result, &error);
    session->pending = false;
    if (g_cancellable_is_cancelled(session->cancel))
        stop_worker(session, NULL);
    else if (!session->sensor)
        stop_worker(session, error->message);
    g_clear_error(&error);
    if (session->stopping)
        return;

    struct pico_controller_stream_config config = {
        .max_age_ns = session->config.max_age_ns,
        .mcu_type = session->config.mcu_type, .radio_type = session->config.radio_type,
        .touch_supported = session->config.touch_supported,
    };
    g_object_get(session->sensor, SSC_SENSOR_UID_HIGH, &config.suid_high,
                 SSC_SENSOR_UID_LOW, &config.suid_low, NULL);
    if (!session->client || !session->config.calibrate(session->config.clock_data, &config.clock) ||
        !pico_controller_stream_begin(&session->stream, &config) ||
        !pico_monado_producer_begin(session->producer, session->stream.generation,
                                    config.touch_supported, session->config.left, session->config.right)) {
        stop_worker(session, "Invalid SSC clock or controller configuration");
        return;
    }
    if (g_cancellable_is_cancelled(session->cancel)) {
        stop_worker(session, NULL);
        return;
    }
    session->last_report_ns = (int64_t)os_monotonic_get_ns();
    g_signal_connect(session->client, "report-timestamped", G_CALLBACK(report), session);
    session->pending = session->enable_attempted = true;
    ssc_sensor_open(session->sensor, session->cancel, opened, session);
}

static gboolean watchdog(gpointer data)
{
    struct pico_ssc_session *session = data;
    int64_t now = (int64_t)os_monotonic_get_ns();
    if (!session->stopping && session->enable_attempted &&
        (now < session->last_report_ns || now - session->last_report_ns >= session->config.timeout_ns))
        stop_worker(session, "Controller report stream stopped");
    return G_SOURCE_CONTINUE;
}

static gpointer worker(gpointer data)
{
    struct pico_ssc_session *session = data;
    g_main_context_push_thread_default(session->context);
    GSource *cancel = g_cancellable_source_new(session->cancel);
    g_source_set_callback(cancel, G_SOURCE_FUNC(cancelled), session, NULL);
    g_source_attach(cancel, session->context);
    guint interval_ms = (guint)MIN(MAX(session->config.timeout_ns / 4000000, 1), 100);
    GSource *timer = g_timeout_source_new(interval_ms);
    g_source_set_callback(timer, watchdog, session, NULL);
    g_source_attach(timer, session->context);
    if (g_cancellable_is_cancelled(session->cancel))
        stop_worker(session, NULL);
    else {
        session->pending = true;
        ssc_sensor_new("ble_mcu", session->cancel, discovered, session);
    }
    for (;;) {
        if (session->stopping && !session->pending) {
            if (session->enable_attempted && !session->close_attempted) {
                session->pending = session->close_attempted = true;
                ssc_sensor_close(session->sensor, NULL, closed, session);
            } else if (!session->released) {
                session->released = true;
                if (session->client)
                    g_signal_handlers_disconnect_by_data(session->client, session);
                g_clear_object(&session->sensor);
                g_clear_object(&session->client);
            }
        }
        if (session->released && !session->live_objects)
            break;
        g_main_context_iteration(session->context, TRUE);
    }
    g_source_destroy(timer);
    g_source_unref(timer);
    g_source_destroy(cancel);
    g_source_unref(cancel);
    g_main_context_pop_thread_default(session->context);
    g_mutex_lock(&session->lock);
    session->status.finished = true;
    g_mutex_unlock(&session->lock);
    return NULL;
}

bool pico_ssc_session_create(const struct pico_ssc_session_config *config,
                             struct pico_ssc_session **session_out,
                             struct xrt_device **left, struct xrt_device **right)
{
    if (!config || !config->calibrate || config->max_age_ns < 0 || config->timeout_ns <= 0 ||
        config->max_age_ns > config->timeout_ns || config->mcu_type < 1 || config->mcu_type > 2 ||
        config->radio_type < 1 || config->radio_type > 2 || !session_out || *session_out ||
        !left || !right || left == right || *left || *right)
        return false;
    struct pico_monado_input check;
    if (!pico_monado_input_begin(&check, 1, 1, config->touch_supported, config->left) ||
        !pico_monado_input_begin(&check, 2, 1, config->touch_supported, config->right))
        return false;
    struct pico_ssc_session *session = g_new0(struct pico_ssc_session, 1);
    session->config = *config;
    if (config->left) {
        session->calibration[0] = *config->left;
        session->config.left = &session->calibration[0];
    }
    if (config->right) {
        session->calibration[1] = *config->right;
        session->config.right = &session->calibration[1];
    }
    g_mutex_init(&session->lock);
    session->context = g_main_context_new();
    session->cancel = g_cancellable_new();
    struct xrt_device *devices[2] = {NULL, NULL};
    if (pico_monado_devices_create(config->timeout_ns, &session->producer, &devices[0], &devices[1]))
        session->thread = g_thread_try_new("pico-ssc", worker, session, NULL);
    if (!session->thread) {
        pico_ssc_session_destroy(&session);
        xrt_device_destroy(&devices[0]);
        xrt_device_destroy(&devices[1]);
        return false;
    }
    *session_out = session;
    *left = devices[0];
    *right = devices[1];
    return true;
}

void pico_ssc_session_get_status(struct pico_ssc_session *session,
                                 struct pico_ssc_session_status *status)
{
    if (!session || !status)
        return;
    g_mutex_lock(&session->lock);
    *status = session->status;
    g_mutex_unlock(&session->lock);
}

void pico_ssc_session_stop(struct pico_ssc_session *session)
{
    if (session)
        g_cancellable_cancel(session->cancel);
}

void pico_ssc_session_destroy(struct pico_ssc_session **session)
{
    if (!session || !*session)
        return;
    struct pico_ssc_session *old = *session;
    pico_ssc_session_stop(old);
    if (old->thread)
        g_thread_join(old->thread);
    pico_monado_producer_destroy(&old->producer);
    g_object_unref(old->cancel);
    g_main_context_unref(old->context);
    g_mutex_clear(&old->lock);
    g_free(old);
    *session = NULL;
}
