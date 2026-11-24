// Bounded, metadata-backed RDS5 BCPack/zlib windows. No source-size search or guessed chunk size.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include "bcpack_decoder.h"
#include "inflate.h"

namespace x2w_startup {
constexpr unsigned max_chunk_boundaries = ((1u << 20) - 16) / 8;
constexpr unsigned max_chunk_decoded_bytes = 16u << 20;
struct ChunkBoundary { std::uint64_t stored, logical; unsigned codec, padding; };
inline ChunkBoundary chunk_boundary(const unsigned char* p)
{
    std::uint32_t lo, hi;
    std::memcpy(&lo, p, 4); std::memcpy(&hi, p + 4, 4);
    return {std::uint64_t{lo & 0x3ffffff} * 256,
            (std::uint64_t{hi & 0xfffff} * 4 + (lo >> 30)) * 4096, (lo >> 26) & 15, hi >> 20};
}
struct ChunkWindow { std::uint64_t start{}; unsigned stored{}, decoded{}, skip{}; };
inline bool find_chunk_window(const unsigned char* header, std::size_t size, std::uint64_t file_size,
                       std::uint64_t offset, unsigned stored, unsigned destination,
                       unsigned format, ChunkWindow& result)
{
    if (!header || size < 32 || std::memcmp(header, "RDS5", 4) || !stored || !destination) return false;
    std::uint32_t count; std::uint64_t logical;
    std::memcpy(&count, header + 4, 4); std::memcpy(&logical, header + 8, 8);
    logical &= (std::uint64_t{1} << 40) - 1;
    if (count < 2 || count > max_chunk_boundaries || size < 16 + std::uint64_t{count} * 8) return false;
    auto previous = chunk_boundary(header + 16);
    const auto last = chunk_boundary(header + 16 + (count - 1) * 8);
    if (previous.logical || previous.codec || previous.stored < 16 + std::uint64_t{count} * 8 ||
        last.stored > file_size || last.logical != logical) return false;
    unsigned matches = 0;
    ChunkWindow candidate;
    for (unsigned i = 1; i < count; ++i) {
        const auto next = chunk_boundary(header + 16 + i * 8);
        if (next.stored <= previous.stored || next.logical <= previous.logical ||
            previous.padding >= next.stored - previous.stored) return false;
        const auto packed = next.stored - previous.stored - previous.padding;
        const auto decoded = next.logical - previous.logical;
        if (previous.codec == format && packed == stored && decoded <= max_chunk_decoded_bytes &&
            packed <= (1u << 20) && offset >= previous.stored) {
            const auto skip = offset - previous.stored;
            if (skip % 65536 == 0 && skip < decoded && destination <= decoded - skip &&
                (skip || destination < decoded)) {
                candidate = {previous.stored, static_cast<unsigned>(packed),
                             static_cast<unsigned>(decoded), static_cast<unsigned>(skip)};
                ++matches;
            }
        }
        previous = next;
    }
    if (matches != 1) return false;
    result = candidate;
    return true;
}

template<class Read>
bool decode_chunk_window(Read read, std::uint64_t file_size, std::uint64_t offset,
                       unsigned source_size, unsigned destination_size, unsigned format,
                       std::vector<unsigned char>& output)
{
    std::vector<unsigned char> header;
    if (!read(0, 32, header) || header.size() != 32 || std::memcmp(header.data(), "RDS5", 4)) return false;
    std::uint32_t count;
    std::memcpy(&count, header.data() + 4, 4);
    if (count < 2 || count > max_chunk_boundaries || !read(0, 16 + std::size_t{count} * 8, header)) return false;
    ChunkWindow window;
    if (!find_chunk_window(header.data(), header.size(), file_size, offset, source_size,
                    destination_size, format, window)) return false;
    std::vector<unsigned char> input;
    if (!read(window.start, window.stored, input) || input.size() != window.stored) return false;
    std::vector<unsigned char> decoded(window.decoded);
    std::size_t produced = 0;
    const bool decoded_ok = format == 8
        ? x2w_inflate::inflate_zlib(input.data(), input.size(), decoded.data(), decoded.size(), &produced)
        : x2w_bcpack::decode_bcpack(input.data(), input.size(), format, decoded.data(), decoded.size(), &produced);
    if (!decoded_ok || produced != decoded.size()) return false;
    std::vector<unsigned char> slice(decoded.begin() + window.skip,
                                     decoded.begin() + window.skip + destination_size);
    output.swap(slice);
    return true;
}
} // namespace x2w_startup
