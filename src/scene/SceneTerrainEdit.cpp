// ブラウザの edit.terrain の受け口（PHYSICS スレッド）: 新規・開く・名前の変更・
// 削除・複製・部分更新・生成・取り消しの操作に振り分ける。
// テレインと文書・.terrain・ブラウザの JSON との出入り（読込・名前と背番号・保存）は
// SceneTerrainFile.cpp、格子の差し替えそのもの（ロック・dirty・Undo・当たり判定）は
// SceneTerrain.cpp の publishTerrainField / reshapeTerrain に任せる。
#include "scene/SceneInternal.h"

#include "core/SafeName.h"

using namespace scene_detail;

namespace wt = wizengine::terrain;

namespace {

// 部分更新のステータスの文（applyTerrainOp）。
const char* terrainPatchStatus(bool hasTerrain, int oldResolution, int newResolution) {
    if (!hasTerrain) return "テレインの設定を更新（無効のまま）";
    // 再サンプル（HeightField::resampled）の性質を一言。
    if (newResolution != oldResolution)
        return "テレインを更新（解像度を下げると細い起伏は消え、穴は少し太ります）";
    return "テレインを更新";
}

// args の key が true か（型違い・無しは false = 投げない）。
bool argIsTrue(const nlohmann::json& a, const char* key) {
    const auto it = a.find(key);
    return it != a.end() && it->is_boolean() && it->get<bool>();
}

// ファイルを増やす・消すのは設計の作業（名前の変更と違い、シミュレート中は受けない）。
// シミュレート中ならステータス（「テレインの<what>はエディタモードでだけできます」）を
// 出して true。deleteTerrain と applyTerrainFileOp の duplicate 分岐が使う。
bool refuseWhileSimulating(EditorState& editor, const char* what) {
    if (editor.mode() != ed::AppMode::Simulate) return false;
    editor.setStatus(std::string("テレインの") + what + "はエディタモードでだけできます");
    return true;
}

}  // namespace

// ---- 新規・開く・名前の変更 ---------------------------------------------------

void Scene::newTerrain(const std::string& base) {
    ed::TerrainDesc desc = terrain_;  // 位置と表面は今のもの
    auto field = flatDefaultTerrain(desc);
    // ステータスは publish の前（系を切り替えたら「物理を切り替えました」が残る）。
    editor_.setStatus("新しいテレインを作りました（保存すると書きます）");
    // 名前は base、空ならシーン名（どちらも空なら uniqueName が "terrain"）。
    publishNewTerrain(desc, std::move(field), base.empty() ? editor_.sceneFile() : base);
    // 作ったテレインを選択にする（Inspector にその欄が出る = 他の追加と同じ）。
    if (hasTerrain_) selectSceneItem(EditorState::SelKind::Terrain);
}

void Scene::openTerrain(const std::string& name) {
    if (isCurrentTerrain(name)) return;  // 開いている
    // 一覧の正式な綴り（"HILL" と送られても "hill"）。名前だけの解決（規則 D）は
    // 名前のファイルがあればその名前を返す。エクスプローラーで足したファイルも
    // 開けるよう、引く前に一覧を取り直す。
    refreshTerrainFiles();
    const std::string file = terrainLibrary_->resolve(name, wt::TerrainGuid{}).file;
    ed::TerrainDesc desc = terrain_;  // 位置と表面は今のもの
    std::string reason = "terrain '" + name + "' is not in the list";
    auto field = file.empty() ? nullptr : readTerrainFile(file, desc, reason);
    if (!field) {
        LOGW("scene", "terrain: cannot open '%s': %s", name.c_str(), reason.c_str());
        editor_.setStatus("テレイン " + name + " を開けません（コンソール参照）");
        return;
    }
    editor_.setStatus("テレイン " + desc.data + " を開きました");
    publishTerrainField(desc, std::move(field), TerrainHistory::Clear,
                        TerrainFileState::Saved);
    selectSceneItem(EditorState::SelKind::Terrain);
}

void Scene::renameTerrain(const std::string& from, const std::string& to) {
    // 名前の付け替えは「文書が参照している名前か」で決める（Missing も含む = Missing の
    // テレインの名前を変えても、XML の参照が古い名前のまま残らない）。
    const bool current = refersToTerrain(from);
    wt::TerrainRenameResult r;
    const std::string toName = wizengine::sanitizeSafeName(to);
    // 改名先の守りは「読めている今のテレイン」だけ（isCurrentTerrain）。読めている
    // テレインの名前（未保存でファイルが無くても）は使用中: 改名させると次の保存で
    // そのファイルを今の格子で上書きしてしまう。Missing の名前へ別のファイルを
    // 改名するのは「ファイルを戻す」のと同じ（Missing は保存で書かない）なので許す。
    if (!current && isCurrentTerrain(toName)) {
        r.status = wt::TerrainRenameStatus::AlreadyExists;
        r.name = toName;
        r.reason = "'" + toName + "' is the name of the current terrain";
    } else {
        r = terrainLibrary_->rename(from, to);
    }
    // 今のテレインの参照（data）だけを新しい名前へ。格子はそのまま。
    auto retarget = [this](const std::string& name) {
        std::lock_guard<std::mutex> lk(objectsMutex_);
        terrain_.data = name;
    };
    switch (r.status) {
        case wt::TerrainRenameStatus::Ok:
            if (current) retarget(r.name);
            if (isUnreadableTerrainFile(from)) {
                // 読めないファイルの名前が変わった（Missing の文の名前を追従させる）。
                std::lock_guard<std::mutex> lk(objectsMutex_);
                terrainUnreadableFile_ = r.name;
            }
            LOGI("scene", "terrain renamed: '%s' -> '%s'", from.c_str(), r.name.c_str());
            editor_.setStatus("テレインの名前を " + r.name + " に変えました");
            return;
        case wt::TerrainRenameStatus::NotFound:
            // まだ保存していない今のテレイン（ファイルが無い）: 参照だけ変える
            // （NotFound なら r.name は正規化済みで空いている）。
            if (current) {
                retarget(r.name);
                // Missing は保存でファイルを作らない（参照だけが変わる）。
                editor_.setStatus(terrainMissing()
                                      ? "テレインの参照を " + r.name +
                                            " に変えました（ファイルはまだありません）"
                                      : "テレインの名前を " + r.name +
                                            " に変えました（未保存 - 保存で書きます）");
                return;
            }
            editor_.setStatus("テレイン " + from + " が見つかりません");
            break;
        case wt::TerrainRenameStatus::InvalidName:
            editor_.setStatus("テレインの名前が不正です（英数字と _ - のみ）");
            break;
        case wt::TerrainRenameStatus::SameName:
            editor_.setStatus("同じ名前です");
            break;
        case wt::TerrainRenameStatus::AlreadyExists:
            editor_.setStatus("テレイン " + r.name + " は既にあります（名前は変えていません）");
            break;
        case wt::TerrainRenameStatus::IoError:
            editor_.setStatus("テレインのファイルの名前を変えられません（コンソール参照）");
            break;
    }
    LOGW("scene", "terrain rename '%s' -> '%s': %s", from.c_str(), to.c_str(),
         r.reason.c_str());
}

// ---- 削除 ---------------------------------------------------------------------

void Scene::deleteTerrain(const std::string& name) {
    if (refuseWhileSimulating(editor_, "削除")) return;
    // シーンが参照しているテレインは消さない。読めているなら次の保存で同じ名前の
    // ファイルが書き戻り、Missing / 読めないなら「ファイルを戻して開き直す」が
    // できなくなる。
    if (refersToTerrain(name)) {
        editor_.setStatus("シーンが使っているテレインは削除できません（別のテレインを開くか外してから）");
        return;
    }
    const wt::TerrainRemoveResult r = terrainLibrary_->remove(name);
    switch (r.status) {
        case wt::TerrainRemoveStatus::Ok:
            LOGI("scene", "terrain '%s' moved to the trash", r.name.c_str());
            editor_.setStatus("テレイン " + r.name + " をごみ箱へ移しました");
            return;
        case wt::TerrainRemoveStatus::NotFound:
            editor_.setStatus("テレイン " + r.name + " が見つかりません");
            break;
        case wt::TerrainRemoveStatus::IoError:
            editor_.setStatus("テレイン " + r.name + " をごみ箱へ移せません（コンソール参照）");
            break;
    }
    LOGW("scene", "terrain delete '%s': %s", r.name.c_str(), r.reason.c_str());
}

// ---- 複製 ---------------------------------------------------------------------

void Scene::duplicateTerrain(const std::string& name) {
    // シーンが参照している名前（未保存でファイルが無い・Missing・読めないも）は使用中:
    // 読めている未保存の名前で書くと開けず（openTerrain は「開いている」で何もしない）
    // 次の保存で上書きされ、Missing の名前で書くと別の中身がその参照に結び付く。
    const wt::TerrainDuplicateResult r = terrainLibrary_->duplicate(
        name, [this](const std::string& n) { return refersToTerrain(n); });
    if (!r.ok()) {
        LOGW("scene", "terrain duplicate '%s': %s", r.source.c_str(), r.reason.c_str());
        editor_.setStatus("複製できません: テレイン " + r.source + "（コンソール参照）");
        return;
    }
    LOGI("scene", "terrain duplicated: '%s' -> '%s'", r.source.c_str(), r.name.c_str());
    openTerrain(r.name);
    // 開けなかったときは openTerrain のステータス（理由）を残す。
    if (isCurrentTerrain(r.name))
        editor_.setStatus(r.source + " を " + r.name + " として複製して開きました");
}

// edit.terrain のうちファイルを扱う分岐（refresh / rename{from,to} / delete /
// open / duplicate / new）。扱ったら true（applyTerrainOp はそこで終わる）。
// open・duplicate・new は、保存していない変更があれば discard:true が付いているときだけ
// 取り替える（ブラウザの dirty 表示が古くても黙って捨てない）。
bool Scene::applyTerrainFileOp(const nlohmann::json& a) {
    // 取り替える前の守り: 未保存の変更があり、捨てる確認が無ければ止める。
    const auto keepUnsaved = [this, &a]() {
        if (!terrainDirty() || argIsTrue(a, "discard")) return false;
        editor_.setStatus("保存していない変更があります（もう一度選ぶと確認します）");
        return true;
    };
    if (argIsTrue(a, "refresh")) {
        // ブラウザのタブに戻ったとき（focus）に来る。ステータスは変えない。
        refreshTerrainFiles();
        return true;
    }
    if (const auto it = a.find("rename"); it != a.end() && it->is_object()) {
        renameTerrain(ed::jsonString(*it, "from", ""), ed::jsonString(*it, "to", ""));
        return true;
    }
    if (const auto it = a.find("delete"); it != a.end()) {
        if (!it->is_string()) {
            editor_.setStatus("削除の指定が正しくありません（delete はテレインの名前）");
            return true;
        }
        deleteTerrain(it->get<std::string>());
        return true;
    }
    if (a.contains("open")) {
        const std::string name = ed::jsonString(a, "open", "");
        // 開いているもの（openTerrain は何もしない）なら守りの文も出さない。
        if (isCurrentTerrain(name) || !keepUnsaved()) openTerrain(name);
        return true;
    }
    // 複製は複製を開く（取り替える）ので open と同じ守り。守りで止めるときは
    // ファイルも作らない。
    // シミュレート中は未保存の確認より先に断る（確認させてから断らない）。
    if (a.contains("duplicate")) {
        if (!refuseWhileSimulating(editor_, "複製") && !keepUnsaved())
            duplicateTerrain(ed::jsonString(a, "duplicate", ""));
        return true;
    }
    // 新規（テレインの有無によらず、今のテレインを平らな新しいテレインに取り替える。
    // 古いテレインのファイルは消さない）。値は名前の元（空ならシーン名）。
    if (a.contains("new")) {
        if (!keepUnsaved()) newTerrain(ed::jsonString(a, "new", ""));
        return true;
    }
    return false;
}

// edit.terrain（PHYSICS スレッド）。applyEditorOp の "terrain" 分岐から呼ぶ。
// ファイルの分岐（applyTerrainFileOp）と enabled(+name) のあとは、Missing
// （ファイルが無い・読めない）なら断る（編集を拒む唯一の場所）。そのあとはキーで
// 分岐: generate{seed,...}（同じメッセージの部分更新を先に当てる）/ undo /
// それ以外は部分更新。モードによらず受ける（名前の変更もシミュレート中に受ける）。
// delete と duplicate だけはエディタモードのみ（deleteTerrain / duplicate 分岐が断る）。
void Scene::applyTerrainOp(const EditorState::Op& op) {
    const nlohmann::json& a = op.args;
    if (applyTerrainFileOp(a)) return;
    // 外す（enabled:false）は Missing でも受ける（Missing から抜ける口。新規は new）。
    // enabled:true はテレインが無いときだけ新規で、Missing では下で断られる。
    if (const auto it = a.find("enabled"); it != a.end() && it->is_boolean()) {
        if (!it->get<bool>()) {
            // 外す（.terrain ファイルは消さない）。ステータスは publish の前。
            editor_.setStatus(terrainDirty() ? "テレインを外しました（保存していない変更は"
                                               "捨てました。ファイルは残ります）"
                                             : "テレインを外しました（ファイルは残ります）");
            publishTerrainField(terrain_, nullptr, TerrainHistory::Clear,
                                TerrainFileState::Saved);
            return;
        }
        if (!hasTerrain_) {
            // 新規（互換の口。new と同じ newTerrain）: 同じパッチの他のキーは
            // 使わず、既定の大きさの平らなテレインを今の位置と表面で作る。
            newTerrain(ed::jsonString(a, "name", ""));
            return;
        }
    }
    // Missing: 格子が無いので generate / undo / 部分更新は全部ここで止める。
    if (terrainMissing()) {
        editor_.setStatus("テレインのファイルが無いので編集できません（別のテレインを開く・"
                          "新しいテレインを作る・外す、のどれかで）");
        return;
    }
    // 送られてきたキーだけ上書きした形（edit.ground と同じ部分更新）。
    const auto patched = [this, &a]() {
        return ed::clampTerrain(ed::terrainFromJson(a, terrain_));
    };
    // generate / undo は形がおかしくても部分更新へ落とさない（成功に見せない）。
    if (const auto it = a.find("generate"); it != a.end()) {
        if (!it->is_object()) {
            editor_.setStatus("ノイズの指定が正しくありません（generate はオブジェクト）");
            return;
        }
        // 同じ 1 通の部分更新（確定待ちだった形の欄）を先に当てる。値が今と同じ
        // なら reshapeTerrain は格子を作り直さない（キーの一覧で判定しない）。
        // keepShape は渡さない: 直後のノイズが格子を全部上書きするので意味が無い。
        reshapeTerrain(patched());
        // 送られなかったキーは既定値（ノイズの値はテレインに覚えない）。
        generateTerrainNoise(ed::terrainNoiseFromJson(*it, ed::TerrainNoiseParams{}));
        return;
    }
    if (const auto it = a.find("undo"); it != a.end()) {
        if (!it->is_boolean() || !it->get<bool>()) {
            editor_.setStatus("取り消しの指定が正しくありません（undo は true）");
            return;
        }
        undoTerrain();
        return;
    }
    // 部分更新。テレインが無い間の値は位置と表面だけが次の新規に効く。
    // 当たり判定はモードによらずその場で張り直す（publishTerrainField）。
    const ed::TerrainDesc next = patched();
    editor_.setStatus(terrainPatchStatus(hasTerrain_, terrain_.resolution, next.resolution));
    // 高さの変更で形を保つか（Inspector の「形を保つ」。height と同じ 1 通で届く）。
    reshapeTerrain(next, argIsTrue(a, "keepShape"));
}
