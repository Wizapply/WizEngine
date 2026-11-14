#include "scene/EditorState.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "core/AssetError.h"
#include "core/Log.h"
#include "core/SafeName.h"

// 保存名に使ってよい文字だけ残す。".." や "/" を弾くのが目的（保存先は
// assets/scenes に固定したい）。
std::string EditorState::sanitizeSceneName(const std::string& name) {
    return wizengine::sanitizeSafeName(name);
}

void EditorState::push(Op op) {
    std::lock_guard<std::mutex> lk(mutex_);
    // 暴走したクライアントがキューを膨らませても、物理スレッドが 1 パスで
    // 捌ける量を大きく超えないように上限を置く。
    if (pending_.size() >= 512) return;
    pending_.push_back(std::move(op));
    pendingCount_.store(int(pending_.size()));
}

std::vector<EditorState::Op> EditorState::drain() {
    std::vector<Op> out;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        out.swap(pending_);
        pendingCount_.store(0);
    }
    return out;
}

std::vector<wizengine::editor::JointDesc> EditorState::joints() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return joints_;
}

void EditorState::setJoints(std::vector<wizengine::editor::JointDesc> joints) {
    std::lock_guard<std::mutex> lk(mutex_);
    joints_ = std::move(joints);
}

int EditorState::addJoint(const wizengine::editor::JointDesc& joint) {
    std::lock_guard<std::mutex> lk(mutex_);
    joints_.push_back(joint);
    return int(joints_.size()) - 1;
}

bool EditorState::removeJoint(int index) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (index < 0 || std::size_t(index) >= joints_.size()) return false;
    joints_.erase(joints_.begin() + index);
    return true;
}

std::size_t EditorState::jointCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return joints_.size();
}

std::vector<wizengine::editor::CableDesc> EditorState::cables() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return cables_;
}

void EditorState::setCables(std::vector<wizengine::editor::CableDesc> cables) {
    std::lock_guard<std::mutex> lk(mutex_);
    cables_ = std::move(cables);
}

int EditorState::addCable(const wizengine::editor::CableDesc& cable) {
    std::lock_guard<std::mutex> lk(mutex_);
    cables_.push_back(cable);
    return int(cables_.size()) - 1;
}

bool EditorState::removeCable(int index) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (index < 0 || std::size_t(index) >= cables_.size()) return false;
    cables_.erase(cables_.begin() + index);
    return true;
}

std::size_t EditorState::cableCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return cables_.size();
}

// ---- イベントアセット -------------------------------------------------------
// 変更は必ず graphVersion_ を進める（物理スレッドが実行キャッシュを取り直す
// 合図）。mutex_ の下で書き、読みはコピーで返す - ジョイントと同じ流儀。
// ノードとワイヤーは「どのアセットのものか」を必ず伴う: 同じ id が別の
// アセットにも居るので、名前を落とすと黙って別のノードを触ってしまう。

namespace {
// name のアセットを探す（見つからなければ nullptr）。呼び出し側は mutex_ を
// 取っていること。
template <typename T>
T* findAsset(std::vector<T>& list, const std::string& name) {
    for (auto& a : list) {
        if (a.desc.name == name) return &a;
    }
    return nullptr;
}
}  // namespace

std::vector<wizengine::editor::EventAssetDesc> EditorState::eventAssets() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<wizengine::editor::EventAssetDesc> out;
    out.reserve(events_.size());
    for (const auto& a : events_) out.push_back(a.desc);
    return out;
}

bool EditorState::hasEventAsset(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& a : events_) {
        if (a.desc.name == name) return true;
    }
    return false;
}

void EditorState::setEventAssets(
    std::vector<wizengine::editor::EventAssetDesc> assets,
    std::vector<std::string> worldEvents) {
    std::lock_guard<std::mutex> lk(mutex_);
    events_.clear();
    for (auto& d : assets) {
        EventAssetState st;
        // id はアセット内で一意なら何でもよい（読み込んだ文書の番号をその
        // まま使う）。次の採番だけ最大値の先へ動かす。
        for (const auto& n : d.nodes) {
            if (n.id >= st.nextNodeId) st.nextNodeId = n.id + 1;
        }
        st.desc = std::move(d);
        events_.push_back(std::move(st));
    }
    worldEvents_ = std::move(worldEvents);
    fireCounts_.clear();
    graphVersion_.fetch_add(1);
}

bool EditorState::addEventAsset(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (name.empty() || findAsset(events_, name) != nullptr) return false;
    EventAssetState st;
    st.desc.name = name;
    events_.push_back(std::move(st));
    graphVersion_.fetch_add(1);
    return true;
}

bool EditorState::addEventAsset(wizengine::editor::EventAssetDesc desc) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (desc.name.empty() || findAsset(events_, desc.name) != nullptr) return false;
    EventAssetState st;
    for (const auto& n : desc.nodes) {
        if (n.id >= st.nextNodeId) st.nextNodeId = n.id + 1;
    }
    st.desc = std::move(desc);
    events_.push_back(std::move(st));
    graphVersion_.fetch_add(1);
    return true;
}

bool EditorState::setEventAssetFile(const std::string& name, const std::string& file) {
    std::lock_guard<std::mutex> lk(mutex_);
    EventAssetState* a = findAsset(events_, name);
    if (a == nullptr) return false;
    a->desc.file = file;
    a->dirty = false;
    return true;
}

bool EditorState::eventAssetDirty(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& a : events_) {
        if (a.desc.name == name) return a.dirty;
    }
    return false;
}

void EditorState::clearEventAssetDirty(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (EventAssetState* a = findAsset(events_, name)) a->dirty = false;
}

bool EditorState::removeEventAsset(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::size_t before = events_.size();
    events_.erase(std::remove_if(events_.begin(), events_.end(),
                                 [&name](const EventAssetState& a) {
                                     return a.desc.name == name;
                                 }),
                  events_.end());
    if (events_.size() == before) return false;
    // ワールドの付け先も外す（オブジェクト側は Scene が外す - BodyDesc を
    // 持っているのは向こうなので）。
    worldEvents_.erase(
        std::remove(worldEvents_.begin(), worldEvents_.end(), name),
        worldEvents_.end());
    graphVersion_.fetch_add(1);
    return true;
}

std::vector<std::string> EditorState::worldEvents() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return worldEvents_;
}

bool EditorState::attachWorldEvent(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (findAsset(events_, name) == nullptr) return false;  // 無いものは付けない
    if (std::find(worldEvents_.begin(), worldEvents_.end(), name) !=
        worldEvents_.end()) {
        return false;  // 二重付けは意味が無い（同じことを 2 回する）
    }
    worldEvents_.push_back(name);
    graphVersion_.fetch_add(1);
    return true;
}

bool EditorState::detachWorldEvent(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::size_t before = worldEvents_.size();
    worldEvents_.erase(
        std::remove(worldEvents_.begin(), worldEvents_.end(), name),
        worldEvents_.end());
    if (worldEvents_.size() == before) return false;
    graphVersion_.fetch_add(1);
    return true;
}

int EditorState::addGraphNode(const std::string& asset,
                              wizengine::editor::NodeDesc node) {
    std::lock_guard<std::mutex> lk(mutex_);
    EventAssetState* a = findAsset(events_, asset);
    if (a == nullptr) return -1;
    node.id = a->nextNodeId++;
    const int id = node.id;
    a->desc.nodes.push_back(std::move(node));
    a->dirty = true;
    graphVersion_.fetch_add(1);
    return id;
}

bool EditorState::updateGraphNode(const std::string& asset, int id,
                                  const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lk(mutex_);
    EventAssetState* a = findAsset(events_, asset);
    if (a == nullptr) return false;
    for (auto& n : a->desc.nodes) {
        if (n.id != id) continue;
        // 種類と id は変えさせない（種類が変わると target の意味とワイヤーの
        // 向きが崩れる。作り直したほうが安全）。
        wizengine::editor::NodeDesc next =
            wizengine::editor::clampNode(wizengine::editor::nodeFromJson(patch, n));
        next.id = n.id;
        next.kind = n.kind;
        n = next;
        a->dirty = true;
        graphVersion_.fetch_add(1);
        return true;
    }
    return false;
}

bool EditorState::removeGraphNode(const std::string& asset, int id) {
    std::lock_guard<std::mutex> lk(mutex_);
    EventAssetState* a = findAsset(events_, asset);
    if (a == nullptr) return false;
    auto& nodes = a->desc.nodes;
    const std::size_t before = nodes.size();
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                               [id](const wizengine::editor::NodeDesc& n) {
                                   return n.id == id;
                               }),
                nodes.end());
    if (nodes.size() == before) return false;
    auto& wires = a->desc.wires;
    wires.erase(std::remove_if(wires.begin(), wires.end(),
                               [id](const wizengine::editor::WireDesc& w) {
                                   return w.from == id || w.to == id;
                               }),
                wires.end());
    a->dirty = true;
    graphVersion_.fetch_add(1);
    return true;
}

bool EditorState::addGraphWire(const std::string& asset, int from, int to) {
    std::lock_guard<std::mutex> lk(mutex_);
    EventAssetState* st = findAsset(events_, asset);
    if (st == nullptr) return false;
    // 両端が存在し、from がトリガー・to がアクションであること。UI も同じ
    // 制約で描くが、リクエストは誰でも作れるので判定はここが持つ。
    const wizengine::editor::NodeDesc* a = nullptr;
    const wizengine::editor::NodeDesc* b = nullptr;
    for (const auto& n : st->desc.nodes) {
        if (n.id == from) a = &n;
        if (n.id == to) b = &n;
    }
    if (!a || !b) return false;
    if (!wizengine::editor::nodeIsTrigger(a->kind)) return false;
    if (wizengine::editor::nodeIsTrigger(b->kind)) return false;
    for (const auto& w : st->desc.wires) {
        if (w.from == from && w.to == to) return false;  // 二重線は張らない
    }
    st->desc.wires.push_back({from, to});
    st->dirty = true;
    graphVersion_.fetch_add(1);
    return true;
}

bool EditorState::removeGraphWire(const std::string& asset, int from, int to) {
    std::lock_guard<std::mutex> lk(mutex_);
    EventAssetState* st = findAsset(events_, asset);
    if (st == nullptr) return false;
    auto& wires = st->desc.wires;
    const std::size_t before = wires.size();
    wires.erase(std::remove_if(wires.begin(), wires.end(),
                               [from, to](const wizengine::editor::WireDesc& w) {
                                   return w.from == from && w.to == to;
                               }),
                wires.end());
    if (wires.size() == before) return false;
    st->dirty = true;
    graphVersion_.fetch_add(1);
    return true;
}

// ---- 計算式アセット ---------------------------------------------------------
namespace {
// 型を名指ししない（FormulaAssetState は private）ようテンプレートで受ける。
template <typename T>
T* findFormula(std::vector<T>& list, const std::string& name) {
    for (auto& a : list) {
        if (a.desc.name == name) return &a;
    }
    return nullptr;
}
}  // namespace

std::vector<wizengine::formula::FormulaGraphDesc> EditorState::formulaAssets() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<wizengine::formula::FormulaGraphDesc> out;
    out.reserve(formulas_.size());
    for (const auto& a : formulas_) out.push_back(a.desc);
    return out;
}

bool EditorState::hasFormulaAsset(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& a : formulas_) {
        if (a.desc.name == name) return true;
    }
    return false;
}

void EditorState::setFormulaAssets(
    std::vector<wizengine::formula::FormulaGraphDesc> assets) {
    std::lock_guard<std::mutex> lk(mutex_);
    formulas_.clear();
    for (auto& d : assets) {
        FormulaAssetState st;
        for (const auto& n : d.nodes) {
            if (n.id >= st.nextNodeId) st.nextNodeId = n.id + 1;
        }
        st.desc = std::move(d);
        formulas_.push_back(std::move(st));
    }
    formulaVersion_.fetch_add(1);
}

bool EditorState::addFormulaAsset(wizengine::formula::FormulaGraphDesc graph) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (graph.name.empty() || findFormula(formulas_, graph.name) != nullptr) return false;
    FormulaAssetState st;
    for (const auto& n : graph.nodes) {
        if (n.id >= st.nextNodeId) st.nextNodeId = n.id + 1;
    }
    st.desc = std::move(graph);
    formulas_.push_back(std::move(st));
    formulaVersion_.fetch_add(1);
    return true;
}

bool EditorState::replaceFormulaAsset(wizengine::formula::FormulaGraphDesc graph) {
    if (graph.name.empty()) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    FormulaAssetState st;
    for (const auto& n : graph.nodes) {
        if (n.id >= st.nextNodeId) st.nextNodeId = n.id + 1;
    }
    st.desc = std::move(graph);
    FormulaAssetState* have = findFormula(formulas_, st.desc.name);
    if (have) *have = std::move(st);
    else formulas_.push_back(std::move(st));
    formulaVersion_.fetch_add(1);
    return true;
}

bool EditorState::removeFormulaAsset(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::size_t before = formulas_.size();
    formulas_.erase(std::remove_if(formulas_.begin(), formulas_.end(),
                                   [&name](const FormulaAssetState& a) {
                                       return a.desc.name == name;
                                   }),
                    formulas_.end());
    if (formulas_.size() == before) return false;
    formulaVersion_.fetch_add(1);
    return true;
}

int EditorState::addFormulaNode(const std::string& asset,
                                wizengine::formula::FormulaNodeDesc node) {
    std::lock_guard<std::mutex> lk(mutex_);
    FormulaAssetState* a = findFormula(formulas_, asset);
    if (a == nullptr || !wizengine::formula::formulaKind(node.kind)) return -1;
    node.id = a->nextNodeId++;
    const int id = node.id;
    a->desc.nodes.push_back(std::move(node));
    a->desc.source.clear();  // グラフを直接変えたら元のテキストはもう合わない
    formulaVersion_.fetch_add(1);
    return id;
}

bool EditorState::updateFormulaNode(const std::string& asset, int id,
                                    const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lk(mutex_);
    FormulaAssetState* a = findFormula(formulas_, asset);
    if (a == nullptr) return false;
    for (auto& n : a->desc.nodes) {
        if (n.id != id) continue;
        wizengine::formula::FormulaNodeDesc next = wizengine::editor::clampFormulaNode(
            wizengine::editor::formulaNodeFromJson(patch, n));
        next.id = n.id;
        next.kind = n.kind;  // 種類はポート数を決めるので変えさせない
        // 位置だけの移動なら元のテキストを保つ（中身は変わらない）。
        if (next.name != n.name || next.params != n.params) a->desc.source.clear();
        n = next;
        formulaVersion_.fetch_add(1);
        return true;
    }
    return false;
}

bool EditorState::removeFormulaNode(const std::string& asset, int id) {
    std::lock_guard<std::mutex> lk(mutex_);
    FormulaAssetState* a = findFormula(formulas_, asset);
    if (a == nullptr) return false;
    auto& nodes = a->desc.nodes;
    const std::size_t before = nodes.size();
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                               [id](const wizengine::formula::FormulaNodeDesc& n) {
                                   return n.id == id;
                               }),
                nodes.end());
    if (nodes.size() == before) return false;
    auto& wires = a->desc.wires;
    wires.erase(std::remove_if(wires.begin(), wires.end(),
                               [id](const wizengine::formula::FormulaWireDesc& w) {
                                   return w.from == id || w.to == id;
                               }),
                wires.end());
    a->desc.source.clear();
    formulaVersion_.fetch_add(1);
    return true;
}

bool EditorState::addFormulaWire(const std::string& asset,
                                 const wizengine::formula::FormulaWireDesc& w) {
    std::lock_guard<std::mutex> lk(mutex_);
    FormulaAssetState* st = findFormula(formulas_, asset);
    if (st == nullptr || w.from == w.to) return false;
    const wizengine::formula::FormulaNodeDesc* a = nullptr;
    const wizengine::formula::FormulaNodeDesc* b = nullptr;
    for (const auto& n : st->desc.nodes) {
        if (n.id == w.from) a = &n;
        if (n.id == w.to) b = &n;
    }
    if (!a || !b) return false;
    const auto* ak = wizengine::formula::formulaKind(a->kind);
    const auto* bk = wizengine::formula::formulaKind(b->kind);
    if (!ak || !bk) return false;
    if (w.fromPort < 0 || w.fromPort >= ak->outputs || w.port < 0 || w.port >= bk->inputs) {
        return false;
    }
    // 循環の検査: to から from へ辿り着けるなら、この線で輪になる。
    // delay（単位遅延）へ入る線は同じティックの依存ではないので辿らない
    // = delay を挟んだフィードバックは組める（Formula の整列と同じ規則）。
    auto breaks = [&st](int id) {
        for (const auto& n : st->desc.nodes) {
            if (n.id == id) return wizengine::formula::formulaBreaksFeedback(n.kind);
        }
        return false;
    };
    if (!wizengine::formula::formulaBreaksFeedback(b->kind)) {
        std::vector<int> stack{w.to};
        std::vector<int> seen;
        while (!stack.empty()) {
            const int cur = stack.back();
            stack.pop_back();
            if (cur == w.from) return false;
            if (std::find(seen.begin(), seen.end(), cur) != seen.end()) continue;
            seen.push_back(cur);
            for (const auto& e : st->desc.wires) {
                if (e.from == cur && !breaks(e.to)) stack.push_back(e.to);
            }
        }
    }
    auto& wires = st->desc.wires;
    // 同じ入力ポートは 1 本だけ: 古い線は張り替える。
    wires.erase(std::remove_if(wires.begin(), wires.end(),
                               [&w](const wizengine::formula::FormulaWireDesc& e) {
                                   return e.to == w.to && e.port == w.port;
                               }),
                wires.end());
    wires.push_back(w);
    st->desc.source.clear();
    formulaVersion_.fetch_add(1);
    return true;
}

bool EditorState::removeFormulaWire(const std::string& asset,
                                    const wizengine::formula::FormulaWireDesc& w) {
    std::lock_guard<std::mutex> lk(mutex_);
    FormulaAssetState* st = findFormula(formulas_, asset);
    if (st == nullptr) return false;
    auto& wires = st->desc.wires;
    const std::size_t before = wires.size();
    wires.erase(std::remove_if(wires.begin(), wires.end(),
                               [&w](const wizengine::formula::FormulaWireDesc& e) {
                                   return e.from == w.from && e.fromPort == w.fromPort &&
                                          e.to == w.to && e.port == w.port;
                               }),
                wires.end());
    if (wires.size() == before) return false;
    st->desc.source.clear();
    formulaVersion_.fetch_add(1);
    return true;
}

// ---- 制御器 -------------------------------------------------------------------
std::vector<wizengine::editor::ControllerDesc> EditorState::controllers() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return controllers_;
}

void EditorState::setControllers(std::vector<wizengine::editor::ControllerDesc> list) {
    std::lock_guard<std::mutex> lk(mutex_);
    controllers_.clear();
    for (auto& c : list) {
        c = wizengine::editor::clampController(c);
        bool dup = false;
        for (const auto& have : controllers_) dup = dup || have.formula == c.formula;
        if (!dup && !c.formula.empty()) controllers_.push_back(std::move(c));
    }
    controlVersion_.fetch_add(1);
}

bool EditorState::addController(const wizengine::editor::ControllerDesc& in) {
    const wizengine::editor::ControllerDesc c = wizengine::editor::clampController(in);
    if (c.formula.empty()) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& have : controllers_) {
        if (have.formula == c.formula) return false;
    }
    controllers_.push_back(c);
    controlVersion_.fetch_add(1);
    return true;
}

bool EditorState::removeController(const std::string& formula) {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::size_t before = controllers_.size();
    controllers_.erase(std::remove_if(controllers_.begin(), controllers_.end(),
                                      [&](const wizengine::editor::ControllerDesc& c) {
                                          return c.formula == formula;
                                      }),
                       controllers_.end());
    if (controllers_.size() == before) return false;
    controlVersion_.fetch_add(1);
    return true;
}

bool EditorState::updateController(const std::string& formula, const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& c : controllers_) {
        if (c.formula != formula) continue;
        nlohmann::json p = patch;
        p.erase("formula");  // 付け替えは remove + add
        c = wizengine::editor::controllerFromJson(p, c);
        controlVersion_.fetch_add(1);
        return true;
    }
    return false;
}

// ---- 制御器のパラメータ -------------------------------------------------------
std::vector<wizengine::editor::ParamDesc> EditorState::params() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return params_;
}

void EditorState::setParams(std::vector<wizengine::editor::ParamDesc> list) {
    std::lock_guard<std::mutex> lk(mutex_);
    params_.clear();
    for (auto& p : list) {
        p = wizengine::editor::clampParam(p);
        bool dup = false;
        for (const auto& have : params_) dup = dup || have.name == p.name;
        if (!dup && !p.name.empty()) params_.push_back(std::move(p));
    }
    paramVersion_.fetch_add(1);
}

bool EditorState::addParam(const wizengine::editor::ParamDesc& in) {
    const wizengine::editor::ParamDesc p = wizengine::editor::clampParam(in);
    if (p.name.empty()) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& have : params_) {
        if (have.name == p.name) return false;
    }
    params_.push_back(p);
    paramVersion_.fetch_add(1);
    return true;
}

bool EditorState::removeParam(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::size_t before = params_.size();
    params_.erase(std::remove_if(params_.begin(), params_.end(),
                                 [&](const wizengine::editor::ParamDesc& p) {
                                     return p.name == name;
                                 }),
                  params_.end());
    if (params_.size() == before) return false;
    paramVersion_.fetch_add(1);
    return true;
}

bool EditorState::updateParam(const std::string& name, const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& p : params_) {
        if (p.name != name) continue;
        nlohmann::json q = patch;
        q.erase("name");  // 改名は remove + add
        p = wizengine::editor::paramFromJson(q, p);
        // 並び替え（move = -1 で前へ、+1 で後ろへ。制御盤のカードの順）。
        if (patch.contains("move") && patch["move"].is_number_integer()) {
            const std::size_t i = std::size_t(&p - params_.data());
            const long to = long(i) + (patch["move"].get<int>() < 0 ? -1 : 1);
            if (to >= 0 && std::size_t(to) < params_.size()) {
                std::swap(params_[i], params_[std::size_t(to)]);
            }
        }
        paramVersion_.fetch_add(1);
        return true;
    }
    return false;
}

// ---- 制御盤のグラフ -------------------------------------------------------------
std::vector<wizengine::editor::ChartDesc> EditorState::charts() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return charts_;
}

void EditorState::setCharts(std::vector<wizengine::editor::ChartDesc> list) {
    std::lock_guard<std::mutex> lk(mutex_);
    charts_.clear();
    for (auto& c : list) charts_.push_back(wizengine::editor::clampChart(c));
    if (charts_.size() > 32) charts_.resize(32);
}

void EditorState::addChart(const wizengine::editor::ChartDesc& c) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (charts_.size() >= 32) return;
    charts_.push_back(wizengine::editor::clampChart(c));
}

bool EditorState::removeChart(int index) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (index < 0 || std::size_t(index) >= charts_.size()) return false;
    charts_.erase(charts_.begin() + index);
    return true;
}

bool EditorState::updateChart(int index, const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (index < 0 || std::size_t(index) >= charts_.size()) return false;
    charts_[std::size_t(index)] =
        wizengine::editor::chartFromJson(patch, charts_[std::size_t(index)]);
    // 並び替え（move = -1 で前へ、+1 で後ろへ）。
    if (patch.contains("move") && patch["move"].is_number_integer()) {
        const int to = index + (patch["move"].get<int>() < 0 ? -1 : 1);
        if (to >= 0 && std::size_t(to) < charts_.size()) {
            std::swap(charts_[std::size_t(index)], charts_[std::size_t(to)]);
        }
    }
    return true;
}

// ---- プレハブ -----------------------------------------------------------------
std::vector<wizengine::editor::PrefabDesc> EditorState::prefabAssets() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return prefabs_;
}

bool EditorState::hasPrefabAsset(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& p : prefabs_) {
        if (p.name == name) return true;
    }
    return false;
}

bool EditorState::prefabAsset(const std::string& name,
                              wizengine::editor::PrefabDesc& out) const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& p : prefabs_) {
        if (p.name == name) {
            out = p;
            return true;
        }
    }
    return false;
}

void EditorState::setPrefabAssets(std::vector<wizengine::editor::PrefabDesc> assets) {
    std::lock_guard<std::mutex> lk(mutex_);
    prefabs_ = std::move(assets);
    prefabVersion_.fetch_add(1);
}

bool EditorState::addPrefabAsset(wizengine::editor::PrefabDesc prefab) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (prefab.name.empty()) return false;
    for (const auto& p : prefabs_) {
        if (p.name == prefab.name) return false;
    }
    prefabs_.push_back(std::move(prefab));
    prefabVersion_.fetch_add(1);
    return true;
}

bool EditorState::removePrefabAsset(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::size_t before = prefabs_.size();
    prefabs_.erase(std::remove_if(prefabs_.begin(), prefabs_.end(),
                                  [&name](const wizengine::editor::PrefabDesc& p) {
                                      return p.name == name;
                                  }),
                   prefabs_.end());
    if (prefabs_.size() == before) return false;
    prefabVersion_.fetch_add(1);
    return true;
}

namespace {
wizengine::editor::PrefabDesc* findPrefab(std::vector<wizengine::editor::PrefabDesc>& list,
                                          const std::string& name) {
    for (auto& p : list) {
        if (p.name == name) return &p;
    }
    return nullptr;
}
}  // namespace

int EditorState::addPart(const std::string& prefab, wizengine::editor::PartDesc part) {
    std::lock_guard<std::mutex> lk(mutex_);
    wizengine::editor::PrefabDesc* p = findPrefab(prefabs_, prefab);
    if (!p || p->parts.size() >= 256) return -1;
    p->parts.push_back(wizengine::editor::clampPart(std::move(part)));
    prefabVersion_.fetch_add(1);
    return int(p->parts.size()) - 1;
}

bool EditorState::updatePart(const std::string& prefab, int index,
                             const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lk(mutex_);
    wizengine::editor::PrefabDesc* p = findPrefab(prefabs_, prefab);
    if (!p || index < 0 || std::size_t(index) >= p->parts.size()) return false;
    p->parts[std::size_t(index)] = wizengine::editor::clampPart(
        wizengine::editor::partFromJson(patch, p->parts[std::size_t(index)]));
    prefabVersion_.fetch_add(1);
    return true;
}

bool EditorState::setPartTransform(const std::string& prefab, int index,
                                   const wizengine::editor::Vec3d* pos,
                                   const wizengine::editor::Vec3d* rot,
                                   const wizengine::editor::Vec3d* size) {
    std::lock_guard<std::mutex> lk(mutex_);
    wizengine::editor::PrefabDesc* p = findPrefab(prefabs_, prefab);
    if (!p || index < 0 || std::size_t(index) >= p->parts.size()) return false;
    wizengine::editor::PartDesc& part = p->parts[std::size_t(index)];
    if (pos) part.position = *pos;
    if (rot) part.rotation = *rot;
    if (size) part.size = *size;
    part = wizengine::editor::clampPart(part);
    prefabVersion_.fetch_add(1);
    return true;
}

bool EditorState::removePart(const std::string& prefab, int index) {
    std::lock_guard<std::mutex> lk(mutex_);
    wizengine::editor::PrefabDesc* p = findPrefab(prefabs_, prefab);
    if (!p || index < 0 || std::size_t(index) >= p->parts.size()) return false;
    p->parts.erase(p->parts.begin() + index);
    prefabVersion_.fetch_add(1);
    return true;
}

bool EditorState::partDesc(const std::string& prefab, int index,
                           wizengine::editor::PartDesc& out) const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& p : prefabs_) {
        if (p.name != prefab) continue;
        if (index < 0 || std::size_t(index) >= p.parts.size()) return false;
        out = p.parts[std::size_t(index)];
        return true;
    }
    return false;
}

void EditorState::noteNodeFired(const std::string& asset, int id) {
    std::lock_guard<std::mutex> lk(mutex_);
    ++fireCounts_[{asset, id}];
}

void EditorState::clearNodeFireCounts() {
    std::lock_guard<std::mutex> lk(mutex_);
    fireCounts_.clear();
}

std::map<std::pair<std::string, int>, int> EditorState::nodeFireCounts() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return fireCounts_;
}

wizengine::editor::SimSettings EditorState::sim() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return sim_;
}

void EditorState::setSim(const wizengine::editor::SimSettings& s) {
    std::lock_guard<std::mutex> lk(mutex_);
    sim_ = s;
}

wizengine::editor::GizmoSettings EditorState::gizmo() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return gizmo_;
}

void EditorState::setGizmo(const wizengine::editor::GizmoSettings& g) {
    std::lock_guard<std::mutex> lk(mutex_);
    gizmo_ = g;
}

void EditorState::setStatus(std::string text) {
    std::lock_guard<std::mutex> lk(mutex_);
    status_ = std::move(text);
    ++statusSeq_;  // 文を書いてから進める（statusSeq() の約束）
}

std::string EditorState::status() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return status_;
}

void EditorState::setSceneFile(std::string name) {
    std::lock_guard<std::mutex> lk(mutex_);
    sceneFile_ = std::move(name);
}

std::string EditorState::sceneFile() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return sceneFile_;
}

std::vector<std::string> EditorState::sceneFiles() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return files_;
}

void EditorState::refreshSceneFiles() {
    std::vector<std::string> found;
    std::error_code ec;
    const std::filesystem::path dir(scenesDir());
    if (std::filesystem::exists(dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            const std::string ext = entry.path().extension().string();
            // .xml が今の形式、.json は旧形式（読み込みのみ）。一覧に出す
            // 名前は拡張子を落としたものなので、同名の新旧が両方あっても
            // タイルは 1 個になる（読み込みは .xml を先に見る）。
            if (ext != ".xml" && ext != ".json") continue;
            found.push_back(entry.path().stem().string());
        }
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    std::lock_guard<std::mutex> lk(mutex_);
    files_.swap(found);
}

std::vector<std::string> EditorState::eventLibrary() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return eventLibrary_;
}

void EditorState::refreshEventLibrary() {
    std::vector<std::string> found;
    std::error_code ec;
    const std::filesystem::path dir(eventsDir());
    if (std::filesystem::exists(dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension().string() != ".xml") continue;
            // 名前に使えない文字のファイル（手で置いた物）は一覧に出さない
            // （参照名 = ファイル名の語幹、の約束が崩れるため）。
            const std::string stem = entry.path().stem().string();
            if (wizengine::editor::sanitizeEventName(stem) != stem) continue;
            found.push_back(stem);
        }
    }
    std::sort(found.begin(), found.end());
    std::lock_guard<std::mutex> lk(mutex_);
    eventLibrary_.swap(found);
}

std::string EditorState::eventsDir() {
    return wizengine::assetPath("events");
}

std::string EditorState::eventLibraryFile(const std::string& name) {
    const std::string safe = wizengine::editor::sanitizeEventName(name);
    if (safe.empty()) return {};
    return "events/" + safe + ".xml";
}

std::string EditorState::scenesDir() {
    // 実行時に読むものは全部 assets/ 以下、という既存の約束に合わせる。
    return wizengine::assetPath("scenes");
}

std::string EditorState::scenePath(const std::string& name) {
    const std::string safe = sanitizeSceneName(name);
    if (safe.empty()) return {};
    return scenesDir() + "/" + safe + ".xml";
}

std::string EditorState::legacyScenePath(const std::string& name) {
    const std::string safe = sanitizeSceneName(name);
    if (safe.empty()) return {};
    return scenesDir() + "/" + safe + ".json";
}

bool EditorState::writeText(const std::string& path, const std::string& text,
                            std::string& reason) {
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        reason = "cannot write: " + path;
        return false;
    }
    f << text;
    if (!f) {
        reason = "write error: " + path;
        return false;
    }
    LOGI("editor", "saved %s", path.c_str());
    return true;
}

bool EditorState::readText(const std::string& path, std::string& text,
                           std::string& reason) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        reason = "not found: " + path;
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(f),
                std::istreambuf_iterator<char>());
    if (f.bad()) {
        reason = "read error: " + path;
        return false;
    }
    LOGI("editor", "loaded %s", path.c_str());
    return true;
}

bool EditorState::readJson(const std::string& path, nlohmann::json& doc,
                           std::string& reason) {
    std::string text;
    if (!readText(path, text, reason)) return false;
    doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        reason = "not readable as JSON: " + path;
        return false;
    }
    return true;
}

