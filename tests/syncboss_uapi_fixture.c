#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#include "syncboss.h"

_Static_assert(sizeof(struct syncboss_driver_data_header_v2_t) == 12, "v2 ABI changed");
_Static_assert(sizeof(struct syncboss_driver_data_header_v3_t) == 21, "v3 ABI changed");

int main(void)
{
    struct uapi_pkt_v2_t v2 = {
        .header = {.header_version = 2, .header_length = sizeof(v2.header),
                   .nsync_offset_status = SYNCBOSS_TIME_OFFSET_VALID, .nsync_offset_us = -12345},
        .payload = {201, 0, 4, 0xde, 0xad, 0xbe, 0xef},
    };
    struct uapi_pkt_v3_t v3 = {
        .header = {.header_version = 3, .header_length = sizeof(v3.header),
                   .nsync_offset_status = SYNCBOSS_TIME_OFFSET_VALID, .nsync_offset_us = 54321,
                   .remote_offset_status = SYNCBOSS_TIME_OFFSET_ERROR, .remote_offset_us = 999},
        .payload = {202, 7, 1, 0xaa},
    };
    struct syncboss_driver_data_header_driver_message_v3_t driver = {
        .header = {.header_version = 3, .header_length = sizeof(driver.header), .from_driver = true},
        .driver_message_type = SYNCBOSS_DRIVER_MESSAGE_POWERSTATE_MSG,
        .driver_message_data = SYNCBOSS_PROX_EVENT_SYSTEM_DOWN,
    };
    if (fwrite(&v2, sizeof(v2.header) + 7, 1, stdout) != 1 ||
        fwrite(&v3, sizeof(v3.header) + 4, 1, stdout) != 1 ||
        fwrite(&driver, sizeof(driver), 1, stdout) != 1)
        return 1;
    return fflush(stdout) != 0;
}
