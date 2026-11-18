#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "ui/UiLayout.h"
#include "ui/UiProps.h"
#include "ui/UiTypes.h"

// 要素の種類の表。種類ごとの違い（名前・一覧のアイコン・id のビット・操作の仕方・見た目の
// 組み立て）はこの表の 1 行と、組み立て関数 1 本（Ui<種類>.cpp）だけが持つ。
// 種類が持つ性質は性質の表（UiProps の kinds）から引く（propsOfKind）ので、
// ここには写しを持たない。種類を足す = 新しい Ui<種類>.cpp + kKinds に 1 行
// （+ UiProps.h の種類のビットと、性質の表の kinds）。
namespace wizengine {
namespace ui {

class FontProvider;
class MeshSink;

// ポインタでどう操作できるか（UI の入力の担当が見る）。
enum class Interaction : std::uint8_t { None, Click };

// 組み立てに渡す要素 1 個（重ね済みの性質と解いた矩形）。組み立ての間だけ
// 使う一時的な束なので、要素と性質は参照で持つ（コピーしない）。
struct ResolvedElement {
    const ElementDesc& desc;
    const PropSet& props;  // resolve 済み（elementProps）
    Rect rect;             // Canvas の設計上の画素
    double alpha;          // 祖先の不透明度を掛けた自分の不透明度
};

using BuildFn = void (*)(const ResolvedElement&, FontProvider&, MeshSink&);

struct ElementKindInfo {
    const char* name;  // XML の要素名 = JSON の kind
    const char* icon;  // ブラウザの階層一覧に出すアイコン（1 文字。schema の kinds で送る）
    KindMask id;       // 性質の表の kinds と照らすビット
    Interaction interaction;
    BuildFn build;
};

// 各 Ui<種類>.cpp が定義する。
extern const ElementKindInfo kTextKind;
extern const ElementKindInfo kImageKind;

// 名前 → 種類（知らなければ nullptr）。
const ElementKindInfo* kindInfo(const std::string& name);
// 表の全行（文書の「もしかして」と検査用）。
std::size_t kindCount();
const ElementKindInfo& kindAt(std::size_t index);

// 要素の色の性質（Color / OutlineColor など Color 型）× 要素の不透明度。
// 種類の組み立てが共通に使う。
void elementColor(const ResolvedElement& e, Prop colorProp, float out[4]);

}  // namespace ui
}  // namespace wizengine
