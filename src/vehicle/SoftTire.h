#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "vehicle/Formula.h"
#include "vehicle/VehicleMath.h"
#include "vehicle/VehicleTypes.h"

// ソフトタイヤ: 車輪の見た目（タイヤのメッシュ）を質点ばねで変形させる。
//
// 車輪は剛体ではない（レイキャスト式、WheelController）ので、シーンの
// ソフトボディ（Chrono の粒子）をそのまま車輪にはできない。代わりに
// タイヤの表面を「リムに付いたトレッドの輪」として質点ばねで持ち、
//   * リムの粒子は運動学的（車輪の姿勢そのもの。舵・回転・サスに追従）
//   * トレッドの粒子は自由で、リムへの「空気圧」ばね・周方向・幅方向・
//     対角線・曲げのばねで結ばれ、地面（接地点の平面）に押し戻される
// ことで、荷重で潰れた接地面・転がりながら戻る変形が出る。ばねの解き方は
// PhysicsWorld のソフトボディと同じ陰解法（1 本ずつ implicit Euler を
// ガウス・ザイデル）なので発散しない。粒子は車輪ローカル（リムと一緒に
// 回る座標系）で解き、地面の平面のほうを回す - 速く回る車輪でも離散化で
// 膨らまない（SoftTire.cpp の step 参照）。
//
// 物理への影響は WheelController 側の「径方向ばね」（サスと直列、潰れ =
// 荷重 / stiffness）だけで、このメッシュ自体は車体に力を返さない
// （見た目）。Chrono も Filament も知らないので vehicle_test で回せる。
//
// 粒子の並び（= 描画の頂点の並び）: 行 r = 0 が左ハブ、1 が左リム（フランジ）、
// 2..rows+1 がトレッド、rows+2 が右リム、rows+3 が右ハブ。行の中は周方向
// i = 0..segments-1 で、粒子 (r, i) の番号は r * segments + i。最後に中心の
// キャップ 2 個（左・右）。表面は隣り合う行を四角で結んだ帯 + 両側の扇で、
// 閉じた面になっている（穴があると背面カリングで内面が透ける）。
namespace wizengine {
namespace vehicle {

struct SoftTireTopology {
    int segments = 0;
    int rows = 0;                         // トレッド列（リムを除く）
    std::size_t vertexCount = 0;          // = 粒子数 = (rows + 4) * segments + 2
    std::vector<std::uint32_t> indices;   // 三角形（外から見て反時計回り）
};

class SoftTire {
public:
    explicit SoftTire(const TireDesc& tire);

    // 描画側が形状を作るための位相（粒子数と三角形）。
    static SoftTireTopology topology(const TireDesc& tire);
    // 粒子の当たり半径（表面はこれだけ外へ押し出して描く）。
    static double particleRadius(const TireDesc& tire);
    // 静止形状の粒子位置（ワールド、3 個で 1 粒子）。center / rot は車輪の
    // 姿勢（X = 車軸）。走らせていないとき（エディタ中）の表示用。
    static void restParticles(const TireDesc& tire, const Vec3& center,
                              const Quat& rot, std::vector<float>& out);

    // 次の step で姿勢に置き直す（走行状態のリセット）。
    void reset();
    // 物理 1 ステップ。center / rot は車輪の姿勢（ワールド、X = 車軸）、
    // grounded なら groundPoint / groundNormal の平面より上にトレッドを保つ。
    // load / deflection は空気圧の式へ渡す文脈（荷重 N、物理側の潰れ m）。
    void step(const Vec3& center, const Quat& rot, bool grounded,
              const Vec3& groundPoint, const Vec3& groundNormal, double load,
              double deflection, double dt);

    // ---- 空気圧 -----------------------------------------------------------
    // 閉じたメッシュが囲む体積から圧力（静止からの増分）を出し、径方向の
    // 陰解法ばね（剛性 = 圧力の一様モードの線形化、自然長 = 静止半径 +
    // 圧力ぶんの膨らみ）としてトレッドへ掛ける。式は TireFormula.h の約束
    // （pressureFormulaInputs / Outputs）で差し替えられる（nullptr = 組み込みの
    // 等温変化）。式が失敗（NaN・実行時エラー）したステップは組み込みで代用
    // し、回数を formulaFailures() に数える。<tire pressure> が 0 なら項ごと切る。
    void setPressureFormula(std::unique_ptr<FormulaInstance> formula);
    int formulaFailures() const { return formulaFailures_; }
    double volumeRatio() const { return ratio_; }   // V / V0（直近の step）
    double pressure() const { return pressure_; }   // 直近の step の増分 Pa

    // 粒子のワールド位置（直近の step の姿勢で置いたもの）。
    const std::vector<Vec3>& particles() const { return world_; }
    std::size_t particleCount() const { return world_.size(); }
    double radius() const { return rp_; }
    // 描画スナップショット用（3 個で 1 粒子）。
    void particlesAsFloats(std::vector<float>& out) const;

private:
    struct Spring {
        int a = 0, b = 0;
        double rest = 0.0, k = 0.0, c = 0.0;
    };
    TireDesc tire_;
    int n_ = 0, rows_ = 0;
    double rp_ = 0.0;
    double mass_ = 0.0;              // 粒子 1 個の質量
    std::vector<Vec3> rest_;         // 車輪ローカルの静止位置
    std::vector<char> kinematic_;    // 1 = リム（姿勢に追従）
    std::vector<Spring> springs_;
    std::vector<Vec3> pos_, vel_;    // 車輪ローカル（リムと一緒に回る座標系）
    std::vector<Vec3> world_;        // 直近の step でワールドへ置いた粒子位置
    bool placed_ = false;

    // 空気圧。indices_ は閉じた面（topology と同じ）、areaVec_ は頂点の面積
    // ベクトル（隣接三角形の面積 × 外向き法線 / 3 の和 = 圧力を掛ける面）。
    std::vector<std::uint32_t> indices_;
    std::vector<Vec3> areaVec_;
    double restVolume_ = 0.0;
    double totalArea_ = 0.0;   // トレッドの面積の合計（陰解法の係数に使う）
    double ratio_ = 1.0;
    double pressure_ = 0.0;
    // 今ステップの圧力を表す径方向ばね: 剛性 = pressureK_ × |A_i|、自然長 =
    // 静止半径 + pressureOffset_（SoftTire.cpp の computePressure 参照）。
    double pressureK_ = 0.0;
    double pressureOffset_ = 0.0;
    std::unique_ptr<FormulaInstance> pressureFormula_;
    int formulaFailures_ = 0;
    double volumeOf(const std::vector<Vec3>& p) const;
    void areaVectorsOf(const std::vector<Vec3>& p, std::vector<Vec3>& out) const;
    void computePressure(double load, double deflection, double dt);
};

}  // namespace vehicle
}  // namespace wizengine
