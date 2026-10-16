#include "quest-camera-mcu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

static unsigned settings(void)
{
    const unsigned settings[] = {0xa7, 0xa0, 0xa4, 0xa5, 0x66, 0xdd, 0x31};
    unsigned cases = 0;
    struct quest_camera_mcu_setting_record records[15];
    uint8_t output[96];
    for (unsigned s = 0; s < sizeof(settings) / sizeof(settings[0]); ++s) {
        unsigned setting = settings[s];
        bool single = setting == 0x66 || setting == 0xdd || setting == 0x31;
        size_t width = setting == 0xa5 ? 6 : 5;
        for (unsigned count = 1; count <= (single ? 1u : 15u); ++count)
        for (unsigned seq = 1; seq <= 254; ++seq) {
            for (unsigned i = 0; i < count; ++i) records[i] = (struct quest_camera_mcu_setting_record){
                UINT32_C(1) << ((i + seq) % 32), (uint8_t)(setting == 0x31 ? seq & 1 : seq + i),
                (uint8_t)(setting == 0xa5 ? 255 - i : 0)};
            size_t written = 999, size = 4 + count * width;
            memset(output, 0xa5, sizeof(output));
            assert(quest_camera_mcu_setting_encode(setting, records, count, seq, output + 1, size, &written));
            assert(written == size && output[0] == 0xa5 && output[size + 1] == 0xa5);
            assert(output[1] == 3 && output[2] == seq && output[3] == size - 3 && output[4] == setting);
            for (unsigned i = 0; i < count; ++i) {
                uint32_t mask; memcpy(&mask, output + 5 + i * width, 4);
                assert(mask == records[i].camera_mask && output[9 + i * width] == records[i].value);
                if (width == 6) assert(output[10 + i * width] == records[i].second);
            }
            ++cases;
        }
    }
    for (unsigned trial = 0; trial < 18; ++trial) {
        unsigned setting = 0xa5, seq = 1; size_t count = 2, cap = sizeof(output), written = 999;
        records[0] = (struct quest_camera_mcu_setting_record){1, 0, 0};
        records[1] = (struct quest_camera_mcu_setting_record){2, 0, 0};
        switch (trial) {
        case 0: count = 0; break;
        case 1: count = 16; break;
        case 2: count = SIZE_MAX; break;
        case 3: setting = 0x2c; break;
        case 4: seq = 0; break;
        case 5: seq = 255; break;
        case 6: records[0].camera_mask = 0; break;
        case 7: records[1].camera_mask = 3; break;
        case 8: setting = 0x66; break;
        case 9: setting = 0xdd; break;
        case 10: setting = 0x31; break;
        case 11: setting = 0x31; count = 1; records[0].value = 2; break;
        case 12: setting = 0xa7; records[1].second = 1; break;
        case 13: cap = 15; break;
        case 14: cap = 0; break;
        default: break;
        }
        memset(output, 0xa5, sizeof(output));
        assert(!quest_camera_mcu_setting_encode(setting, trial == 15 ? NULL : records, count,
            seq, trial == 16 ? NULL : output, cap, trial == 17 ? NULL : &written));
        assert(written == 999);
        for (size_t i = 0; i < sizeof(output); ++i) assert(output[i] == 0xa5);
        ++cases;
    }
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *guard = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(guard != MAP_FAILED && !mprotect(guard + page, page, PROT_NONE));
    for (unsigned count = 1; count <= 15; ++count) {
        for (unsigned i = 0; i < count; ++i)
            records[i] = (struct quest_camera_mcu_setting_record){UINT32_C(1) << i, 255, 255};
        size_t length = 4 + count * 6;
        for (size_t capacity = 0; capacity <= length; ++capacity) {
            size_t written = 999; uint8_t *end = guard + page - capacity;
            memset(end, 0xa5, capacity);
            assert(quest_camera_mcu_setting_encode(0xa5, records, count, 254, end, capacity, &written) == (capacity == length));
            assert(written == (capacity == length ? length : 999));
            if (capacity < length) for (size_t i = 0; i < capacity; ++i) assert(end[i] == 0xa5);
            ++cases;
        }
    }
    assert(!munmap(guard, page * 2));
    return cases;
}

static unsigned frame_settings(void)
{
    unsigned cases = 0;
    struct quest_camera_mcu_period periods[15];
    struct quest_camera_mcu_frame frames[2];
    uint8_t output[216];
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *guard = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(guard != MAP_FAILED && !mprotect(guard + page, page, PROT_NONE));
    for (unsigned count = 1; count <= 15; ++count) {
        const uint32_t values[] = {1, 65535, 65536, 66666, UINT32_MAX};
        for (unsigned i = 0; i < count; ++i) periods[i] = (struct quest_camera_mcu_period){UINT32_C(1) << i, values[i % 5]};
        size_t length = 4 + count * 8;
        for (size_t capacity = 0; capacity <= length; ++capacity) {
            size_t written = 999; uint8_t *end = guard + page - capacity; memset(end, 0xa5, capacity);
            assert(quest_camera_mcu_period_encode(periods, count, 254, end, capacity, &written) == (capacity == length));
            assert(written == (capacity == length ? length : 999));
            if (capacity < length) for (size_t i = 0; i < capacity; ++i) assert(end[i] == 0xa5);
            else for (unsigned i = 0; i < count; ++i) {
                uint32_t mask, period; memcpy(&mask, end + 4 + i * 8, 4); memcpy(&period, end + 8 + i * 8, 4);
                assert(mask == periods[i].camera_mask && period == periods[i].period);
            }
            ++cases;
        }
    }
    for (unsigned count = 1; count <= 2; ++count)
    for (unsigned resolution = 0; resolution < 3; ++resolution) {
        frames[0] = (struct quest_camera_mcu_frame){1, 0, 0, UINT16_MAX, resolution};
        frames[1] = (struct quest_camera_mcu_frame){1, UINT32_MAX, UINT16_MAX, 0, resolution};
        size_t length = 4 + count * 105;
        for (size_t capacity = 0; capacity <= length; ++capacity) {
            size_t written = 999; uint8_t *end = guard + page - capacity; memset(end, 0xa5, capacity);
            assert(quest_camera_mcu_frames_encode(frames, count, end, capacity, &written) == (capacity == length));
            assert(written == (capacity == length ? length : 999));
            if (capacity < length) for (size_t i = 0; i < capacity; ++i) assert(end[i] == 0xa5);
            else {
                assert(end[0] == 3 && end[1] == 0 && end[2] == length - 3 && end[3] == 0xa1);
                for (unsigned i = 0; i < count; ++i) {
                    const uint8_t *frame = end + 4 + i * 105;
                    for (unsigned j = 12; j < 105; ++j) assert(frame[j] == (j == 20 ? resolution : 0));
                }
            }
            ++cases;
        }
    }
    for (unsigned trial = 0; trial < 13; ++trial) {
        periods[0] = (struct quest_camera_mcu_period){1, 66666}; periods[1] = (struct quest_camera_mcu_period){2, 1000};
        size_t count = 2, written = 999; unsigned seq = 1;
        switch (trial) {
        case 0: count = 0; break; case 1: count = 16; break; case 2: count = SIZE_MAX; break;
        case 3: seq = 0; break; case 4: seq = 255; break; case 5: periods[0].camera_mask = 0; break;
        case 6: periods[1].camera_mask = 3; break; case 7: periods[1].period = 0; break; default: break;
        }
        memset(output, 0xa5, sizeof(output));
        if (trial < 12) assert(!quest_camera_mcu_period_encode(trial == 8 ? NULL : periods, count, seq,
            trial == 9 ? NULL : output, trial == 10 ? 19 : sizeof(output), trial == 11 ? NULL : &written));
        else {
            periods[1].camera_mask = 3;
            assert(!quest_camera_mcu_period_encode(periods, 2, 1, output, sizeof(output), &written));
        }
        assert(written == 999); for (unsigned i = 0; i < sizeof(output); ++i) assert(output[i] == 0xa5); ++cases;
    }
    for (unsigned trial = 0; trial < 11; ++trial) {
        frames[0] = (struct quest_camera_mcu_frame){1, 0, 100, 20, 0}; frames[1] = (struct quest_camera_mcu_frame){2, 0, 200, 30, 2};
        size_t count = 2, written = 999;
        switch (trial) {
        case 0: count = 0; break; case 1: count = 3; break; case 2: count = SIZE_MAX; break;
        case 3: frames[0].camera_mask = 0; break; case 4: frames[1].camera_mask = 3; break;
        case 5: frames[1].resolution = 3; break; case 6: frames[1].resolution = 255; break; default: break;
        }
        memset(output, 0xa5, sizeof(output));
        assert(!quest_camera_mcu_frames_encode(trial == 7 ? NULL : frames, count, trial == 8 ? NULL : output,
            trial == 9 ? 213 : sizeof(output), trial == 10 ? NULL : &written));
        assert(written == 999); for (unsigned i = 0; i < sizeof(output); ++i) assert(output[i] == 0xa5); ++cases;
    }
    frames[0] = (struct quest_camera_mcu_frame){1, 0, 5000, 48, 1};
    frames[1] = (struct quest_camera_mcu_frame){2, 1, 4000, 50, 2};
    int fds[2]; assert(!pipe(fds)); assert(!fcntl(fds[0], F_SETFL, O_NONBLOCK)); assert(!fcntl(fds[1], F_SETFL, O_NONBLOCK));
    for (size_t count = 1; count <= 2; ++count) {
        size_t written; uint8_t actual[214];
        assert(quest_camera_mcu_frames_encode(frames, count, output, sizeof(output), &written));
        assert(!quest_camera_mcu_frames_send(fds[1], frames, count));
        assert(read(fds[0], actual, sizeof(actual)) == (ssize_t)written && !memcmp(actual, output, written)); ++cases;
    }
    assert(quest_camera_mcu_frames_send(-1, frames, 1) == -EINVAL);
    assert(quest_camera_mcu_frames_send(fds[1], NULL, 1) == -EINVAL);
    assert(quest_camera_mcu_frames_send(fds[1], frames, 3) == -EINVAL);
    assert(read(fds[0], output, sizeof(output)) == -1 && errno == EAGAIN);
    uint8_t fill[4096] = {0};
    while (write(fds[1], fill, sizeof(fill)) > 0) {}
    assert(errno == EAGAIN && quest_camera_mcu_frames_send(fds[1], frames, 2) == -EAGAIN);
    assert(!close(fds[1])); assert(quest_camera_mcu_frames_send(fds[1], frames, 1) == -EBADF); assert(!close(fds[0]));
    assert(!munmap(guard, page * 2));
    return cases + 5;
}

int main(void)
{
    unsigned cases=settings()+frame_settings();
    for (unsigned op = 0x2c; op <= 0x2f; ++op)
    for (unsigned seq = 1; seq <= 254; ++seq)
    for (unsigned bit = 0; bit < 32; ++bit) {
        uint32_t mask = (UINT32_C(1) << bit) | (bit & 1 ? UINT32_C(0x80000001) : 0);
        uint8_t command[9]; memset(command, 0xa5, sizeof(command));
        assert(quest_camera_mcu_command_encode(op, mask, seq, command + 1, 7));
        assert(command[0] == 0xa5 && command[8] == 0xa5 && command[1] == op && command[2] == seq && command[3] == 4);
        for (unsigned i = 0; i < 4; ++i) assert(command[4 + i] == (uint8_t)(mask >> (8 * i)));
        int32_t status; uint32_t actual;
        assert(quest_camera_mcu_status_decode(command + 4, 4, &status));
        memcpy(&actual, &status, 4); assert(actual == mask); ++cases;
    }
    for (unsigned op = 0; op <= 255; ++op) {
        uint8_t command[7]; memset(command, 0xa5, sizeof(command));
        bool valid = op >= 0x2c && op <= 0x2f;
        assert(quest_camera_mcu_command_encode(op, 1, 1, command, 7) == valid);
        if (!valid) for (unsigned i = 0; i < 7; ++i) assert(command[i] == 0xa5);
    }
    uint8_t command[7]; memset(command, 0xa5, sizeof(command));
    for (unsigned size = 0; size < 7; ++size) assert(!quest_camera_mcu_command_encode(0x2c, 1, 1, command, size));
    assert(!quest_camera_mcu_command_encode(0x2c, 0, 1, command, 7));
    assert(!quest_camera_mcu_command_encode(0x2c, 1, 0, command, 7));
    assert(!quest_camera_mcu_command_encode(0x2c, 1, 255, command, 7));
    assert(!quest_camera_mcu_command_encode(0x2c, 1, 1, NULL, 7));
    for (unsigned i = 0; i < 7; ++i) assert(command[i] == 0xa5);
    int32_t status = 17;
    for (unsigned size = 0; size < 7; ++size) if (size != 4) {
        assert(!quest_camera_mcu_status_decode(command, size, &status) && status == 17);
    }
    assert(!quest_camera_mcu_status_decode(NULL, 4, &status) && status == 17);
    assert(!quest_camera_mcu_status_decode(command, 4, NULL));
    memset(command, 0, 4); assert(quest_camera_mcu_status_decode(command, 4, &status) && status == 0);
    memset(command, 255, 4); assert(quest_camera_mcu_status_decode(command, 4, &status) && status == -1);
    uint32_t ids[5];uint8_t wire[27],payload[60];
    struct quest_camera_mcu_parameters values[5];
    for(unsigned count=1;count<=5;count++)for(unsigned start=0;start<32;start++)for(unsigned seq=1;seq<=254;seq++) {
        for(unsigned i=0;i<count;i++)ids[i]=UINT32_C(1)<<((start+i)%32);
        memset(wire,0xa5,sizeof(wire));assert(quest_camera_metadata_query_encode(ids,count,seq,wire+1,25));
        assert(wire[0]==0xa5 && wire[26]==0xa5 && wire[1]==2 && wire[2]==seq && wire[3]==22 && wire[4]==0x4e && wire[5]==count);
        for(unsigned i=0;i<5;i++) {
            uint32_t actual;memcpy(&actual,wire+6+4*i,4);assert(actual==(i<count?ids[i]:0));
        }
        for(unsigned i=0;i<60;i++)payload[i]=(uint8_t)(i+seq+start);
        assert(quest_camera_metadata_reply_decode(ids,count,payload,60,values,count));
        for(unsigned i=0;i<count;i++) {
            assert(values[i].camera_bit==ids[i]);
            for(unsigned col=0;col<3;col++){uint32_t expected;memcpy(&expected,payload+col*20+i*4,4);assert(values[i].words[col]==expected);}
        }
        cases++;
    }
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    uint8_t *guard=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(guard!=MAP_FAILED && !mprotect(guard+page,page,PROT_NONE));
    ids[0]=1;ids[1]=4;
    for(unsigned len=0;len<=60;len++) {
        uint8_t *data=guard+page-len;memset(data,0xa5,len);memset(values,0xa5,sizeof(values));
        assert(quest_camera_metadata_reply_decode(ids,2,data,len,values,2)==(len==60));
        if(len!=60)for(unsigned i=0;i<sizeof(values);i++)assert(((uint8_t *)values)[i]==0xa5);
        cases++;
    }
    for(unsigned len=0;len<25;len++) {
        memset(wire,0xa5,sizeof(wire));assert(!quest_camera_metadata_query_encode(ids,2,1,wire,len));
        for(unsigned i=0;i<sizeof(wire);i++)assert(wire[i]==0xa5);
        cases++;
    }
    for(unsigned test=0;test<8;test++) {
        ids[0]=1;ids[1]=4;size_t count=2;uint8_t seq=1;
        switch(test){case 0:count=0;break;case 1:count=6;break;case 2:ids[0]=0;break;
        case 3:ids[0]=3;break;case 4:ids[1]=1;break;case 5:seq=0;break;case 6:seq=255;break;case 7:ids[0]=UINT32_MAX;break;}
        memset(wire,0xa5,sizeof(wire));memset(values,0xa5,sizeof(values));
        assert(!quest_camera_metadata_query_encode(ids,count,seq,wire,sizeof(wire)));
        if(seq==1)assert(!quest_camera_metadata_reply_decode(ids,count,payload,60,values,count));
        for(unsigned i=0;i<sizeof(wire);i++)assert(wire[i]==0xa5);
        for(unsigned i=0;i<sizeof(values);i++)assert(((uint8_t *)values)[i]==0xa5);
        cases++;
    }
    ids[0]=1;ids[1]=4;
    assert(!quest_camera_metadata_reply_decode(ids,2,payload,59,values,2));
    assert(!quest_camera_metadata_reply_decode(ids,2,payload,61,values,2));
    assert(!quest_camera_metadata_reply_decode(ids,2,payload,60,values,1));
    assert(!quest_camera_metadata_reply_decode(ids,2,payload,60,values,3));
    assert(!quest_camera_metadata_reply_decode(ids,2,NULL,60,values,2));
    assert(!quest_camera_metadata_reply_decode(ids,2,payload,60,NULL,2));
    assert(!quest_camera_metadata_reply_decode(NULL,2,payload,60,values,2));
    assert(!quest_camera_metadata_query_encode(NULL,2,1,wire,25));
    assert(!quest_camera_metadata_query_encode(ids,2,1,NULL,25));
    assert(!munmap(guard,page*2));printf("QUEST_CAMERA_MCU_PASS cases=%u hardware=0\n",cases);
}
