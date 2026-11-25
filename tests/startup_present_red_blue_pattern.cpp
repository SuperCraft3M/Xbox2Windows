#include "../src/startup_stubs/present_red_blue_pattern.h"

int main()
{
    using x2w_startup::PresentRedBluePattern;
    PresentRedBluePattern mixture, red, purple, near, few;
    for (unsigned i = 0; i < 128; ++i) {
        const unsigned value = i % 2 ? 229 : 26;
        mixture.add(value, 0, 255 - value);
        red.add(255, 0, 0);
        purple.add(255, 0, 255);
        near.add(value, 10, 255 - value);
        if (i < 32) few.add(value, 0, 255 - value);
    }
    return mixture.matches() && !red.matches() && !purple.matches() && !near.matches() && !few.matches() ? 0 : 1;
}
