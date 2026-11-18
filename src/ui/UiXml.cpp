#include "ui/UiXml.h"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/Clamp.h"
#include "core/NearestName.h"
#include "ui/UiKinds.h"
#include "ui/UiProps.h"
#include "ui/UiValue.h"

namespace wizengine {
namespace ui {
namespace {

using xml::Warn;

// 要素を識別する属性（性質ではない）。
constexpr const char* kIdAttr = "id";
constexpr const char* kNameAttr = "name";
constexpr const char* kParentAttr = "parent";
constexpr const char* kFileAttr = "file";
// アニメとキーの属性。
constexpr const char* kLoopAttr = "loop";
constexpr const char* kAutoplayAttr = "autoplay";
constexpr const char* kElementAttr = "element";
constexpr const char* kTimeAttr = "t";

std::string inQuotes(const std::string& s) { return "\"" + s + "\""; }

// 「知らない属性」の警告。candidates から近い名前があれば添える。
void warnUnknownAttr(const std::string& label, const std::string& name,
                     const std::vector<const char*>& candidates, Warn& warn) {
    const char* near = nearestName(name, candidates.data(), candidates.size());
    std::string msg = label + ": unknown attribute " + inQuotes(name);
    if (near) msg += " - did you mean " + inQuotes(near) + "?";
    warn(msg + " - ignored");
}

// e の属性のうち known に無いものを警告する（性質を持たない要素用）。
void warnUnknownAttrs(const xml::Element& e, const std::vector<const char*>& known,
                      const std::string& label, Warn& warn) {
    for (const auto& a : e.attributes()) {
        bool ok = false;
        for (const char* k : known) {
            if (a.first == k) ok = true;
        }
        if (!ok) warnUnknownAttr(label, a.first, known, warn);
    }
}

// 警告に載せる値（長い文字列は中身を出さずに長さだけ）。
constexpr std::size_t kShownValueMax = 64;
std::string shownValue(const std::string& s) {
    if (s.size() <= kShownValueMax) return inQuotes(s);
    return "(" + std::to_string(s.size()) + " bytes)";
}

// 読んだ値を範囲へ丸める（読み込みの丸めの唯一の口）。丸めで値が変わったら
// どの値をどう変えたかを警告する（範囲外の数・許されないパス・名前の正規化・
// 長すぎる文字）。添字の並びは丸めで変わらない（正しくない並びは値として
// 残る = 警告は意味の分かる読み手が出す。readCanvases の cameras）。
PropValue clampRead(const ValueSpec& spec, const PropValue& read, const std::string& label,
                    const std::string& attr, const std::string& text, Warn& warn) {
    PropValue c = clampValue(spec, read);
    if (sameValue(spec, c, read)) return c;
    warn(label + ": " + attr + "=" + shownValue(text) + " is not allowed as written - read as " +
         shownValue(formatValue(spec, c)));
    return c;
}

// 文字列の属性を値の決まり spec で読んで丸める（名前・宣言の file）。
std::string readText(const xml::Element& e, const char* attr, const ValueSpec& spec,
                     const std::string& label, Warn& warn) {
    const std::string raw = e.attr(attr);
    return clampRead(spec, textValue(raw), label, attr, raw, warn).s;
}

// 名前（英数字と _ - だけ = 性質の表の Name 型と同じ正規化）。
const ValueSpec& nameSpec() {
    static const ValueSpec spec{PropType::Name, textValue(""), 0.0, 0.0, nullptr};
    return spec;
}
std::string readName(const xml::Element& e, const std::string& label, Warn& warn) {
    return readText(e, kNameAttr, nameSpec(), label, warn);
}
// 宣言の file（<font> / <ui>）。決まりは要素の file 性質と同じ（assets/ 相対・長さ）。
// rejected = 書いてあったのに決まりで弾かれた（readText が警告済み）。
std::string readFile(const xml::Element& e, const std::string& label, Warn& warn,
                     bool& rejected) {
    std::string file = readText(e, kFileAttr, propInfo(Prop::File).spec, label, warn);
    rejected = !e.attr(kFileAttr).empty() && file.empty();
    return file;
}

// 一覧に同じ名前があるか（フォント・UI アセットの宣言と重複の確かめ）。
template <class T>
bool hasName(const std::vector<T>& list, const std::string& name) {
    for (const auto& item : list) {
        if (item.name == name) return true;
    }
    return false;
}

// 性質を読む。allowed はその要素が持てる性質、keyMode はアニメのキー
// （アニメできない性質を弾く）。identity は性質以外に知っている属性。
PropSet readProps(const xml::Element& e, PropMask allowed, bool keyMode,
                  const std::vector<const char*>& identity, const std::string& label,
                  Warn& warn) {
    PropSet props;
    for (const auto& a : e.attributes()) {
        bool isIdentity = false;
        for (const char* k : identity) {
            if (a.first == k) isIdentity = true;
        }
        if (isIdentity) continue;
        const PropInfo* info = propFromName(a.first);
        if (!info) {
            // 「もしかして」の候補は警告するときだけ作る。
            std::vector<const char*> candidates = identity;
            for (std::size_t k = 0; k < kPropCount; ++k) {
                candidates.push_back(propInfo(Prop(k)).name);
            }
            warnUnknownAttr(label, a.first, candidates, warn);
            continue;
        }
        if (!(allowed & propBit(info->id))) {
            warn(label + ": " + inQuotes(a.first) + " has no effect on <" + e.name() +
                 "> - ignored");
            continue;
        }
        if (keyMode && !info->animatable) {
            warn(label + ": " + inQuotes(a.first) + " cannot be animated - ignored");
            continue;
        }
        PropValue v;
        if (!parseValue(info->spec, a.second, v)) {
            warn(label + ": " + a.first + "=" + shownValue(a.second) +
                 " is not a valid value - using the default");
            continue;
        }
        props.set(info->id, clampRead(info->spec, v, label, a.first, a.second, warn));
    }
    return props;
}

// 書き出し。skipDefaults で既定値と同じ値を書かない（キーは値そのものが意味
// なので書く）。並びは性質の表の順。
void writeProps(xml::Element& e, const PropSet& props, bool skipDefaults) {
    for (std::size_t k = 0; k < kPropCount; ++k) {
        const PropInfo& info = propInfo(Prop(k));
        if (!props.has(info.id)) continue;
        if (skipDefaults && sameValue(info.spec, props.get(info.id), info.spec.def)) continue;
        e.set(info.name, props.format(info.id));
    }
}

// 識別の番号（id / parent）。書いていなければ fallback。書いてあるのに整数の字と
// して読めなければ（"3abc"・"1.5"・int に入らない値）ok = false（呼び手が警告する）で
// fallback。読み方は parseIntText（前後の空白は許す。添字の並びと同じ規則）。
int readIndex(const xml::Element& e, const char* key, int fallback, bool& ok) {
    ok = true;
    if (!e.has(key)) return fallback;
    int k = fallback;
    ok = parseIntText(e.attr(key), k);
    return ok ? k : fallback;
}

// 警告の見出し（"<worldbody> <canvas name=..."）。where が空なら要素だけ。
std::string elementLabel(const std::string& where, const xml::Element& e) {
    std::string s = (where.empty() ? std::string() : where + " ") + "<" + e.name();
    if (e.has(kIdAttr)) s += " id=" + inQuotes(e.attr(kIdAttr));
    if (e.has(kNameAttr)) s += " name=" + inQuotes(e.attr(kNameAttr));
    return s + ">";
}

// アニメ 1 本。キーの element は els の名前でなければならない。
AnimDesc readAnim(const xml::Element& e, const std::vector<ElementDesc>& els,
                  const std::string& where, Warn& warn) {
    const std::string label = elementLabel(where, e);
    warnUnknownAttrs(e, {kNameAttr, kLoopAttr, kAutoplayAttr}, label, warn);
    xml::warnUnknownChildren(e, {kKeyTag}, label.c_str(), warn);
    AnimDesc a;
    a.name = readName(e, label, warn);
    a.loop = e.boolean(kLoopAttr, a.loop);
    a.autoplay = e.boolean(kAutoplayAttr, a.autoplay);
    for (const xml::Element* k : e.all(kKeyTag)) {
        const std::string keyLabel = label + " <key element=" + inQuotes(k->attr(kElementAttr)) + ">";
        const std::string target = k->attr(kElementAttr);
        const ElementDesc* el = findElementByName(els, target);
        if (!el) {
            warn(keyLabel + ": no element is named " + inQuotes(target) + " - ignored");
            continue;
        }
        const ElementKindInfo* kind = kindInfo(el->kind);
        KeyDesc key;
        key.element = target;
        const double t = k->number(kTimeAttr, 0.0);
        key.t = (t >= 0.0) ? t : 0.0;  // NaN・負は 0
        if (!(t >= 0.0)) warn(keyLabel + ": t must be 0 or later - read as 0");
        key.props = readProps(*k, kind ? propsOfKind(kind->id) : 0, true,
                              {kElementAttr, kTimeAttr}, keyLabel, warn);
        a.keys.push_back(std::move(key));
    }
    return a;
}

// Canvas / UI アセットの中身（要素とアニメ）。fonts が null なら font の宣言を
// 確かめない（共用ファイル）。
void readContents(const xml::Element& c, std::vector<ElementDesc>& elements,
                  std::vector<AnimDesc>& anims, const std::vector<FontAssetDesc>* fonts,
                  const std::string& label, Warn& warn) {

    std::set<int> usedIds;
    std::vector<ElementDesc> pending;
    for (const xml::Element& child : c.children()) {
        if (child.name() == kAnimTag) continue;  // 要素を読み終えてから
        const ElementKindInfo* kind = kindInfo(child.name());
        if (!kind) {
            // 「もしかして」の候補（種類の名前と anim）は警告するときだけ作る。
            std::vector<const char*> childNames;
            for (std::size_t k = 0; k < kindCount(); ++k) childNames.push_back(kindAt(k).name);
            childNames.push_back(kAnimTag);
            const char* near = nearestName(child.name(), childNames.data(), childNames.size());
            std::string msg = label + ": unsupported <" + child.name() + ">";
            if (near) msg += " - did you mean <" + std::string(near) + ">?";
            warn(msg + " - ignored");
            continue;
        }
        const std::string elLabel = elementLabel(label, child);
        ElementDesc el;
        el.kind = kind->name;
        bool idOk = true;
        el.id = readIndex(child, kIdAttr, -1, idOk);
        if (!idOk) {
            warn(elLabel + ": id=" + inQuotes(child.attr(kIdAttr)) +
                 " is not a number - numbered automatically");
        } else if (child.has(kIdAttr) && el.id <= 0) {
            warn(elLabel + ": id must be 1 or more - numbered automatically");
            el.id = -1;
        }
        el.name = readName(child, elLabel, warn);
        if (const ElementDesc* same = findElementByName(pending, el.name)) {
            warn(elLabel + ": name=" + inQuotes(el.name) + " is also used by <" + same->kind +
                 " id=\"" + std::to_string(same->id) +
                 "\"> - keys and nodes refer to the first one");
        }
        bool parentOk = true;
        el.parent = readIndex(child, kParentAttr, -1, parentOk);
        if (!parentOk) {
            // 親を名前で書く間違いは起きやすいので、前の要素の名前なら id を教える。
            const std::string text = child.attr(kParentAttr);
            std::string msg = elLabel + ": parent=" + inQuotes(text) + " is not an element id";
            const ElementDesc* named = findElementByName(pending, text);
            const bool hasId = named && named->id > 0;
            if (hasId) msg += " - did you mean parent=\"" + std::to_string(named->id) + "\"?";
            warn(msg + " - placed on the canvas");
            el.parent = -1;
        }
        el.props = readProps(child, propsOfKind(kind->id), false,
                             {kIdAttr, kNameAttr, kParentAttr}, elLabel, warn);
        if (!child.children().empty()) {
            warn(elLabel + ": nested elements are not read - write parent=\"<id>\" on "
                 "them instead");
        }
        const bool fontChecked = fonts != nullptr && el.props.has(Prop::Font);
        const std::string& font = el.props.text(Prop::Font);
        if (fontChecked && !font.empty() && !hasName(*fonts, font)) {
            warn(elLabel + ": font=" + inQuotes(font) +
                 " is not declared in <asset> - using the default font");
        }
        if (el.id > 0 && !usedIds.insert(el.id).second) {
            warn(elLabel + ": duplicates an earlier id - ignored");
            continue;
        }
        pending.push_back(std::move(el));
    }
    // 番号の無い要素に空き番号を振る（イベントのノードと同じ: 最大の次から）。
    int nextId = usedIds.empty() ? 1 : (*usedIds.rbegin() + 1);
    for (auto& el : pending) {
        if (el.id <= 0) el.id = nextId++;
    }
    // 親は自分より前にある要素の id（文書順 = 描く順の約束）。
    std::map<int, std::size_t> indexOfId;
    for (std::size_t i = 0; i < pending.size(); ++i) {
        ElementDesc& el = pending[i];
        if (el.parent >= 0) {
            const auto it = indexOfId.find(el.parent);
            if (it == indexOfId.end()) {
                warn(label + " <" + el.kind + " id=\"" + std::to_string(el.id) +
                     "\">: parent=" + std::to_string(el.parent) +
                     " is not an earlier element - placed on the canvas");
                el.parent = -1;
            }
        }
        indexOfId.emplace(el.id, i);
    }
    elements = std::move(pending);

    std::set<std::string> animNames;
    for (const xml::Element* a : c.all(kAnimTag)) {
        AnimDesc anim = readAnim(*a, elements, label, warn);
        if (anim.name.empty()) {
            warn(label + ": <anim> needs name=\"...\" (letters, digits, _ and - only) - "
                 "ignored");
            continue;
        }
        if (!animNames.insert(anim.name).second) {
            warn(label + ": <anim name=" + inQuotes(anim.name) +
                 "> duplicates an earlier one - ignored");
            continue;
        }
        anims.push_back(std::move(anim));
    }
}

void writeContents(xml::Element& c, const std::vector<ElementDesc>& elements,
                   const std::vector<AnimDesc>& anims) {
    for (const auto& el : elements) {
        xml::Element e(el.kind);
        e.setInt(kIdAttr, el.id);
        if (!el.name.empty()) e.set(kNameAttr, el.name);
        if (el.parent >= 0) e.setInt(kParentAttr, el.parent);
        writeProps(e, el.props, true);
        c.append(std::move(e));
    }
    for (const auto& a : anims) {
        xml::Element e(kAnimTag);
        e.set(kNameAttr, a.name);
        // 既定（false）なら書かない。
        if (a.loop) e.setBool(kLoopAttr, true);
        if (a.autoplay) e.setBool(kAutoplayAttr, true);
        for (const auto& k : a.keys) {
            xml::Element key(kKeyTag);
            key.set(kElementAttr, k.element);
            key.setNumber(kTimeAttr, k.t);
            writeProps(key, k.props, false);
            e.append(std::move(key));
        }
        c.append(std::move(e));
    }
}

// UI アセット 1 個（<asset> の <ui> と、共用ファイルのルート）。file が弾かれた
// 参照は false（<font> と同じく捨てる。埋め込みの空の UI に化けさせない =
// 保存で参照が消えない。使う側の <canvas ui> は「宣言の無い名前」として警告される）。
bool readUiAsset(const xml::Element& e, const std::vector<FontAssetDesc>* fonts,
                 bool allowFile, const std::string& label, Warn& warn, UiAssetDesc& a) {
    a.name = readName(e, label, warn);
    if (allowFile) {
        warnUnknownAttrs(e, {kNameAttr, kFileAttr}, label, warn);
        bool fileRejected = false;
        a.file = readFile(e, label, warn, fileRejected);
        if (fileRejected) {
            warn(label + ": the reference is ignored (its file is not allowed)");
            return false;
        }
    } else {
        warnUnknownAttrs(e, {kNameAttr}, label, warn);
    }
    if (!a.file.empty()) {
        if (!e.children().empty()) {
            warn(label + ": the elements written here are ignored (the file holds them)");
        }
        return true;
    }
    readContents(e, a.elements, a.anims, fonts, label, warn);
    return true;
}

}  // namespace

// ---- 書き出し ----------------------------------------------------------------
xml::Element fontElement(const FontAssetDesc& font) {
    xml::Element e(kFontTag);
    e.set(kNameAttr, font.name);
    e.set(kFileAttr, font.file);
    return e;
}

xml::Element uiAssetElement(const UiAssetDesc& asset, bool withContents) {
    xml::Element e(kUiAssetTag);
    e.set(kNameAttr, asset.name);
    if (!asset.file.empty() && !withContents) {
        e.set(kFileAttr, asset.file);
        return e;
    }
    writeContents(e, asset.elements, asset.anims);
    return e;
}

xml::Element canvasElement(const CanvasDesc& canvas) {
    xml::Element e(kCanvasTag);
    if (!canvas.name.empty()) e.set(kNameAttr, canvas.name);
    for (const auto& a : kCanvasAttrs) {
        const PropValue v = a.get(canvas);
        if (!sameValue(a.spec, v, a.spec.def)) e.set(a.name, formatValue(a.spec, v));
    }
    if (canvas.ui.empty()) writeContents(e, canvas.elements, canvas.anims);
    return e;
}

// ---- 読み込み ----------------------------------------------------------------
void readUiAssets(const xml::Element& asset, std::vector<FontAssetDesc>& fonts,
                  std::vector<UiAssetDesc>& uiAssets, Warn& warn) {
    for (const xml::Element* f : asset.all(kFontTag)) {
        const std::string label = elementLabel("<asset>", *f);
        warnUnknownAttrs(*f, {kNameAttr, kFileAttr}, label, warn);
        FontAssetDesc font;
        font.name = readName(*f, label, warn);
        bool fileRejected = false;
        font.file = readFile(*f, label, warn, fileRejected);
        if (fileRejected) continue;  // readFile が警告済み（同じ要素に 2 回言わない）
        if (font.name.empty() || font.file.empty()) {
            warn(label + ": <font> needs name=\"...\" and file=\"...\" - ignored");
            continue;
        }
        if (hasName(fonts, font.name)) {
            warn(label + ": duplicates an earlier <font> - ignored");
            continue;
        }
        fonts.push_back(std::move(font));
    }
    for (const xml::Element* u : asset.all(kUiAssetTag)) {
        const std::string label = elementLabel("<asset>", *u);
        UiAssetDesc a;
        if (!readUiAsset(*u, &fonts, true, label, warn, a)) continue;
        if (a.name.empty()) {
            warn(label + ": <ui> needs name=\"...\" (letters, digits, _ and - only) - "
                 "ignored");
            continue;
        }
        if (hasName(uiAssets, a.name)) {
            warn(label + ": duplicates an earlier <ui> - ignored");
            continue;
        }
        uiAssets.push_back(std::move(a));
    }
}

void readCanvases(const xml::Element& world, const std::vector<FontAssetDesc>& fonts,
                  const std::vector<UiAssetDesc>& uiAssets, std::vector<CanvasDesc>& out,
                  Warn& warn) {
    std::vector<const char*> known = {kNameAttr};
    for (const auto& a : kCanvasAttrs) known.push_back(a.name);
    std::set<std::string> names;
    for (const xml::Element* e : world.all(kCanvasTag)) {
        const std::string label = elementLabel("", *e);
        warnUnknownAttrs(*e, known, label, warn);
        CanvasDesc c;
        c.name = readName(*e, label, warn);
        if (!c.name.empty() && !names.insert(c.name).second) {
            warn(label + ": another <canvas> has the same name");
        }
        for (const auto& a : kCanvasAttrs) {
            if (!e->has(a.name)) continue;
            PropValue v;
            const std::string text = e->attr(a.name);
            if (!parseValue(a.spec, text, v)) {
                warn(label + ": " + a.name + "=" + shownValue(text) +
                     " is not a valid value - using the default");
                continue;
            }
            a.set(c, clampRead(a.spec, v, label, a.name, text, warn));
        }
        // 正しくない cameras は 1 種類の文で知らせる（Q20: どのカメラにも出さない）。
        if (!c.invalidCameras.empty()) {
            const ValueSpec& cams = canvasAttrFromName(kCanvasCamerasAttr)->spec;
            warn(label + ": cameras=" + shownValue(c.invalidCameras) + ": camera " +
                 firstInvalidIndex(cams, c.invalidCameras) + " does not exist (" +
                 std::to_string(roundToInt(cams.lo, 0)) + "-" + std::to_string(roundToInt(cams.hi, 0)) +
                 ") - the canvas is shown on no camera until this is fixed");
        }
        if (!c.ui.empty()) {
            if (!hasName(uiAssets, c.ui)) {
                warn(label + ": ui=" + inQuotes(c.ui) + " is not declared in <asset>");
            }
            if (!e->children().empty()) {
                warn(label + ": has ui=" + inQuotes(c.ui) +
                     " and its own elements - the elements are ignored (the shared UI "
                     "holds them)");
            }
        } else {
            readContents(*e, c.elements, c.anims, &fonts, label, warn);
        }
        out.push_back(std::move(c));
    }
}

// ---- 共用の UI のファイル ----------------------------------------------------
std::string uiAssetToXmlText(const UiAssetDesc& asset) {
    // 中身ごと書く（withContents = true は file を書かない = 自分を参照しない）。
    return xml::write(uiAssetElement(asset, true));
}

bool parseUiAssetXml(const std::string& text, UiAssetDesc& out, std::string& error,
                     std::vector<std::string>* warnings) {
    xml::Element root;
    if (!xml::parse(text, root, error)) return false;
    Warn warn(warnings);
    if (root.name() != kUiAssetTag) {
        warn("ui file: the root element is <" + root.name() +
             ">, expected <ui> - reading its elements anyway");
    }
    out = UiAssetDesc{};
    readUiAsset(root, nullptr, false, "<ui>", warn, out);  // file は読まない（allowFile = false）
    return true;
}

}  // namespace ui
}  // namespace wizengine
