#include "pico-head-session.h"
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

int main(int argc,char **argv)
{
    if(argc!=2) return 2;
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
        struct pico_head_tracking_config tracking={.sensor_to_head={1,0,0,0,1,0,0,0,1},
            .acceleration_scale=1,.angular_velocity_scale=1,.max_age_ns=500000000,
            .max_gap_ns=350000000,.minimum_accuracy=1};
        struct pico_head_session *session=NULL;
        struct ssc_imu_session_config bad=sensors;bad.bias.rigid_body[1]=1;
        g_assert_false(pico_head_session_create(&bad,&tracking,&session));
        g_assert_null(session);
        g_assert_true(pico_head_session_create(&sensors,&tracking,&session));
        struct xrt_space_relation relation;int64_t capture;
        g_assert_false(pico_head_session_get(session,g_get_monotonic_time()*1000,&relation,&capture));
        g_assert_cmpuint(relation.relation_flags,==,0);
        if(offline) pico_head_session_stop(session);
        gint64 deadline=g_get_monotonic_time()+7000000;
        bool seen=false;
        struct ssc_imu_session_status status={0};
        do {
            pico_head_session_get_status(session,&status);
            bool valid=pico_head_session_get(session,g_get_monotonic_time()*1000,&relation,&capture);
            if(valid) {
                g_assert_true(normal);
                g_assert_cmpuint(relation.relation_flags,==,XRT_SPACE_RELATION_ORIENTATION_VALID_BIT|XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
                g_assert_cmpint(capture,>,0);
                double norm=(double)relation.pose.orientation.x*relation.pose.orientation.x+
                    (double)relation.pose.orientation.y*relation.pose.orientation.y+
                    (double)relation.pose.orientation.z*relation.pose.orientation.z+
                    (double)relation.pose.orientation.w*relation.pose.orientation.w;
                g_assert_cmpfloat(fabs(norm-1),<,0.001);
                seen=true;
                pico_head_session_stop(session);
                g_assert_false(pico_head_session_get(session,g_get_monotonic_time()*1000,&relation,&capture));
                g_assert_cmpuint(relation.relation_flags,==,0);
            }
            if(status.finished) break;
            g_usleep(1000);
        } while(g_get_monotonic_time()<deadline);
        printf("mode=%s cycle=%u raw=%llu/%llu corrected=%llu/%llu error=%s\n", argv[1],cycle,
               (unsigned long long)status.accepted[0],(unsigned long long)status.accepted[1],
               (unsigned long long)status.corrected[0],(unsigned long long)status.corrected[1],status.error);
        g_assert_true(status.finished);
        g_assert_cmpint(seen,==,normal);
        if(!normal && !offline) g_assert_true(status.failed);
        g_assert_true(pico_head_session_destroy(&session));g_assert_null(session);
    }
    printf("PICO_HEAD_SESSION_PASS mode=%s cycles=%u\n",argv[1],cycles);
}
