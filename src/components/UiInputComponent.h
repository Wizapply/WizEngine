#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "scene/SceneComponent.h"
#include "scene/UiState.h"

// UI の入力。INPUT スレッドの onCommand で選択・編集・映像の上のドラッグを受け、
// PHYSICS スレッドの onEditorStep でドラッグを設計値へ当てる:
//
//   select.ui {canvas, element} … Canvas（element = -1）か、その中の要素を選ぶ。
//                                 canvas < 0 で解除。
//   edit.ui.*                   … 編集（一覧と中身は scene/UiOps.h）。ここでは受け付ける
//                                 ページかだけを見て、EditorState の Op（種類 = "ui.*"）
//                                 として積む。当てるのは PHYSICS（Scene::applyEditorOp →
//                                 UiOps）。EditorComponent より先に登録されているので、
//                                 edit.ui.* はそちらへ届かない。edit.ui.xml は壊れた
//                                 XML をここで弾く（uiops::xmlTextProblem）。
//   edit.lib.* {kind: "ui"}     … 共用 UI の操作（share / unshare / import / delete）。
//                                 同じく受け付けるページだけ Op（"lib.*"）として積む
//                                 （当てるのは Scene::applyLibraryOp。UI の部品は
//                                 UiLibraryOps）。プレハブ編集中・知らない操作はステータスで
//                                 知らせる。ほかの kind と refresh（一覧の取り直し。種類に
//                                 よらない）はここでは取らない（EditorComponent へ）。
//   pick / drag / release       … 選んでいる要素の枠の中とつまみ（ui/UiHandles.h）を掴んで
//                                 動かす・大きさを変える。枠の外・見えない要素・編集できない
//                                 中身の押下は素通し（ギズモ・カメラ・物の選択へ）。
//
// 選択の実体は EditorState の editorSel（SelKind::Canvas、index = Canvas の番号、
// sub = 要素の id）。受け付けるページの判定（Scene::editsUiFrom = エディタモード ×
// エディタカメラ × プレハブ編集中でない）と、オブジェクトの選択との排他
// （Scene::setEditorSel）は Scene の口を使う。
//
// ドラッグ: INPUT は掴んだ物（Canvas の番号・要素の id・つまみ）と押した位置・今の
// 位置・押したときのビューの大きさとモードの世代を atomic に置くだけ（ギズモと同じ形）。
// 欄は別々の atomic なので、書いている間は通し番号を奇数にし（書き終えたら偶数）、PHYSICS は
// 読んだ後に通し番号が変わっていないかを確かめる（変わっていれば混ざった = 次のパスで読み直す）。
// PHYSICS が最初のパスで開始時の性質を UiState から撮り、以後は「開始時の値 + 動かした
// 量」だけから pos / size を出して、変わったときだけ書き換える（Inspector と同じ手順 =
// uiops::editElement / writeOwnProps）。離したら届いている最後の位置を当ててから止める。
// 消えた・読み直した（読込・全消し・シーンの XML の適用、その中身だけの XML の適用）・
// モードが切り替わった・編集してよくなくなった・アンカー / ピボット / 親 / Canvas の高さ・
// 表示方法・出すカメラが変わったドラッグはそこで止める（もう書かない。開始時の値で計算すると
// 要素が飛ぶため）。
// ドラッグ中に Canvas の中身を共用 UI にしても、要素の id は変わらないので、そのまま
// 共用 UI の中身を動かし続ける。
// 当たり判定は版の写し（copyIfChanged）を読む（1 フレーム古くてよい。PHYSICS の
// 書き換えとロックで競らない）。ビューの大きさは onRender（RENDER）が atomic へ写す。
// ギズモより前に登録する（UI の枠を押したら、後ろのハンドルや物を掴まない）。
class UiInputComponent : public SceneComponent {
public:
    // cameraCount = カメラの固定スロット数（Scene が作り切ってから登録する）。
    explicit UiInputComponent(std::size_t cameraCount);
    ~UiInputComponent() override;

    bool onCommand(Scene& scene, std::size_t camIndex,
                   const nlohmann::json& msg) override;
    void onEditorStep(Scene& scene, double dt) override;
    void onRender(Scene& scene) override;

private:
    struct Cam;  // カメラごとのドラッグの状態（atomic を含むので .cpp）

    // 編集のコマンドの前置き（edit.ui.*）。
    static constexpr const char* kEditPrefix = "edit.ui.";
    // ライブラリの操作のコマンドの前置き（edit.lib.*。kind = ui だけを取る）。
    static constexpr const char* kLibPrefix = "edit.lib.";
    bool select(Scene& scene, std::size_t camIndex, const nlohmann::json& msg);
    // edit.ui.* を Op として積む（受け付けないページ・知らない操作は捨てる）。
    bool pushEdit(Scene& scene, std::size_t camIndex, const std::string& cmd,
                  const nlohmann::json& msg);
    // edit.lib.* の kind = ui を Op として積む（ほかの kind は false = 取らない）。
    bool pushLibrary(Scene& scene, std::size_t camIndex, const std::string& cmd,
                     const nlohmann::json& msg);
    // 選んでいる要素の枠の中・つまみに当たれば掴んで true（INPUT）。前の押下の印が
    // 残っていれば（届かなかった release）、ここで下ろす。
    bool pick(Scene& scene, std::size_t camIndex, const nlohmann::json& msg);
    struct Grab;  // 掴んだときに INPUT が置いた欄の写し（.cpp）
    // 掴んだときの欄を写す（PHYSICS）。書いている途中・読んでいる間に変わったら false。
    static bool readGrab(const Cam& cam, std::uint64_t serial, Grab& out);
    // ドラッグを始める: 開始時の性質と親の矩形を撮る（PHYSICS）。続けられなければ false。
    bool beginDrag(Scene& scene, std::size_t camIndex, Cam& cam, const Grab& grab);
    // 開始時の値 + 動かした量を当てる（PHYSICS）。続けられなければ false。serial = この
    // パスの初めに読んだ通し番号。
    bool applyDrag(Scene& scene, std::size_t camIndex, Cam& cam, std::uint64_t serial);
    // UiState の写し（INPUT スレッド専用。版が変わったときだけ取り直す）。
    const wizengine::UiState::Snapshot& snapshot(Scene& scene);

    std::vector<std::unique_ptr<Cam>> cams_;  // 数は作るときに決まり、以後変えない
    std::uint64_t known_ = 0;
    wizengine::UiState::Snapshot snap_;
};
