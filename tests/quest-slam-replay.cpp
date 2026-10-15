#define VIT_INTERFACE_IMPLEMENTATION
#include "vit_interface.h"
extern "C" {
#include "quest-monado-hmd.h"
#include "quest-slam-runtime.h"
void quest_replay_clock(int64_t value);
void quest_replay_feed(int64_t stamp, const double accel[3], const double gyro[3]);
void quest_replay_finish(void);
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
static unsigned images, imus;
static vit_result_t image(vit_tracker_t *t,const vit_img_sample_t *s)
{auto r=vit_tracker_push_img_sample(t,s);if(r==VIT_SUCCESS)++images;return r;}
static vit_result_t imu(vit_tracker_t *t,const vit_imu_sample_t *s)
{auto r=vit_tracker_push_imu_sample(t,s);if(r==VIT_SUCCESS)++imus;return r;}
static vit_result_t pose_data(const vit_pose_t *p,vit_pose_data_t *d)
{auto r=vit_pose_get_data(p,d);if(r==VIT_SUCCESS)observed.push_back(*d);return r;}
static std::vector<std::vector<std::string>> csv(const std::filesystem::path &p)
{
    std::ifstream f(p); CHECK(f.good()); std::vector<std::vector<std::string>> rows; std::string line;
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
    CHECK(argc==5 || argc==6);
    const char *fault=argc==6?argv[5]:nullptr;
    CHECK(!fault || !strcmp(fault,"camera-silence") || !strcmp(fault,"clock-regression"));
    Dl_info library{};
    CHECK(dladdr(dlsym(RTLD_DEFAULT,"quest_slam_runtime_poll"),&library) && library.dli_fname && !strcmp(library.dli_fname,argv[4]));
    cv::setNumThreads(1); std::filesystem::path root=argv[1];
    auto left=csv(root/"mav0/cam0/data.csv"),right=csv(root/"mav0/cam1/data.csv"),inertial=csv(root/"mav0/imu0/data.csv");
    CHECK(left.size()==right.size() && left.size()>100 && inertial.size()>1000);
    auto first=cv::imread((root/"mav0/cam0/data"/left[0][1]).string(),cv::IMREAD_GRAYSCALE);CHECK(!first.empty());
    static const imu_temperature_offset offset[]={{25,{0,0,0}}};
    static const quest_imu_timing timing={0,0,100000000};
    quest_slam_runtime_config c{};
    c.sensors.generation=42; c.sensors.max_gap_ns=100000000;
    c.sensors.imu.format=1;c.sensors.imu.max_age_ns=100000000;c.sensors.imu.frame=IMU_FRAME_ORIGINAL;
    for(unsigned i=0;i<3;++i)c.sensors.imu.accelerometer[4*i]=c.sensors.imu.gyroscope[4*i]=c.sensors.imu.device_from_imu[4*i]=1;
    c.sensors.imu.accelerometer_offsets=c.sensors.imu.gyroscope_offsets=offset;
    c.sensors.imu.accelerometer_offset_count=c.sensors.imu.gyroscope_offset_count=1;c.sensors.imu.timing=&timing;
    c.input={2,{0,1,0,0},{(uint32_t)first.cols,(uint32_t)first.cols,0,0},
        {(uint32_t)first.rows,(uint32_t)first.rows,0,0},100000000,100000000,100000};
    c.tracking.openxr_from_slam.orientation.w=c.tracking.imu_from_head.orientation.w=1;
    c.tracking.max_age_ns=c.tracking.max_gap_ns=100000000;c.tracking.max_prediction_ns=50000000;
    c.backend={vit_api_get_version,vit_tracker_create,vit_tracker_destroy,vit_tracker_has_image_format,
        vit_tracker_start,vit_tracker_stop,imu,image,vit_tracker_pop_pose,pose_data,vit_pose_destroy};
    c.backend_config=argv[2];c.clock_expires_ns=std::stoll(inertial.back()[0])+1000000000;
    c.imu_uncertainty_ns=1000;
    int64_t now=std::min(std::stoll(left.front()[0]),std::stoll(inertial.front()[0]))+2000000;
    quest_replay_clock(now);
    quest_slam_runtime *runtime=nullptr;CHECK(quest_slam_runtime_prepare(&c,&runtime));
    xrt_uv_triplet grid[4]={};
    quest_hmd_config hmd_config{};
    hmd_config.serial="reference-replay-optics";hmd_config.screen_width=200;hmd_config.screen_height=100;
    hmd_config.frame_interval_ns=10000000;hmd_config.tracking=runtime;
    for(unsigned i=0;i<2;++i) {
        auto &v=hmd_config.views[i];v.display.viewport={i*100,0,100,100};v.display.display={100,100};
        v.display.rot.v[0]=v.display.rot.v[3]=1;
        v.fov.angle_left=v.fov.angle_down=-.8f;v.fov.angle_right=v.fov.angle_up=.8f;
        v.eye_pose.orientation.w=1;v.eye_pose.position.x=i?.03f:-.03f;
        v.grid_width=v.grid_height=2;v.distortion=grid;
    }
    xrt_device *hmd=nullptr;CHECK(quest_monado_hmd_create(&hmd_config,&hmd));
    CHECK(hmd->update_inputs(hmd)==XRT_SUCCESS && !hmd->inputs[0].active);
    CHECK(quest_slam_runtime_start(runtime));
    size_t received=999; CHECK(quest_slam_runtime_poll(runtime,&received)==0 && !received);
    std::ofstream output(argv[3]);CHECK(output.good());output.precision(12);
    output<<"timestamp_ns,x,y,z,qx,qy,qz,qw\n";
    unsigned poses=0,heads=0,skipped=0,hmd_poses=0,view_pairs=0,active_inputs=0,predictions=0;int64_t last=0;
    auto poll=[&] {
        quest_replay_clock(now);CHECK(quest_slam_runtime_poll(runtime,&received)==1 && received==observed.size());
        for(const auto &p:observed) {
            CHECK(p.timestamp>last);last=p.timestamp;
            output<<p.timestamp<<','<<p.px<<','<<p.py<<','<<p.pz<<','<<p.ox<<','<<p.oy<<','<<p.oz<<','<<p.ow<<'\n';
            xrt_space_relation h{};int64_t time;
            if(quest_slam_runtime_get_relation(runtime,now,p.timestamp,&h,&time)) {
                CHECK(time>=p.timestamp && (h.relation_flags&XRT_SPACE_RELATION_POSITION_TRACKED_BIT));
                CHECK(std::abs(h.pose.position.x-p.px)<.00001 && std::abs(h.pose.position.y-p.py)<.00001 && std::abs(h.pose.position.z-p.pz)<.00001);++heads;
                xrt_space_relation device_pose{};
                CHECK(hmd->get_tracked_pose(hmd,XRT_INPUT_GENERIC_HEAD_POSE,p.timestamp,&device_pose)==XRT_SUCCESS);
                CHECK(device_pose.relation_flags==h.relation_flags && !memcmp(&device_pose.pose,&h.pose,sizeof(h.pose)));
                xrt_space_relation current{};int64_t current_time;
                bool active=quest_slam_runtime_get_relation(runtime,now,now,&current,&current_time);
                CHECK(hmd->update_inputs(hmd)==XRT_SUCCESS && hmd->inputs[0].active==active && hmd->inputs[0].timestamp==current_time);
                active_inputs+=active;++hmd_poses;
                xrt_pose eyes[2]{};xrt_fov fovs[2]{};
                CHECK(hmd->get_view_poses(hmd,nullptr,p.timestamp,XRT_VIEW_TYPE_STEREO,2,&device_pose,fovs,eyes)==XRT_SUCCESS);
                CHECK(device_pose.relation_flags==h.relation_flags && !memcmp(&device_pose.pose,&h.pose,sizeof(h.pose)));
                CHECK(eyes[0].position.x==-.03f && eyes[1].position.x==.03f && fovs[0].angle_left==-.8f);++view_pairs;
                xrt_space_relation predicted{};int64_t predicted_time;
                bool valid=quest_slam_runtime_get_relation(runtime,now,p.timestamp+5000000,&predicted,&predicted_time);
                CHECK(hmd->get_tracked_pose(hmd,XRT_INPUT_GENERIC_HEAD_POSE,p.timestamp+5000000,&device_pose)==XRT_SUCCESS);
                CHECK(device_pose.relation_flags==predicted.relation_flags);
                if(valid){CHECK(!memcmp(&device_pose.pose,&predicted.pose,sizeof(predicted.pose)));++predictions;}
            }
            ++poses;
        }
        observed.clear();
    };
    size_t ci=0,ii=0;
    auto feed=[&](const std::vector<std::string> &r,int64_t time) {
        CHECK(r.size()==7);double gyro[3],accel[3];
        for(unsigned j=0;j<3;++j){gyro[j]=std::stod(r[j+1]);accel[j]=std::stod(r[j+4]);}
        now=time+2000000;quest_replay_clock(now);quest_replay_feed(time,accel,gyro);
    };
    while(ci<left.size() || ii<inertial.size()) {
        if(ii<inertial.size() && (ci==left.size() || std::stoll(inertial[ii][0])<=std::stoll(left[ci][0]))) {
            auto &r=inertial[ii++];feed(r,std::stoll(r[0]));
        } else {
            CHECK(left[ci][0]==right[ci][0]);int64_t time=std::stoll(left[ci][0]);now=time+2000000;quest_replay_clock(now);
            if(!ii){++skipped;++ci;continue;}
            for(int i=1;i>=0;--i) {
                const auto &row=i?right[ci]:left[ci];CHECK(std::filesystem::path(row[1]).filename()==row[1]);
                auto img=cv::imread((root/(i?"mav0/cam1/data":"mav0/cam0/data")/row[1]).string(),cv::IMREAD_GRAYSCALE);
                CHECK(img.cols==first.cols && img.rows==first.rows && img.isContinuous());
                pico_slam_mono8_frame f{(uint8_t)i,(uint32_t)img.cols,(uint32_t)img.rows,img.step,img.total(),img.data,time,ci};
                CHECK(quest_slam_runtime_camera(runtime,42,&f));img.setTo(0xa5);
            }
            ++ci;
        }
        poll();auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(images/2>poses+1 && std::chrono::steady_clock::now()<limit){std::this_thread::sleep_for(std::chrono::milliseconds(1));poll();}
        CHECK(images/2<=poses+1);
    }
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(last<std::stoll(left.back()[0]) && std::chrono::steady_clock::now()<deadline){poll();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    unsigned fault_imus=0;
    if(fault) {
        int result=1;
        if(!strcmp(fault,"clock-regression")) { quest_replay_clock(--now);result=quest_slam_runtime_poll(runtime,&received); }
        else {
            for(unsigned step=1;step<=7 && result==1;++step) {
                feed(inertial.back(),std::stoll(inertial.back()[0])+step*20000000);
                unsigned before=imus;result=quest_slam_runtime_poll(runtime,&received);fault_imus+=imus-before;CHECK(!received && observed.empty());
            }
        }
        CHECK(result<0 && !received);if(!strcmp(fault,"camera-silence"))CHECK(fault_imus>0 && now<c.clock_expires_ns);
        xrt_space_relation invalid{};int64_t time;
        CHECK(!quest_slam_runtime_get_relation(runtime,now,last,&invalid,&time) && !invalid.relation_flags);
        CHECK(!quest_slam_runtime_start(runtime));
        CHECK(hmd->get_tracked_pose(hmd,XRT_INPUT_GENERIC_HEAD_POSE,last,&invalid)==XRT_SUCCESS && !invalid.relation_flags);
        CHECK(hmd->update_inputs(hmd)==XRT_SUCCESS && !hmd->inputs[0].active && !hmd->inputs[0].timestamp);
        printf("QUEST_SLAM_FAULT_PASS cause=%s invalid_before_explicit_stop=1 implicit_restart=0 continued_imu=%u\n",fault,fault_imus);
    }
    quest_slam_runtime_stop(runtime);xrt_space_relation stopped{};int64_t time;
    CHECK(!quest_slam_runtime_get_relation(runtime,now,last,&stopped,&time) && !stopped.relation_flags);
    CHECK(hmd->get_tracked_pose(hmd,XRT_INPUT_GENERIC_HEAD_POSE,last,&stopped)==XRT_SUCCESS && !stopped.relation_flags);
    hmd->destroy(hmd);hmd=nullptr;CHECK(hmd_poses==heads && view_pairs==heads && active_inputs>0 && predictions>0);
    printf("QUEST_INSTALLED_HMD_PASS poses=%u stopped_invalid=1 borrowed_runtime=1 stereo_views=%u active_inputs=%u predictions=%u synthetic_optics=1\n",hmd_poses,view_pairs,active_inputs,predictions);
    quest_slam_runtime_destroy(&runtime);CHECK(!runtime);quest_replay_finish();
    CHECK(skipped==1 && imus==inertial.size()+fault_imus && poses==left.size()-1 && images==poses*2 && heads>=180);
    printf("QUEST_SLAM_REPLAY_PASS poses=%u images=%u imu=%u head_poses=%u startup_skipped=%u modeled_fifo=1 stopped_invalid=1 installed_library=1\n",poses,images,imus,heads,skipped);
}
