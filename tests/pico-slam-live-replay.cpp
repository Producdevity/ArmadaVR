#define VIT_INTERFACE_IMPLEMENTATION
#include "vit_interface.h"
extern "C" {
#include "pico-camera-replay.h"
#ifdef PICO_REPLAY_INSTALLED
#include "pico-monado-hmd.h"
#endif
}
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <opencv2/imgcodecs.hpp>
#include <sstream>
#include <thread>
#include <vector>
#define CHECK(v) do { if (!(v)) { fprintf(stderr,"CHECK line %d: %s\n",__LINE__,#v); abort(); } } while(0)
static std::vector<vit_pose_data_t> observed;
static unsigned images,imus;
static vit_result_t image(vit_tracker_t *t,const vit_img_sample_t *s)
{auto r=vit_tracker_push_img_sample(t,s);if(r==VIT_SUCCESS)++images;return r;}
static vit_result_t imu(vit_tracker_t *t,const vit_imu_sample_t *s)
{auto r=vit_tracker_push_imu_sample(t,s);if(r==VIT_SUCCESS)++imus;return r;}
static vit_result_t pose_data(const vit_pose_t *p,vit_pose_data_t *d)
{auto r=vit_pose_get_data(p,d);if(r==VIT_SUCCESS)observed.push_back(*d);return r;}
static std::vector<std::vector<std::string>> csv(const std::filesystem::path &p)
{
    std::ifstream f(p);CHECK(f.good());std::vector<std::vector<std::string>> rows;std::string line;
    while(std::getline(f,line)) {
        if(line.empty() || line[0]=='#')continue;
        if(line.back()=='\r')line.pop_back();
        std::stringstream fields(line);std::string field;std::vector<std::string> row;
        while(std::getline(fields,field,','))row.push_back(field);
        rows.push_back(row);
    }
    return rows;
}
int main(int argc,char **argv)
{
    CHECK(argc==4 || argc==5);
    if(argc==5) {
        Dl_info library{};
        CHECK(dladdr(dlsym(RTLD_DEFAULT,"pico_slam_live_poll_clock"),&library) && library.dli_fname && !strcmp(library.dli_fname,argv[4]));
        printf("PICO_SLAM_INSTALLED_LIBRARY path=%s\n",library.dli_fname);
    }
    cv::setNumThreads(1);std::filesystem::path root=argv[1];
    auto left=csv(root/"mav0/cam0/data.csv"),right=csv(root/"mav0/cam1/data.csv"),inertial=csv(root/"mav0/imu0/data.csv");
    CHECK(left.size()==right.size() && left.size()>100 && inertial.size()>1000);
    auto first=cv::imread((root/"mav0/cam0/data"/left[0][1]).string(),cv::IMREAD_GRAYSCALE);CHECK(!first.empty());
    pico_slam_live_config c{};
    c.tracking.imu={{1,0,0,0,1,0,0,0,1},1,1,100000000,100000000,2};
    c.tracking.input={2,{3,1,0,0},{(uint32_t)first.cols,(uint32_t)first.cols,0,0},
        {(uint32_t)first.rows,(uint32_t)first.rows,0,0},100000000,100000000,100000};
    c.tracking.tracking.openxr_from_slam.orientation.w=c.tracking.tracking.imu_from_head.orientation.w=1;
    c.tracking.tracking.max_age_ns=c.tracking.tracking.max_gap_ns=100000000;
    c.tracking.tracking.max_prediction_ns=50000000;
    c.tracking.backend={vit_api_get_version,vit_tracker_create,vit_tracker_destroy,vit_tracker_has_image_format,
        vit_tracker_start,vit_tracker_stop,imu,image,vit_tracker_pop_pose,pose_data,vit_pose_destroy};
    c.tracking.backend_config=argv[2];c.tracking.generation=1;
    c.tracking.clock_expires_ns=std::stoll(inertial.back()[0])+100000000;
    pico_camera_replay_live_config(&c,first.cols,first.rows,c.tracking.input.camera_ids);
    pico_slam_live *live=nullptr;CHECK(!pico_slam_live_prepare(&c,&live));
    auto *tracking=pico_slam_live_tracking(live);CHECK(tracking);
    int64_t now=std::min(std::stoll(left.front()[0]),std::stoll(inertial.front()[0]))+2000000;
#ifdef PICO_REPLAY_INSTALLED
    pico_hmd_config optics{};
    optics.serial="recorded-data-fixture";optics.screen_width=1920;optics.screen_height=3664;
    optics.frame_interval_ns=11111111;optics.visual_tracking=tracking;
    for(unsigned i=0;i<2;i++) {
        auto &v=optics.views[i];
        v.display.viewport={0,i*1832,1920,1832};v.display.display={1832,1920};
        v.display.rot.v[0]=v.display.rot.v[3]=1;
        v.fov={-.8f,.8f,.8f,-.8f};v.eye_pose.orientation.w=1;v.eye_pose.position.x=i?.032f:-.032f;
        v.radial_enabled=true;v.radial.tangent_scale[0]=2;v.radial.tangent_scale[1]=-2;
        v.radial.tangent_offset[0]=-1;v.radial.tangent_offset[1]=1;v.radial.radial[0]=1;
    }
    xrt_device *hmd=nullptr;CHECK(pico_monado_hmd_create(&optics,&hmd));
    CHECK(hmd->supported.position_tracking && hmd->supported.orientation_tracking);
    pico_camera_replay_clock(now);
    CHECK(hmd->update_inputs(hmd)==XRT_SUCCESS && !hmd->inputs[0].active);
    pico_camera_replay_clock(0);
    unsigned hmd_poses=0,view_pairs=0,predictions=0,rejected_predictions=0,active_inputs=0;
#endif
    int64_t offsets[2]={-10000,-10000};size_t received;
    auto clock=[](void *data)->int64_t{return *(int64_t *)data;};
    CHECK(pico_slam_live_poll_clock(live,offsets,clock,&now,&received)==0 && !received);
    std::ofstream output(argv[3]);CHECK(output.good());output.precision(12);
    output<<"timestamp_ns,x,y,z,qx,qy,qz,qw\n";
    unsigned poses=0,heads=0,skipped=0;int64_t last=0;
    auto poll=[&] {
        CHECK(pico_slam_live_poll_clock(live,offsets,clock,&now,&received)==1 && received==observed.size());
        for(const auto &p:observed) {
            CHECK(p.timestamp>last);last=p.timestamp;
            output<<p.timestamp<<','<<p.px<<','<<p.py<<','<<p.pz<<','<<p.ox<<','<<p.oy<<','<<p.oz<<','<<p.ow<<'\n';
            xrt_space_relation h{};int64_t time;
            if(pico_slam_tracking_get(tracking,now,p.timestamp,&h,&time)) {
                CHECK(time>=p.timestamp && (h.relation_flags&XRT_SPACE_RELATION_POSITION_TRACKED_BIT));
                CHECK(std::abs(h.pose.position.x-p.px)<0.00001 && std::abs(h.pose.position.y-p.py)<0.00001 &&
                      std::abs(h.pose.position.z-p.pz)<0.00001);++heads;
#ifdef PICO_REPLAY_INSTALLED
                pico_camera_replay_clock(now);
                xrt_space_relation actual{};
                CHECK(hmd->get_tracked_pose(hmd,XRT_INPUT_GENERIC_HEAD_POSE,p.timestamp,&actual)==XRT_SUCCESS);
                CHECK(actual.relation_flags==h.relation_flags);
                CHECK(std::abs(actual.pose.position.x-h.pose.position.x)<.00001 &&
                      std::abs(actual.pose.position.y-h.pose.position.y)<.00001 &&
                      std::abs(actual.pose.position.z-h.pose.position.z)<.00001);
                xrt_space_relation current{};int64_t current_time;
                bool active=pico_slam_tracking_get(tracking,now,now,&current,&current_time);
                CHECK(hmd->update_inputs(hmd)==XRT_SUCCESS && hmd->inputs[0].active==active && hmd->inputs[0].timestamp==current_time);
                active_inputs+=active;
                ++hmd_poses;
                xrt_pose eyes[2]{};xrt_fov fovs[2]{};xrt_vec3 eye_relation{.064f,0,0};
                CHECK(hmd->get_view_poses(hmd,&eye_relation,p.timestamp,XRT_VIEW_TYPE_STEREO,2,&actual,fovs,eyes)==XRT_SUCCESS);
                CHECK(actual.relation_flags==h.relation_flags && eyes[0].position.x==-.032f && eyes[1].position.x==.032f);
                CHECK(fovs[0].angle_left==-.8f && fovs[1].angle_right==.8f);++view_pairs;
                xrt_space_relation predicted{};int64_t sample;
                if(pico_slam_tracking_get(tracking,now,now+10000000,&predicted,&sample)) {
                    CHECK(hmd->get_tracked_pose(hmd,XRT_INPUT_GENERIC_HEAD_POSE,now+10000000,&actual)==XRT_SUCCESS);
                    CHECK(actual.relation_flags==predicted.relation_flags &&
                          std::abs(actual.pose.position.x-predicted.pose.position.x)<.00001 &&
                          std::abs(actual.pose.position.y-predicted.pose.position.y)<.00001 &&
                          std::abs(actual.pose.position.z-predicted.pose.position.z)<.00001);++predictions;
                }
                else {
                    CHECK(hmd->get_tracked_pose(hmd,XRT_INPUT_GENERIC_HEAD_POSE,now+10000000,&actual)==XRT_SUCCESS && !actual.relation_flags);
                    ++rejected_predictions;
                }
                pico_camera_replay_clock(0);
#endif
            }
            ++poses;
        }
        observed.clear();
    };
#ifdef PICO_REPLAY_INSTALLED
    constexpr unsigned allowed_backlog=1;
#else
    constexpr unsigned allowed_backlog=3;
#endif
    size_t ci=0,ii=0;
    while(ci<left.size() || ii<inertial.size()) {
        if(ii<inertial.size() && (ci==left.size() || std::stoll(inertial[ii][0])<=std::stoll(left[ci][0]))) {
            auto &r=inertial[ii++];CHECK(r.size()==7);xrt_imu_sample s{};
            s.timestamp_ns=std::stoll(r[0]);s.gyro_rad_secs={std::stod(r[1]),std::stod(r[2]),std::stod(r[3])};
            s.accel_m_s2={std::stod(r[4]),std::stod(r[5]),std::stod(r[6])};now=s.timestamp_ns+2000000;
            pico_camera_replay_imu(&s,ii,now);
        } else {
            CHECK(left[ci][0]==right[ci][0]);int64_t time=std::stoll(left[ci][0]);now=time+2000000;
            if(!ii){++skipped;++ci;continue;}
            for(int i=1;i>=0;--i) {
                const auto &row=i?right[ci]:left[ci];CHECK(std::filesystem::path(row[1]).filename()==row[1]);
                auto path=root/(i?"mav0/cam1/data":"mav0/cam0/data")/row[1];
                auto img=cv::imread(path.string(),cv::IMREAD_GRAYSCALE);
                CHECK(img.cols==first.cols && img.rows==first.rows && img.isContinuous());
                pico_camera_replay_feed(i,img.data,img.step,time-offsets[i]);
                img.setTo(0xa5);
            }
            ++ci;
        }
        poll();
        auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(images/2>poses+allowed_backlog && std::chrono::steady_clock::now()<limit){std::this_thread::sleep_for(std::chrono::milliseconds(1));poll();}
        CHECK(images/2<=poses+allowed_backlog);
    }
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(last<std::stoll(left.back()[0]) && std::chrono::steady_clock::now()<deadline){poll();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    CHECK(!pico_slam_live_stop(live));xrt_space_relation stopped{};int64_t time;
    CHECK(!pico_slam_tracking_get(tracking,now,last,&stopped,&time));
#ifdef PICO_REPLAY_INSTALLED
    pico_camera_replay_clock(now);
    CHECK(hmd->get_tracked_pose(hmd,XRT_INPUT_GENERIC_HEAD_POSE,last,&stopped)==XRT_SUCCESS && !stopped.relation_flags);
    CHECK(hmd->update_inputs(hmd)==XRT_SUCCESS && !hmd->inputs[0].active);
    hmd->destroy(hmd);pico_camera_replay_clock(0);
    CHECK(hmd_poses==heads && view_pairs==heads && predictions>0 && active_inputs>0 && predictions+rejected_predictions==heads);
    printf("PICO_SLAM_INSTALLED_HMD_PASS poses=%u stereo_views=%u predictions=%u rejected_predictions=%u active_inputs=%u stopped_invalid=1 duplicate_ssc=0 synthetic_optics=1\n",hmd_poses,view_pairs,predictions,rejected_predictions,active_inputs);
#endif
    CHECK(!pico_slam_live_quiesced(live));CHECK(!pico_slam_live_destroy(&live));
    unsigned buffers=pico_camera_replay_finish();
    CHECK(skipped==1 && imus==inertial.size() && poses==left.size()-1 && images==poses*2 && heads>=180);
    CHECK(buffers==images+2);output.close();
    printf("PICO_SLAM_LIVE_REPLAY_PASS poses=%u images=%u imu=%u head_poses=%u buffers=%u startup_skipped=%u simulated_ioctls=1 simulated_ssc=1 stopped_invalid=1\n",
        poses,images,imus,heads,buffers,skipped);
}
