#include "pico-controller.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    unsigned mcu;
    unsigned radio;
    bool touch;
    const char *record;
    const char *expected;
} vectors[] = {
    {1, 1, false,
        "a5011fb068b2e3bff317f3f217dd1beb514b0fa2f6d41558960c3021ab435effd07d87a8d36ae1f4a0a339a913880e260239"
        "dff4c8a96dcaa94b5653a49b475e5d1c55fb80c669e503b856c1006759a4867df1cfc1a13b3f00fe8c03e29e7b9f8e9a5c1a", "02bf00b0e368b202"},
    {1, 1, false,
        "a501b289a1d21aff141f7f48b97493dc47f5268adef438d950e56d3094656265708bb3258e38d5e1556e8293084f56b5a457"
        "dc5eebc18f57c51f0ea02493c385e4bc1a6fb305af123aa046e94e56c75a3038bfd34adf8649e119e83de9ba2ab8165fed9e", "01bf00891aa1d203"},
    {1, 1, true,
        "a501e35d72a2f0bfe417bb75bf27106aa4cbcec4c7ef63d1c9e83b6e71d0d6f6cec22024e81ae8cbe06af0d75afe8daaecd9"
        "656b3d8aec6222ec0b7191824c842de84300881c9b02f33382e1e7acff57069078086442eb6e5524f4f9293661992f2386bb", "02bf045df072a202"},
    {1, 1, true,
        "a5011b7f25ff2eff271f31b35c6af88ecde5aeca895dbaf48d5a5c2d809970e101c3d63a02268f719c439d6e20f78653648d"
        "aeec4c5a279b7f877c805f4a228e06e1e3436a9882a6d897beeb4e1bd5352c850e209e15f06c8eca303c8763061ffd3bb11e", "01bf077f2e25ff03"},
    {1, 2, false,
        "a50107a82b1e49bfce17feda4a4f65754d3f499bcb9e9217803d117230d8fcdbc3c64bd290bbdec1889bb73e2e158fb81180"
        "44c683b5a3cd9cf408e855acf79e2698b457bef8df5d534e31a2305283723de3302b4c78a58a38a872b29ef0efdeec72bf51", "02bf00a8492b1e07"},
    {1, 2, false,
        "a501e67da8d928ff4a1f5e9efdf77c8630f5604c03ef661ee47821a2bdb09caab6afa5cd7d26208a1fd539a32a81aa424a58"
        "f3c9806ad2af1052c2b4d2b02907b0bcfc3cb5e5fac373c2469795c074318a66812b85b9c5e0381d28d091bd3e197029e746", "01bf007d28a8d907"},
    {1, 2, true,
        "a501967d77bdd2bfe417d38ecfe64581ac7c4173f84fde6b86467c6fe8de9f6b7ed35217783ffb989f1e167ea105cfabd9ad"
        "213a8183cf629b844364e778475b9cd6c4bef093c78ec0002443d187f2be4638572806dced8f611c19d0da66028526835669", "02bf047dd277bd07"},
    {1, 2, true,
        "a501f465ccdd66ff191fd15aaffe35495e8d615b6beb889a36dec4003b668bb17336553ab4848ba118c0c8b5662cbd98905c"
        "257e6bbb58ea200ecaf70fe5a494b0f79c6ee166127b581ad522d775f5083df500aac67e6b4df24502343c85b47ac9516ce3", "01bf196566ccdd07"},
    {2, 1, false,
        "a504df6237169dbfc017374cab13711dd4b263d2bce1c87eef1458b2e3cc9a9b2e93fd742faa3f288f9fa818bafdd80a2e36"
        "81f40024664d2daffd063321d44ef8693b6178d3f8bd6dfa9cd47012a27076df530a5b2c25653778ecc1134dae0f1a2781ab", "02bf00629d371602"},
    {2, 1, false,
        "a5062c2e37d679ff7d1fc1b4f73006dd842acb68d51c3c367e7fedb3d8ee5fd1c20645d7b0765123b033230747bc5a04a75b"
        "7a46c9daa8a9e951bd676a6cecad6819f39295b64e717bfa76a5a12a38861905661f79a2cb7edefe11059399a4ec2cce219e", "01bf002e7937d603"},
    {2, 1, true,
        "a5055d238fc0c5bf5c170f830251e824c8e3d072f5a56cd47b458f6d5cc0f99e00d4140cb7a0711ace31cef87a8d79c9eca4"
        "7da999d0ea0f32677538f1a615efee2c386d065a08f0ba054c435f6455e66e16d869ab8d19f61e5697761ed742871e6a5744", "02bf1c23c58fc002"},
    {2, 1, true,
        "a50470688b4d6affe61f59c6907f49b03ea558ef956bd395f59bb6eab24b493c083925ac2b7b76c72e20f15624f5fe86c793"
        "921db157b041de59409578d4673c5debd03bcfb87cc8a1366082431e3cbac4361f46447a2695ba6fc0d9fcd8f9b3e8d88f12", "01bf06686a8b4d03"},
    {2, 2, false,
        "a5057dfe58c4f5bfcf1719e06986cd209039329f1227165ed9d9e5685b54440e6c32a9cb9d6b9eda47fa2d289599b4ed9c09"
        "25e0608b034d4e72e49184f066ab513d68b0d1c1b116d326e9c46ecf3af2536b5c0535aa6be2a728096e06760123657299c9", "02bf00fef558c407"},
    {2, 2, false,
        "a5076b35412a9fff721f688ded2b09cd2d533776aec3006337f5ecb605596723e9382c096dc05739a36502cf961c1d86e24d"
        "92d45621bbcc95ac1fef975c99d8999d9f3a03aabb95512f318e8aaa5669f0b7e7abe5797e9e15bc3f229bad68c56ada589c", "01bf00359f412a07"},
    {2, 2, true,
        "a5079caf2ec15ebfc0173753cfd378976e2bd45bc50bde1c795a08afb94dfb590fb4e74e8cb88c8ac5e3e985bd788879adcd"
        "1a9465e97d8fe6b0dcad5792a2e3eeafb10cc0df81f5aa6204473d88855bfb4941ae763270a04dc9ad025b1b43010b579f46", "02bf00af5e2ec107"},
    {2, 2, true,
        "a50727e0e79e2fffe81f64e653623a1db4e1589713cce53d2ac22618c34bcc26b854dd3d509e6aebd8740d1cf22229434aba"
        "197772afa31ab80b30bb410b3603600d49ccbf91e7c35a2fc636ee8de608a12a0f093d75956b60d333260dd69c79d1dd4f61", "01bf08e02fe79e07"},
};

static unsigned checks;

static void unhex(const char *text, void *output)
{
    unsigned char *bytes = output;
    while (*text) {
        unsigned byte;
        assert(sscanf(text, "%2x", &byte) == 1);
        *bytes++ = (unsigned char)byte;
        text += 2;
    }
}

static void rejected(const void *data, size_t size, unsigned mcu, unsigned radio)
{
    struct pico_controller_sample before, result;
    memset(&before, 0x5a, sizeof(before));
    memcpy(&result, &before, sizeof(result));
    assert(!pico_controller_decode(data, size, mcu, radio, true, &result));
    assert(memcmp(&before, &result, sizeof(result)) == 0);
    ++checks;
}

static void ssc_rejected(const void *data, size_t size, unsigned mcu, unsigned radio)
{
    struct pico_controller_sample before, result;
    memset(&before, 0x5a, sizeof(before));
    memcpy(&result, &before, sizeof(result));
    unsigned accuracy = 99;
    assert(!pico_controller_decode_ssc(data, size, mcu, radio, true, &result, &accuracy));
    assert(memcmp(&before, &result, sizeof(result)) == 0 && accuracy == 99);
    ++checks;
}

static size_t ssc_encode(const unsigned char *record, unsigned words, unsigned mode,
                         unsigned accuracy, unsigned char *payload)
{
    size_t length = 0;
    unsigned packed = mode == 0 ? 0 : mode == 1 ? words : 12;
    if (packed) {
        payload[length++] = 0x0a;
        payload[length++] = (unsigned char)(4 * packed);
        memcpy(payload + length, record, 4 * packed);
        length += 4 * packed;
    }
    for (unsigned i = packed; i < words; ++i) {
        payload[length++] = 0x0d;
        memcpy(payload + length, record + 4 * i, 4);
        length += 4;
    }
    payload[length++] = 0x10;
    payload[length++] = (unsigned char)accuracy;
    return length;
}

static void ssc_cases(unsigned vector, const unsigned char *record,
                      const struct pico_controller_sample *expected)
{
    unsigned char payload[512], words[104] = {0};
    memcpy(words, record, 100);
    for (unsigned mode = 0; mode < 3; ++mode) {
        for (unsigned status = 0; status < 4; ++status) {
            size_t length = ssc_encode(words, 25, mode, status, payload);
            struct pico_controller_sample sample;
            unsigned accuracy = 99;
            for (unsigned align = 0; align < 8; ++align) {
                unsigned char *copy = malloc(length + align);
                assert(copy);
                memcpy(copy + align, payload, length);
                assert(pico_controller_decode_ssc(copy + align, length, vectors[vector].mcu,
                    vectors[vector].radio, vectors[vector].touch, &sample, &accuracy));
                assert(!memcmp(&sample, expected, sizeof(sample)) && accuracy == status);
                free(copy);
                ++checks;
            }
            for (size_t short_length = 0; short_length < length; ++short_length)
                ssc_rejected(payload, short_length, vectors[vector].mcu, vectors[vector].radio);
            const unsigned char extra[] = {
                0x18, 0x80, 0x01, 0x21, 0, 0, 0, 0, 0, 0, 0, 0,
                0x2a, 3, 1, 2, 3, 0x35, 0, 0, 0, 0, 0x10, 3,
            };
            memcpy(payload + length, extra, sizeof(extra));
            assert(pico_controller_decode_ssc(payload, length + sizeof(extra), vectors[vector].mcu,
                vectors[vector].radio, vectors[vector].touch, &sample, &accuracy));
            assert(!memcmp(&sample, expected, sizeof(sample)) && accuracy == 3);
            ++checks;
            payload[length] = 0;
            ssc_rejected(payload, length + 1, vectors[vector].mcu, vectors[vector].radio);
            payload[length - 1] = 4;
            ssc_rejected(payload, length, vectors[vector].mcu, vectors[vector].radio);
        }
        for (unsigned count = 24; count <= 26; count += 2) {
            size_t length = ssc_encode(words, count, mode, 0, payload);
            ssc_rejected(payload, length, vectors[vector].mcu, vectors[vector].radio);
        }
    }
    size_t length = ssc_encode(words, 25, 1, 0, payload);
    for (unsigned bit = 0; bit < 800; ++bit) {
        payload[2 + bit / 8] ^= 1u << (bit % 8);
        ssc_rejected(payload, length, vectors[vector].mcu, vectors[vector].radio);
        payload[2 + bit / 8] ^= 1u << (bit % 8);
    }
    ssc_rejected(payload, length, 0, 1);
    ssc_rejected(payload, length, 1, 3);
}

int main(void)
{
    _Static_assert(sizeof(struct pico_controller_sample) == 8, "Fixture output layout");
    unsigned char data[101] = {0};
    for (unsigned i = 0; i < sizeof(vectors) / sizeof(*vectors); ++i) {
        const unsigned mcu = vectors[i].mcu, radio = vectors[i].radio;
        struct pico_controller_sample expected, sample;
        unhex(vectors[i].record, data);
        unhex(vectors[i].expected, &expected);
        ssc_cases(i, data, &expected);
        for (unsigned alignment = 0; alignment < 8; ++alignment) {
            unsigned char *allocation = malloc(100 + alignment);
            assert(allocation);
            memcpy(allocation + alignment, data, 100);
            assert(pico_controller_decode(allocation + alignment, 100, mcu, radio,
                                           vectors[i].touch, &sample));
            assert(memcmp(&sample, &expected, sizeof(sample)) == 0);
            free(allocation);
            ++checks;
        }
        for (size_t length = 0; length < 100; ++length)
            rejected(data, length, mcu, radio);
        rejected(data, 101, mcu, radio);
        rejected(data, 100, 0, radio);
        rejected(data, 100, 3, radio);
        rejected(data, 100, mcu, 0);
        rejected(data, 100, mcu, 3);
        rejected(data, 100, mcu == 1 ? 2 : 1, radio);
        for (unsigned bit = 0; bit < 800; ++bit) {
            data[bit / 8] ^= 1u << (bit % 8);
            rejected(data, 100, mcu, radio);
            data[bit / 8] ^= 1u << (bit % 8);
        }
    }
    rejected(NULL, 100, 1, 1);
    assert(!pico_controller_decode(data, 100, 1, 1, true, NULL));
    ssc_rejected(NULL, 100, 1, 1);
    ssc_rejected(data, 4097, 1, 1);
    const unsigned char malformed[][12] = {
        {0x0a, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0},
        {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0},
        {0x80, 0x80, 0x80, 0x80, 0x10},
        {0x0a, 3, 1, 2, 3, 0x10, 0}, {0x0e}, {0x0b}, {0x0c},
    };
    for (unsigned i = 0; i < sizeof(malformed) / sizeof(*malformed); ++i)
        ssc_rejected(malformed[i], sizeof(malformed[i]), 1, 1);
    unsigned accuracy = 99;
    struct pico_controller_sample sample;
    assert(!pico_controller_decode_ssc(data, 100, 1, 1, true, NULL, &accuracy));
    assert(!pico_controller_decode_ssc(data, 100, 1, 1, true, &sample, NULL));
    printf("%u controller decoding and rejection checks passed\n", checks);
    return 0;
}
