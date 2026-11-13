#pragma once

namespace wizengine {

// 3 成分の値（位置・回転のオイラー角・寸法など）。シーン文書と UI の文書が共用する。
struct Vec3d {
    double x = 0.0, y = 0.0, z = 0.0;
};

}  // namespace wizengine
