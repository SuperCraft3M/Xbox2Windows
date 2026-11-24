// Source-built consumer for the launcher's --map-drive option (#1158).
//
// A console executable opens its package at a fixed drive letter. The letter comes from
// X2W_PROBE_DRIVE (default Z) because the launcher passes no program arguments.
//
// Two modes, chosen with X2W_PROBE_MODE:
//   (default) one open of "<letter>:\map_drive_probe.txt" through CreateFileW, printing what it read.
//             Under the launcher with --map-drive the open is rewritten and succeeds; without it, it
//             fails, which is the negative control.
//   reuse     a caller-owned UNICODE_STRING is passed to NtCreateFile many times. After every return
//             its length, buffer pointer and contents must be the ones the caller built, and every
//             open must still succeed: the rewritten name is only borrowed for the duration of each
//             call, and one backing buffer is reused rather than one allocation per open.
#include <windows.h>
#include <winternl.h>

#include <cstdio>
#include <cwchar>

namespace {
constexpr int reuse_opens = 32;

// ntdll's declarations in <winternl.h> differ across SDK versions, so resolve the two entry points
// at run time: the fixture only needs to make the call, not to link against a signature.
using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                       PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using NtCloseFn = NTSTATUS(NTAPI*)(HANDLE);

bool resolve(HMODULE ntdll, NtCreateFileFn& create, NtCloseFn& close)
{
    create = reinterpret_cast<NtCreateFileFn>(GetProcAddress(ntdll, "NtCreateFile"));
    close = reinterpret_cast<NtCloseFn>(GetProcAddress(ntdll, "NtClose"));
    return create && close;
}

wchar_t probe_letter()
{
    wchar_t letter = L'Z';
    wchar_t from_env[8]{};
    if (GetEnvironmentVariableW(L"X2W_PROBE_DRIVE", from_env, 8) == 1) letter = from_env[0];
    return letter;
}

// One open through CreateFileW; the mapped letter has no volume of its own.
int single_open()
{
    wchar_t path[64];
    _snwprintf_s(path, 64, _TRUNCATE, L"%c:\\map_drive_probe.txt", probe_letter());
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

int reused_string()
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    NtCreateFileFn create = nullptr;
    NtCloseFn close = nullptr;
    if (!ntdll || !resolve(ntdll, create, close)) {
        std::printf("ntdll entry points not resolved\n");
        return 1;
    }
    wchar_t name[64];
    // NtCreateFile takes the NT form of the name; CreateFileW would add this prefix itself.
    _snwprintf_s(name, 64, _TRUNCATE, L"\\??\\%c:\\map_drive_probe.txt", probe_letter());
    wchar_t built[64];
    wcscpy_s(built, name);

    UNICODE_STRING string{};
    string.Buffer = name;
    string.Length = static_cast<USHORT>(std::wcslen(name) * sizeof(wchar_t));
    string.MaximumLength = string.Length;  // exactly the caller's size, so a longer name must borrow
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &string, OBJ_CASE_INSENSITIVE, nullptr, nullptr);

    const USHORT length = string.Length, maximum = string.MaximumLength;
    PWSTR buffer = string.Buffer;
    for (int index = 0; index < reuse_opens; ++index) {
        HANDLE handle = nullptr;
        IO_STATUS_BLOCK status{};
        const NTSTATUS result = create(&handle, GENERIC_READ | SYNCHRONIZE, &attributes, &status, nullptr,
                                       FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
                                       FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
        if (result < 0) {
            std::printf("reuse open %d failed: 0x%08lx\n", index, static_cast<unsigned long>(result));
            return 1;
        }
        close(handle);
        // The launcher borrows the caller's name for the call only; it must be back exactly as built.
        if (string.Length != length || string.MaximumLength != maximum || string.Buffer != buffer ||
            std::wcscmp(string.Buffer, built) != 0) {
            std::printf("caller name changed after open %d: length=%u maximum=%u buffer_moved=%d text=%ls\n",
                        index, string.Length, string.MaximumLength, string.Buffer != buffer ? 1 : 0,
                        string.Buffer ? string.Buffer : L"(null)");
            return 1;
        }
    }
    std::printf("reuse ok: %d opens, caller name intact after every return\n", reuse_opens);
    return 0;
}
}  // namespace

int main()
{
    wchar_t mode[16]{};
    const bool reuse = GetEnvironmentVariableW(L"X2W_PROBE_MODE", mode, 16) == 5 &&
        _wcsicmp(mode, L"reuse") == 0;
    return reuse ? reused_string() : single_open();
}