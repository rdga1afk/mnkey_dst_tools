#include <gtest/gtest.h>
#include <monkey_dust/render/light_system.h>
#include <cmath>

TEST(LightSystemDefaults, SunDir_NotZero) {
    const LightSystem& ls = LightSystem::Get();
    float l = sqrtf(ls.sun_dir.x * ls.sun_dir.x +
                    ls.sun_dir.y * ls.sun_dir.y +
                    ls.sun_dir.z * ls.sun_dir.z);
    EXPECT_GT(l, 0.0f);
}

TEST(LightSystemDefaults, SunColor_BrightWhite) {
    const LightSystem& ls = LightSystem::Get();
    EXPECT_GE(ls.sun_color.x, 0.5f);
    EXPECT_GE(ls.sun_color.y, 0.5f);
}

TEST(LightSystemDefaults, AmbientColor_Low) {
    const LightSystem& ls = LightSystem::Get();
    EXPECT_LT(ls.ambient_color.x, ls.sun_color.x);
}
