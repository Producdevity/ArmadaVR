#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libqrtr.h>

struct decoded {
    char name[16];
    unsigned int number;
    unsigned int length;
    unsigned char bytes[8];
};

static struct qmi_elem_info fields[] = {
    {.data_type=QMI_STRING, .elem_len=16, .elem_size=1, .tlv_type=1,
     .offset=offsetof(struct decoded, name)},
    {.data_type=QMI_UNSIGNED_4_BYTE, .elem_len=1, .elem_size=4, .tlv_type=2,
     .offset=offsetof(struct decoded, number)},
    {.data_type=QMI_DATA_LEN, .elem_len=1, .elem_size=1, .tlv_type=3,
     .offset=offsetof(struct decoded, length)},
    {.data_type=QMI_UNSIGNED_1_BYTE, .elem_len=8, .elem_size=1, .tlv_type=3,
     .array_type=VAR_LEN_ARRAY, .offset=offsetof(struct decoded, bytes)},
    {0}
};

static int decode(const unsigned char *data, size_t size, struct decoded *out)
{
    unsigned char *copy = malloc(size ? size : 1);
    assert(copy);
    memcpy(copy, data, size);
    struct qrtr_packet packet = {.data=copy, .data_len=size};
    unsigned int txn = 0;
    int result = qmi_decode_message(out, &txn, &packet, QMI_REQUEST, 33, fields);
    free(copy);
    return result;
}

int main(void)
{
    unsigned char valid[] = {0, 100, 0, 33, 0, 6, 0, 1, 3, 0, 'a', 'd', 's'};
    struct decoded out = {0};
    assert(decode(valid, sizeof(valid), &out) >= 0);
    assert(!strcmp(out.name, "ads"));
    for (size_t size=0; size<sizeof(valid); ++size) {
        assert(decode(valid, size, &out) < 0);
        unsigned char *copy=malloc(size ? size : 1);
        assert(copy);
        memcpy(copy, valid, size);
        struct qrtr_packet packet={.data=copy, .data_len=size};
        unsigned int id=0;
        assert(qmi_decode_header(&packet, &id) < 0);
        free(copy);
    }
    unsigned char truncated_tlv[] = {0,100,0,33,0,2,0,1,7};
    unsigned char truncated_payload[] = {0,100,0,33,0,3,0,1,7,0};
    unsigned char unknown_optional[] = {0,100,0,33,0,3,0,32,7,0};
    unsigned char short_scalar[] = {0,100,0,33,0,4,0,2,1,0,1};
    unsigned char short_array_length[] = {0,100,0,33,0,3,0,3,0,0};
    unsigned char short_array_data[] = {0,100,0,33,0,5,0,3,2,0,2,1};
    assert(decode(truncated_tlv,sizeof(truncated_tlv),&out)<0);
    assert(decode(truncated_payload,sizeof(truncated_payload),&out)<0);
    assert(decode(unknown_optional,sizeof(unknown_optional),&out)<0);
    assert(decode(short_scalar,sizeof(short_scalar),&out)<0);
    assert(decode(short_array_length,sizeof(short_array_length),&out)<0);
    assert(decode(short_array_data,sizeof(short_array_data),&out)<0);
    unsigned char full_string[26]={0,100,0,33,0,19,0,1,16,0};
    memset(full_string+10,'x',16);
    assert(decode(full_string,sizeof(full_string),&out)<0);
    unsigned char scalar[] = {0,100,0,33,0,7,0,2,4,0,74,0,0,0};
    assert(decode(scalar,sizeof(scalar),&out)>=0 && out.number==74);
    unsigned char array[] = {0,100,0,33,0,6,0,3,3,0,2,7,8};
    assert(decode(array,sizeof(array),&out)>=0 && out.length==2 && out.bytes[1]==8);
    puts("QMI_BOUNDS_PASS");
    return 0;
}
