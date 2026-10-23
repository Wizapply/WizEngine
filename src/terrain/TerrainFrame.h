#pragma once

#include <cmath>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include "terrain/HeightField.h"

// テレインのフレーム（置き場所 + 回転）。
//
// 高さ格子（HeightField）は**原点基準**（中心 = X / Z の 0、最低点 = Y の 0）で
// 持ち、ワールドへの置き場所はこのフレームで表す: ワールド = origin +
// rotation * ローカル。origin は文書の <terrain pos>（X / Z が中心・Y が
// 最低点）、rotation は <terrain euler>。テレインを動かす・回すのはフレームを
// 差し替えるだけで、格子・描画のメッシュ・接触の早期棄却の値は作り直さない。
//
// 格子を問い合わせる側（接触 = PhysicsWorld、車輪のレイ = VehicleComponent、
// 筆のレイ = TerrainComponent）は、問い合わせの前にワールド → ローカルへ直し、答えを
// ローカル → ワールドへ戻す。描画は Renderer::setTerrainTransform（エンティティの
// 変換）で同じフレームを掛ける。HeightField と同じく Chrono も Filament も
// シーン文書も知らない。
namespace wizengine {
namespace terrain {

struct TerrainFrame {
    Vec3 origin = Vec3::Zero();
    Eigen::Matrix3d rotation = Eigen::Matrix3d::Identity();  // ローカル → ワールド

    // 位置と四元数（w, x, y, z。正規化は呼び手に任せない = ここで正規化する）から。
    // 四元数が零・有限でなければ回転なし。
    static TerrainFrame fromPose(double px, double py, double pz, double qw, double qx,
                                 double qy, double qz) {
        TerrainFrame f;
        f.origin = Vec3(px, py, pz);
        Eigen::Quaterniond q(qw, qx, qy, qz);
        const double n = q.norm();
        if (std::isfinite(n) && n > 1e-12) {
            f.rotation = Eigen::Quaterniond(q.coeffs() / n).toRotationMatrix();
        }
        return f;
    }

    // 点 / 向きの変換。
    Vec3 toLocal(const Vec3& p) const { return rotation.transpose() * (p - origin); }
    Vec3 toWorld(const Vec3& p) const { return rotation * p + origin; }
    Vec3 dirToLocal(const Vec3& d) const { return rotation.transpose() * d; }
    Vec3 dirToWorld(const Vec3& d) const { return rotation * d; }

    // ワールドのレイと地表の交差（HeightField::raycast をローカルで）。距離 t は
    // 回転で長さが変わらないのでそのまま、法線はワールドへ戻す。当たらなければ
    // false で t / normal は触らない。
    bool raycast(const HeightField& field, const Vec3& origin_, const Vec3& dir,
                 double maxDist, double& t, Vec3& normal) const {
        double hitT = 0.0;
        Vec3 n = Vec3::Zero();
        // 判定の戻り値は一度変数に受けてから使う（出力引数と同じ式に並べない規約）。
        const bool hit = field.raycast(toLocal(origin_), dirToLocal(dir), maxDist, hitT, n);
        if (!hit) return false;
        t = hitT;
        normal = dirToWorld(n);
        return true;
    }
};

}  // namespace terrain
}  // namespace wizengine
