#define VIT_INTERFACE_IMPLEMENTATION
#include <vit/vit_interface.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(expression)                                                      \
  do {                                                                         \
    if (!(expression)) {                                                       \
      std::fprintf(stderr, "CHECK line %d: %s\n", __LINE__, #expression);      \
      std::abort();                                                            \
    }                                                                          \
  } while (0)

int main(int argc, char **argv) {
  CHECK(argc == 2);
  std::vector<uint8_t> pixels(960 * 960, 127);
  unsigned stopped = 0;
  for (unsigned cycle = 0; cycle < 10; ++cycle) {
    for (unsigned mode = 0; mode < 7; ++mode) {
      vit_config_t config{};
      config.file = argv[1];
      config.cam_count = 2;
      config.imu_count = 1;
      vit_tracker_t *tracker = nullptr;
      CHECK(vit_tracker_create(&config, &tracker) == VIT_SUCCESS && tracker);
      for (auto format : {VIT_IMAGE_FORMAT_L8, VIT_IMAGE_FORMAT_L16}) {
        bool supported = false;
        CHECK(vit_tracker_has_image_format(tracker, format, &supported) == VIT_SUCCESS && supported);
      }
      if (mode != 5) {
        CHECK(vit_tracker_start(tracker) == VIT_SUCCESS);
        bool running = false;
        CHECK(vit_tracker_is_running(tracker, &running) == VIT_SUCCESS &&
              running);
      }
      if (mode == 1 || mode == 4) {
        for (unsigned i = 0; i < 10; ++i) {
          vit_imu_sample_t sample{};
          sample.timestamp = 1000000000 + i * 1000000;
          sample.ay = 9.81;
          CHECK(vit_tracker_push_imu_sample(tracker, &sample) == VIT_SUCCESS);
        }
      }
      if ((mode >= 2 && mode <= 4) || mode == 6) {
        for (unsigned i = 0; i < (mode == 2 ? 1u : mode == 6 ? 20u : 2u); ++i) {
          vit_img_sample_t image{};
          image.timestamp = 1005000000 + (i / 2) * 20000000;
          image.cam_index = i % 2;
          image.data = pixels.data();
          image.width = image.height = 960;
          image.stride = 960;
          image.size = pixels.size();
          image.format = VIT_IMAGE_FORMAT_L8;
          CHECK(vit_tracker_push_img_sample(tracker, &image) == VIT_SUCCESS);
        }
      }
      CHECK(vit_tracker_stop(tracker) == VIT_SUCCESS);
      CHECK(vit_tracker_stop(tracker) == VIT_SUCCESS);
      bool running = true;
      CHECK(vit_tracker_is_running(tracker, &running) == VIT_SUCCESS &&
            !running);
      vit_pose_t *pose = nullptr;
      unsigned count = 0;
      do {
        CHECK(vit_tracker_pop_pose(tracker, &pose) == VIT_SUCCESS);
        if (pose) {
          vit_pose_data_t data{};
          CHECK(vit_pose_get_data(pose, &data) == VIT_SUCCESS);
          vit_pose_destroy(pose);
          CHECK(++count <= 1);
        }
      } while (pose);
      CHECK(vit_tracker_pop_pose(tracker, nullptr) == VIT_SUCCESS);
      vit_tracker_destroy(tracker);
      ++stopped;
    }
  }
  std::printf("BASALT_LIFECYCLE_PASS stopped=%u modes=7 cycles=10\n", stopped);
}
