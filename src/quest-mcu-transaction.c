#include "quest-mcu-transaction.h"
#include "quest-mcu-control.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct quest_mcu_transaction {
    struct quest_mcu_transaction_config config;
    void (*invalidate)(void *, uint64_t, int);
    void *opaque;
    uint8_t payload[252], sequence;
    int64_t last_ns, deadline_ns;
    int error;
    bool started, complete;
};
static int fail(struct quest_mcu_transaction *t, int error)
{
    if (!t->error) {
        t->error = error;
        t->invalidate(t->opaque, t->config.generation, error);
    }
    return t->error;
}
bool quest_mcu_transaction_create(const struct quest_mcu_transaction_config *c,
    void (*invalidate)(void *, uint64_t, int), void *opaque,
    struct quest_mcu_transaction **transaction)
{
    if (!c || !invalidate || !transaction || *transaction || c->command_fd < 0 ||
        c->reply_fd < 0 || c->command_fd == c->reply_fd || !c->generation ||
        c->timeout_ns <= 0 || c->timeout_ns > INT64_C(10000000000) ||
        !c->command_type || !c->reply_type || c->payload_size > 252 || c->reply_size > 252 ||
        (c->payload_size && !c->payload)) return false;
    struct quest_mcu_transaction *t = calloc(1, sizeof(*t));
    if (!t) return false;
    t->config = *c; t->invalidate = invalidate; t->opaque = opaque;
    if (c->payload_size) memcpy(t->payload, c->payload, c->payload_size);
    t->config.payload = t->payload;
    *transaction = t;
    return true;
}
int quest_mcu_transaction_start(struct quest_mcu_transaction *t, int64_t now)
{
    if (!t || now <= 0 || now > INT64_MAX - t->config.timeout_ns) return -EINVAL;
    if (t->error) return t->error;
    if (t->started) return -EALREADY;
    t->started = true; t->last_ns = now; t->deadline_ns = now + t->config.timeout_ns;
    int error = quest_mcu_sequence_allocate(t->config.command_fd, &t->sequence);
    if (error) return fail(t, error);
    error = quest_mcu_packet_send(t->config.command_fd, t->config.command_type,
                                  t->sequence, t->payload, t->config.payload_size);
    return error ? fail(t, error) : 0;
}
int quest_mcu_transaction_poll(struct quest_mcu_transaction *t, uint64_t generation,
    int64_t now, void *reply, size_t size)
{
    if (!t || size != t->config.reply_size || (size && !reply)) return -EINVAL;
    if (t->error) return t->error;
    if (t->complete) return -EALREADY;
    if (!t->started) return -EINVAL;
    if (generation != t->config.generation) return fail(t, -ESTALE);
    if (now <= 0 || now < t->last_ns) return fail(t, -ERANGE);
    if (now >= t->deadline_ns) return fail(t, -ETIMEDOUT);
    t->last_ns = now;
    uint8_t bytes[252];
    int status = quest_mcu_reply_receive(t->config.reply_fd, t->config.reply_type,
                                        t->sequence, bytes, size);
    if (status < 0) return fail(t, status);
    if (!status) return 0;
    int error = quest_mcu_sequence_release(t->config.command_fd, &t->sequence);
    if (error) return fail(t, error);
    t->complete = true;
    if (size) memcpy(reply, bytes, size);
    return 1;
}
void quest_mcu_transaction_cancel(struct quest_mcu_transaction *t)
{
    if (t && !t->complete) fail(t, -ECANCELED);
}
bool quest_mcu_transaction_destroy(struct quest_mcu_transaction **transaction)
{
    if (!transaction) return false;
    if (!*transaction) return true;
    if ((*transaction)->started && !(*transaction)->complete) return false;
    free(*transaction); *transaction = NULL;
    return true;
}
void quest_mcu_transaction_closed(struct quest_mcu_transaction **transaction)
{
    if (transaction) { free(*transaction); *transaction = NULL; }
}
