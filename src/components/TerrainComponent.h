#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <math/vec3.h>
#include <nlohmann/json.hpp>

#include "scene/SceneComponent.h"
#include "terrain/TerrainBrush.h"
#include "terrain/TerrainFrame.h"

// テレインの筆: ブラウザの edit.terrain.brush の受け口（画面の点 → テレインの
// ローカルのスタンプ）と、地表に沿うカーソルの輪。
//
// ---- スレッド ---------------------------------------------------------------
//   onCommand    : INPUT  … レイをテレインのローカルへ直し、輪の位置を書き、
//                           begin / move のスタンプと begin / end の印を積む
//                           （積んだときのモード切替の世代 EditorState::modeEpoch
//                           を添える。世代が変わっていたら線分のつなぎも下ろす）
//   onEditorStep : PHYSICS… 積んだものを取り出し、Scene のストローク API へ
//                           （begin / stamp / end。入力が途絶えたら閉じる）。
//                           今の世代でないもの = モード切替の前に積まれたものは
//                           捨てる（エディタへ戻ったときに勝手に当たらない）
//   onRender     : RENDER … 輪を細線セット（gizmo::kLineSetBrushRing）へ
//                           （入力が途絶えた・エディタでないなら消す）
// スレッドをまたぐのは ring_ と pending_ だけで、どちらの mutex も末端の
// ロック（写す・積む間だけ持ち、中で他のロックを取らない。onRender は
// objectsMutex_ を持ったまま ringMutex_ を取る）。
class TerrainComponent : public SceneComponent {
public:
    bool onCommand(Scene& scene, std::size_t camIndex,
                   const nlohmann::json& msg) override;
    void onEditorStep(Scene& scene, double dt) override;
    void onRender(Scene& scene) override;

private:
    using Clock = std::chrono::steady_clock;

    // カーソルの輪（ローカルの中心と半径）。INPUT が書き、RENDER が読む。
    struct Ring {
        bool visible = false;
        double x = 0.0;
        double z = 0.0;
        double radius = 0.0;
        // 最後に筆の入力（hover を含む）を受けた時刻 = 輪の寿命の起点。
        // 送るかどうかの比較（==）には入れない。
        Clock::time_point seen{};
        bool operator==(const Ring& o) const {
            return visible == o.visible && x == o.x && z == o.z && radius == o.radius;
        }
    };
    // INPUT → PHYSICS の 1 件（begin / end の印か、スタンプ 1 個）。
    struct Pending {
        enum class Kind { Begin, Stamp, End };
        Kind kind = Kind::Stamp;
        wizengine::terrain::BrushStamp stamp;
        std::uint64_t epoch = 0;  // 積んだときの EditorState::modeEpoch
    };

    void setRing(const Ring& ring);
    // 積む（世代は seenEpoch_ = この入力を受けたときに見た値）。
    void push(Pending::Kind kind, const wizengine::terrain::BrushStamp& stamp = {});
    // ストロークの線分のつなぎと Flatten の目標を下ろす（INPUT）。
    void resetStroke();

    std::mutex ringMutex_;
    Ring ring_;
    std::mutex pendingMutex_;
    std::vector<Pending> pending_;
    // 最後にストロークの入力（begin / move / end。hover は含まない）を受けた
    // 時刻（pendingMutex_）。ストロークのタイムアウトの起点。
    Clock::time_point lastInput_{};

    // INPUT スレッドだけが使う（ストロークの線分のつなぎ）。
    std::uint64_t seenEpoch_ = 0;   // 前回の入力で見たモード切替の世代
    bool stroking_ = false;        // begin を受けて end をまだ受けていない
    bool havePrev_ = false;         // 前回の点がある（無ければ次の点は線分の始点）
    double prevX_ = 0.0, prevZ_ = 0.0;
    Clock::time_point prevTime_{};
    bool haveTarget_ = false;       // Flatten の目標（ストロークで最初に当たった点の高さ）
    double flattenTarget_ = 0.0;

    // PHYSICS スレッドだけが使う（ストロークが開いているかは Scene が持つ）。
    std::vector<Pending> work_;     // pending_ と入れ替える受け皿
    std::vector<wizengine::terrain::BrushStamp> batch_;  // 1 パスぶんのスタンプ

    // RENDER スレッドだけが使う（前回送った輪。同じなら送らない）。
    Ring drawn_;
    std::shared_ptr<const wizengine::terrain::HeightField> drawnField_;
    wizengine::terrain::TerrainFrame drawnFrame_;
    std::vector<filament::math::float3> ringPoints_;
};
