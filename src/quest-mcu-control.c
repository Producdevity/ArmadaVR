#include "quest-mcu-control.h"
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int result(int value)
{
    return value == 0 ? 0 : value < 0 ? -errno : -EPROTO;
}
static int valid_sequence(uint8_t sequence) { return sequence > 0 && sequence < 255; }

int quest_mcu_sequence_allocate(int fd, uint8_t *sequence)
{
    if (fd < 0 || !sequence || *sequence) return -EINVAL;
    uint8_t value = 0;
    int error = result(ioctl(fd, 0x80010a02UL, &value));
    if (error) return error;
    if (!valid_sequence(value)) return -EPROTO;
    *sequence = value;
    return 0;
}
int quest_mcu_sequence_release(int fd, uint8_t *sequence)
{
    if (fd < 0 || !sequence || !valid_sequence(*sequence)) return -EINVAL;
    uint8_t value = *sequence;
    int error = result(ioctl(fd, 0x40010a03UL, &value));
    if (!error) *sequence = 0;
    return error;
}
int quest_mcu_packet_send(int fd, uint8_t type, uint8_t sequence, const void *payload, size_t size)
{
    if (fd < 0 || !type || !valid_sequence(sequence) || size > 252 || (size && !payload)) return -EINVAL;
    uint8_t packet[255] = {type, sequence, (uint8_t)size};
    if (size) memcpy(packet + 3, payload, size);
    ssize_t written = write(fd, packet, size + 3);
    return written < 0 ? -errno : (size_t)written == size + 3 ? 0 : -EIO;
}
int quest_mcu_reply_receive(int fd, uint8_t type, uint8_t sequence, void *payload, size_t size)
{
    if (fd < 0 || !type || !valid_sequence(sequence) || size > 252 || (size && !payload)) return -EINVAL;
    uint8_t packet[276];
    ssize_t received = read(fd, packet, sizeof(packet));
    if (received < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -errno;
    if (!received) return -EPIPE;
    if (received < 3 || (size_t)received > sizeof(packet) ||
        !((packet[0] == 2 && packet[1] == 12) || (packet[0] == 3 && packet[1] == 21))) return -EPROTO;
    size_t header = packet[1];
    if ((size_t)received < header || packet[2] > 1) return -EPROTO;
    if (packet[2]) return -EPIPE;
    if ((size_t)received < header + 3 || !packet[header] || !valid_sequence(packet[header + 1]) ||
        packet[header + 2] > 252 || (size_t)received != header + 3 + packet[header + 2]) return -EPROTO;
    if (packet[header + 1] != sequence) return 0;
    if (packet[header] != type || packet[header + 2] != size) return -EPROTO;
    if (size) memcpy(payload, packet + header + 3, size);
    return 1;
}
