#pragma once

#include <string>
#include <vector>

#include "core/Xml.h"
#include "core/XmlWarnings.h"
#include "transmission/AtTypes.h"

// シーン文書の <control> の中の <transmission> 節 ⇔ AtDesc。
//
//   <transmission name="jr913e" input="torque" load="vehicle" rating="700"
//                 inputShaft="input" outputShaft="output">
//     <engine inertia="0.18" idle="650" idleGain="1" redline="7000"/>
//             （input="body" なら body="<剛体の名前>" axis="0 0 1" も）
//     <converter sr="0 0.2 ..." k="120 121 ..." tr="1.9 1.71 ..."
//                pumpInertia="0.1" turbineInertia="0.05"/>
//     <lockup capacity="900" tau="0.25" fromGear="2" speed="18"
//             stiffness="1500" damping="3" inertia="0.03"/>
//     <pump displacement="14" pressureMin="4" pressureMax="18" efficiency="0.85"/>
//     <shift upLight="1500" upFull="6500" downLight="1050" downFull="3500" .../>
//     <output finalDrive="3.133" wheelRadius="0.337" mass="1700" .../>
//     <shaft name="input" inertia="0.012" drag="0.0008"/>  ...
//     <gearset name="P1" sun="input" carrier="c1" ring="n1" zs="60" zr="128"
//              zp="34" planets="4" efficiency="0.985"/>  ...
//     <element name="A" type="brake" a="s2" capacity="0" fill="0.08" tau="0.03"/>
//     <element name="D" type="clutch" a="c1" b="n2"/>  ...
//     <gear name="1" elements="A B D"/>  ...  <gear name="R" elements="A B C"/>
//     <neutral elements="A B"/>
//   </transmission>
//
// preset="jr913e" を書くと JR913E の値から始めて、書いた節だけを上書きする
// （<shaft> / <gearset> / <element> / <gear> を 1 つでも書けば、歯車列はそちらに
// 置き換わる）。手書きの最小の文書は <transmission name="at" preset="jr913e"/> で足りる。
// 保存は常に全部を書く（文書だけで中身が分かるように）。
// 読み取りは失敗しない（無い属性・節は既定値）。つなぎ先の名前の打ち間違い・
// 段の拘束の過不足などは warnings に文で入る（AtKinematics で構造を解いて確かめる）。
namespace wizengine {
namespace transmission {

xml::Element transmissionElement(const AtDesc& desc);
AtDesc transmissionFromXml(const xml::Element& e, xml::Warn& warn);
AtDesc transmissionFromXml(const xml::Element& e, std::vector<std::string>* warnings = nullptr);

}  // namespace transmission
}  // namespace wizengine
