#include <windows.h>

int main(int argc, char **argv) {
    volatile unsigned sum = 0;
    for (unsigned value = 1; value <= 1000; ++value) sum += value;
    if (sum != 500500) return 1;
    const char message[] = "ARMADA_VR_WINDOWS_X64_PASS sum=500500\n";
    HANDLE output = argc == 2 ? CreateFileA(argv[1], GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                           FILE_ATTRIBUTE_NORMAL, NULL) : GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == INVALID_HANDLE_VALUE) return 2;
    DWORD written = 0;
    if (!WriteFile(output, message, sizeof(message) - 1, &written, NULL)) return 2;
    if (argc == 2) CloseHandle(output);
    return written == sizeof(message) - 1 ? 0 : 3;
}
