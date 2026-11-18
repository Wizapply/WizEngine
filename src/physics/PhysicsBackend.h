#pragma once

// どの Chrono の系で回すか。Chrono のヘッダを含まない軽いヘッダに分けてある
// （起動設定 app/AppConfig が Chrono 抜きで読めるように）。Multicore には
// MULTICORE モジュール付きの Chrono（CMake: -DWIZ_USE_MULTICORE=ON）が要り、
// 無ければ PhysicsWorld が Core に落とす。
enum class PhysicsBackend {
    Core,       // ChSystemNSC - serial solver, supports sleeping
    Multicore,  // ChSystemMulticoreNSC - OpenMP solver + collision, no sleeping
};
