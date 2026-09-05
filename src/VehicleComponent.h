#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <nlohmann/json.hpp>

#include "SceneComponent.h"
#include "vehicle/LuaFormula.h"
#include "vehicle/VehicleModel.h"

// 車両（BodyDesc::hasVehicle のオブジェクト）を走らせ、車輪を描く
// SceneComponent。
//
//   onCommand     (INPUT)   … "drive" {throttle, brake, steer, handbrake} を
//                             atomic に置く。どのカメラのページからでも可
//                             （運転はシーンの書き換えではない）。
//   onPhysicsStep (PHYSICS) … シミュレート中、車両ごとの VehicleModel を回して
//                             車体（Chrono のボディ）へサス力・タイヤ力・
//                             空気抵抗を「点に掛かる力」として積む。車輪の
//                             ローカル姿勢を描画用のスナップショットへ書く。
//   onEditorStep  (PHYSICS) … 実行状態を捨て、設計値から車輪の置き場を出す
//                             （静的な沈み込みぶん下げた「止まっている車」）。
//   （描画は PrefabComponent: 車輪と車体の飾りはプレハブの部品として描く。
//     ここは車輪の姿勢を wheelLocalPoses で渡すだけ）
//
// 車輪は Chrono の剛体ではない（レイキャスト式）。地面は物理の床と同じ
// y = 0 の平面として見る - 他のオブジェクトの上には乗れない（今回の範囲外）。
// 複数の車両があれば全部が同じ入力で走る。
//
// ロック: visMutex_ は葉のロック（この中で他のロックを取らない）。RENDER
// スレッドは objectsMutex_ を持ったまま取り、PHYSICS スレッドは何も持たずに
// 取るので順序の問題は無い。
class VehicleComponent : public SceneComponent {
public:
    VehicleComponent();

    bool onCommand(Scene& scene, std::size_t camIndex,
                   const nlohmann::json& msg) override;
    void onPhysicsStep(Scene& scene, double dt) override;
    void onEditorStep(Scene& scene, double dt) override;

    // RENDER スレッド用: そのオブジェクトの車輪の車体ローカル姿勢（物理
    // スレッドが最後に渡したもの）。無ければ false。PrefabComponent が
    // ソケット付きの部品（タイヤ）をこれに重ねる。
    bool wheelLocalPoses(std::size_t objectIndex,
                         std::vector<wizengine::vehicle::WheelLocalPose>& out);

    // /stats 用の計測値（最初の車両ぶん）。どのスレッドから読んでもよい。
    struct Snapshot {
        bool present = false;
        int count = 0;
        double rpm = 0.0;
        int gear = 0;
        double speed = 0.0;   // m/s、前向きが +
        double clutch = 0.0;
    };
    Snapshot snapshot() const;

private:
    // ブラウザからの目標値（INPUT → PHYSICS）。
    std::atomic<double> throttle_{0.0};
    std::atomic<double> brake_{0.0};
    std::atomic<double> steer_{0.0};
    std::atomic<double> handbrake_{0.0};

    // ノード式の実行エンジン（LuaJIT）。Lua ステートは物理スレッド専用で、
    // VehicleModel の生成（式のコンパイル）と onPhysicsStep の中でしか触らない。
    std::unique_ptr<wizengine::vehicle::LuaRuntime> lua_;
    // 実行状態（PHYSICS スレッド専用）。キーはオブジェクト番号。
    std::map<std::size_t, std::unique_ptr<wizengine::vehicle::VehicleModel>> models_;
    std::map<std::size_t, int> reportedFailures_;  // 式の失敗を 1 回だけ報告
    // 取り込んだ計算式の版（EditorState::formulaVersion）。変わっていたら
    // 車両モデルを作り直す = 編集がシミュレート中にもすぐ効く（走行状態は
    // 一度リセットされる）。
    std::uint64_t formulaVersion_ = ~std::uint64_t(0);

    // 車輪の見た目（PHYSICS → RENDER）。キーはオブジェクト番号。
    std::mutex visMutex_;
    std::map<std::size_t, std::vector<wizengine::vehicle::WheelLocalPose>> visuals_;


    // 計測値（PHYSICS → HTTP）。
    std::atomic<int> count_{0};
    std::atomic<double> rpm_{0.0};
    std::atomic<int> gear_{0};
    std::atomic<double> speed_{0.0};
    std::atomic<double> clutch_{0.0};
};
