#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ui/UiLayout.h"
#include "ui/UiMeshFormat.h"
#include "ui/UiProps.h"
#include "ui/UiTypes.h"

// Canvas 1 枚の要素 → 四角形の集まり（UiMeshData）。要素を文書順に走り、
// 種類の表の組み立て関数を呼ぶ。同じテクスチャ・材質が続く範囲は 1 run に
// まとめる。テクスチャの番号は呼び側が決める（UiTextureLookup）ので、ここは
// 描画側（Renderer）を知らない。
namespace wizengine {
namespace ui {

class FontProvider;

// テクスチャの番号の引き方（呼び側が渡す）。
struct UiTextureLookup {
    std::function<int(int page)> fontPage;             // 字の置き場のページ → 番号
    std::function<int(const std::string& file)> image;  // 画像のパス → 番号
};

// 四角 1 枚。uv はテクスチャの範囲。
struct UiQuad {
    Rect rect;
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    int textureId = kUiNoTexture;
    int materialId = kUiMaterialImage;
    float outline = 0.0f;  // UiVertex::outline と同じ意味
    float outlineColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // UiVertex::outlineR..A
};

// 組み立て関数が四角を足す先。
class MeshSink {
public:
    MeshSink(UiMeshData& out, const UiTextureLookup& textures)
        : out_(out), textures_(textures) {}
    // element は要素の矩形（頂点の uv1 = 要素の中の位置を出すため）。
    void quad(const UiQuad& q, const Rect& element);
    // 番号の引き方が無ければ kUiNoTexture。
    int fontPageTexture(int page) const;
    int imageTexture(const std::string& file) const;

private:
    UiMeshData& out_;
    const UiTextureLookup& textures_;
};

// resolved は要素ごとの重ね済みの性質（読み方は elementProps）。
// 見えない要素と知らない種類は描かない。
UiMeshData buildCanvasMesh(const std::vector<ElementDesc>& elements,
                           const std::vector<PropSet>& resolved, const Rect& canvas,
                           FontProvider& fonts, const UiTextureLookup& textures);

}  // namespace ui
}  // namespace wizengine
