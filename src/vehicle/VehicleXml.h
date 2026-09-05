#pragma once

#include <string>
#include <vector>

#include "document/SceneXml.h"
#include "vehicle/VehicleTypes.h"

// シーン文書の <body> の中の <vehicle> 節 ⇔ VehicleDesc。
//
//   <vehicle ticks="10" brake="2500" drag="0.7" steerRate="4" steerSpeed="12"
//            tcs="0.25" pedalRate="6">
//     <engine torque="300" idle="900" max="6500" inertia="0.25"
//             friction="0.03" frictionConst="10"
//             curve="0 0.35 1000 0.55 2000 0.75 ..."/>      ... rpm 値 の対
//     <clutch torque="500" engage="1300" full="2200"/>
//     <gearbox ratios="3.6 2.1 1.4 1 0.8" reverse="3.4" final="3.9"
//              efficiency="0.92" up="5800" down="2300" time="0.25"
//              automatic="true"/>
//     <center type="open" bias="0.5" stiffness="20" lock="300"/>
//     <formula name="tire_mf"> <node .../> <wire .../> </formula>   ... FormulaXml.h
//     <tire .../> <suspension .../>        ... 全軸の既定値（省略可）
//     <axle z="1.3" y="-0.15" track="1.6" steer="32" ackermann="1"
//           driven="false" brake="1" handbrake="0"
//           diff="open" stiffness="20" lock="300">
//       <tire radius="0.32" width="0.22" inertia="1.2" mu="1" bx="10" cx="1.65" ex="0.97"
//             by="12" cy="1.4" ey="-1" relax="0.15" rolling="0.015"
//             formula="tire_mf"/>            ... 省略 = 組み込みのタイヤ
//       <suspension rest="0.35" travel="0.25" stiffness="35000"
//                   bump="3500" rebound="4500" antiroll="6000"/>
//     </axle>
//   </vehicle>
//
// 読み取りは失敗しない（無い属性・節は既定値）。怪しい内容は warnings に
// 文で入る（SceneDocument と同じ約束）。
namespace wizengine {
namespace vehicle {

xml::Element vehicleElement(const VehicleDesc& desc);
VehicleDesc vehicleFromXml(const xml::Element& e,
                           std::vector<std::string>* warnings = nullptr);

}  // namespace vehicle
}  // namespace wizengine
