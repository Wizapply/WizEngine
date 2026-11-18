#include "autogen/GlbBounds.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

#include <nlohmann/json.hpp>

namespace wizengine {
namespace autogen {

namespace {

using Mat4 = std::array<double, 16>;  // glTF と同じ列優先

Mat4 identity() {
    Mat4 m{};
    m[0] = m[5] = m[10] = m[15] = 1.0;
    return m;
}

Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    }
    return r;
}

// 配列 / オブジェクトの子を参照で（value() はコピーを返すので、大きな
// glTF でプリミティブごとに accessors 全体を写さないように）。
const nlohmann::json& child(const nlohmann::json& j, const char* key, bool array) {
    static const nlohmann::json kEmptyArray = nlohmann::json::array();
    static const nlohmann::json kEmptyObject = nlohmann::json::object();
    if (j.is_object()) {
        const auto it = j.find(key);
        if (it != j.end() && (array ? it->is_array() : it->is_object())) return *it;
    }
    return array ? kEmptyArray : kEmptyObject;
}

double num(const nlohmann::json& j, std::size_t i, double fallback) {
    if (!j.is_array() || i >= j.size() || !j[i].is_number()) return fallback;
    return j[i].get<double>();
}

// ノードの局所変換（matrix があればそれ、無ければ T * R * S）。
Mat4 localMatrix(const nlohmann::json& node) {
    if (node.contains("matrix") && node["matrix"].is_array() &&
        node["matrix"].size() == 16) {
        Mat4 m{};
        for (std::size_t i = 0; i < 16; ++i) m[i] = num(node["matrix"], i, 0.0);
        return m;
    }
    const auto& t = node.contains("translation") ? node["translation"] : nlohmann::json();
    const auto& r = node.contains("rotation") ? node["rotation"] : nlohmann::json();
    const auto& s = node.contains("scale") ? node["scale"] : nlohmann::json();
    const double qx = num(r, 0, 0), qy = num(r, 1, 0), qz = num(r, 2, 0), qw = num(r, 3, 1);
    const double sx = num(s, 0, 1), sy = num(s, 1, 1), sz = num(s, 2, 1);
    Mat4 m{};
    // 回転（単位四元数）× 拡縮を列ごとに。
    m[0] = (1 - 2 * (qy * qy + qz * qz)) * sx;
    m[1] = (2 * (qx * qy + qz * qw)) * sx;
    m[2] = (2 * (qx * qz - qy * qw)) * sx;
    m[4] = (2 * (qx * qy - qz * qw)) * sy;
    m[5] = (1 - 2 * (qx * qx + qz * qz)) * sy;
    m[6] = (2 * (qy * qz + qx * qw)) * sy;
    m[8] = (2 * (qx * qz + qy * qw)) * sz;
    m[9] = (2 * (qy * qz - qx * qw)) * sz;
    m[10] = (1 - 2 * (qx * qx + qy * qy)) * sz;
    m[12] = num(t, 0, 0);
    m[13] = num(t, 1, 0);
    m[14] = num(t, 2, 0);
    m[15] = 1.0;
    return m;
}

// 量子化した整数の min / max を実数へ（normalized のときだけ）。
double dequantize(double v, int componentType, bool normalized) {
    if (!normalized) return v;
    switch (componentType) {
        case 5120: return std::max(v / 127.0, -1.0);    // BYTE
        case 5121: return v / 255.0;                     // UNSIGNED_BYTE
        case 5122: return std::max(v / 32767.0, -1.0);  // SHORT
        case 5123: return v / 65535.0;                   // UNSIGNED_SHORT
        default: return v;
    }
}

struct Walker {
    const nlohmann::json& doc;
    Bounds out;
    int depth = 0;

    void addPoint(const Mat4& m, double x, double y, double z) {
        const double p[3] = {m[0] * x + m[4] * y + m[8] * z + m[12],
                             m[1] * x + m[5] * y + m[9] * z + m[13],
                             m[2] * x + m[6] * y + m[10] * z + m[14]};
        for (int a = 0; a < 3; ++a) {
            if (!out.valid) {
                out.min[a] = out.max[a] = p[a];
            } else {
                out.min[a] = std::min(out.min[a], p[a]);
                out.max[a] = std::max(out.max[a], p[a]);
            }
        }
        out.valid = true;
    }

    void addMesh(int meshIndex, const Mat4& world) {
        const auto& meshes = child(doc, "meshes", true);
        const auto& accessors = child(doc, "accessors", true);
        if (meshIndex < 0 || std::size_t(meshIndex) >= meshes.size()) return;
        const auto& prims = child(meshes[std::size_t(meshIndex)], "primitives", true);
        for (const auto& prim : prims) {
            const auto& attrs = child(prim, "attributes", false);
            if (!attrs.contains("POSITION") || !attrs["POSITION"].is_number_integer())
                continue;
            const int ai = attrs["POSITION"].get<int>();
            if (ai < 0 || std::size_t(ai) >= accessors.size()) continue;
            const auto& acc = accessors[std::size_t(ai)];
            if (!acc.contains("min") || !acc.contains("max")) continue;
            const int ct = acc.value("componentType", 5126);
            const bool norm = acc.value("normalized", false);
            double lo[3], hi[3];
            for (std::size_t a = 0; a < 3; ++a) {
                lo[a] = dequantize(num(acc["min"], a, 0.0), ct, norm);
                hi[a] = dequantize(num(acc["max"], a, 0.0), ct, norm);
            }
            for (int c = 0; c < 8; ++c) {
                addPoint(world, (c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1],
                         (c & 4) ? hi[2] : lo[2]);
            }
        }
    }

    void node(int index, const Mat4& parent) {
        const auto& nodes = child(doc, "nodes", true);
        if (index < 0 || std::size_t(index) >= nodes.size() || depth > 64) return;
        const auto& n = nodes[std::size_t(index)];
        const Mat4 world = mul(parent, localMatrix(n));
        if (n.contains("mesh") && n["mesh"].is_number_integer()) {
            addMesh(n["mesh"].get<int>(), world);
        }
        if (n.contains("children") && n["children"].is_array()) {
            ++depth;
            for (const auto& c : n["children"]) {
                if (c.is_number_integer()) node(c.get<int>(), world);
            }
            --depth;
        }
    }
};

Bounds boundsFromJson(const nlohmann::json& doc, std::string* error) {
    Walker w{doc, Bounds{}, 0};
    const auto& scenes = child(doc, "scenes", true);
    const int sceneIndex = doc.value("scene", 0);
    if (sceneIndex >= 0 && std::size_t(sceneIndex) < scenes.size()) {
        for (const auto& n :
             child(scenes[std::size_t(sceneIndex)], "nodes", true)) {
            if (n.is_number_integer()) w.node(n.get<int>(), identity());
        }
    } else {
        // シーンの無い glTF（仕様上は許される）: 全メッシュを単位変換で。
        const auto& meshes = child(doc, "meshes", true);
        for (std::size_t i = 0; i < meshes.size(); ++i) w.addMesh(int(i), identity());
    }
    if (!w.out.valid && error) *error = "no POSITION accessor with min/max";
    return w.out;
}

std::uint32_t readU32(const std::string& d, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | std::uint8_t(d[at + std::size_t(i)]);
    return v;
}

}  // namespace

Bounds gltfBoundsFromMemory(const std::string& data, std::string* error) {
    std::string jsonText;
    if (data.size() >= 20 && data.compare(0, 4, "glTF") == 0) {
        // GLB: 12 バイトのヘッダ + 最初のチャンク（JSON）。
        const std::uint32_t chunkLen = readU32(data, 12);
        const std::uint32_t chunkType = readU32(data, 16);
        if (chunkType != 0x4E4F534Au || 20u + chunkLen > data.size()) {
            if (error) *error = "malformed GLB header";
            return {};
        }
        jsonText = data.substr(20, chunkLen);
    } else {
        jsonText = data;
    }
    const auto doc = nlohmann::json::parse(jsonText, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        if (error) *error = "glTF JSON does not parse";
        return {};
    }
    try {
        return boundsFromJson(doc, error);
    } catch (const std::exception& e) {  // 型の違う値（壊れたファイル）
        if (error) *error = std::string("glTF JSON is malformed: ") + e.what();
        return {};
    }
}

Bounds gltfBounds(const std::string& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open '" + path + "'";
        return {};
    }
    const std::string data((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    return gltfBoundsFromMemory(data, error);
}

}  // namespace autogen
}  // namespace wizengine
