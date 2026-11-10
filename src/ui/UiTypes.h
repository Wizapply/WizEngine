#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "document/EditorTypes.h"
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

// Canvas の既定値（CanvasDesc の初期値と kCanvasAttrs の既定値が同じ値を使う）。
inline constexpr double kCanvasDefaultHeight = 1080.0;
inline constexpr double kCanvasDefaultDistance = 1.0;
inline constexpr double kCanvasDefaultSizeX = 1.6;
inline constexpr double kCanvasDefaultSizeY = 0.9;

struct CanvasDesc {
    std::string name;
    CanvasMode mode = CanvasMode::Overlay;
    std::vector<int> cameras;                  // 空 = 全カメラに出す
    double height = kCanvasDefaultHeight;      // 基準の高さ（設計上の画素）
    int order = 0;                             // Canvas どうしの重なり順（大きいほど手前）
    double distance = kCanvasDefaultDistance;  // camera: カメラからの距離（m）
    editor::Vec3d pos;                         // world: 置き場所（m）
    editor::Vec3d rot;                         // world: 向き（オイラー角、度）
    double sizeX = kCanvasDefaultSizeX;        // world / camera: 板の幅（m）
    double sizeY = kCanvasDefaultSizeY;        // world / camera: 板の高さ（m）
    std::string ui;                            // 共用の UI の名前。空 = 下の elements を使う
    std::vector<ElementDesc> elements;
    std::vector<AnimDesc> anims;
};

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

inline const CanvasAttrInfo kCanvasAttrs[] = {
    {"mode", {PropType::Enum, numberValue(0), 0.0, 0.0, kCanvasModeNames}, "表示方法",
     [](const CanvasDesc& c) { return numberValue(double(c.mode)); },
     [](CanvasDesc& c, const PropValue& v) {
         c.mode = CanvasMode(enumIndex(v.v[0], kCanvasModeNames, int(CanvasMode::Overlay)));
     }},
    // カメラの番号はスロットの範囲（0..kMaxCameraSlots-1）。
    {"cameras",
     {PropType::IndexList, numberValue(0), 0.0, double(editor::kMaxCameraSlots - 1)},
     "表示するカメラ",
     [](const CanvasDesc& c) { return numberValue(double(indexMaskFromList(c.cameras))); },
     [](CanvasDesc& c, const PropValue& v) {
         c.cameras.clear();
         for (const int k : indicesFromMask(maskOf(v.v[0]))) {
             if (k < editor::kMaxCameraSlots) c.cameras.push_back(k);
         }
     }},
    {"height", {PropType::Number, numberValue(kCanvasDefaultHeight), 16.0, 16384.0}, "基準の高さ",
     [](const CanvasDesc& c) { return numberValue(c.height); },
     [](CanvasDesc& c, const PropValue& v) { c.height = v.v[0]; }},
    {"order", {PropType::Int, numberValue(0), -10000.0, 10000.0}, "重なり順",
     [](const CanvasDesc& c) { return numberValue(double(c.order)); },
     [](CanvasDesc& c, const PropValue& v) { c.order = roundInt(v.v[0], 0); }},
    {"distance", {PropType::Number, numberValue(kCanvasDefaultDistance), 0.01, 10000.0}, "カメラからの距離",
     [](const CanvasDesc& c) { return numberValue(c.distance); },
     [](CanvasDesc& c, const PropValue& v) { c.distance = v.v[0]; }},
    {"pos",
     {PropType::Vec3, numberValue(0, 0, 0), -editor::kPlacementMaxMeters,
      editor::kPlacementMaxMeters},
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
    {"ui", {PropType::Name, textValue(""), 0.0, 0.0}, "共用の UI",
     [](const CanvasDesc& c) { return textValue(c.ui); },
     [](CanvasDesc& c, const PropValue& v) { c.ui = v.s; }},
};
inline constexpr std::size_t kCanvasAttrCount = sizeof(kCanvasAttrs) / sizeof(kCanvasAttrs[0]);

}  // namespace ui
}  // namespace wizengine
