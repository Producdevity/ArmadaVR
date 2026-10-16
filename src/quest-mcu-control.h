#ifndef REFRAMED_QUEST_MCU_CONTROL_H
#define REFRAMED_QUEST_MCU_CONTROL_H
#include <stddef.h>
#include <stdint.h>

/* Borrowed, exclusively owned descriptors: sequence allocation and writes use
 * syncboss0; receive uses a separately opened nonblocking syncboss_control0 FIFO.
 * No node is opened, closed, reset or enabled here. Serialize calls and retain
 * sequence ownership through response completion. Stop using the channel on MCU
 * restart, timeout or ambiguous failure; these calls do not prove quiescence.
 * Allocation starts with *sequence zero. Release clears it only on success.
 * No syscall is retried; a failed/short write may already have reached the MCU. */
int quest_mcu_sequence_allocate(int fd, uint8_t *sequence);
int quest_mcu_sequence_release(int fd, uint8_t *sequence);
int quest_mcu_packet_send(int fd, uint8_t type, uint8_t sequence, const void *payload, size_t size);

/* Read at most one copied FIFO record. Return 1 for an exact reply, 0 for EAGAIN
 * or a complete response belonging to another sequence, and negative errno on
 * failure. Headers v2/v3 are supported; driver messages invalidate this channel.
 * Validate record/type/sequence/length before copying. Failure or no match leaves
 * output unchanged. The owner enforces deadlines and invalidates stale queries. */
int quest_mcu_reply_receive(int fd, uint8_t type, uint8_t sequence, void *payload, size_t size);
#endif
