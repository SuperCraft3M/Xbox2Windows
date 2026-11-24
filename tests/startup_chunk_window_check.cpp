#include "startup_stubs/storage_chunk_window.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <windows.h>

namespace {
void word(std::vector<unsigned char>& data, std::size_t at, std::uint32_t value) {
    std::memcpy(data.data() + at, &value, 4);
}
void boundary(std::vector<unsigned char>& data, unsigned index, unsigned stored, unsigned logical,
              unsigned codec = 0, unsigned padding = 0) {
    word(data, 16 + index * 8, stored / 256 | codec << 26 | ((logical / 4096) & 3) << 30);
    word(data, 20 + index * 8, logical / 16384 | padding << 20);
}
struct Object { void** table; };
template<class R, class... A> R call(Object* object, unsigned slot, A... args) {
    return reinterpret_cast<R(__stdcall*)(Object*, A...)>(object->table[slot])(object, args...);
}
bool runtime(const char* dll, const std::filesystem::path& path, const std::vector<unsigned char>& archive,
             unsigned packed, unsigned format, const std::vector<unsigned char>& expected, bool enabled) {
    { std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(archive.data()), archive.size()); }
    HMODULE library = LoadLibraryW(std::filesystem::path(dll).c_str());
    if (!library) return false;
    auto factory_fn = reinterpret_cast<long(__stdcall*)(const GUID*, void**)>(GetProcAddress(library, "DStorageGetFactory"));
    Object* factory = nullptr;
    if (!factory_fn || factory_fn(nullptr, reinterpret_cast<void**>(&factory))) return false;
    Object* file = nullptr;
    if (call<long>(factory, 4, path.c_str(), static_cast<const GUID*>(nullptr), reinterpret_cast<void**>(&file))) return false;
    unsigned char desc[40] = {};
    Object* queue = nullptr;
    if (call<long>(factory, 10, desc, static_cast<const GUID*>(nullptr), reinterpret_cast<void**>(&queue))) return false;
    for (unsigned skip : {0u, 65536u}) {
        Object* status = nullptr;
        if (call<long>(factory, 5, 1u, "window", static_cast<const GUID*>(nullptr), reinterpret_cast<void**>(&status))) return false;
        std::vector<unsigned char> out(65536 + 16, 0xcc);
        alignas(8) unsigned char request[0x48] = {};
        const std::uint64_t options = format == 8 ? 1 : format * 2;
        std::memcpy(request, &options, 8);
        auto* pointer = out.data(); std::memcpy(request + 8, &pointer, 8);
        const unsigned size = 65536; std::memcpy(request + 0x18, &size, 4);
        std::memcpy(request + 0x20, &file, 8);
        const std::uint64_t offset = 8192 + skip; std::memcpy(request + 0x28, &offset, 8);
        std::memcpy(request + 0x30, &packed, 4);
        call<void>(queue, 3, request); call<void>(queue, 4, status, 0u); call<void>(queue, 6);
        unsigned tries = 0;
        while (!call<int>(status, 3, 0u) && ++tries < 500) Sleep(10);
        if (tries == 500) return false;
        const long hr = call<long>(status, 4, 0u);
        if (enabled) {
            if (hr || !std::equal(out.begin(), out.begin() + 65536, expected.begin() + skip)) return false;
        } else if (hr >= 0 || !std::all_of(out.begin(), out.end(), [](unsigned char b) { return b == 0xcc; })) return false;
        if (!std::all_of(out.begin() + 65536, out.end(), [](unsigned char b) { return b == 0xcc; })) return false;
    }
    call<void>(file, 3);
    std::filesystem::remove(path);
    // Keep the module loaded until process exit: its worker and heartbeat are alive.
    return true;
}
}
int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) return 2;
    const bool enabled = argc == 4;
    SetEnvironmentVariableA("X2W_DSTORAGE_BCPACK_WINDOWS", nullptr);
    SetEnvironmentVariableA("X2W_DSTORAGE_CHUNK_WINDOWS", enabled ? "1" : "0");
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<unsigned char> packed{std::istreambuf_iterator<char>(file), {}};
    constexpr unsigned decoded_size = 131072, start = 8192, destination = 65536;
    std::vector<unsigned char> full(decoded_size);
    std::size_t produced = 0;
    if (!x2w_bcpack::decode_bcpack(packed.data(), packed.size(), 1, full.data(), full.size(), &produced) || produced != full.size()) return 3;
    std::vector<unsigned char> short_output(destination);
    if (x2w_bcpack::decode_bcpack(packed.data(), packed.size(), 1, short_output.data(), short_output.size(), &produced)) return 4;
    const unsigned end = start + static_cast<unsigned>((packed.size() + 255) & ~std::size_t{255});
    std::vector<unsigned char> archive(end);
    std::memcpy(archive.data(), "RDS5", 4); word(archive, 4, 3); word(archive, 8, 4096 + decoded_size);
    boundary(archive, 0, 4096, 0);
    boundary(archive, 1, start, 4096, 1, end - start - static_cast<unsigned>(packed.size()));
    boundary(archive, 2, end, 4096 + decoded_size);
    std::copy(packed.begin(), packed.end(), archive.begin() + start);
    if (!runtime(argv[3], std::filesystem::path(argv[1]).replace_extension(".rds5"), archive, packed.size(), 1, full, enabled)) return 13;
    auto read = [&](std::uint64_t offset, std::size_t bytes, std::vector<unsigned char>& out) {
        if (offset > archive.size() || bytes > archive.size() - offset) return false;
        out.assign(archive.begin() + offset, archive.begin() + offset + bytes); return true;
    };
    for (unsigned skip : {0u, destination}) {
        std::vector<unsigned char> out{0xcc};
        if (!x2w_startup::decode_chunk_window(read, archive.size(), start + skip, static_cast<unsigned>(packed.size()),
                                            destination, 1, out) || out.size() != destination ||
            !std::equal(out.begin(), out.end(), full.begin() + skip)) return 5;
    }
    auto refused = [&](std::uint64_t offset, unsigned bytes, unsigned format, unsigned size) {
        std::vector<unsigned char> out{0xcc};
        return !x2w_startup::decode_chunk_window(read, archive.size(), offset, bytes, size, format, out) && out == std::vector<unsigned char>{0xcc};
    };
    if (!refused(start + 1, packed.size(), 1, destination) ||
        !refused(start, packed.size() - 1, 1, destination) ||
        !refused(start, packed.size(), 7, destination) ||
        !refused(start + destination, packed.size(), 1, destination + 1)) return 6;
    // Invalid data cannot publish a prefix, even with a valid boundary table.
    std::fill(archive.begin() + start, archive.end(), 0);
    if (!refused(start, packed.size(), 1, destination)) return 7;
    word(archive, 4, 0xffffffff);
    if (!refused(start, packed.size(), 1, destination)) return 8;
    // Two chunks can satisfy the same offset/size convention: ambiguity must be refused.
    std::vector<unsigned char> header(48);
    std::memcpy(header.data(), "RDS5", 4); word(header, 4, 4); word(header, 8, 4096 + 2 * 262144);
    boundary(header, 0, 4096, 0); boundary(header, 1, 8192, 4096, 1);
    boundary(header, 2, 8192 + 65536, 4096 + 262144, 1);
    boundary(header, 3, 8192 + 2 * 65536, 4096 + 2 * 262144);
    x2w_startup::ChunkWindow window{123, 456, 789, 0};
    if (x2w_startup::find_chunk_window(header.data(), header.size(), 8192 + 2 * 65536,
                               8192 + 65536, 65536, 65536, 1, window) || window.start != 123) return 9;
    // The same validated windows for zlib must verify the whole checksum first.
    std::ifstream zfile(argv[2], std::ios::binary);
    std::vector<unsigned char> zpacked{std::istreambuf_iterator<char>(zfile), {}};
    constexpr unsigned zdecoded_size = 4u << 20;
    const unsigned zend = start + static_cast<unsigned>((zpacked.size() + 255) & ~std::size_t{255});
    archive.assign(zend, 0);
    std::memcpy(archive.data(), "RDS5", 4); word(archive, 4, 3); word(archive, 8, 4096 + zdecoded_size);
    boundary(archive, 0, 4096, 0);
    boundary(archive, 1, start, 4096, 8, zend - start - static_cast<unsigned>(zpacked.size()));
    boundary(archive, 2, zend, 4096 + zdecoded_size);
    std::copy(zpacked.begin(), zpacked.end(), archive.begin() + start);
    std::vector<unsigned char> zfull(zdecoded_size);
    for (unsigned i = 0; i < zdecoded_size; ++i) zfull[i] = static_cast<unsigned char>(i * 17 + i / 31);
    if (!runtime(argv[3], std::filesystem::path(argv[2]).replace_extension(".rds5"), archive, zpacked.size(), 8, zfull, enabled)) return 14;
    for (unsigned skip : {0u, destination, zdecoded_size - destination}) {
        std::vector<unsigned char> out{0xcc};
        if (!x2w_startup::decode_chunk_window(read, archive.size(), start + skip, zpacked.size(), destination, 8, out)) return 10;
        for (unsigned i = 0; i < destination; ++i)
            if (out[i] != static_cast<unsigned char>((i + skip) * 17 + (i + skip) / 31)) return 11;
    }
    archive[start + zpacked.size() - 1] ^= 1;
    if (!refused(start, zpacked.size(), 8, destination)) return 12;
    if (!runtime(argv[3], std::filesystem::path(argv[2]).replace_extension(".bad.rds5"), archive, zpacked.size(), 8, zfull, false)) return 15;
    word(archive, 8, 4096 + x2w_startup::max_chunk_decoded_bytes + 4096);
    boundary(archive, 2, zend, 4096 + x2w_startup::max_chunk_decoded_bytes + 4096);
    if (!refused(start, zpacked.size(), 8, destination)) return 16;
    std::puts("BCPack windows: exact first/continuation bytes; malformed, corrupt, wrong-codec, out-of-range and ambiguous requests refused");
}
