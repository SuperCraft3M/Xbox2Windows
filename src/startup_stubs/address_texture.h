#pragma once

// Textures a title publishes by address (device slot 70): a resource description, a view and the
// address of the texels, with no resource object behind them. This header decides, without a
// device, which of those descriptions the startup renderer reads from the address: a sampled-only,
// two-dimensional, single-sample texture whose Layout field holds a console swizzle mode that
// gfx10_layout.h addresses. Its texels are in that console layout.
// Everything else is refused here: the desktop's unknown layout and the console's linear mode (the
// extent of such rows is not established), other dimensions, multisampled textures, textures that
// are also targets, formats without a known element size and swizzle modes without an equation.

#include "startup_stubs/gfx10_layout.h"

namespace x2w_startup {
inline bool console_address_texture(const D3D12_RESOURCE_DESC& r)
{
    // Each flag is cast on its own: with MinGW-w64 headers the flag enum's operator| is not constexpr.
    constexpr UINT not_sampled_only = static_cast<UINT>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) |
                                      static_cast<UINT>(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) |
                                      static_cast<UINT>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) |
                                      static_cast<UINT>(D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    // A desktop block-compressed texture needs whole 4x4 blocks in its largest mip.
    const UINT texels = gfx10::element_of(r.Format).texels;
    return r.SampleDesc.Quality == 0 && !(static_cast<UINT>(r.Flags) & not_sampled_only) && gfx10::supports(r) &&
           r.Width % texels == 0 && r.Height % texels == 0;
}

// Formats with one typeless parent hold the same bytes; a view may name any of them.
inline UINT format_family(DXGI_FORMAT format)
{
    const UINT f = static_cast<UINT>(format);
    constexpr UINT first[] = {1, 5, 9, 15, 19, 23, 27, 33, 39, 44, 48, 53, 60, 70, 73, 76, 79, 82, 94, 97};
    constexpr UINT last[] = {4, 8, 14, 18, 22, 25, 32, 38, 43, 47, 52, 59, 64, 72, 75, 78, 81, 84, 96, 99};
    for (UINT i = 0; i < sizeof(first) / sizeof(first[0]); ++i)
        if (f >= first[i] && f <= last[i]) return first[i];
    if (f == 87 || f == 90 || f == 91) return 90; // B8G8R8A8
    if (f == 88 || f == 92 || f == 93) return 92; // B8G8R8X8
    return f;
}

// A two-dimensional or array view that stays inside the texture and reads its texels as stored.
inline bool address_view_fits(const D3D12_RESOURCE_DESC& r, const D3D12_SHADER_RESOURCE_VIEW_DESC& v)
{
    if (format_family(v.Format) != format_family(r.Format)) return false;
    const auto inside = [](UINT first, UINT count, UINT total) {
        return first < total && (count == ~0u || (count && count <= total - first));
    };
    if (v.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D)
        return inside(v.Texture2D.MostDetailedMip, v.Texture2D.MipLevels, r.MipLevels) && !v.Texture2D.PlaneSlice;
    if (v.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY)
        return inside(v.Texture2DArray.MostDetailedMip, v.Texture2DArray.MipLevels, r.MipLevels) &&
               inside(v.Texture2DArray.FirstArraySlice, v.Texture2DArray.ArraySize, r.DepthOrArraySize) &&
               !v.Texture2DArray.PlaneSlice;
    return false;
}
} // namespace x2w_startup
