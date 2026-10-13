#define main diagnostic_main
#include "../tools/pico-diagnostic-log.c"
#undef main
#include <assert.h>

int main(void)
{
    const char *begin = "ARMADA_PICO_DIAGNOSTIC_BEGIN kind=physical serial=test-device\n";
    const unsigned char data[] = "ARMADA_PICO_DIAGNOSTIC_BEGIN kind=physical serial=test-device\n"
        "FILE /proc/version\nLinux test\nARMADA_PICO_DIAGNOSTIC_END\n";
    assert(complete(data, sizeof(data) - 1, begin));
    for (size_t i = 0; i < sizeof(data) - 1; ++i) assert(!complete(data, i, begin));
    assert(!complete(data, sizeof(data) - 1, "ARMADA_PICO_DIAGNOSTIC_BEGIN kind=physical serial=other\n"));
    assert(!complete(data, sizeof(data) - 1, "ARMADA_PICO_DIAGNOSTIC_BEGIN kind=virtual serial=qemu\n"));
    const unsigned char end[] = "ARMADA_PICO_DIAGNOSTIC_END\n";
    assert(!complete(end, sizeof(end) - 1, begin));
    char *args[] = {"diagnostic", "--serial", "qemu", NULL};
    assert(diagnostic_main(3, args) == 2);
    puts("PICO_DIAGNOSTIC_LOG_PASS");
    return 0;
}
