#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>
#include <vector>

#include "document/EditorTypes.h"
#include "render/Renderer.h"
#include "scene/SceneComponent.h"

// パーティクルのエミッタ（<worldbody> の <emitter>、EditorTypes.h の
// EmitterDesc）を動かす係。見た目だけ = 物理には何も返さない。
//
//   PHYSICS : 毎ステップ、エミッタごとの量（文書の rate、制御器が
//             emitter:<名前>.rate を書けばそちら）を積み、整数になったぶんの粒子を
//             「付け先のその瞬間の姿勢と速度」で生む。生まれる時刻は
//             シミュレーション時刻（ステップの中で均等にばらす）。
//   RENDER  : 生まれた粒子を受け取り、描くフレームのシミュレーション時刻
//             （Scene::renderSimTime = 補間した姿勢と同じ時刻）で年齢を数えて、
//             位置・大きさ・色を決めて Renderer::setParticles に渡す。点光源を
//             持つエミッタは、生きている粒子の量で明るさを揺らす。
//
// 粒子の動きは「生まれたときの速度で等速」だけ（力も衝突も無い）。時計は物理の
// 時計なので、実時間より遅く回るシーンでも炎の長さ（クランク角）は変わらない。
// シミュレートの開始・停止・Reset（Scene::runGeneration）で全部消える。
class EffectsComponent : public SceneComponent {
public:
    // PHYSICS スレッド: 制御器が「このステップの量」（粒子 / 秒）を書く。
    // 書かなかったステップは文書の rate に戻る（ゼロ次ホールドは制御器の側）。
    // 制御器は自分より先に onPhysicsStep が呼ばれる（登録順）。
    void setRate(int emitter, double rate);

    void onPhysicsStep(Scene& scene, double dt) override;
    void onRender(Scene& scene) override;

private:
    struct Spawn {
        float p[3];
        float v[3];
        double t0 = 0.0;   // 生まれたシミュレーション時刻
        int emitter = -1;
        std::uint64_t gen = 0;
    };

    // ---- PHYSICS スレッド専用 ----
    std::uint64_t physGen_ = ~std::uint64_t(0);
    std::uint64_t physVersion_ = ~std::uint64_t(0);
    std::vector<wizengine::editor::EmitterDesc> physEmitters_;
    std::vector<double> rateOverride_;
    std::vector<char> overrideSet_;
    std::vector<double> carry_;  // 端数（次のステップへ持ち越す粒子の数）
    std::vector<Spawn> pending_;
    std::mt19937 rng_{20261115u};

    // ---- 共有（mutex_ の下）----
    std::mutex mutex_;
    std::vector<Spawn> spawned_;

    // ---- RENDER スレッド専用 ----
    std::uint64_t renderGen_ = ~std::uint64_t(0);
    std::uint64_t renderVersion_ = ~std::uint64_t(0);
    std::vector<wizengine::editor::EmitterDesc> renderEmitters_;
    std::vector<Spawn> alive_;
    std::size_t mesh_ = std::size_t(-1);
    std::size_t meshCapacity_ = 0;
    std::vector<std::size_t> lights_;  // エミッタごとの Renderer のライト番号（無し = -1）
    std::vector<wizengine::Renderer::Particle> scratch_;
};
