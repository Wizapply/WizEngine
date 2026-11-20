#pragma once

// 四角 1 枚の組み立て（種類 image の見た目。button の背景も同じ物を使う）。
namespace wizengine {
namespace ui {

struct ResolvedElement;
class MeshSink;

// 要素の矩形に四角 1 枚: 色は Color × 不透明度、画像は File（空なら白 = 色の板）。
void addImageQuad(const ResolvedElement& e, MeshSink& sink);

}  // namespace ui
}  // namespace wizengine
