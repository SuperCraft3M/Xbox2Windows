// Source-built consumer for the launcher's --map-drive option (#1158).
//
// A console executable opens its package at a fixed drive letter. This consumer does the same: it
// opens "<letter>:\map_drive_probe.txt" and prints the first line. The letter comes from
// X2W_PROBE_DRIVE (default Z) because the launcher passes no program arguments. Run under the
// launcher with --map-drive <letter>=<folder>, the open is rewritten to the real folder and
// succeeds; without the option it fails, which is the negative control.
#include <windows.h>

#include <cstdio>

int main()
{
    wchar_t letter = L'Z';
    wchar_t from_env[8]{};
    if (GetEnvironmentVariableW(L"X2W_PROBE_DRIVE", from_env, 8) == 1) letter = from_env[0];

    wchar_t path[64];
    _snwprintf_s(path, 64, _TRUNCATE, L"%c:\\map_drive_probe.txt", letter);

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::printf("open failed: %lu\n", static_cast<unsigned long>(GetLastError()));
        return 2;
    }
    char text[64]{};
    DWORD got = 0;
    const BOOL ok = ReadFile(file, text, sizeof(text) - 1, &got, nullptr);
    CloseHandle(file);
    if (!ok) return 3;
    std::printf("read %lu bytes: %s\n", static_cast<unsigned long>(got), text);
    return 0;
}