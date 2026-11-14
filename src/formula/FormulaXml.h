#pragma once

#include <string>
#include <vector>

#include "document/SceneXml.h"
#include "formula/Formula.h"

// ノード式の XML（シーン文書の <asset> の <formula>）:
//
//   <formula name="arm_pid">
//     <node id="1" type="in" name="joint:arm.angle" pos="40 40"/>
//     <node id="2" type="const" params="0.5"/>
//     <node id="3" type="sub"/>
//     <node id="4" type="pid" params="40 10 4 -50 50 0.02"/>
//     <node id="5" type="out" name="joint:arm.target"/>
//     <wire from="2" to="3"/>                  ... 出力 0 → 入力 0
//     <wire from="1" to="3" port="1"/>         ... 入力 1 へ
//     <wire from="3" to="4"/>
//     <wire from="4" fromPort="0" to="5"/>
//   </formula>
//
// 読み取りは失敗しない（無い属性は既定値）。おかしな内容は warnings に文で。
namespace wizengine {
namespace formula {

xml::Element formulaElement(const FormulaGraphDesc& graph);
FormulaGraphDesc formulaFromXml(const xml::Element& e,
                                std::vector<std::string>* warnings = nullptr);

}  // namespace formula
}  // namespace wizengine
