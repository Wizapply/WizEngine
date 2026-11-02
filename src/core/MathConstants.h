#pragma once

// 層をまたいで使う数学の定数。値はここ 1 か所に書き、各層はこれを使う
// （scene の scenemath::kPi もこれの別名）。
namespace wizengine {

inline constexpr double kPi = 3.14159265358979323846;

}  // namespace wizengine
