#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace x2w_startup {

using TextureDescriptorWords = std::array<std::uint32_t, 8>;

// A table entry can contain a single index into the bound guest view heap.
// Only follow zero-padded indices and accept a terminal SRV token.
template<class ReadDescriptor>
std::optional<TextureDescriptorWords> indexed_texture_descriptor(
    TextureDescriptorWords words, std::uint32_t count, std::uint32_t token_magic, ReadDescriptor read)
{
    constexpr unsigned max_hops = 8;
    std::array<std::uint32_t, max_hops> visited{};
    for (unsigned hop = 0; hop < max_hops; ++hop) {
        const auto index = words[0];
        if (!index || index >= count ||
            !std::all_of(words.begin() + 1, words.end(), [](auto word) { return word == 0; }) ||
            std::find(visited.begin(), visited.begin() + hop, index) != visited.begin() + hop)
            return std::nullopt;
        visited[hop] = index;
        if (!read(index, words)) return std::nullopt;
        if (words[0] == token_magic)
            return words[2] == 18 ? std::optional{words} : std::nullopt;
    }
    return std::nullopt;
}

} // namespace x2w_startup
