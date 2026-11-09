#pragma once

#include <atomic>

// 物理エンジンの実行中の調整値。ブラウザのコマンドの側（PhysicsControlComponent、
// INPUT スレッド）と、PhysicsWorld を持ちステップの境目で値を当てる main.cpp の
// 物理ループとで共有する。
//
// 欄は 2 種類:
//   - 直接の値（paused, substeps, physicsHz）: ループが毎パス読むので、書けば
//     次のステップから効く。ただし substeps と physicsHz は、ループが毎パス
//     シーン設定（scene.substeps() / scene.physicsHz() = SimSettings）から写す
//     ので、ここへ直接書いても次のパスで上書きされる（変えるなら SimSettings）。
//   - 保留の値（pendingIterations, pendingEnvelope, pendingRecovery）:
//     当てるには Chrono を呼ぶ必要があり、それは物理スレッドでステップの間に
//     行う - ループが exchange() で取り出す。0 は「当てるものが無い」。
//     envelope/recovery は最後に当てた値も持ち、計測の表示に出せるようにする。
struct PhysicsTuning {
    std::atomic<bool> reset{false};       // 1 回だけ: 置いた姿勢（設計値）へ戻す
    std::atomic<bool> paused{false};
    std::atomic<int> substeps{1};
    std::atomic<int> physicsHz{60};
    std::atomic<int> pendingIterations{0};   // 0 = 当てるものが無い
    std::atomic<double> pendingEnvelope{0.0};
    std::atomic<double> envelope{0.0};       // 最後に当てた値（表示用）
    std::atomic<double> pendingRecovery{0.0};
    std::atomic<double> recovery{0.0};       // 最後に当てた値（表示用）
};
