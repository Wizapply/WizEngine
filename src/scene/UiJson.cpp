#include "scene/UiJson.h"

#include <cstddef>

#include "scene/UiOps.h"
#include "ui/UiKinds.h"
#include "ui/UiLayout.h"  // parentIndices
#include "ui/UiProps.h"
#include "ui/UiValue.h"
#include "ui/UiXml.h"

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
        // ノード（SetUiProp）がこの性質の値を置く欄（ui::nodeField）と、性質を選び直したときに
        // 入れる欄の既定値（{欄の名前: 値} = ノードの JSON の形のまま。ブラウザはそのまま送る）
        // と成分の数（ノードエディタの値の欄）。
        const ui::NodeField field = ui::nodeField(p.spec.type);
        const char* fieldName = ui::nodeFieldName(field);
        editor::NodeDesc def;
        editor::setUiNodeFields(def, ui::nodeFieldsFromValue(p.spec, p.spec.def));
        j["nodeField"] = fieldName;
        j["nodeDef"] = {{fieldName, editor::toJson(def)[fieldName]}};
        j["components"] = ui::componentCount(p.spec.type);
        // 既定が要素ごとに決まる性質は表の既定値を送らない（選択中の要素の defaults が既定）。
        if (ui::defaultIsDerived(p.id)) j["def"] = nullptr;
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
        kinds.push_back({{"name", info.name},
                         {"icon", info.icon},
                         {"props", names},
                         {"click", ui::kindCatchesPointer(info)}});
    }
    nlohmann::json canvas = nlohmann::json::array();
    for (const auto& a : ui::kCanvasAttrs) {
        nlohmann::json j = specJson(a.name, a.spec, a.label);
        // 欄にしてよいか（ui の付け替えは共用にする / やめる操作の仕事）。
        j["editable"] = ui::canvasAttrEditable(a);
        // 効く表示方法の名前の並び（Inspector は今の表示方法に効く欄だけを出す）。
        nlohmann::json modes = nlohmann::json::array();
        for (std::size_t m = 0; ui::kCanvasModeNames[m]; ++m) {
            if (a.modes & ui::canvasModeBit(ui::CanvasMode(m))) {
                modes.push_back(ui::kCanvasModeNames[m]);
            }
        }
        j["modes"] = modes;
        canvas.push_back(j);
    }
    return {{"props", props}, {"kinds", kinds}, {"canvas", canvas}};
}

// Canvas の中身（要素とアニメ）を XML のモーダルで編集するテキスト（<ui name> の書式 =
// ui.xml が受ける形）。<canvas ui> なら使っている UI の中身、無ければ自分の中身。
std::string contentsXmlText(const UiState::Snapshot& snap, const ui::CanvasDesc& desc) {
    if (desc.ui.empty()) {
        ui::UiAssetDesc own;
        own.name = desc.name;
        ui::copyContents(desc, own);
        return ui::uiAssetToXmlText(own);
    }
    const UiState::UiAssetState* a = snap.uiAsset(desc.ui);
    if (!a) return {};
    return ui::uiAssetToXmlText(a->desc);
}

// 選択中の物の設計値（Canvas そのものなら属性と中身の XML、要素ならその性質）。
nlohmann::json selectedJson(const UiState::Snapshot& snap, int canvas, int element) {
    const ui::CanvasDesc& desc = *snap.liveCanvas(canvas);
    nlohmann::json j;
    j["canvas"] = canvas;
    j["element"] = element;
    j["shared"] = !desc.ui.empty();
    // 要素を編集してよいか（唯一の規則 uiops::elementsEditable。ブラウザは表示だけかを
    // これで決める）。
    j["editable"] = uiops::elementsEditable(snap.contentsInfo(canvas));
    j["canvasLabel"] = ui::canvasLabel(desc, canvas);
    if (element < 0) {
        j["name"] = desc.name;
        j["label"] = ui::canvasLabel(desc, canvas);
        j["attrs"] = canvasAttrsJson(desc);
        j["xml"] = contentsXmlText(snap, desc);
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
        // 書いていないときの値が要素ごとに決まる性質（hover / pressed = 背景から作る色。
        // ui::pointerColor）は、その値を「既定」として送る（schema の既定値より優先）。
        nlohmann::json defaults = nlohmann::json::object();
        const ui::ElementKindInfo* kind = ui::kindInfo(e.kind);
        const ui::PropMask mask = kind ? ui::propsOfKind(kind->id) : 0;
        for (const auto& c : ui::kPointerColorProps) {
            if (!(mask & ui::propBit(c.prop))) continue;
            const ui::PropInfo& info = ui::propInfo(c.prop);
            defaults[info.name] = ui::valueToJson(info.spec, ui::pointerColor(e.props, c.phase));
        }
        j["defaults"] = defaults;
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

std::shared_ptr<const Cache::Entry> Cache::get(const UiState& state, int selCanvas,
                                               int selElement) {
    std::lock_guard<std::mutex> lk(mutex_);
    const bool changed = state.copyIfChanged(known_, snap_);
    const bool sameSelection = selCanvas == canvas_ && selElement == element_;
    if (!entry_ || changed || !sameSelection) {
        auto e = std::make_shared<Entry>();
        const nlohmann::json j = toJson(snap_, selCanvas, selElement);
        e->text = j.dump();
        e->selected = !j.at("selected").is_null();
        entry_ = std::move(e);
        canvas_ = selCanvas;
        element_ = selElement;
    }
    return entry_;
}

nlohmann::json toJson(const UiState::Snapshot& snap, int selCanvas, int selElement) {
    nlohmann::json canvases = nlohmann::json::array();
    for (std::size_t i = 0; i < snap.canvases.size(); ++i) {
        const ui::CanvasDesc* desc = snap.liveCanvas(int(i));
        if (!desc) continue;  // 消えた枠は出さない（番号は空く）
        nlohmann::json elements = nlohmann::json::array();
        // 一覧の段（depth。Canvas の直下が 1）と、書いた親を引けなかった印（parentMissing =
        // 無い・後ろにある親。その要素は Canvas の直下に置く）は、配置と同じ規則
        // （ui::parentIndices）から作る = ブラウザは表示するだけ。
        const std::vector<ui::ElementDesc>& list = snap.elementsOf(*desc);
        const std::vector<int> parents = ui::parentIndices(list);
        std::vector<int> depth(list.size(), 1);
        for (std::size_t k = 0; k < list.size(); ++k) {
            const ui::ElementDesc& e = list[k];
            const int p = parents[k];
            if (p >= 0) depth[k] = depth[std::size_t(p)] + 1;
            elements.push_back({{"id", e.id},
                                {"kind", e.kind},
                                {"parent", e.parent},
                                {"depth", depth[k]},
                                {"parentMissing", e.parent >= 0 && p < 0},
                                {"name", e.name},
                                {"label", ui::elementLabel(e)}});
        }
        nlohmann::json c;
        c["index"] = int(i);
        c["name"] = desc->name;
        c["label"] = ui::canvasLabel(*desc, int(i));
        c["mode"] = ui::kCanvasModeNames[static_cast<std::size_t>(desc->mode)];
        c["ui"] = desc->ui;
        c["editable"] = uiops::elementsEditable(snap.contentsInfo(int(i)));
        c["camerasValid"] = desc->invalidCameras.empty();
        c["elements"] = elements;
        canvases.push_back(c);
    }
    // 宣言した UI（アセットパネルのタイル。file あり = 共用）。
    nlohmann::json assets = nlohmann::json::array();
    for (const auto& a : snap.uiAssets) {
        assets.push_back({{"name", a.desc.name},
                          {"file", a.desc.file},
                          {"elements", a.desc.elements.size()},
                          {"unreadable", a.unreadable}});
    }
    nlohmann::json j;
    j["version"] = snap.version;  // 設計値の版（ブラウザが送った値の戻りを待つ目印）
    j["canvases"] = canvases;
    j["assets"] = assets;
    j["selected"] = snap.selectionExists(selCanvas, selElement)
                        ? selectedJson(snap, selCanvas, selElement)
                        : nlohmann::json(nullptr);
    j["schema"] = schema();
    return j;
}

}  // namespace uijson
}  // namespace wizengine
