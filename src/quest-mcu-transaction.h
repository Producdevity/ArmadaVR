#ifndef REFRAMED_QUEST_MCU_TRANSACTION_H
#define REFRAMED_QUEST_MCU_TRANSACTION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct quest_mcu_transaction;
struct quest_mcu_transaction_config {
    int command_fd, reply_fd;
    uint64_t generation;
    int64_t timeout_ns;
    uint8_t command_type, reply_type;
    const void *payload;
    size_t payload_size, reply_size;
};

/* One command on exclusively borrowed Syncboss/control descriptors. The control
 * FIFO must already be nonblocking and clear of earlier replies for this owner.
 * Creation copies the payload; calls and invalidate are serial and non-reentrant.
 * The callback must invalidate consumers of this MCU generation. No nodes are
 * opened, enabled, reset or closed. No send, allocation or release is retried. */
bool quest_mcu_transaction_create(const struct quest_mcu_transaction_config *config,
    void (*invalidate)(void *opaque, uint64_t generation, int error), void *opaque,
    struct quest_mcu_transaction **transaction);
/* One attempt: allocate sequence then send. A failure can have changed MCU or
 * kernel state, including when allocation returns no sequence. Keep the owner. */
int quest_mcu_transaction_start(struct quest_mcu_transaction *transaction, int64_t now_ns);
/* Poll through silence: 0 pending, 1 exact reply delivered, negative errno on
 * failure. Validate current generation and monotonic time before reading. Output
 * is copied only after one successful sequence release, exactly once. A valid
 * reply describes transport completion; the caller must check firmware status. */
int quest_mcu_transaction_poll(struct quest_mcu_transaction *transaction,
    uint64_t generation, int64_t now_ns, void *reply, size_t size);
void quest_mcu_transaction_cancel(struct quest_mcu_transaction *transaction);
/* Only an unstarted or successfully completed owner can be destroyed normally.
 * On failure/cancellation after start, stop all users of both borrowed fds and
 * independently establish control I/O quiescence before closing both fds. Then
 * closed discards local state; it does not retry an uncertain sequence release
 * or establish that a camera command did not execute. */
bool quest_mcu_transaction_destroy(struct quest_mcu_transaction **transaction);
void quest_mcu_transaction_closed(struct quest_mcu_transaction **transaction);
#endif
