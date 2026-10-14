#define VIT_INTERFACE_IMPLEMENTATION
#include "vit_interface.h"
extern "C" {
#include "pico-slam-input.h"
#include "pico-slam-tracking.h"
}
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <opencv2/imgcodecs.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <xrt/xrt_frame.h>
#define CHECK(v)                                                               \
  do {                                                                         \
    if (!(v)) {                                                                \
      fprintf(stderr, "CHECK line %d: %s\n", __LINE__, #v);                    \
      abort();                                                                 \
    }                                                                          \
  } while (0)
static vit_tracker_t *tracker;
static unsigned delivered_images, delivered_imu;
struct CameraSink {
  xrt_frame_sink base;
  unsigned index;
};
static void camera(xrt_frame_sink *sink, xrt_frame *f) {
  auto *s = reinterpret_cast<CameraSink *>(sink);
  vit_img_sample_t image{};
  image.cam_index = s->index;
  image.timestamp = f->timestamp;
  image.data = f->data;
  image.width = f->width;
  image.height = f->height;
  image.stride = f->stride;
  image.size = f->size;
  image.format = VIT_IMAGE_FORMAT_L8;
  CHECK(vit_tracker_push_img_sample(tracker, &image) == VIT_SUCCESS);
  ++delivered_images;
}
static void imu(xrt_imu_sink *, xrt_imu_sample *s) {
  vit_imu_sample_t v{};
  v.timestamp = s->timestamp_ns;
  v.ax = s->accel_m_s2.x;
  v.ay = s->accel_m_s2.y;
  v.az = s->accel_m_s2.z;
  v.wx = s->gyro_rad_secs.x;
  v.wy = s->gyro_rad_secs.y;
  v.wz = s->gyro_rad_secs.z;
  CHECK(vit_tracker_push_imu_sample(tracker, &v) == VIT_SUCCESS);
  ++delivered_imu;
}
static std::vector<std::vector<std::string>>
csv(const std::filesystem::path &path) {
  std::ifstream f(path);
  CHECK(f.good());
  std::string line;
  std::vector<std::vector<std::string>> rows;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    if (line.back() == '\r')
      line.pop_back();
    std::stringstream fields(line);
    std::string field;
    std::vector<std::string> row;
    while (std::getline(fields, field, ','))
      row.push_back(field);
    rows.push_back(row);
  }
  return rows;
}
static void put32(uint8_t *p, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i)
    p[i] = n >> (8 * i);
}
static void put64(uint8_t *p, uint64_t n) {
  for (unsigned i = 0; i < 8; ++i)
    p[i] = n >> (8 * i);
}
int main(int argc, char **argv) {
  CHECK(argc == 4);
  cv::setNumThreads(1);
  const std::filesystem::path root = argv[1];
  auto left = csv(root / "mav0/cam0/data.csv"),
       right = csv(root / "mav0/cam1/data.csv"),
       inertial = csv(root / "mav0/imu0/data.csv");
  CHECK(left.size() == right.size() && left.size() > 100 &&
        inertial.size() > 1000);
  auto first = cv::imread((root / "mav0/cam0/data" / left[0][1]).string(),
                          cv::IMREAD_GRAYSCALE);
  CHECK(!first.empty());
  int64_t end = std::stoll(inertial.back()[0]);
  vit_config_t config{};
  config.file = argv[2];
  config.cam_count = 2;
  config.imu_count = 1;
  uint32_t major, minor, patch;
  CHECK(vit_api_get_version(&major, &minor, &patch) == VIT_SUCCESS &&
        major == 2 && minor == 0 && patch == 1);
  CHECK(vit_tracker_create(&config, &tracker) == VIT_SUCCESS && tracker);
  CHECK(vit_tracker_start(tracker) == VIT_SUCCESS);
  CameraSink cams[2] = {{{camera}, 0}, {{camera}, 1}};
  xrt_imu_sink isink{imu};
  xrt_slam_sinks sinks{};
  sinks.cam_count = 2;
  sinks.cams[0] = &cams[0].base;
  sinks.cams[1] = &cams[1].base;
  sinks.imu = &isink;
  pico_slam_input_config c{};
  c.count = 2;
  c.camera_ids[0] = 3;
  c.camera_ids[1] = 1;
  for (unsigned i = 0; i < 2; ++i) {
    c.width[i] = first.cols;
    c.height[i] = first.rows;
  }
  c.max_age_ns = 100000000;
  c.max_gap_ns = 100000000;
  c.max_skew_ns = 100000;
  pico_slam_input *input = nullptr;
  CHECK(pico_slam_input_create(&c, &sinks, &input));
  CHECK(pico_slam_input_begin(input, 1, 0, 0, end + 100000000));
  std::ofstream poses(argv[3]);
  CHECK(poses.good());
  poses.precision(12);
  poses << "timestamp_ns,x,y,z,qx,qy,qz,qw\n";
  pico_slam_tracking_config pose_config{};
  pose_config.openxr_from_slam.orientation.w = 1;
  pose_config.imu_from_head.orientation.w = 1;
  pose_config.max_age_ns = 100000000;
  pose_config.max_gap_ns = 100000000;
  pose_config.max_prediction_ns = 50000000;
  pico_slam_tracking *tracking = nullptr;
  CHECK(pico_slam_tracking_create(&pose_config, &tracking));
  CHECK(pico_slam_tracking_begin(tracking, 1));
  unsigned head_poses = 0;
  unsigned count = 0;
  int64_t last_pose = 0;
  double minpos[3] = {1e9, 1e9, 1e9}, maxpos[3] = {-1e9, -1e9, -1e9};
  auto pop = [&] {
    vit_pose_t *pose = nullptr;
    while (true) {
      CHECK(vit_tracker_pop_pose(tracker, &pose) == VIT_SUCCESS);
      if (!pose)
        break;
      vit_pose_data_t d{};
      CHECK(vit_pose_get_data(pose, &d) == VIT_SUCCESS);
      CHECK(d.timestamp > last_pose);
      CHECK(d.timestamp >= std::stoll(left.front()[0]) &&
            d.timestamp <= std::stoll(left.back()[0]));
      last_pose = d.timestamp;
      double n = d.ox * d.ox + d.oy * d.oy + d.oz * d.oz + d.ow * d.ow;
      CHECK(std::isfinite(n) && fabs(n - 1) < 0.001);
      double p[] = {d.px, d.py, d.pz};
      for (unsigned i = 0; i < 3; ++i) {
        CHECK(std::isfinite(p[i]));
        minpos[i] = std::min(minpos[i], p[i]);
        maxpos[i] = std::max(maxpos[i], p[i]);
      }
      poses << d.timestamp << ',' << d.px << ',' << d.py << ',' << d.pz << ','
            << d.ox << ',' << d.oy << ',' << d.oz << ',' << d.ow << '\n';
      xrt_pose imu_pose{{d.ox, d.oy, d.oz, d.ow}, {d.px, d.py, d.pz}};
      CHECK(pico_slam_tracking_push(tracking, 1, &imu_pose, d.timestamp, 0, d.timestamp));
      xrt_space_relation head{};
      int64_t sample_time = 0;
      bool valid = pico_slam_tracking_get(tracking, d.timestamp, d.timestamp, &head, &sample_time);
      CHECK(valid == (count != 0));
      if (valid) {
        CHECK(sample_time == d.timestamp && (head.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT));
        CHECK(std::abs(head.pose.position.x-d.px)<0.00001 &&
              std::abs(head.pose.position.y-d.py)<0.00001 && std::abs(head.pose.position.z-d.pz)<0.00001);
        ++head_poses;
      }
      vit_pose_destroy(pose);
      ++count;
    }
  };
  size_t ci = 0, ii = 0;
  while (ci < left.size() || ii < inertial.size()) {
    if (ii < inertial.size() &&
        (ci == left.size() ||
         std::stoll(inertial[ii][0]) <= std::stoll(left[ci][0]))) {
      auto &r = inertial[ii++];
      CHECK(r.size() == 7);
      xrt_imu_sample s{};
      s.timestamp_ns = std::stoll(r[0]);
      s.gyro_rad_secs = {std::stod(r[1]), std::stod(r[2]), std::stod(r[3])};
      s.accel_m_s2 = {std::stod(r[4]), std::stod(r[5]), std::stod(r[6])};
      CHECK(pico_slam_input_imu(input, 1, &s, 0, s.timestamp_ns + 2000000));
    } else {
      CHECK(left[ci].size() == 2 && right[ci].size() == 2 &&
            left[ci][0] == right[ci][0]);
      int64_t time = std::stoll(left[ci][0]);
      for (int i = 1; i >= 0; --i) {
        auto &r = i ? right[ci] : left[ci];
        CHECK(std::filesystem::path(r[1]).filename() == r[1]);
        auto path = root / (i ? "mav0/cam1/data" : "mav0/cam0/data") / r[1];
        auto image = cv::imread(path.string(), cv::IMREAD_GRAYSCALE);
        CHECK(image.cols == first.cols && image.rows == first.rows &&
              image.isContinuous());
        size_t size = image.total();
        std::vector<uint8_t> wire(89 + size);
        wire[0] = c.camera_ids[i];
        put32(wire.data() + 1, image.cols);
        put32(wire.data() + 5, image.rows);
        put32(wire.data() + 9, 0x123);
        put32(wire.data() + 13, 1);
        put64(wire.data() + 25, time + 52000);
        put64(wire.data() + 33, time);
        put64(wire.data() + 41, 1);
        put64(wire.data() + 49, ci);
        put32(wire.data() + 57, size);
        put32(wire.data() + 65, 1);
        std::copy(image.data, image.data + size, wire.data() + 89);
        CHECK(pico_slam_input_camera(input, 1, wire.data(), wire.size(),
                                     time + 2000000));
        std::fill(wire.begin(), wire.end(), 0xa5);
      }
      ++ci;
    }
    pop();
  }
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (last_pose < std::stoll(left.back()[0]) &&
         std::chrono::steady_clock::now() < deadline) {
    pop();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  pico_slam_input_destroy(&input);
  pop();
  CHECK(vit_tracker_stop(tracker) == VIT_SUCCESS);
  vit_tracker_destroy(tracker);
  tracker = nullptr;
  pico_slam_tracking_stop(tracking);
  xrt_space_relation stopped{};
  int64_t sample_time;
  CHECK(!pico_slam_tracking_get(tracking, last_pose, last_pose, &stopped, &sample_time));
  pico_slam_tracking_destroy(&tracking);
  CHECK(head_poses + 1 == count);
  CHECK(count == left.size() - 1 && delivered_images == 2 * count &&
        delivered_imu == inertial.size());
  double span = 0;
  for (unsigned i = 0; i < 3; ++i)
    span += pow(maxpos[i] - minpos[i], 2);
  CHECK(span > 0.01 && span < 100);
  poses.close();
  printf("PICO_SLAM_HEAD_POSES_PASS poses=%u stopped_invalid=1\n", head_poses);
  printf("PICO_SLAM_REPLAY_PASS frames=%zu images=%u imu=%u poses=%u "
         "position_span=%.6f api=%u.%u.%u\n",
         left.size(), delivered_images, delivered_imu, count, sqrt(span), major,
         minor, patch);
}
