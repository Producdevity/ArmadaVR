#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "servreg_loc.h"

static void require(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "ARMADA_QMI_FAIL: %s (errno=%d)\n", message, errno);
        exit(1);
    }
}

static long milliseconds(void)
{
    struct timespec value;
    require(clock_gettime(CLOCK_MONOTONIC, &value) == 0, "monotonic clock");
    return value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static struct qrtr_packet receive_packet(int fd, char *buffer, size_t size)
{
    struct sockaddr_qrtr address = {0};
    socklen_t length = sizeof(address);
    ssize_t received = recvfrom(fd, buffer, size, 0, (void *)&address, &length);
    require(received > 0 && length == sizeof(address), "receive QRTR packet");
    struct qrtr_packet packet = {0};
    require(qrtr_decode(&packet, buffer, received, &address) == 0, "decode QRTR packet");
    return packet;
}

static struct qrtr_packet discover(int fd)
{
    require(qrtr_new_lookup(fd, 64, 1, 1) == 0, "lookup service 64 version 1 instance 1");
    long deadline = milliseconds() + 5000;
    char buffer[4096];
    while (milliseconds() < deadline) {
        int ready = qrtr_poll(fd, 100);
        require(ready >= 0, "poll service discovery");
        if (!ready)
            continue;
        struct qrtr_packet packet = receive_packet(fd, buffer, sizeof(buffer));
        if (packet.type == QRTR_TYPE_NEW_SERVER && packet.service == 64 &&
            packet.version == 1 && packet.instance == 1 && packet.node && packet.port) {
            printf("ARMADA_QMI_DISCOVERED node=%u port=%u version=%u instance=%u\n",
                   packet.node, packet.port, packet.version, packet.instance);
            return packet;
        }
    }
    require(false, "service discovery timed out");
    return (struct qrtr_packet){0};
}

static void query(int fd, const struct qrtr_packet *server, unsigned int transaction,
                  const char *name, unsigned int domains, bool charger, bool malformed)
{
    struct servreg_loc_get_domain_list_req request = {0};
    struct servreg_loc_get_domain_list_resp response = {0};
    DEFINE_QRTR_PACKET(encoded, 512);
    snprintf(request.name, sizeof(request.name), "%s", name);
    /* The vendor PDR client encodes a string as NO_ARRAY. The mapper's
     * VAR_LEN_ARRAY request descriptor is used only for decoding upstream. */
    struct qmi_elem_info request_ei[] = {{
        .data_type = QMI_STRING, .elem_len = sizeof(request.name),
        .elem_size = sizeof(char), .array_type = NO_ARRAY, .tlv_type = 1,
        .offset = offsetof(struct servreg_loc_get_domain_list_req, name),
    }, {0}};
    ssize_t size = malformed ? 1 : qmi_encode_message(&encoded, QMI_REQUEST, SERVREG_LOC_GET_DOMAIN_LIST,
                                    transaction, &request, request_ei);
    require(size > 0, "encode QMI request");
    if (malformed) {
        unsigned char invalid[] = {0, transaction & 255, transaction >> 8, 33, 0, 3, 0, 1, 7, 0};
        require(qrtr_sendto(fd, server->node, server->port, invalid, sizeof(invalid)) == 0,
                "send malformed QMI request");
    } else {
        require(qrtr_sendto(fd, server->node, server->port, encoded.data, encoded.data_len) == 0,
                "send QMI request");
    }
    char buffer[8192];
    long deadline = milliseconds() + 2000;
    while (milliseconds() < deadline) {
        int ready = qrtr_poll(fd, 100);
        require(ready >= 0, "poll QMI response");
        if (!ready)
            continue;
        struct qrtr_packet packet = receive_packet(fd, buffer, sizeof(buffer));
        if (packet.type != QRTR_TYPE_DATA)
            continue;
        require(packet.node == server->node && packet.port == server->port, "response server identity");
        unsigned int actual_transaction = 0;
        require(qmi_decode_message(&response, &actual_transaction, &packet, QMI_RESPONSE,
                SERVREG_LOC_GET_DOMAIN_LIST, servreg_loc_get_domain_list_resp_ei) >= 0, "decode QMI response");
        require(actual_transaction == transaction, "response transaction identity");
        if (malformed) {
            require(response.result.result == QMI_RESULT_FAILURE &&
                    response.result.error == QMI_ERR_MALFORMED_MSG, "malformed request must fail");
            return;
        }
        require(response.result.result == QMI_RESULT_SUCCESS && response.result.error == 0, "query result");
        require(response.total_domains_valid && response.total_domains == domains &&
                response.domain_list_len == domains, "exact domain count");
        require(response.domain_list_valid == (domains != 0), "domain list presence");
        require(response.db_revision_valid && response.db_revision == 1, "service map revision");
        const char *expected[] = {"msm/adsp/root_pd", "msm/adsp/sensor_pd",
                                  "msm/adsp/audio_pd", "msm/adsp/charger_pd"};
        unsigned int seen = 0;
        bool found_charger = false;
        for (unsigned int i = 0; i < domains; ++i) {
            require(response.domain_list[i].instance_id == 74, "firmware QMI instance");
            unsigned int match = 4;
            for (unsigned int j = 0; j < 4; ++j)
                if (!strcmp(response.domain_list[i].name, expected[j]))
                    match = j;
            require(match < 4 && !(seen & (1u << match)), "unique expected domain");
            seen |= 1u << match;
            if (!strcmp(response.domain_list[i].name, "msm/adsp/charger_pd"))
                found_charger = true;
        }
        require(found_charger == charger, "charger domain mapping");
        return;
    }
    require(false, "QMI response timed out");
}

int main(void)
{
    FILE *file = fopen("/proc/device-tree/compatible", "rb");
    require(file != NULL, "QEMU identity file");
    char compatible[128] = {0};
    size_t length = fread(compatible, 1, sizeof(compatible) - 1, file);
    fclose(file);
    require(length > 0 && !strcmp(compatible, "linux,dummy-virt"), "QEMU-only probe");
    int fd = qrtr_open(0);
    require(fd >= 0, "open QRTR socket");
    struct qrtr_packet server = discover(fd);
    for (unsigned int i = 0; i < 32; ++i) {
        query(fd, &server, 100 + i * 3, "tms/servreg", 4, true, false);
        query(fd, &server, 101 + i * 3, "armada/missing", 0, false, false);
        query(fd, &server, 102 + i * 3, "", 0, false, true);
    }
    qrtr_close(fd);
    puts("ARMADA_QMI_PASS requests=96 charger=msm/adsp/charger_pd instance=74");
    return 0;
}
