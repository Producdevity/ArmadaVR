#include <Eigen/Core>
#include <basalt/calibration/calibration.hpp>
#include <basalt/serialization/headers_serialization.h>
#include <cereal/archives/json.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
extern "C" {
#include "fisheye62.h"
#include "imu-calibration.h"
}
#define CHECK(v) do { if (!(v)) { std::fprintf(stderr,"CHECK line %d: %s\n",__LINE__,#v); std::abort(); } } while (0)

int main(int argc, char **argv)
{
    CHECK(argc == 4);
    basalt::Calibration<double> calibration;
    std::ifstream input(argv[1]); CHECK(input.good());
    { cereal::JSONInputArchive archive(input); archive(calibration); }
    std::ifstream reference(argv[2]); CHECK(reference.good());
    double accel[9], gyro[9], device[16], rotation[9];
    for (double &v : accel) reference >> v;
    for (double &v : gyro) reference >> v;
    for (double &v : device) reference >> v;
    for (int i=0; i<3; ++i) for (int j=0; j<3; ++j) rotation[i*3+j]=device[i*4+j];
    const bool aligned=std::string(argv[3])=="accelerometer";
    CHECK(aligned || std::string(argv[3])=="original");
    imu_rectification rectified{};
    CHECK(imu_prepare_rectification(accel,gyro,rotation,aligned?IMU_FRAME_ACCELEROMETER:IMU_FRAME_ORIGINAL,&rectified));
    Eigen::Matrix4d device_from_imu=Eigen::Matrix4d::Identity();
    for (int i=0; i<3; ++i) {
        device_from_imu(i,3)=device[i*4+3];
        for (int j=0; j<3; ++j) device_from_imu(i,j)=rectified.device_from_imu[i*3+j];
    }
    unsigned cameras; reference >> cameras;
    CHECK(cameras>=2 && cameras<=4 && calibration.intrinsics.size()==cameras && calibration.T_i_c.size()==cameras);
    unsigned samples=0;
    for (unsigned i=0; i<cameras; ++i) {
        double p[11]; for (double &v : p) reference >> v;
        Eigen::Matrix4d device_from_camera;
        for (int row=0; row<4; ++row) for (int col=0; col<4; ++col) reference >> device_from_camera(row,col);
        int width,height; reference >> width >> height; CHECK(reference.good());
        CHECK(calibration.resolution[i][0]==width && calibration.resolution[i][1]==height);
        CHECK((calibration.T_i_c[i].matrix()-device_from_imu.inverse()*device_from_camera).norm()<1e-12);
        const auto &camera=calibration.intrinsics[i]; CHECK(camera.getName()=="fisheye624");
        for (unsigned n=0; n<=2000; ++n) {
            double radius=n==0?0:1.3*double(n%127)/127, angle=double(n)*.172;
            double point[3]={radius*std::cos(angle),radius*std::sin(angle),1}, pixel[2];
            CHECK(fisheye62_project(p,point,pixel));
            Eigen::Vector4d ray(point[0],point[1],1,0),back;
            Eigen::Vector2d projected; CHECK(camera.project(ray,projected));
            CHECK((projected-Eigen::Vector2d(pixel[0],pixel[1])).norm()<1e-9);
            CHECK(camera.unproject(projected,back));
            CHECK((back.head<3>().normalized()-ray.head<3>().normalized()).norm()<1e-9);
            ++samples;
        }
    }
    Eigen::Vector3d sample(.1,-9.7,.3);
    CHECK((calibration.calib_accel_bias.getCalibrated(sample)-sample).norm()==0);
    CHECK((calibration.calib_gyro_bias.getCalibrated(sample)-sample).norm()==0);
    CHECK(calibration.cam_time_offset_ns==0);
    std::printf("QUEST_BASALT_CALIBRATION_PASS cameras=%u projections=%u frame=%s\n",cameras,samples,argv[3]);
}
