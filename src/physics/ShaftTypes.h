#pragma once

#include <cstddef>
#include <vector>

// 1 次元のシャフト網（Chrono の ChShaft と、それをつなぐ拘束・力）の設計と計測値。
// Chrono を含まない値型（PhysicsWorld の口で受け渡す）。AT の駆動系はこの語彙で組む
// （components/AtDriveline）。番号はすべて shafts の並び、-1 はケース（固定の軸。
// 網ごとに 1 本、内部で作る）。
namespace wizengine {
namespace physics {

struct ShaftNetworkSpec {
    struct Shaft {
        double inertia = 0.01;  // kg m²
        double w0 = 0.0;        // 初めの角速度 (rad/s)
    };
    // 単純遊星: ωs + k ωr − (1 + k) ωc = 0（ChShaftsPlanetary、shaft1 = キャリヤ）。
    struct Planetary {
        int carrier = -1, sun = -1, ring = -1;
        double k = 2.0;
    };
    // 摩擦クラッチ / ブレーキ（ChShaftsClutch。b = -1 でケース = ブレーキ）。伝えられる
    // トルクの上限は毎ステップ setShaftClutchLimits で与える（初めは 0 = 切れている）。
    struct Clutch {
        int a = -1, b = -1;
    };
    // ねじりばね（ChShaftsTorsionSpring）。
    struct Spring {
        int a = -1, b = -1;
        double stiffness = 1000.0, damping = 1.0;
    };
    // 歯車（ChShaftsGear）: ωb = ratio ωa。
    struct Gear {
        int a = -1, b = -1;
        double ratio = 1.0;
    };
    // トルクコンバータ（ChShaftsTorqueConverter。ステータはケース）。表は速度比 SR の
    // 折れ線: 容量係数 K = ω_pump / √T_pump（(rad/s)/√(N·m)）とトルク比 TR。
    struct Converter {
        int pump = -1, turbine = -1;
        std::vector<double> sr, k, tr;
    };
    // 速度モータ（ChShaftsMotorSpeed、軸 ⇄ ケース）。速度は毎ステップ setShaftMotorSpeeds。
    struct SpeedMotor {
        int shaft = -1;
    };
    // 3D の剛体へ直結（ChShaftBodyRotation）。dir は剛体のローカルの回転軸。
    struct BodyLink {
        int shaft = -1;
        std::size_t body = static_cast<std::size_t>(-1);
        double dir[3] = {0.0, 0.0, 1.0};
    };

    std::vector<Shaft> shafts;
    std::vector<Planetary> planetaries;
    std::vector<Clutch> clutches;
    std::vector<Spring> springs;
    std::vector<Gear> gears;
    std::vector<Converter> converters;
    std::vector<SpeedMotor> motors;
    std::vector<BodyLink> bodyLinks;
};

// 直前のステップの結果。トルクは「その軸に掛かる」向き（N·m）。
struct ShaftNetworkState {
    std::vector<double> w;             // 軸の角速度 (rad/s)
    std::vector<double> clutchTorque;  // クラッチが軸 a に掛けたトルク
    std::vector<double> sunTorque;     // 遊星が太陽に掛けたトルク（かみ合いの損失の見積もり）
    std::vector<double> springTorque;  // ばねが軸 b に掛けたトルク
    std::vector<double> gearTorque;    // 歯車が軸 b に掛けたトルク
    std::vector<double> motorTorque;   // モータが軸に掛けたトルク
    std::vector<double> bodyTorque;    // 剛体との結合が軸に掛けたトルク
    struct ConverterState {
        double pumpTorque = 0.0, turbineTorque = 0.0;  // 羽根車 / タービンに掛かるトルク
        double speedRatio = 0.0;
    };
    std::vector<ConverterState> converters;
};

}  // namespace physics
}  // namespace wizengine
