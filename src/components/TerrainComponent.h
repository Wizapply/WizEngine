#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <math/vec3.h>
#include <math/vec4.h>
#include <nlohmann/json.hpp>

#include "scene/SceneComponent.h"
#include "terrain/TerrainBrush.h"
#include "terrain/TerrainFrame.h"

namespace wizengine {
class Renderer;
}

// テレインの筆: ブラウザの edit.terrain.brush の受け口（画面の点 → テレインの
// ローカルのスタンプ）と、地表に沿うカーソルの輪と円盤。
//
// ---- スレッド ---------------------------------------------------------------
//   onCommand    : INPUT  … レイをテレインのローカルへ直して輪の位置を書き、
//                           begin / end の印と、begin / move / end の点ごとの
//                           スタンプ（end は離した点までの最後の 1 区間）を積む。
//                           積むものには入力を受けたときに読んだモード切替の
//                           世代（EditorState::modeEpoch）を添え、前の入力から
//                           世代が変わっていたら線分のつなぎと Flatten の目標を
//                           下ろす
//   onEditorStep : PHYSICS… 積んだものを取り出し、Scene のストローク API へ
//                           （begin / stamp / end。入力が途絶えたら閉じる）。
//                           今の世代でないもの = モード切替の前に積まれたものは
//                           捨てる（エディタへ戻ったときに勝手に当たらない）
//   onRender     : RENDER … 最初の呼び出しで輪の細線セットと円盤のオーバーレイを
//                           作り、輪と円盤を地表に沿わせて送る（入力が途絶えた・
//                           エディタでないなら消す）
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
    // 前の点（無ければ同じ点）から (x, z) までのスタンプを積み、(x, z) を前の点に
    // する（INPUT）。havePrev_ は呼び手が決める。
    void pushSegment(wizengine::terrain::BrushStamp s, double x, double z,
                     Clock::time_point now);
    // 輪と円盤を消す（前回出していたときだけ空を送る。RENDER）。
    void hideCursor(wizengine::Renderer& renderer);

    std::mutex ringMutex_;
    Ring ring_;
    std::mutex pendingMutex_;
    std::vector<Pending> pending_;
    // 最後にストロークの入力（begin / move / end。hover は含まない）を受けた
    // 時刻（pendingMutex_）。ストロークのタイムアウトの起点。
    Clock::time_point lastInput_{};

    // INPUT スレッドだけが使う（ストロークの線分のつなぎ）。
    std::uint64_t seenEpoch_ = 0;   // 前回の入力で見たモード切替の世代
    bool stroking_ = false;         // begin を受けて end をまだ受けていない
    bool havePrev_ = false;         // 前回の点がある（無ければ次の点は線分の始点）
    double prevX_ = 0.0, prevZ_ = 0.0;
    double prevY_ = 0.0;            // 前回の当たった点の高さ（外れた点の平面）
    Clock::time_point prevTime_{};
    bool haveTarget_ = false;       // Flatten の目標（ストロークで最初に当たった点の高さ）
    double flattenTarget_ = 0.0;
    std::uint32_t stampSerial_ = 0; // スタンプの通し番号（Paint のくじの種）

    // PHYSICS スレッドだけが使う（ストロークが開いているかは Scene が持つ）。
    std::vector<Pending> work_;     // pending_ と入れ替える受け皿
    std::vector<wizengine::terrain::BrushStamp> batch_;  // 1 パスぶんのスタンプ

    // RENDER スレッドだけが使う（前回送った輪。同じなら送らない）。
    bool cursorMade_ = false;       // 細線セットとオーバーレイを作った
    std::size_t ringSet_ = 0;       // 輪の細線セット（Renderer::addLineSet）
    std::size_t discMesh_ = 0;      // 円盤のオーバーレイ（Renderer::addOverlayMesh）
    Ring drawn_;
    std::shared_ptr<const wizengine::terrain::HeightField> drawnField_;
    wizengine::terrain::TerrainFrame drawnFrame_;
    std::vector<filament::math::float3> ringPoints_;
    std::vector<filament::math::float3> discPositions_;
    std::vector<filament::math::float4> discColors_;
    std::vector<uint16_t> discIndices_;
};
