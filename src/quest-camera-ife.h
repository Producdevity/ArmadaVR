#ifndef REFRAMED_QUEST_CAMERA_IFE_H
#define REFRAMED_QUEST_CAMERA_IFE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define QUEST_CAMERA_IFE_TITAN636 0x636100U
#define QUEST_CAMERA_IFE_COMMON_SIZE 12
#define QUEST_CAMERA_IFE_RDI_SIZE 44

struct quest_camera_ife_rdi {
    uint32_t revision, rdi, sample_pattern, width, height;
};

/* Titan636 Lite only, one unbatched uncropped RDI0..3 input. Includes the
 * metadata row in height. Caller must verify the acquired hardware revision,
 * physical RDI routing and exclusive ownership of the common bus setting.
 * No frame/line skipping, remote remapping or pixel processing. These commands
 * do not select clocks, CSI wiring or a safe sensor operating point. */
bool quest_camera_ife_common_command(uint32_t revision, void *output, size_t size);
bool quest_camera_ife_rdi_commands(const struct quest_camera_ife_rdi *config, void *output, size_t size);
#endif
