#pragma once

#include <cstdint>

namespace x2w_startup {
struct PresentRedBluePattern {
    std::uint64_t samples = 0, palette = 0, red_side = 0, blue_side = 0;

    void add(unsigned r, unsigned g, unsigned b)
    {
        ++samples;
        if (g > 8 || r + b < 247 || r + b > 263) return;
        ++palette;
        if (r > b + 32) ++red_side;
        if (b > r + 32) ++blue_side;
    }

    bool matches() const
    {
        return samples >= 64 && palette * 20 >= samples * 19 &&
               red_side * 10 >= samples && blue_side * 10 >= samples;
    }
};
}
