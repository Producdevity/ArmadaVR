#include "pico-slam-runtime.h"
#include <pthread.h>
#include <gio/gio.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static bool calibrate(void *data, struct ssc_clock *clock)
{
    if (!strcmp(data,"clock-failure")) return false;
    int64_t anchor=g_get_monotonic_time()*1000-100000000;
    return ssc_clock_init(clock,UINT64_C(0x123456789abcdef),19200000,anchor,anchor,0);
}

static pthread_t owner;
static unsigned imus;
struct vit_tracker { bool started; };
static vit_result_t version(uint32_t *a,uint32_t *b,uint32_t *c)
{g_assert_true(pthread_equal(owner,pthread_self()));*a=2;*b=0;*c=1;return VIT_SUCCESS;}
static vit_result_t create(const vit_config_t *c,vit_tracker_t **t)
{g_assert_cmpuint(c->cam_count,==,2);*t=g_new0(struct vit_tracker,1);return VIT_SUCCESS;}
static void destroy(vit_tracker_t *t){g_assert_false(t->started);g_free(t);}
static vit_result_t format(const vit_tracker_t *t,vit_image_format_t f,bool *supported)
{g_assert_nonnull(t);*supported=f==VIT_IMAGE_FORMAT_L8;return VIT_SUCCESS;}
static vit_result_t start(vit_tracker_t *t){t->started=true;return VIT_SUCCESS;}
static vit_result_t stop(vit_tracker_t *t){t->started=false;return VIT_SUCCESS;}
static vit_result_t imu(vit_tracker_t *t,const vit_imu_sample_t *v)
{
    g_assert_true(pthread_equal(owner,pthread_self()));g_assert_true(t->started);
    g_assert_cmpfloat(v->ax,==,9.5f);g_assert_cmpfloat(v->ay,==,1.5f);g_assert_cmpfloat(v->az,==,-2.75f);
    g_assert_cmpfloat(v->wx,==,-0.375f);g_assert_cmpfloat(v->wy,==,1.5f);g_assert_cmpfloat(v->wz,==,-2.75f);
    ++imus;return VIT_SUCCESS;
}
static vit_result_t image(vit_tracker_t *t,const vit_img_sample_t *v)
{(void)t;(void)v;g_assert_not_reached();return VIT_ERROR_INVALID_VALUE;}
static vit_result_t pop(vit_tracker_t *t,vit_pose_t **p)
{g_assert_true(t->started);*p=NULL;return VIT_SUCCESS;}
static vit_result_t pose_data(const vit_pose_t *p,vit_pose_data_t *d)
{(void)p;(void)d;g_assert_not_reached();return VIT_ERROR_INVALID_VALUE;}
static void pose_destroy(vit_pose_t *p){(void)p;g_assert_not_reached();}
int main(int argc,char **argv)
{
    if(argc!=2) return 2;
    owner=pthread_self();
    bool offline=!strcmp(argv[1],"no-qrtr");
    if(offline) {
        int fd=socket(42,SOCK_DGRAM|SOCK_CLOEXEC,0);
        if(fd>=0) {close(fd);return 2;}
        if(errno!=EAFNOSUPPORT && errno!=EPROTONOSUPPORT) return 2;
    } else {
        gchar *identity=NULL,*cmdline=NULL;
        bool virtual=g_file_get_contents("/proc/device-tree/compatible",&identity,NULL,NULL) &&
            !strcmp(identity,"linux,dummy-virt") &&
            g_file_get_contents("/proc/cmdline",&cmdline,NULL,NULL) &&
            strstr(cmdline,"reframed.ssc_test=1");
        g_free(identity);g_free(cmdline);
        if(!virtual) return 2;
    }
    bool normal=!strcmp(argv[1],"normal") || !strcmp(argv[1],"single-bias");
    unsigned cycles=offline?100:normal?3:1;
    for(unsigned cycle=0;cycle<cycles;cycle++) {
        struct ssc_imu_session_config sensors={
            .suid_high={0xACC10001,0xACC10002},.suid_low={0xACC20001,0xACC20002},
            .sample_rate={1000,1000},.max_age_ns=1000000000,
            .startup_timeout_ns=2000000000,.report_timeout_ns=1000000000,
            .calibrate=calibrate,.data=argv[1],
            .bias={.suid_high={0xACC10003,0xACC10004},.suid_low={0xACC20003,0xACC20004},
                   .rigid_body={0,0},.max_age_ns=500000000},
        };
        struct pico_slam_runtime_config c={
            .sensors=sensors,
            .imu={.sensor_to_imu={1,0,0,0,1,0,0,0,1},.acceleration_scale=1,.angular_velocity_scale=1,
                  .max_age_ns=500000000,.max_gap_ns=350000000,.minimum_accuracy=1},
            .input={.count=2,.camera_ids={0,1},.width={3,3},.height={2,2},
                    .max_age_ns=500000000,.max_gap_ns=350000000,.max_skew_ns=100000},
            .tracking={.openxr_from_slam.orientation.w=1,.imu_from_head.orientation.w=1,
                       .max_age_ns=500000000,.max_gap_ns=350000000,.max_prediction_ns=50000000},
            .backend={version,create,destroy,format,start,stop,imu,image,pop,pose_data,pose_destroy},
            .backend_config="synthetic-backend",.generation=cycle+7,
            .clock_expires_ns=g_get_monotonic_time()*1000+7000000000,
        };
        struct pico_slam_runtime *session=NULL;
        struct pico_slam_runtime_config bad=c;bad.sensors.bias.rigid_body[1]=1;
        g_assert_false(pico_slam_runtime_create(&bad,&session));
        g_assert_null(session);imus=0;
        g_assert_true(pico_slam_runtime_create(&c,&session));
        if(offline) pico_slam_runtime_stop(session);
        gint64 deadline=g_get_monotonic_time()+7000000;
        bool seen=false;int runtime_failure=0;
        struct ssc_imu_session_status status={0};
        do {
            pico_slam_runtime_get_status(session,&status);
            size_t poses;
            int running=pico_slam_runtime_poll(session,g_get_monotonic_time()*1000,&poses);
            if(running<0)runtime_failure=running;
            g_assert_cmpuint(poses,==,0);
            if(running==1) {
                g_assert_true(normal);g_assert_cmpuint(imus,>,0);seen=true;
                pico_slam_runtime_stop(session);
                g_assert_cmpint(pico_slam_runtime_poll(session,g_get_monotonic_time()*1000,&poses),<,0);
            }
            if(status.finished) break;
            g_usleep(1000);
        } while(g_get_monotonic_time()<deadline);
        printf("mode=%s cycle=%u raw=%llu/%llu corrected=%llu/%llu runtime=%d imu=%u error=%s\n", argv[1],cycle,
               (unsigned long long)status.accepted[0],(unsigned long long)status.accepted[1],
               (unsigned long long)status.corrected[0],(unsigned long long)status.corrected[1],runtime_failure,imus,status.error);
        g_assert_true(status.finished);
        g_assert_cmpint(seen,==,normal);
        if(!normal && !offline) {g_assert_cmpint(runtime_failure,<,0);g_assert_cmpuint(imus,==,0);}
        g_assert_true(pico_slam_runtime_destroy(&session));g_assert_null(session);
    }
    printf("PICO_SLAM_SENSORS_PASS mode=%s cycles=%u\n",argv[1],cycles);
}
