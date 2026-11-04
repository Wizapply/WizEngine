#include "components/ScatterComponent.h"

#include <algorithm>

#include <math/vec3.h>

#include "core/AssetError.h"
#include "core/Log.h"
#include "render/Renderer.h"
#include "scene/MathBridge.h"
#include "scene/Scene.h"

namespace wt = wizengine::terrain;
using filament::math::float3;
using filament::math::mat4f;

namespace {

// 代役（モデルの無い原型）の見た目。幹は原型の寸法（scatterTrunk）で描き、
// 冠の大きさと色はここで決める（当たり判定は幹だけなので描画の定数）。
// 冠の直径 (m、倍率 1 のとき)。
constexpr float kStandInCrownDiameter = 1.6f;
// 冠の中心を幹の上端からどれだけ上に置くか（冠の直径に対する割合。0.5 未満 =
// 冠の下側が幹の上端にかぶさる）。
constexpr float kStandInCrownLift = 0.3f;
// 幹と冠の色（リニア）。
const float3 kStandInTrunkColor{0.30f, 0.20f, 0.12f};
const float3 kStandInCrownColor{0.14f, 0.36f, 0.12f};

// 隠す変換（スケール 0。Renderer::releaseModelInstance と同じ隠し方）。
mat4f hidden() { return mat4f::scaling(float3{0.0f}); }

}  // namespace

std::size_t ScatterComponent::modelFor(wizengine::Renderer& r, const std::string& file) {
    if (file.empty()) return wizengine::Renderer::kInvalidModel;
    const auto it = models_.find(file);
    if (it != models_.end()) return it->second;
    std::size_t id = wizengine::Renderer::kInvalidModel;
    try {
        id = r.loadModel(file);
        LOGI("scatter", "model '%s' loaded for terrain scatter", file.c_str());
    } catch (const wizengine::AssetError& e) {
        LOGW("scatter", "terrain scatter: %s (drawing a stand-in instead)", e.what());
    }
    models_.emplace(file, id);
    return id;
}

ScatterComponent::Slot ScatterComponent::makeSlot(wizengine::Renderer& r, std::size_t wanted) {
    Slot s;
    s.wanted = wanted;
    if (wanted != wizengine::Renderer::kInvalidModel) {
        s.id = r.addModelInstance(wanted);
        s.model = s.id != wizengine::Renderer::kInvalidModel;
    }
    if (!s.model) {
        s.id = r.addShape(wizengine::ShapeMesh::Cylinder);
        r.setShapeColor(s.id, kStandInTrunkColor);
        s.crown = r.addShape(wizengine::ShapeMesh::Sphere);
        r.setShapeColor(s.crown, kStandInCrownColor);
    }
    return s;
}

void ScatterComponent::release(wizengine::Renderer& r, const Slot& s) {
    if (s.model) {
        r.releaseModelInstance(s.id);
    } else {
        r.removeShape(s.id);
        r.removeShape(s.crown);
    }
}

void ScatterComponent::releaseAll(wizengine::Renderer& r) {
    for (const Slot& s : slots_) release(r, s);
    slots_.clear();
}

void ScatterComponent::hide(wizengine::Renderer& r, const Slot& s) {
    if (s.model) {
        r.setModelInstanceTransform(s.id, hidden());
    } else {
        r.setBoxTransform(s.id, hidden());
        r.setBoxTransform(s.crown, hidden());
    }
}

void ScatterComponent::place(wizengine::Renderer& r, const Slot& s, const wt::HeightField& field,
                             const wt::ScatterPrototype& proto, const wt::ScatterInstance& inst,
                             const mat4f& frame) {
    wt::ScatterTrunk t;
    if (!wt::scatterTrunk(field, proto, inst, t)) {
        // 足元が穴（彫った・穴を開けた）: 配置は残し、描画だけ隠す。
        hide(r, s);
        return;
    }
    if (s.model) {
        // 足元（地表）に原点を置き、Y 軸まわりに向けてモデルの倍率を掛ける。
        const float scale = float(wt::scatterModelScale(proto, inst));
        r.setModelInstanceTransform(
            s.id, frame * mat4f::translation(float3{float(t.x), float(t.ground), float(t.z)}) *
                      mat4f::rotation(inst.yaw, float3{0.0f, 1.0f, 0.0f}) *
                      mat4f::scaling(float3{scale}));
        return;
    }
    // 代役の幹: 単位円柱（軸 X・直径 1・長さ 1）を ScatterTrunk の取り決めどおり
    // Z まわり 90° で立て、（長さ, 直径, 直径）=（高さ, 2 × 半径, 2 × 半径）に伸ばす。
    const float diameter = float(2.0 * t.radius);
    const Eigen::Quaterniond upright = wt::scatterTrunkUpright();
    r.setBoxTransform(
        s.id, frame * mat4f::translation(float3{float(t.x), float(t.centerY()), float(t.z)}) *
                  toFilament(BodyTransform{0.0, 0.0, 0.0, upright.w(), upright.x(), upright.y(),
                                           upright.z()}) *
                  mat4f::scaling(float3{float(t.height), diameter, diameter}));
    // 冠: 幹の上端の少し上に、配置の倍率に比例した直径の球（幹と同じく原型の
    // 倍率 = モデルの見た目には左右されない）。
    const float crown = kStandInCrownDiameter * inst.scale;
    const float crownY = float(t.ground + t.height) + kStandInCrownLift * crown;
    r.setBoxTransform(s.crown, frame *
                                   mat4f::translation(float3{float(t.x), crownY, float(t.z)}) *
                                   mat4f::scaling(float3{crown}));
}

void ScatterComponent::onRender(Scene& scene) {
    wizengine::Renderer& r = scene.renderer();
    // 格子・フレーム・散布物を 1 回のロックで組で取る（新しい散布物と古い格子の
    // 組を見ない）。末端ロックだけなので objects のロックの中から呼べる。
    BodyTransform pose;
    std::shared_ptr<const wt::TerrainScatter> scatter;
    const std::shared_ptr<const wt::HeightField> field =
        scene.terrainFieldAndScatter(pose, scatter);
    // テレイン本体（Scene::applyToRenderer の setTerrainTransform）と同じ変換。
    const mat4f frameM = toFilament(pose);
    if (scatter == drawnScatter_ && field == drawnField_ && frameM == drawnTransform_) return;
    drawnScatter_ = scatter;
    drawnField_ = field;
    drawnTransform_ = frameM;

    // テレインが無い（外した・別の物を開く途中・Missing）: 全部返す。
    if (!field || !field->valid() || !scatter) {
        releaseAll(r);
        return;
    }

    // 原型ごとのモデル（8 個だけなので配置ごとにパスを引かない）。
    std::size_t wanted[wt::kMaxScatterPrototypes];
    for (int k = 0; k < wt::kMaxScatterPrototypes; ++k) {
        wanted[k] = modelFor(r, scatter->prototypes[std::size_t(k)].file);
    }

    const std::vector<wt::ScatterInstance>& list = scatter->instances;
    // 数の上限は Scene の検査（pruneScatterInstances）が守るが、描く側でも越えない。
    const std::size_t n = std::min(list.size(), std::size_t(wt::kMaxScatterInstances));
    // 減ったぶんを後ろから返す。
    while (slots_.size() > n) {
        release(r, slots_.back());
        slots_.pop_back();
    }
    for (std::size_t i = 0; i < n; ++i) {
        const wt::ScatterInstance& inst = list[i];
        const std::size_t k = inst.prototype;
        // 原型の番号は Scene の検査（pruneScatterInstances）済み。外れていても
        // スロットの番号はずらさず、隠すだけにする。
        const bool known = k < std::size_t(wt::kMaxScatterPrototypes);
        const std::size_t want = known ? wanted[k] : wizengine::Renderer::kInvalidModel;
        if (i == slots_.size()) {
            slots_.push_back(makeSlot(r, want));
        } else if (slots_[i].wanted != want) {
            // 原型（のモデル）が前回と違うスロットだけ作り直す。
            release(r, slots_[i]);
            slots_[i] = makeSlot(r, want);
        }
        if (known) {
            place(r, slots_[i], *field, scatter->prototypes[k], inst, frameM);
        } else {
            hide(r, slots_[i]);
        }
    }
}
