#pragma once

#include <string>
#include <vector>

#include "document/SceneXml.h"
#include "vehicle/Formula.h"

// ノード式の XML（<vehicle> の中の <formula>）:
//
//   <formula name="tire_mf">
//     <node id="1" type="in" name="kappa" pos="40 40"/>
//     <node id="2" type="const" params="1.5"/>
//     <node id="3" type="curve" params="0 0 1000 0.5 2000 1"/>
//     <node id="9" type="magic"/>
//     <node id="20" type="out" name="fx"/>
//     <wire from="1" to="9"/>                  ... 出力 0 → 入力 0
//     <wire from="2" to="9" port="1"/>         ... 入力 1 へ
//     <wire from="9" fromPort="0" to="20"/>
//   </formula>
//
// 読み取りは失敗しない（無い属性は既定値）。おかしな内容は warnings に文で。
namespace wizengine {
namespace vehicle {

xml::Element formulaElement(const FormulaGraphDesc& graph);
FormulaGraphDesc formulaFromXml(const xml::Element& e,
                                std::vector<std::string>* warnings = nullptr);

}  // namespace vehicle
}  // namespace wizengine
