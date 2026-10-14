#define VIT_INTERFACE_IMPLEMENTATION
#include "vit_interface.h"
extern "C" {
#include "pico-slam-input.h"
#include "pico-slam-tracking.h"
#include "pico-slam-session.h"
#include "pico-camera-stream.h"
#include "pico-camera-replay.h"
}
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
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
struct CaptureSink {
  pico_slam_session *session{};
  unsigned phase{}, frames{}, releases{}, invalidations{};
};
static bool capture_sync(void *opaque, void *, bool end) {
  auto *s = static_cast<CaptureSink *>(opaque);
  CHECK(s->phase == (end ? 2U : 0U));
  s->phase = end ? 3 : 1;
  return true;
}
static bool capture_frame(void *opaque, uint64_t generation, const pico_camera_capture_frame *frame, int64_t now) {
  auto *s = static_cast<CaptureSink *>(opaque);
  CHECK(s->phase == 1);
  s->phase = 2;
  ++s->frames;
  return pico_slam_session_captured(s->session, generation, frame, now);
}
static bool capture_release(void *opaque, void *token) {
  auto *s = static_cast<CaptureSink *>(opaque);
  CHECK(s->phase == 3);
  s->phase = 0;
  ++s->releases;
  auto *pixels = static_cast<std::vector<uint8_t> *>(token);
  std::fill(pixels->begin(), pixels->end(), 0xa5);
  return true;
}
static void capture_invalid(void *opaque, uint64_t generation) {
  auto *s = static_cast<CaptureSink *>(opaque);
  CHECK(generation == 1);
  ++s->invalidations;
  pico_slam_session_stop(s->session);
}
static bool transport_frame(void *opaque, uint64_t generation, const pico_camera_capture_frame *frame, int64_t now) {
  auto *s = static_cast<CaptureSink *>(opaque);
  ++s->frames;
  return pico_slam_session_captured(s->session, generation, frame, now);
}
static vit_tracker_t *tracker;
static std::vector<vit_pose_data_t> observed;
static unsigned delivered_images, delivered_imu;
static unsigned image_bytes = 1;
static vit_result_t session_image(vit_tracker_t *t, const vit_img_sample_t *s) {
  auto result = vit_tracker_push_img_sample(t, s);
  if (result == VIT_SUCCESS) ++delivered_images;
  return result;
}
static vit_result_t session_imu(vit_tracker_t *t, const vit_imu_sample_t *s) {
  auto result = vit_tracker_push_imu_sample(t, s);
  if (result == VIT_SUCCESS) ++delivered_imu;
  return result;
}
static vit_result_t observe_pose(const vit_pose_t *p, vit_pose_data_t *d) {
  auto result = vit_pose_get_data(p, d);
  if (result == VIT_SUCCESS) observed.push_back(*d);
  return result;
}
struct CameraSink {
  xrt_frame_sink base;
  unsigned index;
};
static void camera(xrt_frame_sink *sink, xrt_frame *f) {
  auto *s = reinterpret_cast<CameraSink *>(sink);
  vit_img_sample_t image{};
  image.cam_index = s->index;
  image.timestamp = f->timestamp;
  image.width = f->width;
  image.height = f->height;
  image.stride = f->width * image_bytes + 13;
  image.size = image.stride * image.height;
  std::vector<uint8_t> padded(image.size + 1, 0x5a);
  image.data = padded.data() + 1;
  image.format = image_bytes == 1 ? VIT_IMAGE_FORMAT_L8 : VIT_IMAGE_FORMAT_L16;
  for (unsigned y = 0; y < image.height; ++y)
    for (unsigned x = 0; x < image.width; ++x) {
      uint16_t value = (uint16_t)f->data[y * f->stride + x] << 8;
      auto *pixel = image.data + y * image.stride + x * image_bytes;
      if (image_bytes == 1) *pixel = value >> 8;
      else std::memcpy(pixel, &value, sizeof(value));
    }
  CHECK(vit_tracker_push_img_sample(tracker, &image) == VIT_SUCCESS);
  std::fill(padded.begin(), padded.end(), 0xa5);
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
  CHECK(argc == 5);
  image_bytes = std::stoi(argv[4]);
  CHECK(image_bytes <= 6);
  bool transport_input = image_bytes == 6;
  bool capture_input = image_bytes >= 4;
  bool stream_input = image_bytes == 5;
  bool native_input = image_bytes >= 3;
  bool owned = image_bytes == 0 || native_input;
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
  if (!owned) {
    CHECK(vit_tracker_create(&config, &tracker) == VIT_SUCCESS && tracker);
    CHECK(vit_tracker_start(tracker) == VIT_SUCCESS);
  }
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
  pico_slam_tracking_config pose_config{};
  pose_config.openxr_from_slam.orientation.w = 1;
  pose_config.imu_from_head.orientation.w = 1;
  pose_config.max_age_ns = 100000000;
  pose_config.max_gap_ns = 100000000;
  pose_config.max_prediction_ns = 50000000;
  pico_slam_tracking *tracking = nullptr;
  pico_slam_input *input = nullptr;
  pico_slam_session *session = nullptr;
  int64_t arrival = std::min(std::stoll(left.front()[0]), std::stoll(inertial.front()[0])) + 2000000;
  if (owned) {
    pico_slam_backend api{vit_api_get_version, vit_tracker_create, vit_tracker_destroy,
      vit_tracker_has_image_format, vit_tracker_start, vit_tracker_stop,
      session_imu, session_image, vit_tracker_pop_pose, observe_pose, vit_pose_destroy};
    CHECK(pico_slam_session_create(&c, &pose_config, &api, argv[2], &session));
    CHECK(pico_slam_session_begin(session, 1, 0, 0, end + 100000000, arrival));
    tracking = pico_slam_session_tracking(session);
  } else {
    CHECK(pico_slam_input_create(&c, &sinks, &input));
    CHECK(pico_slam_input_begin(input, 1, 0, 0, end + 100000000));
    CHECK(pico_slam_tracking_create(&pose_config, &tracking));
    CHECK(pico_slam_tracking_begin(tracking, 1));
  }
  CaptureSink capture_sink{session};
  pico_camera_transport *transport = nullptr;
  pico_camera_capture *capture_owner = nullptr;
  pico_camera_stream *camera_stream = nullptr;
  uint32_t event_sequence[2] = {UINT32_MAX-3, UINT32_MAX-3};
  if (transport_input) {
    pico_camera_transport_config config{};
    pico_camera_replay_config(&config, first.cols, first.rows, c.camera_ids);
    pico_camera_transport_ops ops{transport_frame, capture_invalid};
    CHECK(!pico_camera_transport_create(&config, &ops, &capture_sink, &transport));
  } else if (capture_input) {
    pico_camera_capture_config cfg{1, {c.camera_ids[0], c.camera_ids[1], 0, 0}, 2, 100000000};
    pico_camera_capture_ops ops{capture_sync, capture_frame, capture_release, capture_invalid};
    if (stream_input) {
      pico_camera_stream_config stream_config{cfg, 77, {100, 101, 0, 0}, 1};
      CHECK(pico_camera_stream_create(&stream_config, &ops, &capture_sink, &camera_stream));
    } else CHECK(pico_camera_capture_create(&cfg, &ops, &capture_sink, &capture_owner));
  }
  std::ofstream poses(argv[3]);
  CHECK(poses.good());
  poses.precision(12);
  poses << "timestamp_ns,x,y,z,qx,qy,qz,qw\n";
  unsigned head_poses = 0;
  unsigned count = 0;
  int64_t last_pose = 0;
  double minpos[3] = {1e9, 1e9, 1e9}, maxpos[3] = {-1e9, -1e9, -1e9};
  auto pop = [&] {
    vit_pose_t *pose = nullptr;
    if (owned) {
      size_t received;
      CHECK(pico_slam_session_poll(session, 1, arrival, &received));
      CHECK(received == observed.size());
    }
    size_t index = 0;
    while (true) {
      vit_pose_data_t d{};
      if (owned) {
        if (index == observed.size()) break;
        d = observed[index++];
      } else {
        CHECK(vit_tracker_pop_pose(tracker, &pose) == VIT_SUCCESS);
        if (!pose) break;
        CHECK(vit_pose_get_data(pose, &d) == VIT_SUCCESS);
      }
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
      if (!owned) CHECK(pico_slam_tracking_push(tracking, 1, &imu_pose, d.timestamp, 0, d.timestamp));
      xrt_space_relation head{};
      int64_t sample_time = 0;
      bool valid = pico_slam_tracking_get(tracking, owned ? arrival : d.timestamp, d.timestamp, &head, &sample_time);
      if (!owned) CHECK(valid == (count != 0));
      if (valid) {
        CHECK(sample_time >= d.timestamp && (head.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT));
        CHECK(std::abs(head.pose.position.x-d.px)<0.00001 &&
              std::abs(head.pose.position.y-d.py)<0.00001 && std::abs(head.pose.position.z-d.pz)<0.00001);
        ++head_poses;
      }
      if (pose) vit_pose_destroy(pose);
      ++count;
    }
    observed.clear();
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
      arrival = s.timestamp_ns + 2000000;
      CHECK(owned ? pico_slam_session_imu(session, 1, &s, 0, arrival) :
                    pico_slam_input_imu(input, 1, &s, 0, arrival));
    } else {
      CHECK(left[ci].size() == 2 && right[ci].size() == 2 &&
            left[ci][0] == right[ci][0]);
      int64_t time = std::stoll(left[ci][0]);
      arrival = time + 2000000;
      int64_t transport_offset = -10000 - static_cast<int64_t>(ci % 7);
      if (transport_input) {
        int64_t offsets[2] = {transport_offset, transport_offset};
        CHECK(!pico_camera_transport_queue(transport, offsets, arrival));
      }
      for (int i = 1; i >= 0; --i) {
        auto &r = i ? right[ci] : left[ci];
        CHECK(std::filesystem::path(r[1]).filename() == r[1]);
        auto path = root / (i ? "mav0/cam1/data" : "mav0/cam0/data") / r[1];
        auto image = cv::imread(path.string(), cv::IMREAD_GRAYSCALE);
        CHECK(image.cols == first.cols && image.rows == first.rows &&
              image.isContinuous());
        size_t size = image.total();
        if (native_input) {
          size_t stride = capture_input ? ((image.cols + 15) / 16 + 1) * 16 : image.cols + 17;
          std::vector<uint8_t> padded(stride * image.rows + 1, 0xee);
          for (int row = 0; row < image.rows; ++row)
            std::copy(image.ptr(row), image.ptr(row) + image.cols, padded.data() + 1 + row * stride);
          pico_slam_mono8_frame view{c.camera_ids[i], static_cast<uint32_t>(image.cols),
            static_cast<uint32_t>(image.rows), stride, padded.size() - 1, padded.data() + 1, time, ci};
          if (transport_input) {
            pico_camera_replay_feed(i, view.pixels, view.stride, time - transport_offset);
          } else if (capture_input) {
            pico_camera_capture_buffer buffer{&padded, ci + 1, 100 + i, 200 + i,
              c.camera_ids[i], view.width, view.height, static_cast<uint32_t>(stride),
              view.pixels, view.size};
            if (stream_input) {
              int64_t offset = -10000 - static_cast<int64_t>(ci % 7);
              CHECK(pico_camera_stream_reserve(camera_stream, &buffer, offset, arrival));
              pico_camera_event fence{};
              fence.type = 0x08000000;
              fence.sequence = event_sequence[0]++;
              put32(fence.data, 200+i); put32(fence.data+4, 2);
              put64(fence.data+8, 2); put64(fence.data+16, ci+1);
              CHECK(pico_camera_stream_event(camera_stream, PICO_CAMERA_SYNC_EVENTS, &fence, arrival) == PICO_CAMERA_CAPTURE_IGNORED);
              fence.sequence = event_sequence[0]++; put64(fence.data+8, 1);
              pico_camera_event sof{};
              sof.type = 0x08000000; sof.sequence = event_sequence[1]++;
              put32(sof.data, 77); put64(sof.data+8, ci+1); put64(sof.data+16, ci);
              put64(sof.data+24, time-offset); put32(sof.data+32, 100+i);
              if (ci % 2) {
                CHECK(pico_camera_stream_event(camera_stream, PICO_CAMERA_SYNC_EVENTS, &fence, arrival) == PICO_CAMERA_CAPTURE_PENDING);
                CHECK(pico_camera_stream_event(camera_stream, PICO_CAMERA_CRM_EVENTS, &sof, arrival) == PICO_CAMERA_CAPTURE_DELIVERED);
              } else {
                CHECK(pico_camera_stream_event(camera_stream, PICO_CAMERA_CRM_EVENTS, &sof, arrival) == PICO_CAMERA_CAPTURE_PENDING);
                CHECK(pico_camera_stream_event(camera_stream, PICO_CAMERA_SYNC_EVENTS, &fence, arrival) == PICO_CAMERA_CAPTURE_DELIVERED);
              }
            } else {
              CHECK(pico_camera_capture_reserve(capture_owner, &buffer, arrival));
              CHECK(pico_camera_capture_fence(capture_owner, 2, ci+1, 200+i, 2, arrival) == PICO_CAMERA_CAPTURE_IGNORED);
              if (ci % 2) {
                CHECK(pico_camera_capture_fence(capture_owner, 1, ci+1, 200+i, 2, arrival) == PICO_CAMERA_CAPTURE_PENDING);
                CHECK(pico_camera_capture_timing(capture_owner, 1, ci+1, c.camera_ids[i], ci, time, arrival) == PICO_CAMERA_CAPTURE_DELIVERED);
              } else {
                CHECK(pico_camera_capture_timing(capture_owner, 1, ci+1, c.camera_ids[i], ci, time, arrival) == PICO_CAMERA_CAPTURE_PENDING);
                CHECK(pico_camera_capture_fence(capture_owner, 1, ci+1, 200+i, 2, arrival) == PICO_CAMERA_CAPTURE_DELIVERED);
              }
            }
            CHECK(capture_sink.phase == 0 && padded[1] == 0xa5);
          } else CHECK(pico_slam_session_mono8(session, 1, &view, arrival));
          std::fill(padded.begin(), padded.end(), 0xa5);
        } else {
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
          CHECK(owned ? pico_slam_session_camera(session, 1, wire.data(), wire.size(), arrival) :
                        pico_slam_input_camera(input, 1, wire.data(), wire.size(), arrival));
          std::fill(wire.begin(), wire.end(), 0xa5);
        }
      }
      if (transport_input) CHECK(pico_camera_transport_poll(transport, arrival) == 4);
      ++ci;
    }
    pop();
    if (owned) {
      auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      while (delivered_images / 2 > count + 3 && std::chrono::steady_clock::now() < limit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        pop();
      }
      CHECK(delivered_images / 2 <= count + 3);
    }
  }
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (last_pose < std::stoll(left.back()[0]) &&
         std::chrono::steady_clock::now() < deadline) {
    pop();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  pico_slam_input_destroy(&input);
  pop();
  if (transport_input) {
    CHECK(!pico_camera_transport_destroy(&transport));
    capture_sink.releases = pico_camera_replay_finish();
    CHECK(capture_sink.frames == left.size()*2 && capture_sink.frames == capture_sink.releases && capture_sink.invalidations == 1);
    printf("PICO_SLAM_TRANSPORT_PASS buffers=%u simulated_ioctls=1 simulated_dma=1 poisoned=1\n", capture_sink.releases);
  } else if (capture_input) {
    if (stream_input) CHECK(pico_camera_stream_destroy(&camera_stream));
    else CHECK(pico_camera_capture_destroy(&capture_owner));
    CHECK(capture_sink.frames == left.size()*2 && capture_sink.releases == capture_sink.frames && capture_sink.invalidations == 1);
    printf("PICO_SLAM_CAPTURE_PASS buffers=%u simulated_fences=1 simulated_cache=1 poisoned=1 stopped_invalid=1\n", capture_sink.releases);
  }
  if (stream_input) {
    CHECK(event_sequence[0] < 1000 && event_sequence[1] < 1000);
    printf("PICO_SLAM_STREAM_PASS simulated_events=1 sequence_wrap=1 per_request_timing=1\n");
  }
  if (owned) pico_slam_session_stop(session);
  else {
    CHECK(vit_tracker_stop(tracker) == VIT_SUCCESS);
    vit_tracker_destroy(tracker);
    tracker = nullptr;
    pico_slam_tracking_stop(tracking);
  }
  xrt_space_relation stopped{};
  int64_t sample_time;
  CHECK(!pico_slam_tracking_get(tracking, owned ? arrival : last_pose, last_pose, &stopped, &sample_time));
  if (owned) {
    pico_slam_session_destroy(&session);
    CHECK(head_poses >= 180);
  } else {
    pico_slam_tracking_destroy(&tracking);
    CHECK(head_poses + 1 == count);
  }
  CHECK(count == left.size() - 1 && delivered_images == 2 * count &&
        delivered_imu == inertial.size());
  double span = 0;
  for (unsigned i = 0; i < 3; ++i)
    span += pow(maxpos[i] - minpos[i], 2);
  CHECK(span > 0.01 && span < 100);
  poses.close();
  if (native_input) printf("PICO_SLAM_NATIVE_INPUT_PASS padded=1 unaligned=1 poisoned=1\n");
  if (owned) printf("PICO_SLAM_SESSION_REPLAY_PASS poses=%u images=%u stopped_invalid=1\n", count, delivered_images);
  if (!owned) printf("PICO_SLAM_IMAGE_FORMAT_PASS bytes=%u padded=1 unaligned=1\n", image_bytes);
  printf("PICO_SLAM_HEAD_POSES_PASS poses=%u stopped_invalid=1\n", head_poses);
  printf("PICO_SLAM_REPLAY_PASS frames=%zu images=%u imu=%u poses=%u "
         "position_span=%.6f api=%u.%u.%u\n",
         left.size(), delivered_images, delivered_imu, count, sqrt(span), major,
         minor, patch);
}
