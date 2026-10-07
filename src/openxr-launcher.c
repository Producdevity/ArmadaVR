#include <windows.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

static const wchar_t *extension_values[] = {
    L"openxr_vulkan_instance_extensions", L"openxr_vulkan_device_extensions",
    L"openxr_vulkan_device_vid", L"openxr_vulkan_device_pid"
};

static DWORD WINAPI send_quit(void *milliseconds) {
    Sleep((DWORD)(uintptr_t)milliseconds);
    INPUT_RECORD input[2] = {0};
    input[0].EventType = input[1].EventType = KEY_EVENT;
    input[0].Event.KeyEvent.bKeyDown = TRUE;
    input[0].Event.KeyEvent.wRepeatCount = input[1].Event.KeyEvent.wRepeatCount = 1;
    input[0].Event.KeyEvent.wVirtualKeyCode = input[1].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
    input[0].Event.KeyEvent.uChar.UnicodeChar = input[1].Event.KeyEvent.uChar.UnicodeChar = L'\r';
    DWORD written;
    return WriteConsoleInputW(GetStdHandle(STD_INPUT_HANDLE), input, 2, &written) && written == 2 ? 0 : 1;
}

static BOOL initialize_runtime(HKEY key) {
    for (unsigned i = 0; i < 4; ++i) {
        LSTATUS status = RegDeleteValueW(key, extension_values[i]);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) return FALSE;
    }
    HMODULE dll = LoadLibraryW(L"wineopenxr.dll");
    if (!dll) return FALSE;
    BOOL (__cdecl *init)(void) = (void *)GetProcAddress(dll, "wineopenxr_init_registry");
    BOOL valid = init && init();
    // The pinned Proton export returns TRUE even when its native runtime query fails.
    for (unsigned i = 0; valid && i < 4; ++i) {
        DWORD type, size = 16384;
        wchar_t data[8192];
        valid = RegQueryValueExW(key, extension_values[i], NULL, &type, (BYTE *)data, &size) == ERROR_SUCCESS;
        if (i < 2) valid = valid && type == REG_SZ && size >= 2 && size % 2 == 0 && data[size / 2 - 1] == L'\0';
        else valid = valid && type == REG_DWORD && size == sizeof(DWORD);
    }
    FreeLibrary(dll);
    return valid;
}

int wmain(int argc, wchar_t **argv) {
    wchar_t enabled[2];
    if (GetEnvironmentVariableW(L"ARMADA_VR_OPENXR_ONLY", enabled, 2) != 1 || enabled[0] != L'1') {
        fputs("Use run-proton-openxr with its separate OpenXR prefix.\n", stderr);
        return 2;
    }
    int first = 1;
    DWORD quit_ms = 0;
    if (argc > 2 && !wcscmp(argv[1], L"--test-quit-after")) {
        wchar_t *end;
        unsigned long seconds = wcstoul(argv[2], &end, 10);
        if (*end || seconds < 1 || seconds > 60) return 2;
        quit_ms = seconds * 1000;
        first = 3;
    }
    if (first >= argc) {
        fputs("Usage: openxr-launcher [--test-quit-after 1..60] PROGRAM [ARGS...]\n", stderr);
        return 2;
    }
    HANDLE lock = CreateMutexW(NULL, TRUE, L"Local\\ArmadaVROpenXRLauncher");
    if (!lock || GetLastError() == ERROR_ALREADY_EXISTS) {
        fputs("An OpenXR prefix session is already active.\n", stderr);
        if (lock) CloseHandle(lock);
        return 3;
    }
    int result = 4;
    HKEY key = NULL;
    DWORD state = 0xffffffff;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Wine\\VR", 0, NULL,
                       REG_OPTION_VOLATILE, KEY_ALL_ACCESS, NULL, &key, NULL)) goto done;
    if (RegSetValueExW(key, L"state", 0, REG_DWORD, (BYTE *)&state, sizeof(state))) goto done;
    if (!initialize_runtime(key)) {
        fputs("Native OpenXR/Vulkan initialization did not produce complete runtime data.\n", stderr);
        goto done;
    }
    state = 1;
    if (RegSetValueExW(key, L"state", 0, REG_DWORD, (BYTE *)&state, sizeof(state))) goto done;
    if (quit_ms) {
        HANDLE thread = CreateThread(NULL, 0, send_quit, (void *)(uintptr_t)quit_ms, 0, NULL);
        if (!thread) goto done;
        CloseHandle(thread);
    }
    intptr_t child = _wspawnv(_P_WAIT, argv[first], (const wchar_t * const *)(argv + first));
    result = child < 0 ? 5 : (int)child;
done:
    if (key) {
        state = 0xffffffff;
        RegSetValueExW(key, L"state", 0, REG_DWORD, (BYTE *)&state, sizeof(state));
        RegCloseKey(key);
    }
    ReleaseMutex(lock);
    CloseHandle(lock);
    return result;
}
