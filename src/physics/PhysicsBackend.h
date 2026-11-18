#pragma once

// どの Chrono の系で回すか。Chrono のヘッダを含まない軽いヘッダに分けてある
// （起動設定 app/AppConfig が Chrono 抜きで読めるように）。Multicore には
// MULTICORE モジュール付きの Chrono（CMake: -DWIZ_USE_MULTICORE=ON）が要り、
// 無ければ Core に倒して LOGW で知らせる（PhysicsSystem::create）。
enum class PhysicsBackend {
    // ChSystemNSC - 接触ソルバは逐次（衝突判定・FEA・Eigen は SetNumThreads の
    // スレッドを使う）、スリープ対応
    Core,
    Multicore,  // ChSystemMulticoreNSC - OpenMP のソルバ + 衝突判定、スリープ無し
};
