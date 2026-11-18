#include "scene/UiOps.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>

#include "core/Clamp.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/SafeName.h"
#include "document/EditorTypes.h"  // kMaxXmlEditBytes
#include "scene/StatusCaveat.h"  // warningCountCaveat, unusableNameCaveat
#include "ui/UiKinds.h"
#include "ui/UiProps.h"
#include "ui/UiValue.h"
#include "ui/UiXml.h"

namespace wizengine {
namespace uiops {
namespace {

// 操作の名前（Op の種類）。
constexpr const char* kAdd = "ui.add";
constexpr const char* kRemove = "ui.remove";
constexpr const char* kSet = "ui.set";
constexpr const char* kElementAdd = "ui.element.add";
constexpr const char* kElementRemove = "ui.element.remove";
constexpr const char* kElementSet = "ui.element.set";
constexpr const char* kXml = "ui.xml";
constexpr const char* const kOps[] = {kAdd,        kRemove,        kSet,        kElementAdd,
                                      kElementRemove, kElementSet, kXml};


// 新しい Canvas の名前の元（空いていなければ canvas_2 …）。
constexpr const char* kNewCanvasName = "canvas";

// 書かなかった名前の注意（ステータスの末尾。無ければ空）。
std::string rejectedCaveat(const std::vector<std::string>& names) {
    if (names.empty()) return {};
    std::string s = "（書けなかった値: ";
    for (std::size_t k = 0; k < names.size(); ++k) s += (k ? ", " : "") + names[k];
    return s + "）";
}

// 正しくない cameras の注意（Q20: どのカメラにも出さない）。正しければ空。
std::string camerasCaveat(const ui::CanvasDesc& desc) {
    if (desc.invalidCameras.empty()) return {};
    const ui::ValueSpec& spec = ui::canvasAttrFromName(ui::kCanvasCamerasAttr)->spec;
    return "（表示するカメラの番号 " + ui::firstInvalidIndex(spec, desc.invalidCameras) +
           " はありません（" + std::to_string(roundToInt(spec.lo, 0)) + "〜" +
           std::to_string(roundToInt(spec.hi, 0)) +
           "）- 直すまでどのカメラにも出しません）";
}

// アニメのキーを付け替えた・外した注意（0 件なら空）。
std::string keysCaveat(std::size_t keys, const char* what) {
    if (keys == 0) return {};
    return "（アニメのキー " + std::to_string(keys) + " 個も" + what + "）";
}

// args の name を名前の決まり（ui::nameSpec の clampValue）で正規化して読む。
NameArg nameArg(const nlohmann::json& args, std::string& out) {
    auto normalize = [](const std::string& raw) {
        return ui::clampValue(ui::nameSpec(), ui::textValue(raw)).s;
    };
    return readNameArg(args, normalize, out);
}

// args の name を current へ当てる。taken(名前) = ほかの物が使っているか。当てなかったら
// その理由の注意（ステータスの末尾）を返す。other は「別の○○が使っています」の○○。
template <class Taken>
std::string applyName(const nlohmann::json& args, std::string& current, Taken taken,
                      const char* other) {
    std::string name;
    switch (nameArg(args, name)) {
        case NameArg::Absent:
            return {};
        case NameArg::Unusable:
            return unusableNameCaveat();
        case NameArg::Ok:
            break;
    }
    if (name == current) return {};
    if (taken(name)) return "（名前 " + name + " は別の" + other + "が使っています）";
    current = name;
    return {};
}

// 同じ並びに同じ名前の要素があるか（except の id は数えない）。
bool elementNameTaken(const std::vector<ui::ElementDesc>& elements, const std::string& name,
                      int except) {
    return ui::nameTaken(elements, name, [except](const ui::ElementDesc& e) {
        return e.id != except ? &e.name : nullptr;
    });
}

// アニメのキーのうち、要素の名前が names にあるものを to へ付け替える（to が空なら外す）。
// 戻り値 = 付け替えた・外した数。
std::size_t retargetKeys(ui::CanvasDesc& desc, const std::set<std::string>& names,
                         const std::string& to) {
    std::size_t count = 0;
    for (auto& anim : desc.anims) {
        const auto pointed = [&names](const ui::KeyDesc& k) { return names.count(k.element) > 0; };
        count += std::size_t(std::count_if(anim.keys.begin(), anim.keys.end(), pointed));
        if (to.empty()) {
            anim.keys.erase(std::remove_if(anim.keys.begin(), anim.keys.end(), pointed),
                            anim.keys.end());
            continue;
        }
        for (auto& k : anim.keys) {
            if (pointed(k)) k.element = to;
        }
    }
    return count;
}

// 要素 e がアニメのキーの指す先か（名前で引いて最初に当たる要素 = キーの規則）。
bool keysPointAt(const ui::CanvasDesc& desc, const ui::ElementDesc& e) {
    return ui::findElementByName(desc.elements, e.name) == &e;
}

// args の中の小さなオブジェクト（attrs / props / reset）。無ければ null。
nlohmann::json member(const nlohmann::json& args, const char* key) {
    return args.is_object() && args.contains(key) ? args[key] : nlohmann::json();
}

// ---- Canvas の操作 -------------------------------------------------------------
// 確かめ（名前の重なり・宣言の有無）は UiState がロックの下で答える（全体を写さない）。
// 書くのは PHYSICS だけなので、確かめと書き込みの間に他の書き手は入らない。
Result addCanvas(UiState& state, const nlohmann::json& args) {
    auto taken = [&state](const std::string& n) { return state.canvasNameTaken(n, -1); };
    ui::CanvasDesc desc;
    std::string name;
    const NameArg given = nameArg(args, name);
    const bool named = given == NameArg::Ok && !name.empty();
    desc.name = firstFreeName(named ? name : kNewCanvasName, kSafeNameMaxLength, taken);
    // 正規化で全部落ちた名前は既定の名前で作り、理由を出す（setCanvas と同じ文）。
    const std::string caveat = given == NameArg::Unusable ? unusableNameCaveat() : "";
    const std::vector<std::string> rejected = setCanvasAttrs(member(args, "attrs"), desc);
    // 宣言した UI を表示する Canvas（アセットパネルの UI のタイル）。宣言が無ければ空の
    // Canvas にして知らせる。
    const std::string uiName = jsonString(args, "ui", "");
    std::string uiFile;
    const bool uiDeclared = !uiName.empty() && state.findUiAsset(uiName, uiFile);
    if (uiDeclared) desc.ui = uiName;
    const std::string uiCaveat =
        (!uiName.empty() && !uiDeclared) ? "（UI " + uiName + " の宣言がありません）" : "";
    const std::string tail = caveat + uiCaveat + rejectedCaveat(rejected) + camerasCaveat(desc);
    Result r;
    r.select = true;
    r.canvas = state.addCanvas(desc);
    r.status = "UI: " + ui::canvasLabel(desc, r.canvas) + " を追加しました" + tail;
    return r;
}

Result removeCanvas(UiState& state, const nlohmann::json& args) {
    const int index = jsonInt(args, "canvas", -1);
    ui::CanvasDesc removed;
    Result r;
    if (!state.removeCanvas(index, &removed)) return r;  // 行き違い（もう無い）
    r.removedCanvas = index;
    r.status = "UI: " + ui::canvasLabel(removed, index) + " を削除しました";
    return r;
}

Result setCanvas(UiState& state, const nlohmann::json& args) {
    const int index = jsonInt(args, "canvas", -1);
    ui::CanvasDesc desc;
    Result r;
    if (!state.copyCanvas(index, desc)) return r;  // 行き違い（もう無い）
    auto taken = [&state, index](const std::string& n) { return state.canvasNameTaken(n, index); };
    const std::string caveat = applyName(args, desc.name, taken, " Canvas ");
    const std::vector<std::string> rejected = setCanvasAttrs(member(args, "attrs"), desc);
    r.status = "UI: " + ui::canvasLabel(desc, index) + " を更新しました" + caveat +
               rejectedCaveat(rejected) + camerasCaveat(desc);
    state.replaceCanvas(index, std::move(desc));
    return r;
}

// ---- 要素の操作 -----------------------------------------------------------------
// 中身を編集してよくないときのステータス（elementsEditable が false の理由）。
std::string notEditableStatus(const UiState::ContentsInfo& info) {
    if (!info.declared) return "UI: UI " + info.asset + " の宣言が無いので、中身は編集できません";
    return "UI: 共用 UI（" + info.asset +
           "）のファイルが読めないので、中身は編集できません（ファイルを戻して読み直してください）";
}

// 要素 e の種類が持つ性質の集合（種類の表から。知らない種類は 0）。
ui::PropMask ownPropMask(const ui::ElementDesc& e) {
    const ui::ElementKindInfo* kind = ui::kindInfo(e.kind);
    return kind ? ui::propsOfKind(kind->id) : 0;
}

// Canvas canvas の中身（要素とアニメ）を書き換える手順（唯一の定義。要素の操作・XML の
// 適用・editElement が通る）: 持ち主ごと 1 枚写し、編集してよいかを確かめ、
// edit(写した Canvas, 持ち主の様子) が true を返したら持ち主へ戻す。wholesale = 中身を丸ごと
// 入れ替える（戻すのと同じロックで中身の世代を新しくする = UiState::replaceCanvasContents）。
ElementWrite editContents(
    UiState& state, int canvas,
    const std::function<bool(ui::CanvasDesc&, const UiState::ContentsInfo&)>& edit,
    bool wholesale = false) {
    ElementWrite w;
    ui::CanvasDesc desc;
    UiState::ContentsInfo info;
    if (!state.copyCanvasContents(canvas, desc, info)) return w;  // 消えた
    if (!elementsEditable(info)) {
        w.refusal = notEditableStatus(info);
        return w;
    }
    if (!edit(desc, info)) return w;
    w.written = state.replaceCanvasContents(canvas, std::move(desc), wholesale);
    return w;
}

Result addElementOp(UiState& state, const nlohmann::json& args) {
    const int index = jsonInt(args, "canvas", -1);
    Result r;
    const std::string kindName = jsonString(args, "kind", "");
    const ui::ElementKindInfo* kind = ui::kindInfo(kindName);
    if (!kind) {
        // 知らない種類（Canvas が今もあるときだけ知らせる = 行き違いは黙る）。
        ui::CanvasDesc probe;
        if (state.copyCanvas(index, probe)) r.status = "UI: 知らない種類の要素です: " + kindName;
        return r;
    }
    std::string caveat, label, canvasLabel;
    std::vector<std::string> rejected;
    int id = -1;
    const ElementWrite w = editContents(
        state, index, [&](ui::CanvasDesc& desc, const UiState::ContentsInfo& info) {
            ui::ElementDesc e;
            e.kind = kind->name;
            e.parent = jsonInt(args, "parent", -1);
            caveat = applyName(
                args, e.name,
                [&desc](const std::string& n) { return elementNameTaken(desc.elements, n, -1); },
                "要素");
            rejected = setElementProps(member(args, "props"), nlohmann::json(), e);
            id = info.nextElementId;
            addElement(desc.elements, e, id);
            label = ui::elementLabel(desc.elements.back());
            canvasLabel = ui::canvasLabel(desc, index);
            return true;
        });
    r.status = w.refusal;
    if (!w.written) return r;  // 行き違い（もう無い）・編集してよくない
    r.select = true;
    r.canvas = index;
    r.element = id;
    r.status = "UI: " + label + " を " + canvasLabel + " に追加しました" + caveat +
               rejectedCaveat(rejected);
    return r;
}

Result removeElementOp(UiState& state, const nlohmann::json& args) {
    const int index = jsonInt(args, "canvas", -1);
    const int id = jsonInt(args, "element", -1);
    Result r;
    std::string label;
    RemoveCount removed;
    const ElementWrite w = editElement(state, index, id, [&](ui::CanvasDesc& desc, std::size_t at) {
        label = ui::elementLabel(desc.elements[at]);
        removed = removeElement(desc, id);
        return true;
    });
    r.status = w.refusal;
    if (!w.written) return r;  // 行き違い（もう無い）・編集してよくない
    const std::string children =
        removed.elements > 1 ? "（中の要素 " + std::to_string(removed.elements - 1) + " 個も）"
                             : std::string();
    r.status =
        "UI: " + label + " を削除しました" + children + keysCaveat(removed.keys, "外しました");
    return r;
}

Result setElementOp(UiState& state, const nlohmann::json& args) {
    const int index = jsonInt(args, "canvas", -1);
    const int id = jsonInt(args, "element", -1);
    Result r;
    std::string caveat, label, name;
    std::vector<std::string> rejected;
    std::size_t keys = 0;
    const ElementWrite w = editElement(state, index, id, [&](ui::CanvasDesc& desc, std::size_t at) {
        name = desc.elements[at].name;
        caveat = applyName(
            args, name,
            [&desc, id](const std::string& n) { return elementNameTaken(desc.elements, n, id); },
            "要素");
        keys = renameElement(desc, id, name);
        ui::ElementDesc& e = desc.elements[at];
        rejected = setElementProps(member(args, "props"), member(args, "reset"), e);
        label = ui::elementLabel(e);
        return true;
    });
    r.status = w.refusal;
    if (!w.written) return r;  // 行き違い（もう無い）・編集してよくない
    r.status = "UI: " + label + " を更新しました" + caveat + rejectedCaveat(rejected) +
               keysCaveat(keys, name.empty() ? "外しました" : "付け替えました");
    return r;
}

// ui.xml: Canvas の中身を <ui name> の書式のテキストで置き換える。name は使わない（名前は
// 一覧と Inspector で変える）。警告は LOGW と件数。
Result xmlOp(UiState& state, const nlohmann::json& args) {
    const int index = jsonInt(args, "canvas", -1);
    Result r;
    // 確かめと読み込みは 1 回（INPUT が積む前に確かめたテキストでも、PHYSICS で読み直す）。
    ui::UiAssetDesc got;
    std::vector<std::string> warnings;
    r.status = readXmlText(jsonString(args, "text", ""), got, warnings);
    if (!r.status.empty()) return r;
    std::string label, nameCaveat;
    auto replace = [&](ui::CanvasDesc& desc, const UiState::ContentsInfo& info) {
        const std::string& own = info.asset.empty() ? desc.name : info.asset;
        if (!got.name.empty() && got.name != own) {
            nameCaveat = "（XML の name は使いません。名前は一覧と Inspector で変えます）";
        }
        ui::moveContents(got, desc);
        label = info.asset.empty() ? ui::canvasLabel(desc, index) : "UI " + info.asset;
        return true;
    };
    // 中身を丸ごと入れ替える = その中身の上の映像のドラッグを止める（同じ id の別の要素に
    // 書かない）。世代は入れ替えと同じロックで新しくなる。
    const ElementWrite w = editContents(state, index, replace, true);
    r.status = w.refusal;
    if (!w.written) return r;  // 行き違い（もう無い）・編集してよくない
    for (const auto& m : warnings) LOGW("ui", "ui.xml: %s", m.c_str());
    r.status = "UI: " + label + " の中身を XML で置き換えました" + nameCaveat +
               warningCountCaveat("警告", warnings.size());
    return r;
}

}  // namespace

bool elementsEditable(const UiState::ContentsInfo& info) {
    return info.asset.empty() || (info.declared && !info.unreadable);
}

std::string readXmlText(const std::string& text, ui::UiAssetDesc& out,
                        std::vector<std::string>& warnings) {
    if (text.empty()) return "UI: XML が空です";
    if (text.size() > editor::kMaxXmlEditBytes) {
        const std::size_t mb = editor::kMaxXmlEditBytes / (1024 * 1024);
        return "UI: XML が大きすぎます（" + std::to_string(mb) + "MB まで）";
    }
    std::string error;
    if (!ui::parseUiAssetXml(text, out, error, &warnings)) return "UI: XML が読めません: " + error;
    return {};
}

NameArg readNameArg(const nlohmann::json& args,
                    const std::function<std::string(const std::string&)>& normalize,
                    std::string& out) {
    if (!args.is_object()) return NameArg::Absent;
    const auto it = args.find("name");
    if (it == args.end() || !it->is_string()) return NameArg::Absent;
    const std::string raw = it->get<std::string>();
    out = normalize(raw);
    return (!raw.empty() && out.empty()) ? NameArg::Unusable : NameArg::Ok;
}

std::string xmlTextProblem(const std::string& text) {
    ui::UiAssetDesc got;
    std::vector<std::string> warnings;
    return readXmlText(text, got, warnings);
}

bool isUiOp(const std::string& kind) {
    return std::any_of(std::begin(kOps), std::end(kOps),
                       [&kind](const char* op) { return kind == op; });
}

Result apply(UiState& state, const std::string& kind, const nlohmann::json& args) {
    if (kind == kAdd) return addCanvas(state, args);
    if (kind == kRemove) return removeCanvas(state, args);
    if (kind == kSet) return setCanvas(state, args);
    if (kind == kElementAdd) return addElementOp(state, args);
    if (kind == kElementRemove) return removeElementOp(state, args);
    if (kind == kElementSet) return setElementOp(state, args);
    if (kind == kXml) return xmlOp(state, args);
    return {};
}

std::vector<std::string> setCanvasAttrs(const nlohmann::json& attrs, ui::CanvasDesc& desc) {
    std::vector<std::string> rejected;
    if (!attrs.is_object()) return rejected;
    for (auto it = attrs.begin(); it != attrs.end(); ++it) {
        const ui::CanvasAttrInfo* a = ui::canvasAttrFromName(it.key());
        ui::PropValue v;
        const bool known = a && ui::canvasAttrEditable(*a);
        if (!known || !ui::valueFromJson(a->spec, attrs, a->name, v)) {
            rejected.push_back(it.key());
            continue;
        }
        a->set(desc, ui::clampValue(a->spec, std::move(v)));
    }
    return rejected;
}

std::vector<std::string> setElementProps(const nlohmann::json& props,
                                         const nlohmann::json& reset, ui::ElementDesc& e) {
    std::vector<std::string> rejected;
    const ui::PropMask mask = ownPropMask(e);
    auto ownProp = [mask](const std::string& name) -> const ui::PropInfo* {
        const ui::PropInfo* p = ui::propFromName(name);
        return (p && (mask & ui::propBit(p->id))) ? p : nullptr;
    };
    if (props.is_object()) {
        // 型の確かめと読み方は性質の表（fromJson）。知らない名前・その種類に無い性質も
        // 書かない（文書の読み込みが警告して無視するのと同じ）。
        ui::PropSet incoming;
        rejected = ui::fromJson(props, incoming);
        for (auto it = props.begin(); it != props.end(); ++it) {
            if (ownProp(it.key())) continue;
            const bool listed =
                std::find(rejected.begin(), rejected.end(), it.key()) != rejected.end();
            if (!listed) rejected.push_back(it.key());
            if (const ui::PropInfo* known = ui::propFromName(it.key())) incoming.clear(known->id);
        }
        writeOwnProps(e, std::move(incoming));
    }
    for (const std::string& name : stringList(reset)) {
        if (const ui::PropInfo* p = ownProp(name)) e.props.clear(p->id);
        else rejected.push_back(name);
    }
    return rejected;
}

std::vector<std::string> writeOwnProps(ui::ElementDesc& e, ui::PropSet incoming) {
    std::vector<std::string> rejected;
    const ui::PropMask mask = ownPropMask(e);
    for (std::size_t k = 0; k < ui::kPropCount; ++k) {
        const ui::Prop p = ui::Prop(k);
        if (!incoming.has(p) || (mask & ui::propBit(p))) continue;
        rejected.push_back(ui::propInfo(p).name);
        incoming.clear(p);
    }
    e.props.overlay(ui::clampProps(std::move(incoming)));
    return rejected;
}

ElementWrite editElement(UiState& state, int canvas, int id,
                         const std::function<bool(ui::CanvasDesc&, std::size_t)>& edit) {
    return editContents(state, canvas, [&](ui::CanvasDesc& desc, const UiState::ContentsInfo&) {
        const std::size_t at = ui::elementIndex(desc.elements, id);
        if (at == desc.elements.size()) return false;  // 消えた
        return edit(desc, at);
    });
}

void addElement(std::vector<ui::ElementDesc>& elements, ui::ElementDesc e, int id) {
    e.id = id;
    if (ui::elementIndex(elements, e.parent) == elements.size()) e.parent = -1;
    elements.push_back(std::move(e));
}

RemoveCount removeElement(ui::CanvasDesc& desc, int id) {
    RemoveCount count;
    std::vector<ui::ElementDesc>& elements = desc.elements;
    if (ui::elementIndex(elements, id) == elements.size()) return count;
    // 子孫を集める（親は自分より前 = 文書順に 1 回なめれば足りる）。消える要素がキーの
    // 指す先なら、その名前も集める（名前が重なった後ろの要素を消してもキーは残る）。
    std::set<int> gone{id};
    std::set<std::string> keyNames;
    for (const auto& e : elements) {
        const bool removed = e.id == id || (e.parent >= 0 && gone.count(e.parent) > 0);
        if (!removed) continue;
        gone.insert(e.id);
        if (keysPointAt(desc, e)) keyNames.insert(e.name);
    }
    count.keys = retargetKeys(desc, keyNames, std::string());
    const std::size_t before = elements.size();
    const auto removed = [&gone](const ui::ElementDesc& e) { return gone.count(e.id) > 0; };
    elements.erase(std::remove_if(elements.begin(), elements.end(), removed), elements.end());
    count.elements = before - elements.size();
    return count;
}

std::size_t renameElement(ui::CanvasDesc& desc, int id, const std::string& name) {
    const std::size_t at = ui::elementIndex(desc.elements, id);
    if (at == desc.elements.size()) return 0;
    ui::ElementDesc& e = desc.elements[at];
    if (e.name == name) return 0;
    const bool follow = keysPointAt(desc, e);
    const std::string old = e.name;
    e.name = name;
    return follow ? retargetKeys(desc, {old}, name) : 0;
}

}  // namespace uiops
}  // namespace wizengine
