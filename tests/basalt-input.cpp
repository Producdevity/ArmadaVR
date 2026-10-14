#define VIT_INTERFACE_IMPLEMENTATION
#include <vit/vit_interface.h>
#include <cassert>
#include <cstdio>
#include <vector>

int main(int argc, char **argv)
{
    assert(argc == 2);
    std::vector<uint8_t> pixels(960 * 960, 127);
    unsigned exhausted = 0, malformed = 0;
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        for (unsigned mode = 0; mode < 10; ++mode) {
            vit_config_t config{};
            config.file = argv[1]; config.cam_count = 2; config.imu_count = 1;
            vit_tracker_t *tracker = nullptr;
            assert(vit_tracker_create(&config, &tracker) == VIT_SUCCESS && tracker);
            vit_imu_sample_t imu{};
            imu.timestamp = 1000000000; imu.ay = 9.81;
            assert(vit_tracker_push_imu_sample(tracker, &imu) == VIT_ERROR_INVALID_VALUE);
            assert(vit_tracker_start(tracker) == VIT_SUCCESS);
            vit_img_sample_t image{};
            image.timestamp = 1000000000; image.data = pixels.data();
            image.width = image.height = image.stride = 960;
            image.size = pixels.size(); image.format = VIT_IMAGE_FORMAT_L8;
            vit_result_t result = VIT_SUCCESS;
            if (mode == 0) {
                unsigned i = 0;
                for (; i < 1000 && result == VIT_SUCCESS; ++i) {
                    imu.timestamp += 1000000;
                    result = vit_tracker_push_imu_sample(tracker, &imu);
                }
                assert(result == VIT_ERROR_ALLOCATION_FAILURE && i > 100 && i < 1000);
                ++exhausted;
            } else if (mode == 1) {
                unsigned i = 0;
                for (; i < 200 && result == VIT_SUCCESS; ++i) {
                    image.cam_index = i % 2; image.timestamp = 1000000000 + (i / 2) * 20000000;
                    result = vit_tracker_push_img_sample(tracker, &image);
                }
                assert(result == VIT_ERROR_ALLOCATION_FAILURE && i < 200);
                ++exhausted;
            } else {
                switch (mode) {
                case 2: image.format = VIT_IMAGE_FORMAT_R8G8B8; break;
                case 3: image.stride = 959; break;
                case 4: --image.size; break;
                case 5: image.width = 0; break;
                case 6: image.cam_index = 1; break;
                case 7: image.mask_count = 1; break;
                case 8: image.data = nullptr; break;
                case 9:
                    assert(vit_tracker_push_img_sample(tracker, &image) == VIT_SUCCESS);
                    image.cam_index = 1; ++image.timestamp;
                    break;
                }
                assert(vit_tracker_push_img_sample(tracker, &image) == VIT_ERROR_INVALID_VALUE);
                ++malformed;
            }
            assert(vit_tracker_push_imu_sample(tracker, &imu) == VIT_ERROR_INVALID_VALUE);
            assert(vit_tracker_push_img_sample(tracker, &image) == VIT_ERROR_INVALID_VALUE);
            assert(vit_tracker_stop(tracker) == VIT_SUCCESS);
            assert(vit_tracker_stop(tracker) == VIT_SUCCESS);
            assert(vit_tracker_push_imu_sample(tracker, &imu) == VIT_ERROR_INVALID_VALUE);
            vit_tracker_destroy(tracker);
        }
    }
    std::printf("BASALT_INPUT_PASS exhausted=%u malformed=%u stopped=30\n", exhausted, malformed);
}
