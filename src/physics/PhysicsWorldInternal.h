#pragma once

// PhysicsWorld の実装ファイル（PhysicsWorld*.cpp）だけが使う私的ヘッダ。実装を
// 担当ごとに分けたので、2 つ以上のファイルが使う型と小さな関数をここに 1 か所で
// 持つ。公開 API は PhysicsWorld.h。他のファイルからは含めないこと。
//
//   PhysicsWorld.cpp             : 構築・1 ステップの順番・全体を起こす・機能の有無
//   PhysicsWorldSystem.cpp       : 系を組む順番と系の設定の中継（本体は PhysicsSystem）
//   PhysicsWorldRegistry.cpp     : ボディの登録と退場の順番（名簿は BodyRegistry）
//   PhysicsWorldBodies.cpp       : ボディの組み立て・形の記録の作成
//   PhysicsWorldMotion.cpp       : 力・速度・質量・姿勢・固定
//   PhysicsWorldSleeping.cpp     : 眠りの設定と計時
//   PhysicsWorldMaterials.cpp    : 共有 / 専用の材質
//   PhysicsWorldLoads.cpp        : 定常荷重
//   PhysicsWorldGravity.cpp      : ボディ別の重力オフ
//   PhysicsWorldDamping.cpp      : 減衰の設定
//   PhysicsWorldSoft.cpp         : ソフトボディ
//   PhysicsWorldContacts.cpp     : 接触の報告
//   PhysicsWorldStaticPairs.cpp  : 固定物どうしの衝突ペアを作らせない
//   PhysicsWorldTerrain.cpp      : テレインの接触
//   PhysicsWorldJoints.cpp       : ジョイント
//   PhysicsWorldJointsManual.cpp : Multicore 用の手計算のジョイント
//   PhysicsWorldCables.cpp       : FEA ケーブル
//   PhysicsWorldModal.cpp        : モーダル解析
//   PhysicsWorldDiag.cpp         : 診断ログ
//
// using namespace はここに書かない（各 .cpp に書く）。

#include "physics/PhysicsParts.h"
#include "physics/PhysicsWorld.h"
#include "physics/ChronoCompat.h"
#include "terrain/HeightField.h"
#include "terrain/TerrainContact.h"

#ifdef WIZ_HAVE_BULLET_PAIR_FILTER
#include <chrono/collision/bullet/BulletCollision/CollisionDispatch/cbtCollisionWorld.h>
#endif
#include <chrono/core/ChMatrix33.h>
#include <chrono/physics/ChContactMaterial.h>
#include <chrono/physics/ChSystem.h>

#include <array>
#include <cmath>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

// +Z をこの向きに合わせる回転。ChLinkLockRevolute はリンク座標系の Z 軸まわり
// に回り、ChLinkLockPrismatic は Z 軸方向にスライドするので、ユーザーが指定した
// ワールド軸をリンクの Z に持ってくる必要がある。
// Chrono のベクトル演算子は版ごとに名前が違うので、成分計算で書いてある。
inline chrono::ChQuaternion<> quatFromZAxis(const chrono::ChVector3d& axis) {
    double ax = axis.x(), ay = axis.y(), az = axis.z();
    const double n = std::sqrt(ax * ax + ay * ay + az * az);
    if (n < 1e-9) return chrono::ChQuaternion<>(1, 0, 0, 0);
    ax /= n;
    ay /= n;
    az /= n;

    const double c = az;  // dot((0,0,1), axis)
    if (c < -0.999999) {  // 真後ろ: X 軸まわりに 180 度
        return chrono::ChQuaternion<>(0, 1, 0, 0);
    }
    // 最短回転。cross((0,0,1), axis) = (-ay, ax, 0)
    chrono::ChQuaternion<> q(1.0 + c, -ay, ax, 0.0);
    q.Normalize();
    return q;
}

// ---- テレインの自前の接触（型と小道具）---------------------------------------
namespace wterrain = wizengine::terrain;

// テレインの接触の判定の設定。値はベンチ（B1 / B2、chrono_bench の mode=hf
// hfimpl=lib）の推薦値: envelope 6 mm + 近づく速度の項 1.5·max(0, −v_n)·dt、
// 1 ボディ 16 点まで、箱の面サンプル無し。dt は毎ステップ書く。
// pointId（反力キャッシュの添字）は faceSamples で変わるので、proxy の点数を
// 数えるのも判定するのもこの同じ値を使う（途中で変えない）。
inline wterrain::ContactParams terrainContactParams() {
    wterrain::ContactParams p;
    p.envelope = 0.006;
    p.approachSpeedFactor = 1.5;
    p.dt = 0.0;
    p.maxPerBody = 16;
    p.faceSamples = 0;
    return p;
}

inline wterrain::Vec3 toTerrainVec(const chrono::ChVector3d& v) {
    return wterrain::Vec3(v.x(), v.y(), v.z());
}

inline wterrain::Mat3 toTerrainMat(const chrono::ChMatrix33<>& r) {
    wterrain::Mat3 m;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m(i, j) = r(i, j);
    }
    return m;
}

// ボディ 1 個の形の記録と、点ごとの反力キャッシュ（Chrono の
// ChCollisionInfo::reaction_cache へアドレスを渡す。接触は毎ステップ作り直される
// ので、ウォームスタートはこれ頼み）。was = 前のステップにその点が接触したか
// （していなければ、今回渡す前にキャッシュを 0 に戻す = 古い反力で始めない）。
// fallback = 形の記録を作れなかった動くボディの安全網（collectTerrainContacts が
// 初めて見たときに作る）。proxy は 8 点の Points で、毎ステップ衝突モデルの
// AABB（ワールド軸、絶対座標）の 8 角をボディ原点からの相対で入れ直す。
struct PhysicsWorld::TerrainProxyRec {
    wterrain::BodyProxy proxy;
    std::vector<std::array<float, 6>> cache;  // 点ごとの永続 float[6]（再確保しない）
    std::vector<char> was;
    std::vector<char> now;  // 作業用（was と入れ替える）
    bool fallback = false;
};

struct PhysicsWorld::TerrainState {
    std::shared_ptr<const wterrain::HeightField> field;  // null = 接触しない
    wterrain::ContactTerrain contactTerrain;             // field から作った早期棄却の値
    std::size_t ghost = PhysicsWorld::kInvalidId;        // 接触の相手のボディ（physId）
    BodyOptions options;                                 // テレイン側の物性
    // 丸い形（Contact::round = 球）との接触 = options のまま。それ以外 = 転がり・
    // スピン摩擦 0（多点で触れる箱が BB ソルバで斜面に沈むため。ベンチ B1 / B2）。
    std::shared_ptr<chrono::ChContactMaterial> matRound;
    std::shared_ptr<chrono::ChContactMaterial> matFlat;
    wterrain::ContactParams params = terrainContactParams();
    std::vector<wterrain::Contact> contacts;  // 作業用（ボディ 1 個ぶん）
};

// ChSystem::ComputeCollisions の末尾（Bullet の接触を足した後）から呼ばれる。
// PhysicsWorld が系を持つ（p_）ので、系より先に world が消えることはない。
struct PhysicsWorld::TerrainCallback : public chrono::ChSystem::CustomCollisionCallback {
    explicit TerrainCallback(PhysicsWorld* w) : world(w) {}
    void OnCustomCollision(chrono::ChSystem* sys) override {
        if (world && sys) world->collectTerrainContacts(*sys);
    }
    PhysicsWorld* world = nullptr;
};

// 固定物どうしの衝突ペアを作らせないフィルタ。理由と仕組みは
// PhysicsWorldStaticPairs.cpp の注記。
#ifdef WIZ_HAVE_BULLET_PAIR_FILTER
struct PhysicsWorld::StaticPairFilter : public cbtOverlapFilterCallback {
    std::unordered_set<const void*> statics;  // 固定ボディの ChCollisionModelBullet*
    cbtCollisionWorld* world = nullptr;       // 付けている衝突ワールド（null = 無効）
    bool ready = false;  // 衝突系の初期化後に集合を作った（それまでは実体が無い）

    bool isStatic(const cbtBroadphaseProxy* p) const {
        const auto* o = static_cast<const cbtCollisionObject*>(p->m_clientObject);
        return o && statics.count(o->getUserPointer()) != 0;
    }
    bool needBroadphaseCollision(cbtBroadphaseProxy* p0, cbtBroadphaseProxy* p1) const override {
        // 既定の判定（衝突ファミリの group / mask）はそのまま残す。
        bool collides = (p0->m_collisionFilterGroup & p1->m_collisionFilterMask) != 0;
        collides = collides && (p1->m_collisionFilterGroup & p0->m_collisionFilterMask);
        if (!collides) return false;
        return !(isStatic(p0) && isStatic(p1));
    }
    // 既にある固定どうしのペアを消す。消した数を返す。
    std::size_t purge() {
        if (!world) return 0;
        cbtOverlappingPairCache* cache = world->getPairCache();
        cbtBroadphasePairArray& pairs = cache->getOverlappingPairArray();
        std::vector<std::pair<cbtBroadphaseProxy*, cbtBroadphaseProxy*>> doomed;
        for (int i = 0; i < pairs.size(); ++i) {
            if (isStatic(pairs[i].m_pProxy0) && isStatic(pairs[i].m_pProxy1)) {
                doomed.emplace_back(pairs[i].m_pProxy0, pairs[i].m_pProxy1);
            }
        }
        // 走査中に消すと配列が詰まるので、集めてから消す。
        for (const auto& d : doomed) cache->removeOverlappingPair(d.first, d.second, world->getDispatcher());
        return doomed.size();
    }
};
#else
struct PhysicsWorld::StaticPairFilter {};
#endif
