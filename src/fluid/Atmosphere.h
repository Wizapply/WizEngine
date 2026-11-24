#pragma once
// 空気の状態（密度・気象条件・風）の純粋な計算。Chrono も Filament も知らない。
//
// 密度の決め方は 2 通り（シーン文書の <option>、`SimSettings` の air*）:
//   * 直接: density（kg/m³）をそのまま使う（MuJoCo の <option density> と同じ）。
//   * 気象条件から（atmosphere="true"）: 温度・湿度・海面気圧・標高から湿り空気の
//     密度を出す。
//       その場の気圧 p = p0 (1 − 2.25577e-5 h)^5.25588
//             （国際標準大気の対流圏の気圧の式。h は標高 m、p0 は海面気圧 =
//               天気予報の気圧。11 km まで）
//       飽和水蒸気圧 es = 611.21 exp((18.678 − T/234.5) T / (257.14 + T))
//             （Buck の式。T は ℃、Pa）
//       水蒸気分圧 pv = RH/100 × es、乾燥空気の分圧 pd = p − pv
//       ρ = pd / (Rd T_K) + pv / (Rv T_K)、Rd = 287.058、Rv = 461.495 J/(kg·K)
//     15 ℃・0 %・101325 Pa・標高 0 で 1.2250 kg/m³（標準大気）。暑い・湿っている・
//     高地ほど薄い（20 ℃ 50 % で 1.199、30 ℃ 80 % で 1.150、標高 1600 m・15 ℃ で 1.010）。
//     温度はその場の気温（標高で下げない = 計測した値を入れる）。
//
// 制御器（スクリプト）からの上書き（`AirOverride`）: air:<量> に書いた値がそのステップの
// 文書の値より勝つ（AirOverride の has* が立った項目だけ）。気象条件を 1 つでも書けば
// 密度は気象条件から計算し直す（文書が「直接」でも）。air:density を書けばそれが勝つ。
// 文書（シーンの保存）は書き換えない - 実行中だけの値で、シミュレートを止めると戻る。

#include <algorithm>
#include <cmath>

#include "fluid/AirDrag.h"
#include "fluid/FluidMath.h"

namespace wizengine::fluid {

constexpr double kAirRd = 287.058;   // 乾燥空気の気体定数 J/(kg·K)
constexpr double kAirRv = 461.495;   // 水蒸気の気体定数 J/(kg·K)
constexpr double kSeaLevelPressure = 101325.0;  // Pa

// 標高 h (m) のその場の気圧（海面気圧 p0 から。国際標準大気の対流圏）。
inline double pressureAtAltitude(double seaLevelPa, double altitudeM) {
    const double base = std::max(1e-6, 1.0 - 2.25577e-5 * altitudeM);
    return seaLevelPa * std::pow(base, 5.25588);
}

// 飽和水蒸気圧 (Pa、Buck の式。T は ℃)。
inline double saturationVaporPressure(double celsius) {
    return 611.21 * std::exp((18.678 - celsius / 234.5) * celsius / (257.14 + celsius));
}

// 湿り空気の密度 (kg/m³)。温度 ℃・相対湿度 %・その場の気圧 Pa。
inline double humidAirDensity(double celsius, double humidityPercent, double pressurePa) {
    const double tk = celsius + 273.15;
    if (tk <= 1.0 || pressurePa <= 0.0) return 0.0;
    const double rh = std::clamp(humidityPercent, 0.0, 100.0) / 100.0;
    // 水蒸気分圧は全圧を超えない（沸点より上の湿度 100 % など）。
    const double pv = std::min(rh * saturationVaporPressure(celsius), pressurePa);
    const double pd = pressurePa - pv;
    return pd / (kAirRd * tk) + pv / (kAirRv * tk);
}

// 文書の空気の設定（SimSettings の air* を詰め替えたもの。fluid/ は文書を知らない）。
struct AirSettings {
    double density = 0.0;         // 直接指定の密度 kg/m³（0 = 空気抵抗なし）
    bool atmosphere = false;      // true = 気象条件から密度を出す
    double temperature = 15.0;    // ℃
    double humidity = 0.0;        // 相対湿度 %
    double pressure = kSeaLevelPressure;  // 海面気圧 Pa
    double altitude = 0.0;        // 標高 m
    Vec3 wind;                    // m/s
    double drag = 1.0, lift = 1.0, magnus = 1.0;  // 倍率
};

// 制御器（スクリプト）がこのステップに書いた値。has が立った項目だけ文書より勝つ。
struct AirOverride {
    enum Field {
        Density, Temperature, Humidity, Pressure, Altitude,
        WindX, WindY, WindZ, Drag, Lift, Magnus, kCount
    };
    bool has[kCount] = {};
    double value[kCount] = {};
    void set(Field f, double v) {
        has[f] = true;
        value[f] = v;
    }
    bool any() const {
        for (bool h : has) {
            if (h) return true;
        }
        return false;
    }
    bool anyCondition() const {
        return has[Temperature] || has[Humidity] || has[Pressure] || has[Altitude];
    }
};

// いまの空気（文書 + 上書きを解いた結果）。
struct AirState {
    double density = 0.0;          // 実際に使う密度 kg/m³（0 = 空気抵抗なし）
    bool fromConditions = false;   // 密度を気象条件から出したか
    double temperature = 15.0;     // ℃
    double humidity = 0.0;         // %
    double pressure = kSeaLevelPressure;  // 海面気圧 Pa
    double altitude = 0.0;         // m
    double localPressure = kSeaLevelPressure;  // その場の気圧 Pa
    double conditionsDensity = 0.0;  // 気象条件から出した密度（直接指定のときも参考に）
    Vec3 wind;
    double drag = 1.0, lift = 1.0, magnus = 1.0;
    bool scripted = false;         // 制御器が上書きしている項目がある

    bool enabled() const { return density > 0.0 && drag + lift + magnus > 0.0; }
    AirParams params() const {
        AirParams p;
        p.density = density;
        p.wind = wind;
        p.dragScale = drag;
        p.liftScale = lift;
        p.magnusScale = magnus;
        return p;
    }
};

// 値の範囲（スクリプトから来る値もここで丸める。文書は clampSim が同じ範囲で丸める）。
inline double clampAirTemperature(double c) { return std::clamp(c, -80.0, 80.0); }
inline double clampAirHumidity(double h) { return std::clamp(h, 0.0, 100.0); }
inline double clampAirPressure(double p) { return std::clamp(p, 1000.0, 200000.0); }
inline double clampAirAltitude(double h) { return std::clamp(h, -500.0, 11000.0); }
inline double clampAirDensity(double d) { return std::clamp(d, 0.0, 100.0); }
inline double clampAirWind(double w) { return std::clamp(w, -100.0, 100.0); }
inline double clampAirScale(double s) { return std::clamp(s, 0.0, 10.0); }

inline AirState resolveAir(const AirSettings& doc, const AirOverride& ov) {
    using F = AirOverride;
    auto pick = [&](F::Field f, double docValue) { return ov.has[f] ? ov.value[f] : docValue; };
    AirState s;
    s.temperature = clampAirTemperature(pick(F::Temperature, doc.temperature));
    s.humidity = clampAirHumidity(pick(F::Humidity, doc.humidity));
    s.pressure = clampAirPressure(pick(F::Pressure, doc.pressure));
    s.altitude = clampAirAltitude(pick(F::Altitude, doc.altitude));
    s.localPressure = pressureAtAltitude(s.pressure, s.altitude);
    s.conditionsDensity = humidAirDensity(s.temperature, s.humidity, s.localPressure);
    s.wind = {clampAirWind(pick(F::WindX, doc.wind.x)), clampAirWind(pick(F::WindY, doc.wind.y)),
              clampAirWind(pick(F::WindZ, doc.wind.z))};
    s.drag = clampAirScale(pick(F::Drag, doc.drag));
    s.lift = clampAirScale(pick(F::Lift, doc.lift));
    s.magnus = clampAirScale(pick(F::Magnus, doc.magnus));
    if (ov.has[F::Density]) {
        s.density = clampAirDensity(ov.value[F::Density]);
    } else if (doc.atmosphere || ov.anyCondition()) {
        s.density = s.conditionsDensity;
        s.fromConditions = true;
    } else {
        s.density = clampAirDensity(doc.density);
    }
    s.scripted = ov.any();
    return s;
}

}  // namespace wizengine::fluid
