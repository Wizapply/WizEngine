#pragma once

// テレインの接触: 高さ格子との自前の接触を Chrono の接触へ詰め替える。形の記録は
// ボディの追加の通知で受け取る。物理スレッド専用。公開の意味は PhysicsWorld.h の
// 同じ役目の宣言（setTerrain ほか）。
//
// 担当から窓口を呼ばない: 眠っている物を起こす必要があるときは戻り値で伝え、
// 起こすのは窓口。

#include <cstddef>
#include <memory>
#include <vector>

#include "physics/PhysicsPart.h"
#include "physics/PhysicsTypes.h"

namespace chrono {
class ChSystem;
}

namespace wizengine {
namespace terrain {
class HeightField;
}
namespace physics {

class BodyRegistry;
class ContactMaterials;
class PhysicsSystem;

class TerrainContact : public PhysicsPart {
public:
    TerrainContact(PhysicsSystem& system, BodyRegistry& bodies, ContactMaterials& materials)
        : system_(system), bodies_(bodies), materials_(materials) {}
    // 系がコールバックをまだ持っているので、コールバックの持ち主を null にする。
    ~TerrainContact() override;
    TerrainContact(const TerrainContact&) = delete;
    TerrainContact& operator=(const TerrainContact&) = delete;

    // 新しい系へコールバックを登録する（Core のみ。Multicore では登録しない）。
    void attach();
    // 系の作り直し: ゴースト・コールバックを捨てる。フレームは残す（形の記録は
    // onBodiesCleared が捨てる）。
    void clear();

    // field を張る / 差し替える。戻り値 = ゴーストの physId（Multicore では kInvalidId）。
    // field は null でないこと。成功したら呼び出し側が眠っている物を起こす。
    std::size_t set(std::shared_ptr<const terrain::HeightField> field, const BodyOptions& options);
    // 起こす必要があるときだけ true。
    bool setFrame(const chrono::ChVector3d& origin, const chrono::ChQuaternion<>& rot);
    // 接触をやめる（ゴーストは残す）。起こす必要があるときだけ true。
    bool clearField();

    bool hasTerrain() const;
    std::size_t bodyId() const;
    bool isGhost(std::size_t id) const;
    bool hasProxy(std::size_t id) const;
    TerrainContactShape contactShape(std::size_t id) const;

    void onBodiesCleared() override;
    void onBodyAdded(std::size_t id, const BodyAddInfo& info) override;
    void onBodyPlaced(std::size_t id) override;
    void onWakeAll() override;
    void onMaterialsChanged() override;

private:
    struct ProxyRec;  // ボディ 1 個の形の記録 + 点ごとの反力キャッシュ
    struct State;     // 格子・ゴースト・材質・判定の設定
    struct Callback;  // ChSystem::CustomCollisionCallback

    // id のボディの形を記録する。
    void setProxy(std::size_t id, const terrain::BodyProxy& proxy);
    // id の「前のステップに接触した」印と反力キャッシュを 0 に戻す（前回の走行の
    // 反力で次の最初のステップを始めないため）。
    void forgetContacts(std::size_t id);
    // テレインと接触しうるか（contactShape と collectContacts の共通の条件。眠りは見ない）。
    bool eligible(std::size_t id) const;
    // 安全網の記録（無ければ作る。LOGW は初めての 1 体だけ）。
    ProxyRec* fallbackRec(std::size_t id);
    // テレインの 2 つの材質へシーン設定とテレイン側の物性を書き直す。
    void refreshMaterials();
    // コールバックの本体（DoStepDynamics の衝突判定の末尾）。
    void collectContacts(chrono::ChSystem& sys);
    // コールバックの持ち主を null にして手放す。
    void dropCallback();

    PhysicsSystem& system_;
    BodyRegistry& bodies_;
    ContactMaterials& materials_;
    // 名簿の番号と並ぶ: テレインに当たる形の記録（無ければ null = テレインの
    // 接触の対象外）。反力キャッシュは記録のときに 1 回だけ確保し、
    // 以後は再確保しない（アドレスを Chrono の接触へ渡すため）。
    std::vector<std::shared_ptr<ProxyRec>> proxies_;
    // テレインのフレーム（setFrame）。単位なら変換を省く。系の作り直しでも残す。
    chrono::ChVector3d frameOrigin_{0, 0, 0};
    chrono::ChQuaternion<> frameRot_{1, 0, 0, 0};
    bool frameIdentity_ = true;
    // テレイン（set が作る。clear で捨てる）とコールバック
    // （attach が Core のときだけ作って系に登録する）。
    std::shared_ptr<State> state_;
    std::shared_ptr<Callback> callback_;
};

}  // namespace physics
}  // namespace wizengine
