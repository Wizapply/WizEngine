#include "scene/UiJson.h"

#include <cstddef>

#include "ui/UiKinds.h"
#include "ui/UiProps.h"
#include "ui/UiValue.h"

namespace wizengine {
namespace uijson {
namespace {

// 値の決まり 1 行（性質と Canvas の属性が同じ形で送る）。
nlohmann::json specJson(const char* name, const ui::ValueSpec& spec, const char* label) {
    nlohmann::json j;
    j["name"] = name;
    j["type"] = ui::propTypeName(spec.type);
    j["label"] = label;
    j["def"] = ui::valueToJson(spec, spec.def);
    j["lo"] = spec.lo;
    j["hi"] = spec.hi;
    if (spec.type == ui::PropType::Enum && spec.enumNames) {
        nlohmann::json names = nlohmann::json::array();
        for (std::size_t k = 0; spec.enumNames[k]; ++k) names.push_back(spec.enumNames[k]);
        j["enum"] = names;
    }
    return j;
}

nlohmann::json buildSchema() {
    nlohmann::json props = nlohmann::json::array();
    for (std::size_t i = 0; i < ui::kPropCount; ++i) {
        const ui::PropInfo& p = ui::propInfo(static_cast<ui::Prop>(i));
        nlohmann::json j = specJson(p.name, p.spec, p.label);
        j["animatable"] = p.animatable;
        props.push_back(j);
    }
    nlohmann::json kinds = nlohmann::json::array();
    for (std::size_t k = 0; k < ui::kindCount(); ++k) {
        const ui::ElementKindInfo& info = ui::kindAt(k);
        const ui::PropMask mask = ui::propsOfKind(info.id);
        nlohmann::json names = nlohmann::json::array();
        for (std::size_t i = 0; i < ui::kPropCount; ++i) {
            const ui::Prop p = static_cast<ui::Prop>(i);
            if (mask & ui::propBit(p)) names.push_back(ui::propInfo(p).name);
        }
        kinds.push_back({{"name", info.name}, {"props", names}});
    }
    nlohmann::json canvas = nlohmann::json::array();
    for (const auto& a : ui::kCanvasAttrs) {
        nlohmann::json j = specJson(a.name, a.spec, a.label);
        // 欄にしてよいか（ui の付け替えは共用にする / やめる操作の仕事）。
        j["editable"] = ui::canvasAttrEditable(a);
        canvas.push_back(j);
    }
    return {{"props", props}, {"kinds", kinds}, {"canvas", canvas}};
}

// 選択中の物の設計値（Canvas そのものなら属性、要素ならその性質）。
nlohmann::json selectedJson(const UiState::Snapshot& snap, int canvas, int element) {
    const ui::CanvasDesc& desc = *snap.liveCanvas(canvas);
    nlohmann::json j;
    j["canvas"] = canvas;
    j["element"] = element;
    j["shared"] = !desc.ui.empty();
    j["canvasLabel"] = ui::canvasLabel(desc, canvas);
    if (element < 0) {
        j["name"] = desc.name;
        j["label"] = ui::canvasLabel(desc, canvas);
        j["attrs"] = canvasAttrsJson(desc);
        return j;
    }
    for (const auto& e : snap.elementsOf(desc)) {
        if (e.id != element) continue;
        j["kind"] = e.kind;
        j["name"] = e.name;
        j["label"] = ui::elementLabel(e);
        j["parent"] = e.parent;
        // 書いてある性質だけ（残りは schema の既定値。Inspector が「既定」と出す）。
        j["props"] = ui::toJson(e.props);
        break;
    }
    return j;
}

}  // namespace

const nlohmann::json& schema() {
    static const nlohmann::json kSchema = buildSchema();
    return kSchema;
}

nlohmann::json canvasAttrsJson(const ui::CanvasDesc& desc) {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& a : ui::kCanvasAttrs) j[a.name] = ui::valueToJson(a.spec, a.get(desc));
    return j;
}

nlohmann::json toJson(const UiState::Snapshot& snap, int selCanvas, int selElement) {
    nlohmann::json canvases = nlohmann::json::array();
    for (std::size_t i = 0; i < snap.canvases.size(); ++i) {
        const ui::CanvasDesc* desc = snap.liveCanvas(int(i));
        if (!desc) continue;  // 消えた枠は出さない（番号は空く）
        nlohmann::json elements = nlohmann::json::array();
        for (const auto& e : snap.elementsOf(*desc)) {
            elements.push_back({{"id", e.id},
                                {"kind", e.kind},
                                {"parent", e.parent},
                                {"name", e.name},
                                {"label", ui::elementLabel(e)}});
        }
        nlohmann::json c;
        c["index"] = int(i);
        c["name"] = desc->name;
        c["label"] = ui::canvasLabel(*desc, int(i));
        c["mode"] = ui::kCanvasModeNames[static_cast<std::size_t>(desc->mode)];
        c["ui"] = desc->ui;
        c["camerasValid"] = desc->invalidCameras.empty();
        c["elements"] = elements;
        canvases.push_back(c);
    }
    nlohmann::json j;
    j["version"] = snap.version;  // 設計値の版（ブラウザが送った値の戻りを待つ目印）
    j["canvases"] = canvases;
    j["selected"] = snap.selectionExists(selCanvas, selElement)
                        ? selectedJson(snap, selCanvas, selElement)
                        : nlohmann::json(nullptr);
    j["schema"] = schema();
    return j;
}

}  // namespace uijson
}  // namespace wizengine
