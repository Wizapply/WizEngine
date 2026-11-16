#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/Clamp.h"
#include "core/SceneLimits.h"
#include "core/Vec3d.h"
#include "ui/UiProps.h"  // ElementDesc / KeyDesc の PropSet
#include "ui/UiValue.h"  // kCanvasAttrs の ValueSpec

// UI の文書の形（シーン文書の <canvas> / <ui> / <font> と 1 対 1）。値の型だけで、
// 読み書き（UiXml）・状態（UiState）・描画はここを知って使う側に置く。
namespace wizengine {
namespace ui {

// ---- 要素とアニメ ------------------------------------------------------------
// 要素 1 個。並びは文書順（= 描く順。後ろほど手前）で、親は自分より前にある。
struct ElementDesc {
    int id = -1;        // Canvas（または UI アセット）の中で一意。削除しても詰めない
    std::string kind;   // 種類の名前（"text" / "image" …。種類の表 UiKinds が引く）
    int parent = -1;    // 親の要素の id。-1 = Canvas の直下
    PropSet props;      // 書いてある性質だけが立つ（残りは表の既定値）
    std::string name;   // ノードやアニメのキーが要素を名前で指すための名前（空可）
};

// 名前が同じか（重なり・名前での参照の規則の唯一の定義）: 空の名前はどれとも同じでない
// （名前の無い物どうしは重ならず、名前で指されもしない）。
inline bool sameName(const std::string& a, const std::string& b) {
    return !a.empty() && a == b;
}
// 並びに name と同じ名前の物があるか（sameName の規則）。nameOf(item) はその物の名前を
// 返す（数えない物 = 自分自身・生きていない枠などは nullptr）。Canvas・要素の名前の
// 重なりの確かめ（文書の読み込みの警告とエディタの名前の変更）が使う。
template <class List, class NameOf>
bool nameTaken(const List& list, const std::string& name, NameOf nameOf) {
    for (const auto& item : list) {
        const std::string* n = nameOf(item);
        if (n && sameName(*n, name)) return true;
    }
    return false;
}

// 名前で要素を引く規則（唯一の定義）: 最初に一致した要素（無い・空の名前は
// nullptr）。名前が重なった文書は読み込みが警告し、キーやノードは最初の要素を指す。
inline const ElementDesc* findElementByName(const std::vector<ElementDesc>& elements,
                                            const std::string& name) {
    for (const auto& e : elements) {
        if (sameName(e.name, name)) return &e;
    }
    return nullptr;
}

// 名前の値の決まり（Name 型 = core/SafeName.h の sanitizeSafeName で正規化）。Canvas・
// 要素・宣言の名前を読む・書く側はこの決まりの clampValue で正規化する（1 本）。
inline const ValueSpec& nameSpec() {
    static const ValueSpec spec{PropType::Name, textValue(""), 0.0, 0.0, nullptr};
    return spec;
}

// 名前の無い物の呼び方（ブラウザの一覧・Inspector とステータスが同じ文字列を使う唯一の
// 定義）: 名前があれば名前、無ければ「種類 + 番号」。
inline std::string elementLabel(const ElementDesc& e) {
    return e.name.empty() ? e.kind + " " + std::to_string(e.id) : e.name;
}

// 次に振る要素の id の下限（文書の読み込みの採番と、読み込んだ Canvas の採番の始まり）:
// 今ある最大の id の次（1 から。番号の無い -1 は数えない）。エディタの追加は消した番号を
// 使い回さないよう、UiState が Canvas ごとに増える一方の番号を持つ（これが下限）。
inline int nextElementId(const std::vector<ElementDesc>& elements) {
    int maxId = 0;
    for (const auto& e : elements) maxId = std::max(maxId, e.id);
    return maxId + 1;
}

// アニメのキー 1 個（<key element t ...>）。props はその時刻の値。評価は UiAnim。
struct KeyDesc {
    std::string element;  // 要素の名前
    double t = 0.0;       // 秒（シミュレート時間）
    PropSet props;
};

struct AnimDesc {
    std::string name;
    bool loop = false;
    bool autoplay = false;
    std::vector<KeyDesc> keys;
};

// 共用の UI（<asset> の <ui name file>。中身は assets/ui/*.xml）。
struct UiAssetDesc {
    std::string name;
    std::string file;  // assets/ からの相対パス
    std::vector<ElementDesc> elements;
    std::vector<AnimDesc> anims;
};

// フォントの宣言（<asset> の <font name file>）。
struct FontAssetDesc {
    std::string name;
    std::string file;  // assets/ からの相対パス
};

// ---- Canvas ------------------------------------------------------------------
enum class CanvasMode : std::uint8_t { Overlay, Camera, World };
inline const char* const kCanvasModeNames[] = {"overlay", "camera", "world", nullptr};
static_assert(sizeof(kCanvasModeNames) / sizeof(kCanvasModeNames[0]) - 1 ==
                  std::size_t(CanvasMode::World) + 1,
              "kCanvasModeNames must have one name per CanvasMode");
// cameras 属性の名前（表の行と、それを名前で引く側が共用する）。
inline constexpr const char* kCanvasCamerasAttr = "cameras";
// ui 属性（共用の UI の名前）の名前（表の行と canvasAttrEditable が共用する）。
inline constexpr const char* kCanvasUiAttr = "ui";

// Canvas の既定値（CanvasDesc の初期値と kCanvasAttrs の既定値が同じ値を使う）。
inline constexpr double kCanvasDefaultHeight = 1080.0;
inline constexpr double kCanvasDefaultDistance = 1.0;
inline constexpr double kCanvasDefaultSizeX = 1.6;
inline constexpr double kCanvasDefaultSizeY = 0.9;

struct CanvasDesc {
    std::string name;
    CanvasMode mode = CanvasMode::Overlay;
    std::vector<int> cameras;                  // 空 = 全カメラに出す（Q8）
    // 書かれた cameras が正しくない（無い番号・読めない番号を含む）とき、その
    // 文字列。空でなければどのカメラにも出さない（Q20）。保存はこの文字列を
    // そのまま書き戻す。表示側は canvasShownOnCamera だけを見る。
    std::string invalidCameras;
    double height = kCanvasDefaultHeight;      // 基準の高さ（設計上の画素）
    int order = 0;                             // Canvas どうしの重なり順（大きいほど手前）
    double distance = kCanvasDefaultDistance;  // camera: カメラからの距離（m）
    Vec3d pos;                                 // world: 置き場所（m）
    Vec3d rot;                                 // world: 向き（オイラー角、度）
    double sizeX = kCanvasDefaultSizeX;        // world / camera: 板の幅（m）
    double sizeY = kCanvasDefaultSizeY;        // world / camera: 板の高さ（m）
    std::string ui;                            // 共用の UI の名前。空 = 下の elements を使う
    std::vector<ElementDesc> elements;
    std::vector<AnimDesc> anims;
};

// Canvas の呼び方（elementLabel と同じ流儀。index は UiState の Canvas の番号）。
inline std::string canvasLabel(const CanvasDesc& c, int index) {
    return c.name.empty() ? "Canvas " + std::to_string(index) : c.name;
}

// Canvas 専用の属性の表（mode / cameras / height / order / distance / pos /
// euler / size / ui）。値の型・既定値・範囲は性質と同じ ValueSpec で持つので、
// 文字列 / JSON の変換と丸めは性質の表と同じ関数（parseValue など）で行う。
// get / set が CanvasDesc の欄との詰め替え。属性を足す = ここに 1 行。
struct CanvasAttrInfo {
    const char* name;   // XML の属性名 = JSON のキー
    ValueSpec spec;
    const char* label;  // Inspector の表示名（日本語）
    PropValue (*get)(const CanvasDesc&);
    void (*set)(CanvasDesc&, const PropValue&);
};

// 名前 → Canvas の属性（無ければ nullptr。定義は表の後ろ）。表の set が自分の
// 行の決まり（spec）を引くのにも使う。
inline const CanvasAttrInfo* canvasAttrFromName(const std::string& name);

inline const CanvasAttrInfo kCanvasAttrs[] = {
    {"mode", {PropType::Enum, numberValue(0), 0.0, 0.0, kCanvasModeNames}, "表示方法",
     [](const CanvasDesc& c) { return numberValue(double(c.mode)); },
     [](CanvasDesc& c, const PropValue& v) {
         c.mode = CanvasMode(enumIndex(v.v[0], kCanvasModeNames, int(CanvasMode::Overlay)));
     }},
    // カメラの番号はスロットの範囲（0..kMaxCameraSlots-1）。
    {kCanvasCamerasAttr,
     {PropType::IndexList, numberValue(0), 0.0, double(kMaxCameraSlots - 1)},
     "表示するカメラ",
     [](const CanvasDesc& c) {
         if (!c.invalidCameras.empty()) return textValue(c.invalidCameras);
         return numberValue(double(indexMaskFromList(c.cameras)));
     },
     [](CanvasDesc& c, const PropValue& v) {
         // 丸めを通さずに呼ばれても Q20 の規則どおりにする: この行の決まりで
         // 丸め直す（範囲の外の番号を含む並びは、黙って減らさず正しくない並びに
         // なる。範囲の規則は clampValue / firstInvalidIndex の 1 か所）。
         const PropValue checked = clampValue(canvasAttrFromName(kCanvasCamerasAttr)->spec, v);
         c.cameras.clear();
         c.invalidCameras.clear();
         if (!indexListValid(checked)) {
             c.invalidCameras = checked.s;
             return;
         }
         c.cameras = indicesFromMask(maskOf(checked.v[0]));
     }},
    {"height", {PropType::Number, numberValue(kCanvasDefaultHeight), 16.0, 16384.0}, "基準の高さ",
     [](const CanvasDesc& c) { return numberValue(c.height); },
     [](CanvasDesc& c, const PropValue& v) { c.height = v.v[0]; }},
    {"order", {PropType::Int, numberValue(0), -10000.0, 10000.0}, "重なり順",
     [](const CanvasDesc& c) { return numberValue(double(c.order)); },
     [](CanvasDesc& c, const PropValue& v) { c.order = roundToInt(v.v[0], 0); }},
    {"distance", {PropType::Number, numberValue(kCanvasDefaultDistance), 0.01, 10000.0}, "カメラからの距離",
     [](const CanvasDesc& c) { return numberValue(c.distance); },
     [](CanvasDesc& c, const PropValue& v) { c.distance = v.v[0]; }},
    {"pos",
     {PropType::Vec3, numberValue(0, 0, 0), -kPlacementMaxMeters,
      kPlacementMaxMeters},
     "位置",
     [](const CanvasDesc& c) { return numberValue(c.pos.x, c.pos.y, c.pos.z); },
     [](CanvasDesc& c, const PropValue& v) { c.pos = {v.v[0], v.v[1], v.v[2]}; }},
    {"euler", {PropType::Euler, numberValue(0, 0, 0)}, "回転",
     [](const CanvasDesc& c) { return numberValue(c.rot.x, c.rot.y, c.rot.z); },
     [](CanvasDesc& c, const PropValue& v) { c.rot = {v.v[0], v.v[1], v.v[2]}; }},
    {"size", {PropType::Vec2, numberValue(kCanvasDefaultSizeX, kCanvasDefaultSizeY), 0.01, 10000.0}, "板の大きさ",
     [](const CanvasDesc& c) { return numberValue(c.sizeX, c.sizeY); },
     [](CanvasDesc& c, const PropValue& v) {
         c.sizeX = v.v[0];
         c.sizeY = v.v[1];
     }},
    {kCanvasUiAttr, {PropType::Name, textValue(""), 0.0, 0.0}, "共用の UI",
     [](const CanvasDesc& c) { return textValue(c.ui); },
     [](CanvasDesc& c, const PropValue& v) { c.ui = v.s; }},
};
inline constexpr std::size_t kCanvasAttrCount = sizeof(kCanvasAttrs) / sizeof(kCanvasAttrs[0]);

// この Canvas をカメラ（スロット番号）camera に出すか（唯一の定義）。cameras が
// 正しくなければどこにも出さない（Q20）、書いていなければ全カメラ（Q8）。
inline bool canvasShownOnCamera(const CanvasDesc& c, int camera) {
    if (!c.invalidCameras.empty()) return false;
    if (c.cameras.empty()) return true;
    for (const int k : c.cameras) {
        if (k == camera) return true;
    }
    return false;
}

// エディタの部分更新（Inspector の欄）で書き換えてよい属性か（唯一の定義。編集の操作と
// ブラウザの schema が引く）。ui は付け替えると Canvas の中身の持ち主が変わる（自分の
// 要素は書き出されなくなり保存で消える）ので、共用にする / やめる操作だけが書き換える。
inline bool canvasAttrEditable(const CanvasAttrInfo& a) {
    return std::string(a.name) != kCanvasUiAttr;
}

inline const CanvasAttrInfo* canvasAttrFromName(const std::string& name) {
    for (const auto& a : kCanvasAttrs) {
        if (name == a.name) return &a;
    }
    return nullptr;
}

}  // namespace ui
}  // namespace wizengine
