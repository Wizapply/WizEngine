#pragma once

#include <chrono>

// 計時の小さな関数（エンジン非依存）。経過時間の式をあちこちに書かない。
namespace wizengine {

// from から to までの秒。
inline double secondsBetween(std::chrono::steady_clock::time_point from,
                             std::chrono::steady_clock::time_point to) {
    return std::chrono::duration<double>(to - from).count();
}

// from から今までのミリ秒（重い作業のログ用）。
inline double elapsedMs(std::chrono::steady_clock::time_point from) {
    return secondsBetween(from, std::chrono::steady_clock::now()) * 1000.0;
}

}  // namespace wizengine
