#include "ssc-imu-session.h"

#include <libssc-sensor.h>
#include <math.h>

struct channel {
    struct ssc_imu_session *session;
    SSCSensor *sensor;
    GObject *client;
    unsigned index;
    int64_t last_report_ns;
    bool pending, attempted, closed, released;
};

struct ssc_imu_session {
    struct ssc_imu_session_config config;
    struct ssc_imu_session_status status;
    struct ssc_imu_stream stream;
    struct channel channels[2];
    GMutex lock;
    GThread *thread;
    GMainContext *context;
    GCancellable *cancel;
    gint live_objects;
    int64_t started_ns;
    unsigned opened;
    bool configured, stopping;
};

static int64_t now_ns(void)
{
    return g_get_monotonic_time() * INT64_C(1000);
}

static void failure(struct ssc_imu_session *s, const char *message)
{
    g_mutex_lock(&s->lock);
    if (!s->status.failed) {
        s->status.failed = true;
        g_strlcpy(s->status.error, message, sizeof(s->status.error));
    }
    g_mutex_unlock(&s->lock);
}

static void stop_worker(struct ssc_imu_session *s, const char *error)
{
    if (s->stopping) return;
    s->stopping = true;
    ssc_imu_invalidate(&s->stream);
    if (error) failure(s, error);
    g_mutex_lock(&s->lock);
    s->status.streaming = false;
    s->status.stopping = true;
    g_mutex_unlock(&s->lock);
    s->config.invalidate(s->config.data);
    g_cancellable_cancel(s->cancel);
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
    struct ssc_imu_session *s = data;
    g_atomic_int_add(&s->live_objects, -1);
    g_main_context_wakeup(s->context);
}

static void watch(struct ssc_imu_session *s, GObject *object)
{
    if (!object) return;
    g_atomic_int_inc(&s->live_objects);
    g_object_weak_ref(object, finalized, s);
}

static void closed(GObject *object, GAsyncResult *result, gpointer data)
{
    struct channel *channel = data;
    GError *error = NULL;
    if (!ssc_sensor_close_finish(SSC_SENSOR(object), result, &error))
        failure(channel->session, error ? error->message : "IMU disable failed");
    g_clear_error(&error);
    channel->pending = false;
}

static void received(GObject *client, guint id, guint64 high, guint64 low,
                     guint64 ticks, GBytes *payload, gpointer data)
{
    struct channel *channel = data;
    struct ssc_imu_session *s = channel->session;
    unsigned index = channel->index;
    if (s->stopping || s->opened != 2 || g_cancellable_is_cancelled(s->cancel)) return;
    gsize length;
    const void *bytes = g_bytes_get_data(payload, &length);
    struct ssc_imu_event event;
    int64_t now = now_ns();
    bool accepted = client == channel->client && high == s->config.suid_high[index] &&
                    low == s->config.suid_low[index] &&
                    ssc_imu_report(&s->stream, s->stream.generation, id, high, low, ticks,
                                    bytes, length, now, &event);
    if (accepted) channel->last_report_ns = now;
    g_mutex_lock(&s->lock);
    if (accepted) s->status.accepted[index]++;
    else s->status.rejected[index]++;
    g_mutex_unlock(&s->lock);
    if (accepted) s->config.report(s->config.data, &event);
}

static void opened(GObject *object, GAsyncResult *result, gpointer data)
{
    struct channel *channel = data;
    struct ssc_imu_session *s = channel->session;
    GError *error = NULL;
    bool success = ssc_sensor_open_finish(SSC_SENSOR(object), result, &error);
    channel->pending = false;
    if (g_cancellable_is_cancelled(s->cancel)) stop_worker(s, NULL);
    else if (!success) stop_worker(s, error ? error->message : "IMU enable failed");
    else if (!s->stopping && ++s->opened == 2) {
        s->channels[0].last_report_ns = s->channels[1].last_report_ns = now_ns();
        g_mutex_lock(&s->lock);
        s->status.streaming = true;
        g_mutex_unlock(&s->lock);
    }
    g_clear_error(&error);
}

static bool metadata_matches(struct channel *channel)
{
    struct ssc_imu_session *s = channel->session;
    unsigned index = channel->index;
    const char *names[] = {SSC_SENSOR_STREAM_TYPE, SSC_SENSOR_AVAILABLE, SSC_SENSOR_SAMPLE_RATE};
    const GType types[] = {G_TYPE_UINT, G_TYPE_BOOLEAN, G_TYPE_FLOAT};
    for (unsigned i = 0; i < G_N_ELEMENTS(names); i++) {
        GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(channel->sensor), names[i]);
        if (!spec || G_PARAM_SPEC_VALUE_TYPE(spec) != types[i]) return false;
    }
    guint type;
    guint64 high, low;
    gboolean available;
    gfloat rate;
    g_object_get(channel->sensor, SSC_SENSOR_UID_HIGH, &high, SSC_SENSOR_UID_LOW, &low,
                 SSC_SENSOR_STREAM_TYPE, &type, SSC_SENSOR_AVAILABLE, &available,
                 SSC_SENSOR_SAMPLE_RATE, &rate, NULL);
    return channel->client && high == s->config.suid_high[index] && low == s->config.suid_low[index] &&
           type == 0 && available && isfinite(rate) && rate == s->config.sample_rate[index];
}

static void discovered(GObject *object, GAsyncResult *result, gpointer data)
{
    struct channel *channel = data;
    struct ssc_imu_session *s = channel->session;
    GError *error = NULL;
    watch(s, object);
    g_object_get(object, SSC_SENSOR_CLIENT, &channel->client, NULL);
    watch(s, channel->client);
    channel->sensor = ssc_sensor_new_finish(result, &error);
    channel->pending = false;
    if (g_cancellable_is_cancelled(s->cancel)) stop_worker(s, NULL);
    else if (!channel->sensor) stop_worker(s, error ? error->message : "IMU discovery failed");
    else if (!metadata_matches(channel)) stop_worker(s, "IMU identity or metadata differs");
    g_clear_error(&error);
    if (s->stopping || s->configured || s->channels[0].pending || s->channels[1].pending) return;
    s->configured = true;
    struct ssc_imu_config config = {.max_age_ns = s->config.max_age_ns};
    for (unsigned i = 0; i < 2; i++) {
        config.suid_high[i] = s->config.suid_high[i];
        config.suid_low[i] = s->config.suid_low[i];
    }
    bool calibrated = s->config.calibrate(s->config.data, &config.clock);
    if (g_cancellable_is_cancelled(s->cancel)) {
        stop_worker(s, NULL);
        return;
    }
    if (!calibrated || !ssc_imu_begin(&s->stream, &config)) {
        stop_worker(s, "Invalid IMU clock calibration");
        return;
    }
    for (unsigned i = 0; i < 2; i++) {
        struct channel *c = &s->channels[i];
        g_signal_connect(c->client, "report-timestamped", G_CALLBACK(received), c);
        c->pending = c->attempted = true;
        ssc_sensor_open(c->sensor, s->cancel, opened, c);
    }
}

static gboolean watchdog(gpointer data)
{
    struct ssc_imu_session *s = data;
    int64_t now = now_ns();
    if (s->stopping) return G_SOURCE_CONTINUE;
    if (now < s->started_ns || (s->opened != 2 && now - s->started_ns >= s->config.startup_timeout_ns))
        stop_worker(s, "IMU startup deadline exceeded");
    else if (s->opened == 2) {
        for (unsigned i = 0; i < 2; i++)
            if (now < s->channels[i].last_report_ns ||
                now - s->channels[i].last_report_ns >= s->config.report_timeout_ns)
                stop_worker(s, i == 0 ? "Accelerometer report stream stopped" : "Gyroscope report stream stopped");
    }
    return G_SOURCE_CONTINUE;
}

static gpointer worker(gpointer data)
{
    struct ssc_imu_session *s = data;
    g_main_context_push_thread_default(s->context);
    GSource *cancel = g_cancellable_source_new(s->cancel);
    g_source_set_callback(cancel, G_SOURCE_FUNC(cancelled), s, NULL);
    g_source_attach(cancel, s->context);
    int64_t timeout = MIN(s->config.report_timeout_ns, s->config.startup_timeout_ns);
    GSource *timer = g_timeout_source_new((guint)MIN(MAX(timeout / 4000000, 1), 100));
    g_source_set_callback(timer, watchdog, s, NULL);
    g_source_attach(timer, s->context);
    s->started_ns = now_ns();
    if (g_cancellable_is_cancelled(s->cancel)) stop_worker(s, NULL);
    else {
        s->channels[0].pending = s->channels[1].pending = true;
        ssc_sensor_new("accel", s->cancel, discovered, &s->channels[0]);
        ssc_sensor_new("gyro", s->cancel, discovered, &s->channels[1]);
    }
    for (;;) {
        if (s->stopping) {
            for (unsigned i = 0; i < 2; i++) {
                struct channel *c = &s->channels[i];
                if (c->pending) continue;
                if (c->attempted && !c->closed) {
                    c->pending = c->closed = true;
                    ssc_sensor_close(c->sensor, NULL, closed, c);
                } else if (!c->released) {
                    c->released = true;
                    if (c->client) g_signal_handlers_disconnect_by_data(c->client, c);
                    g_clear_object(&c->sensor);
                    g_clear_object(&c->client);
                }
            }
        }
        if (s->channels[0].released && s->channels[1].released && !g_atomic_int_get(&s->live_objects)) break;
        g_main_context_iteration(s->context, TRUE);
    }
    g_source_destroy(timer); g_source_unref(timer);
    g_source_destroy(cancel); g_source_unref(cancel);
    g_main_context_pop_thread_default(s->context);
    g_mutex_lock(&s->lock);
    s->status.finished = true;
    g_mutex_unlock(&s->lock);
    return NULL;
}

bool ssc_imu_session_create(const struct ssc_imu_session_config *config, struct ssc_imu_session **output)
{
    if (!config || !output || *output || !config->calibrate || !config->report || !config->invalidate ||
        config->max_age_ns < 0 || config->startup_timeout_ns <= 0 || config->report_timeout_ns <= 0 ||
        config->max_age_ns > config->report_timeout_ns ||
        (config->suid_high[0] == config->suid_high[1] && config->suid_low[0] == config->suid_low[1])) return false;
    for (unsigned i = 0; i < 2; i++)
        if ((!config->suid_high[i] && !config->suid_low[i]) || !isfinite(config->sample_rate[i]) ||
            config->sample_rate[i] <= 0) return false;
    struct ssc_imu_session *s = g_new0(struct ssc_imu_session, 1);
    s->config = *config;
    g_mutex_init(&s->lock);
    s->context = g_main_context_new();
    s->cancel = g_cancellable_new();
    for (unsigned i = 0; i < 2; i++) s->channels[i] = (struct channel){.session = s, .index = i};
    s->thread = g_thread_try_new("ssc-imu", worker, s, NULL);
    if (!s->thread) {
        ssc_imu_session_destroy(&s);
        return false;
    }
    *output = s;
    return true;
}

void ssc_imu_session_get_status(struct ssc_imu_session *s, struct ssc_imu_session_status *status)
{
    if (!s || !status) return;
    g_mutex_lock(&s->lock);
    *status = s->status;
    g_mutex_unlock(&s->lock);
}

void ssc_imu_session_stop(struct ssc_imu_session *s)
{
    if (s) g_cancellable_cancel(s->cancel);
}

bool ssc_imu_session_destroy(struct ssc_imu_session **session)
{
    if (!session || !*session) return true;
    struct ssc_imu_session *s = *session;
    if (s->thread && g_thread_self() == s->thread) return false;
    ssc_imu_session_stop(s);
    if (s->thread) g_thread_join(s->thread);
    g_object_unref(s->cancel);
    g_main_context_unref(s->context);
    g_mutex_clear(&s->lock);
    g_free(s);
    *session = NULL;
    return true;
}
