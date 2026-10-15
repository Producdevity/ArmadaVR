#include "pico-hmd-config.h"
#include "ssc-clock-linux.h"
#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const cJSON *field(const cJSON *object, const char *name)
{
    if (!cJSON_IsObject(object)) return NULL;
    const cJSON *found = NULL;
    for (const cJSON *p = object->child; p; p = p->next) {
        if (!strcmp(p->string, name)) { if (found) return NULL; found = p; }
    }
    return found;
}

static bool number(const cJSON *v, double low, double high, double *out)
{
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < low || v->valuedouble > high) return false;
    *out = v->valuedouble; return true;
}

static bool integer(const cJSON *v, int64_t low, int64_t high, int64_t *out)
{
    double n;
    if (!number(v, (double)low, (double)high, &n) || floor(n) != n) return false;
    *out = (int64_t)n; return true;
}

static bool floats(const cJSON *array, unsigned count, float *out)
{
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != (int)count) return false;
    for (unsigned i = 0; i < count; i++) {
        double n;
        if (!number(cJSON_GetArrayItem(array, (int)i), -1000000, 1000000, &n)) return false;
        out[i] = (float)n;
    }
    return true;
}

static bool hex64(const cJSON *v, uint64_t *out)
{
    if (!cJSON_IsString(v) || strlen(v->valuestring) != 16) return false;
    uint64_t n = 0;
    for (unsigned i = 0; i < 16; i++) {
        char c = v->valuestring[i];
        unsigned digit = c >= '0' && c <= '9' ? (unsigned)(c-'0') :
                         c >= 'a' && c <= 'f' ? (unsigned)(c-'a'+10) : 16;
        if (digit > 15) return false;
        n = (n << 4) | digit;
    }
    *out = n; return true;
}

static bool calibrate(void *data, struct ssc_clock *clock)
{
    (void)data;
    return ssc_clock_calibrate_monotonic(clock, 16, 1000000);
}

static bool parse(const cJSON *json, struct pico_hmd_config *c)
{
    int64_t n, version;
    const cJSON *serial = field(json, "serial"), *model = field(json, "device");
    if (!integer(field(json, "schema_version"), 1, 2, &version) ||
        !cJSON_IsString(model) || strcmp(model->valuestring, "PICOA7H10") ||
        !cJSON_IsString(serial) || !*serial->valuestring || strlen(serial->valuestring) >= XRT_DEVICE_NAME_LEN) return false;
    c->serial = strdup(serial->valuestring);
    if (!c->serial) return false;
    const cJSON *screen = field(json, "screen");
    if (!integer(field(screen, "width"), 1, 16384, &n)) return false;
    c->screen_width = (unsigned)n;
    if (!integer(field(screen, "height"), 1, 16384, &n)) return false;
    c->screen_height = (unsigned)n;
    if (!integer(field(screen, "frame_interval_ns"), 1000000, 100000000, &c->frame_interval_ns)) return false;
    const cJSON *views = field(json, "views");
    if (!cJSON_IsArray(views) || cJSON_GetArraySize(views) != 2) return false;
    for (unsigned i = 0; i < 2; i++) {
        struct calibrated_hmd_view *v = &c->views[i];
        const cJSON *item = cJSON_GetArrayItem(views, (int)i), *viewport = field(item, "viewport"), *size = field(item, "display");
        uint32_t *vp[] = {&v->display.viewport.x_pixels, &v->display.viewport.y_pixels,
                          &v->display.viewport.w_pixels, &v->display.viewport.h_pixels};
        uint32_t *ds[] = {&v->display.display.w_pixels, &v->display.display.h_pixels};
        if (!cJSON_IsArray(viewport) || cJSON_GetArraySize(viewport) != 4 ||
            !cJSON_IsArray(size) || cJSON_GetArraySize(size) != 2) return false;
        for (unsigned j = 0; j < 4; j++) {
            if (!integer(cJSON_GetArrayItem(viewport, (int)j), j < 2 ? 0 : 1, 16384, &n)) return false;
            *vp[j] = (uint32_t)n;
        }
        for (unsigned j = 0; j < 2; j++) {
            if (!integer(cJSON_GetArrayItem(size, (int)j), 1, 16384, &n)) return false;
            *ds[j] = (uint32_t)n;
        }
        float f[4], p[7];
        if (!floats(field(item,"rotation"),4,v->display.rot.v) ||
            !floats(field(item,"fov"),4,f) || !floats(field(item,"eye_pose"),7,p)) return false;
        v->fov = (struct xrt_fov){.angle_left=f[0],.angle_right=f[1],.angle_up=f[2],.angle_down=f[3]};
        v->eye_pose = (struct xrt_pose){.orientation={p[0],p[1],p[2],p[3]},.position={p[4],p[5],p[6]}};
        if (version == 2) {
            if (cJSON_GetObjectItemCaseSensitive(item,"grid_width") || cJSON_GetObjectItemCaseSensitive(item,"grid_height") ||
                cJSON_GetObjectItemCaseSensitive(item,"distortion")) return false;
            const cJSON *radial = field(item,"analytic_distortion"), *type = field(radial,"type");
            if (!cJSON_IsString(type) || strcmp(type->valuestring,"pico-radial-2") ||
                !floats(field(radial,"tangent_scale"),2,v->radial.tangent_scale) ||
                !floats(field(radial,"tangent_offset"),2,v->radial.tangent_offset) ||
                !floats(field(radial,"radial_coefficients"),11,v->radial.radial) ||
                !floats(field(radial,"chromatic_coefficients"),6,v->radial.chromatic)) return false;
            v->radial_enabled = true;
            continue;
        }
        if (cJSON_GetObjectItemCaseSensitive(item,"analytic_distortion")) return false;
        if (!integer(field(item,"grid_width"),2,65,&n)) return false;
        v->grid_width = (unsigned)n;
        if (!integer(field(item,"grid_height"),2,65,&n)) return false;
        v->grid_height = (unsigned)n;
        const cJSON *grid = field(item,"distortion");
        unsigned count = v->grid_width * v->grid_height;
        if (!cJSON_IsArray(grid) || cJSON_GetArraySize(grid) != (int)count) return false;
        struct xrt_uv_triplet *values = calloc(count,sizeof(*values));
        if (!values) return false;
        v->distortion = values;
        for (unsigned j=0;j<count;j++) {
            float channels[6];
            if (!floats(cJSON_GetArrayItem(grid,(int)j),6,channels)) return false;
            values[j]=(struct xrt_uv_triplet){.r={channels[0],channels[1]},.g={channels[2],channels[3]},.b={channels[4],channels[5]}};
        }
    }
    const cJSON *imu = field(json,"imu"), *suids = field(imu,"suids");
    if (!cJSON_IsArray(suids) || cJSON_GetArraySize(suids) != 4) return false;
    for (unsigned i=0;i<4;i++) {
        const cJSON *pair = cJSON_GetArrayItem(suids,(int)i);
        uint64_t high, low;
        if (!cJSON_IsArray(pair) || cJSON_GetArraySize(pair)!=2 ||
            !hex64(cJSON_GetArrayItem(pair,0),&high) || !hex64(cJSON_GetArrayItem(pair,1),&low) || (!high && !low)) return false;
        if (i<2) { c->sensors.suid_high[i]=high;c->sensors.suid_low[i]=low; }
        else { c->sensors.bias.suid_high[i-2]=high;c->sensors.bias.suid_low[i-2]=low; }
    }
    if (!floats(field(imu,"sample_rates"),2,c->sensors.sample_rate) ||
        !floats(field(imu,"sensor_to_head"),9,c->tracking.sensor_to_head) ||
        !number(field(imu,"acceleration_scale"),1e-12,1e12,&c->tracking.acceleration_scale) ||
        !number(field(imu,"angular_velocity_scale"),1e-12,1e12,&c->tracking.angular_velocity_scale) ||
        !integer(field(imu,"max_age_ns"),1,1000000000,&c->tracking.max_age_ns) ||
        !integer(field(imu,"max_gap_ns"),1,c->tracking.max_age_ns,&c->tracking.max_gap_ns) ||
        !integer(field(imu,"minimum_accuracy"),1,3,&n)) return false;
    c->tracking.minimum_accuracy=(unsigned)n;
    c->sensors.max_age_ns=c->tracking.max_age_ns;
    if (!integer(field(imu,"startup_timeout_ns"),1,60000000000LL,&c->sensors.startup_timeout_ns) ||
        !integer(field(imu,"report_timeout_ns"),1,1000000000,&c->sensors.report_timeout_ns) ||
        !integer(field(imu,"bias_max_age_ns"),1,60000000000LL,&c->sensors.bias.max_age_ns)) return false;
    c->sensors.calibrate=calibrate;
    return true;
}

void pico_hmd_config_destroy(struct pico_hmd_config **config)
{
    if (!config || !*config) return;
    struct pico_hmd_config *c=*config;
    free((void *)c->serial);
    for (unsigned i=0;i<2;i++) free((void *)c->views[i].distortion);
    free(c);*config=NULL;
}

bool pico_hmd_config_load(const char *path, struct pico_hmd_config **config)
{
    if (!path || !config || *config) return false;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NONBLOCK|O_NOFOLLOW);
    if (fd<0) return false;
    struct stat st;
    char *buffer=NULL;
    struct pico_hmd_config *c=NULL;
    bool ok=false;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<=0 || st.st_size>2*1024*1024) goto done;
    buffer=malloc((size_t)st.st_size+1);
    if (!buffer) goto done;
    size_t used=0;
    while (used<(size_t)st.st_size) {
        ssize_t count=read(fd,buffer+used,(size_t)st.st_size-used);
        if (count<0 && errno==EINTR) continue;
        if (count<=0) goto done;
        used+=(size_t)count;
    }
    char extra;
    if (read(fd,&extra,1)!=0 || memchr(buffer,0,used)) goto done;
    buffer[used]=0;
    cJSON *json=cJSON_ParseWithLengthOpts(buffer,used+1,NULL,true);
    c=calloc(1,sizeof(*c));
    if (c && json) ok=parse(json,c);
    cJSON_Delete(json);
done:
    free(buffer);close(fd);
    if (ok) *config=c;
    else pico_hmd_config_destroy(&c);
    return ok;
}
