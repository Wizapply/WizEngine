#pragma once

#include <string>

// glTF / GLB の外接箱（モデル単位）。AI のシーン生成が、生成したモデルを
// 「頼んだ高さ」に合わせる倍率（<mesh scale>）を決めるのと、配置の重なりを
// 見るのに使う。
//
// 頂点は読まない: glTF の仕様で POSITION アクセサには min / max が必須なので、
// その 8 隅をノードの変換（matrix か TRS、親から順に掛ける）で写して包む。
// 量子化（KHR_mesh_quantization の normalized 整数）も min / max を実数へ
// 直す。Draco 圧縮でも min / max は残る仕様なので同じに読める。
// 依存は nlohmann/json だけ（cgltf は gltfio とシンボルがぶつかる都合で
// MeshCollision.cpp が名前を付け替えて抱えているので、ここでは使わない）。
namespace wizengine {
namespace autogen {

struct Bounds {
    bool valid = false;
    double min[3] = {0, 0, 0};
    double max[3] = {0, 0, 0};
    double size(int axis) const { return valid ? max[axis] - min[axis] : 0.0; }
};

// path はファイルの実パス（assetPath 済み）。読めない・POSITION が無い
// ときは valid = false（error に英語の理由）。
Bounds gltfBounds(const std::string& path, std::string* error = nullptr);

// メモリ上の .glb / .gltf（テスト用と、ダウンロード直後の検査）。
Bounds gltfBoundsFromMemory(const std::string& data, std::string* error = nullptr);

}  // namespace autogen
}  // namespace wizengine
