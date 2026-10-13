#define main diagnostic_main
#include "../system/pico/diagnostic-init.c"
#undef main
#include <assert.h>

int main(void)
{
    assert(getpid() != 1);
    assert(diagnostic_main() == 2);
    const char *command = "rdinit=/init armada.pico=diagnostic androidboot.serialno=test";
    assert(token(command, "armada.pico=diagnostic"));
    assert(token(command, "androidboot.serialno=test"));
    assert(!token(command, "androidboot.serialno=tes"));
    assert(!token(command, "armada.pico=diagnostic-test"));
    assert(!token("armada.pico=diagnostic armada.pico=diagnostic", "armada.pico=diagnostic"));
    assert(!token("armada.pico=diagnostic armada.pico=wrong", "armada.pico=diagnostic"));
    assert(!token("badarmada.pico=diagnostic", "armada.pico=diagnostic"));
    assert(token("\tarmada.pico=diagnostic\n", "armada.pico=diagnostic"));
    assert(!token("", "armada.pico=diagnostic"));
    for (unsigned int i = 0; i < 2000; ++i) record("%0100u\n", i);
    assert(used == sizeof(report) - 1 && report[used] == 0);
    puts("PICO_DIAGNOSTIC_INIT_PASS");
    return 0;
}
