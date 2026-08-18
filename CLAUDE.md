# CLAUDE.md

Claude Code 向けのプロジェクト前提メモ。作業開始時にまず読むこと。

## 言語

- 応答・説明・コミットメッセージはすべて日本語で行う。
- コード内のコメントも日本語で書く。
- ファイル名・ディレクトリ名・識別子・APIは英語のまま（従来どおり）。

（より確実に固定したい場合は `.claude/settings.json` に `"language": "japanese"` を併用する。）

## これは何か

**WizEngine** は、サーバー側で **Chrono**（物理）＋ **Filament**（描画）を回し、オフスクリーンで
描いたフレームを **GStreamer** で H.264 にして RTP/UDP で配信するプロトタイプ。
クライアントは `gst-launch-1.0` のワンライナーで受ける（専用クライアントは未作成）。

言語は C++17 / CMake。Windows（VS2022 / Vulkan バックエンド）でビルド・動作確認済み。

## データの流れ

```
PhysicsWorld.step(dt)
  -> 箱の姿勢 (BodyTransform)
  -> toFilament()  [math_bridge.h]  四元数/位置 -> mat4f
  -> Renderer.setCubeTransform()
  -> Renderer.renderFrame()  Filamentで描画 + readPixels でRGBA取得
  -> VideoStreamer.pushFrame()  appsrc -> x264enc -> rtph264pay -> udpsink
```

## 2つのモード（エディタ / シミュレート）

**エディタ** は物理を止めて、置く・大きさを決める・ジョイントを設計する時間。
**シミュレート** は今までどおり Chrono を回す時間。切り替えはブラウザ上部の
2 ボタン、既定は `SceneConfig.h` の `kStartMode`（初期値 Simulate＝従来の挙動）。

- **設計値と実体を分ける**。オブジェクトは `GameObject::desc`（`editor::BodyDesc`＝
  形・大きさ・置いた姿勢・質量・色）と、実体（`physId` = Chrono、`renderId` =
  Filament）を両方持つ。シミュレートを止めると desc の姿勢へ全部戻るので、
  何度走らせても設計は壊れない。`reset()` も同じ意味になった（以前は
  `gridPos()+jitter()` で置き直していたが、いまは「置いた場所へ戻す」）。
- **編集は必ずキュー経由**（`src/EditorState.{h,cpp}`）。ブラウザ → INPUT スレッドが
  `EditorState::Op`（種類 + JSON）を積み、**物理スレッドが drain して実行**する。
  Chrono を触ってよいのは物理スレッドだけ、という約束をエディタでも崩さないため。
  Op を増やしても配管（キュー・構造体・分岐）は触らなくてよい。
- **生成は2スレッドにまたがる**。物理スレッドが剛体を作って `boxes_` に足し
  （`renderId` は未確定）、描画スレッドが次のフレームで `syncRenderables()` から
  レンダラブルを作る。削除はその逆順。つまり「置いた次のフレームから見える」。
- **削除しても番号は詰めない**（`GameObject::alive = false`）。詰めるとブラウザの
  選択とジョイントの参照が別の物を指す。Chrono 側も本当には消さず
  `PhysicsWorld::disableBody()`（当たり判定を切って地面の下へ）。Multicore
  バックエンドはボディ削除でデータマネージャの配列が壊れることがあるため。
- **形・大きさ・質量の変更は剛体の作り直しが要る**（Chrono は形を後から変えられ
  ない）。エディタ中は `physDirty` を立てるだけで、**シミュレート開始時に
  まとめて作り直す**。スライダーを動かすたびに剛体を捨てないための遅延。
- **ジョイントはシミュレート開始のたびに作り直す**（`Scene::buildJoints`）。
  エディタで物を動かしたあとも、拘束が今の位置に合った状態で張られる。
  停止時は `removeAllJoints()`。種類は `PhysicsWorld::JointType` の 5 種で、
  Chrono の `ChLinkLockLock` / `...Revolute` / `...Spherical` / `...Prismatic` /
  `ChLinkDistance`。**Revolute と Prismatic はリンク座標系の Z 軸が基準**なので、
  ユーザー指定のワールド軸を Z に合わせる四元数を作って渡している。
- **`objectsMutex_` は「一覧の構造」だけを守る**。書くのは物理スレッドだけなので
  そのスレッドは読むときにロック不要。他のスレッド（HTTP の階層 JSON、INPUT の
  選択、RENDER の反映）は `Scene::lockObjects()` を取る。`applyToRenderer()` は
  丸ごとロックを持つので、その中から呼ばれる `onRender` はロックを取らない
  （取ると自分自身で詰まる）。ロック順は **objects → editor → poses** の一方向。
- **保存/読込は `assets/scenes/*.json`**（`documentJson()` / `loadDocument()`）。
  保存時にオブジェクト番号を詰め、ジョイントの参照も付け替える。名前は英数字と
  `_ -` だけに正規化（保存先を assets/scenes に固定するため）。
- **シーンを書き換えるエディタ操作はエディタカメラ（`kEditorCamera`、既定 0）
  のページ専用**。edit.*（sim を除く）は EditorComponent が、ギズモの pick /
  drag / hover は GizmoComponent が、他のカメラからのぶんを弾く。UI 側もその
  ページ以外では Inspector タブを隠すが、**判定はサーバーが持つ**（リクエスト
  は誰でも作れるため）。例外は 2 つ。`mode`（エディタ⇄シミュレート）は
  **どのカメラからでも切り替え可** - シーンの中身を書き換える操作ではなく、
  Camera 1/2 で観察しながら回す・止めるのは普通の使い方のため（エディタ
  カメラは「編集できる」カメラなだけで、モードを握ってはいない）。
  `edit.sim`（シミュレート設定）も全カメラ可: Physics タブは全ページにあり、
  隣に並ぶ Solver / Rate は誰でも触れるので、これだけ弾くと分かりにくい。
- **ライトとカメラもエディタで編集できる**（追加・削除・位置・向き。
  **エディタモード×エディタカメラ限定** - edit.light.* / edit.camera.* と
  select.light / select.camera は EditorComponent が両方の条件で弾く）。
  ライトは GameObject と同じ「設計値 + 実体番号」（`Scene::LightItem`、
  `ed::LightDesc`）で Scene が持ち、実体（Filament のライト）の生成・破棄・
  反映は RENDER スレッドの `syncLights()`。**向きはオイラー角で「ゼロ = 真下
  (0,-1,0)」**（`scenemath::lightDirection`）。種類・影・減衰・円錐角は
  Filament のライト実体を決めるので、変更 = 実体の作り直し（`rebuild`）。
  初期 2 灯は `lightConfigs()`（方向ベクトル表記）を取り込んで編集可能に
  している。カメラは **`kMaxCameras`（既定 5、exe 引数 `--max-cameras N` で
  1〜16 に上書き可 - スロット数は実行環境の割り当てなので CPU コア指定と
  同じく起動引数。main が Scene のコンストラクタへ渡す）個の固定スロット**。
  ページと
  WebRTC の受け口は起動時に全部できているが、**動画ビュー（スワップチェーン +
  読み戻しバッファ）は予備スロットぶんを作り置きせず、「カメラを追加」
  （またはページ初訪問）の時点で描画スレッドがフレーム境界で生成**する
  （main.cpp の「動画ビューの遅延生成」。Filament を触れるのはそのスレッド
  だけなので、生成場所もそこに固定）。一度作ったビューは返さない。
  「追加 / 削除」は active フラグの上げ下げ（削除したページの URL は
  生きたまま、一覧から消える）。位置・向きの編集はオービット表現との変換
  （位置 = 視点ごと平行移動、回転 = pitch/yaw ⇄ elevation/azimuth。
  `Scene::cameraEditPose` が唯一の定義で、UI の数字・ギズモ・camera.set が
  揃う。ロールは無い）。**選択は EditorState の editorSel（atomic）**で、
  オブジェクト選択（BoxController）とは排他 - 立てる側が必ず反対を消す。
  エディタカメラのビューには**ライト（黄）/ カメラ（水色）の線画アイコン**
  （GizmoComponent がバッチ 5/6 に毎フレーム構築、エディタ専用レイヤ）が
  出て、クリックで選択、ギズモで移動 / 回転できる（拡縮は移動に丸める。
  エディタカメラ自身は選べない）。保存文書は **version 2**（lights /
  cameras 節が加わった。無い v1 文書は読み込み時に初期構成へリセット）。
- **System タブで変えた値も保存に映る**。PhysicsControlComponent は
  rate / substeps / solver / envelope / recovery を PhysicsTuning に書くとき、
  同じ値を EditorState の SimSettings にもミラーする。シーン保存はそちらを
  書き出すので、怠ると「見ている物理」と「保存される物理」が食い違う。
- ブラウザ側のタブは **Scene / Inspector / Physics**（`web/index.html` +
  `app.js` の `renderEditor()`。Inspector の内部 id は `tabEditor` /
  `paneEditor` のまま）。Inspector の選択オブジェクトは **Unity 風の
  Transform 行**（`.trRow`、位置 / 回転 / スケールに X/Y/Z の色タグ。id は
  edPX〜edSZ のまま、スケールは倍率ではなく実寸 m）。**ギズモのモード切替
  （✥/⟳/⤢ と World⇄Local）と設定の ⚙ は映像左上のツールバー**（`#gizmoBar`、
  Unity のシーンビュー左上相当）に重ねて出す: `#hit`(z=1) より上の z=2 に
  置いてクリックを受け、エディタモード×Editor Camera のページでだけ
  `renderEditor()` が表示する。**Inspector は「選択しているオブジェクトの
  内容」だけ**: Transform・質量・固定・色に加え、ジョイント節も選択時のみ
  表示し、一覧は選択が関わるものに絞る（どのジョイントにも地面でない体が
  必ずあるので、どれかを選べば必ず一覧に届く）。ギズモの数値設定（スナップ・
  刻み・グリッド）は普段隠れた `secGizmo` 節で、ツールバー右端の ⚙
  （`gzSettings` → `toggleGizmoSettings()`）が開閉する。ブラウザ内の表示
  状態だけの話なのでサーバーには送らない。
  ビューの下には**アセットパネル**（`#assets`、
  `renderAssets()`）: プリミティブ（Box/球）はクリックで配置、保存済み
  シーンはダブルクリックで読込（confirm 付き。読込は現在の配置を置き換える
  ため）。見出し下の操作列（`.asBar`）に、新規オブジェクトの初期値
  （edNewSize / edNewColor）とシーン名・💾保存・🗑全消し（旧 Inspector の
  配置・シーン節の移設先。読込ボタンと保存済み select は廃止＝タイルが
  受け持つ）。エディタモード中の Editor Camera ページにだけ出る。見た目と見出しはサイド
  バーの流儀（見出しクリックで折りたたみ）。高さは textarea と同じ右下の
  つまみ（CSS の `resize: vertical`。ドラッグ結果はブラウザが inline style に
  書くだけなので、ResizeObserver で拾って localStorage に記憶・復元する）。
  入りきらないタイルは縦スクロール。シミュレート設定（重力・摩擦・反発・減衰・スリープ）
  は Physics タブに置く。入力欄は 500ms ポーリングで上書きされるが、
  **フォーカス中の欄だけは触らない**（打っている途中の数字が消えるため）。

## ギズモ（`src/GizmoComponent.{h,cpp}`）

選択中のオブジェクトに出る Unity 風の移動 / 回転 / 拡縮ハンドル。
**エディタカメラ専用**: 操作はそのページからだけ受け、描画もそのビューに
だけ出す。バッチを `Renderer::kLayerEditorOnly` レイヤに置き、エディタ
カメラのビューだけ `setViewEditorLayerVisible` で見せている（シーンは全
ビュー共有のままなので、レイヤで映る/映らないを切るのが一番安い）。

- **サーバー側で 3D の線として描く**。ブラウザに届くのは映像なので、HTML/SVG で
  重ねるとカメラを回した瞬間にオーバーレイだけ先に動いて映像が 1〜2 フレーム
  遅れる。シーンの中の線にしておけば必ず同じフレームに乗り、手前の物に
  隠れる挙動も勝手に付いてくる（グラブ線と同じ理屈）。
- **当たり判定は NDC（画面）上**でやる（`scenemath::distanceToSegment2D`）。
  3D で線との距離を測ると奥の軸ほど掴みにくくなり「見えているとおりに掴めない」。
- **`pick` を最初に見る**ので、コンポーネントの登録順は
  Gizmo → Camera → Box。ハンドルに当たったら `pick` をそこで止め、選択の
  作り直しと自由移動（グラブ）に渡さない。
- **ドラッグ中は開始時の値しか見ない**（開始姿勢・開始座標系・軸上の開始
  パラメータ）。現在値から差分を取ると、適用結果が次のフレームの入力に
  混ざって発散する。
- 回転は atan2 の折り返しを差分の積み上げで吸収する。ワールド軸の回転は
  `q_new = AngleAxis(角度, 軸) * q_start`。
- **オイラー角と四元数の変換は `scene_math.h` に 1 か所だけ**
  （`quatFromEulerDegrees` / `eulerDegreesFromQuat`、順序は R = Rz*Ry*Rx）。
  インスペクタの数字・ギズモの回転・Chrono に渡す姿勢がここで揃う。
- 線は `Renderer::configureLineBatches` / `setLineBatch` で色ごとに 1 個の
  レンダラブルに詰める（1本1レンダラブルのグラブ線方式では回転リング
  144 本が重すぎる）。頂点数は固定にして余りは**面積 0 の三角形**で埋める
  ＝実行中にプリミティブ数を変える API（版によって名前が違う）を使わずに済む。
  中身が前回と同じフレームは転送を省く。
- **バッチには太線と塗りつぶしの面を混ぜられる**（`wizengine::BatchShape`）。
  1 スロットは「四角 2 枚 = 4 三角形」ぶんの席で、太線はそれを丸ごと（板 2 枚）、
  塗りつぶしの四角は片側だけ使い、残り半分は 1 点に潰す。三角形は `d = c` の
  四角として渡す。おかげで矢じり（塗った円錐）・平面ハンドル・拡縮のつまみを、
  インデックスバッファもレンダラブルも作り直さずに同じ入れ物へ入れられる。
  面の向き（巻き方）は `culling : none` なので気にしなくてよい。
- **Filament に線の太さは無い**（`PrimitiveType::LINES` はどのバックエンドでも
  1 ピクセル）。太く見せるため、1 本を**直交する 2 枚の板**＝4 三角形として
  描く（`Renderer::buildTube`）。カメラを向く 1 枚の板にすると別のビューから
  真横になって消えるので、向きに依存しないこの形にしてある。両面を出すため
  `line.mat` に `culling : none` が要る。太さは本ごとに渡す: バッチは全カメラ
  共有だが、太さはそのギズモを見ているカメラからの距離で決まるため。
  太さの調整は `GizmoComponent.cpp` の `kThickness` / `kRingThickness`
  （ギズモ長さに対する割合。0.03 で 720p の約 4 ピクセル）。
- ハイライトのために、掴んでいないときだけブラウザが `hover` を ~14Hz で送る。
  押す前にどの軸を掴めるか分からない、では使いづらいため。
- **Y=0 のグリッドは太線バッチではなく細線セット**（`Renderer::addLineSet` /
  `setLineSet`、1 ピクセルの LINES）。作業の目安にポリゴンのコストを払わない:
  頂点は 1 本 2 個（太線の 1/4）で塗りも無く、頂点を作り直すのは表示の
  ON/OFF か間隔が変わったときだけ（`GizmoComponent::onRender` が前回値と
  比較）。本数が変わったらレンダラブルごと作り直す＝実行中にプリミティブ数を
  変える版依存 API を避ける、はバッチと同じ方針。セットは灰（格子）・赤
  （X 軸）・青（Z 軸）の 3 本立てで、エディタ専用レイヤに乗る。
  広さは `kGridHalf`（50m ＝ 100×100m。見える地面 16m・物理の床 20m より
  広い作業目安なので、床の外はシミュレートで落ちる）。表示と間隔は
  GizmoSettings（`grid` / `gridStep`、Inspector タブ、下限 0.25m）。

## イベントグラフ（ノードベースのイベント設計）

Object / Light / Camera の Inspector と、映像上のノードエディタ（ギズモバーの
⚡、Node-RED 風）で組む「トリガー → アクション」。「衝突したら色を黒にする」
をエディタで設計し、シミュレート中に発火する。

- **型は EditorTypes.h**（`NodeKind` / `NodeDesc` / `WireDesc`）。トリガーは
  衝突（OnCollision）・開始（OnSimStart）・タイマー（OnTimer）、アクションは
  色（SetColor）・力（ApplyImpulse）・固定（SetFixed）・ライトの色/強さ
  （SetLight*）・カメラ注視（CameraLookAt）。ワイヤーはトリガー → アクションの
  1 段だけ（連鎖なし）。target の指す種別は `nodeTargetKind(kind)` が唯一の
  定義（object / light は保存で番号が詰まるので、詰め替え・掃除が全部ここで
  分岐する）。
- **グラフ本体は EditorState**（ジョイントと同じ mutex 流儀）。ノード id は
  再利用しない（ワイヤーが別のノードを指し直すため）。編集は
  edit.node.* / edit.wire.* → Op キュー経由で物理スレッドが適用。ワイヤーの
  向き（from = トリガー、to = アクション）と重複は `addGraphWire` が検証する。
- **実行は物理スレッド**（`Scene::runEventGraph`、`physics_.step()` の直後 =
  そのステップの接触を見る）。毎ステップのロックを避けるため、
  `EditorState::graphVersion()`（変更ごとに進む版番号）が変わったときだけ
  一覧をコピーする。タイマー等の実行状態は **ノード id で引く**
  （`Scene::GraphRuntime`）ので、シミュレート中の編集で他のノードの状態が
  リセットされない。
- **衝突は「新しく触れたペア」だけ**。NSC は載っているだけでも毎ステップ接触が
  立つので、前ステップとの差分を取り、さらに**最初の収集パスは覚えるだけ**
  （priming）にして開始時点で触れていたぶんを発火させない。接触の列挙は
  `PhysicsWorld::activeContactPairs()`（`ReportContactCallback` を走査、
  ChBody* → physId は追加時に作る逆引きマップ）。Multicore の接触コンテナが
  `ReportAllContacts` を実装しない版では空が返る＝衝突トリガーだけ効かない。
- **アクションは実行時の上書き**で、desc（設計値）は書き換えない。色は
  `GameObject::runtimeColor`、ライトは `LightItem::runtime*`（syncLights が
  実体へ流す直前に重ねる）、固定は `fixedTouched` に記録。シミュレートの
  開始・停止・Reset で `resetGraphRuntime()` が全部戻す（姿勢が desc へ戻るの
  と同じ原則）。SetColor が効くのは組み込みメッシュ描画だけ（glTF
  インスタンスは個別のベース色を持てない）。ApplyImpulse は
  F = m・Δv/dt を `applyForce` に渡す＝レート非依存で Δv がそのまま乗る。
- **対象が消えたノードは掃除**（`pruneGraphForRemoved`、ジョイントの掃除と
  同じ判断）。OnCollision の相手フィルタだけが消えたときはノードを残して
  「何でも」(-2) に戻す。
- **保存文書は version 3**（nodes / wires 節）。保存でオブジェクト・ライトの
  番号を詰めるのに合わせて target / other も付け替える（ライトにも remap 表が
  要るようになった）。読込はジョイントと同じく base / lightBase ぶんずらす。
  v2 以前の文書はグラフ無し＝空で読める。
- **UI は /scene の `graph`**（発火回数 fired 付き＝ノードの ⚡ バッジ）を
  ポーリングで描く。グラフと選択肢が変わったときだけ DOM を組み直し、
  **ドラッグ中とノード内入力のフォーカス中は組み直さない**（入力欄の
  「フォーカス中は触らない」と同じ理由）。線はキャンバス座標の SVG ベジェ 2 本
  （見える線 + 太い透明の当たり判定）。ポートの座標は DOM を測らず
  「ノード左上 + 定数」で計算する（app.js の NODE_W / PORT_Y）。
  エディタは**画面中央の 90% ウィンドウ（.ndWin）**で、外側の全画面レイヤ
  （.ndBack、position:fixed。サイドバー z:40 より上の z:60。#stage の子の
  ままなのでフルスクリーン中も出る）が後ろを `backdrop-filter: blur` で
  ぼかす（非対応ブラウザは @supports で濃い半透明に落とす）。✕ / Esc /
  外側クリックで閉じる。
- **/input は誰でも叩けるので型を信じない**。数値の取り出しは
  `EditorTypes.h` の `jsonNumber` / `jsonInt`（型が違えば既定値）を通す。
  nlohmann の `value()` は型違いで投げ、処理スレッドごと落ちるため。

## ファイル

- **CPU コアの固定**（`src/CpuAffinity.{h,cpp}`、Windows / Linux 両対応）。設定は **exe 引数**（`--physics-cores "0-11"` / `--render-cores "12-15"` /
  `--physics-threads N`、`--help` で一覧）。**scene.cpp には置かない**（scene はユーザーが
  触るシーン内容、CPU 割り当ては実行環境の設定という分離）。オプションはモードの前後
  どこに書いてもよく、位置引数とは分けて解析される。Windows は `SetThreadAffinityMask` /
  `GetProcessAffinityMask`、Linux は `pthread_setaffinity_np` / `sched_getaffinity`。
  **Chrono::Multicore は OpenMP で解くので、呼び出し元スレッドを固定しても
  ワーカーは固定されない**（Windows では新規スレッドは"プロセスの"アフィニティを継承し、
  MSVC の OpenMP は 2.0 なので OMP_PLACES/OMP_PROC_BIND も使えない）。対策として
  `pinOpenMpWorkers()` が並列領域に入り**各ワーカー自身に 1 コアずつピンさせる**
  （`omp_set_dynamic(0)` で本数を固定）。呼ぶのは物理スレッド内・最初のステップ前。
  `SetNumThreads` はプール確保時に効くので `scene.build()` より前に設定。物理スレッドと
  描画スレッドはそれぞれ自分もピンする。OpenMP 無しビルドではメッセージを出して無効化。既存の外部アフィニティ（taskset /
  start /affinity）は `availableCores()` で尊重。失敗しても続行（性能の問題であって
  正しさの問題ではない）。
- `src/PhysicsWorld.{h,cpp}` — 物理エンジン（Chrono）。重力・接触・材質の設定のみ。
  `addBox(...)` で剛体追加、`step` / `bodyTransform(id)` / `setBodyPose(id,...)`。
  エディタ用に `placeBody`（起こすための落下速度を与えない置き直し）、
  `setBodyFixed`、`disableBody`（削除相当。当たり判定を切って地面の下へ退避し、
  番号は残す）、`setGravityY`、そして `addJoint` / `removeAllJoints` を持つ。
  ジョイントの `Initialize` は Chrono 9 で `ChCoordsys` → `ChFrame` に変わった
  ので、この版から既にあるスリープ/速度と同じ SFINAE の書き方で両対応にしてある。
  NSC・Bullet・`make_shared` 整列。シーンの中身は持たない。
  スリープ（`setSleepingEnabled`）は Chrono 9 名（`SetSleepingAllowed` /
  `SetSleepTime` / `SetSleepMinLinVel` / `SetSleepMinAngVel`）を使用。旧名
  （`SetUseSleeping` / `SetSleepMinSpeed` / `SetSleepMinWvel`）にも自動で
  フォールバックする。眠ったボディは接触でしか起きないため、しきい値は既定より
  厳しめ（1.0s / 0.02 m/s / 0.02 rad/s）。リセット時は `wakeAll()`。
- **フレーム読み戻しは非同期**（`Renderer::renderFrame`）。以前は `readPixels` の直後に
  `flushAndWait()` で完了を待っており、これが描画スレッドのストールになっていた。現在は
  ビューごとに**キャプチャバッファ 2 枚**を持ち、GPU が片方を埋めている間にもう片方を
  エンコーダへ渡す。完了フラグは `shared_ptr<atomic<bool>>` でコールバックと共有（遅延
  完了が解放済みメモリに触れないため）。未配信のバッファは `std::deque` で保持し**投入順に
  配信**（1 枚しか覚えないと 2 枚目以降が失われてストリームが止まる）。両方使用中の
  フレームは**待たずにスキップ**（CPU を止めるよりフレームを落とす方が良い）。
  代償は 1〜2 フレームの遅延。終了時は `finishPendingReadbacks()` で完了を待つ
  （main の join 後、HTTP 停止前）。
  なお GPU→CPU→GPU の往復自体は残っている（真のゼロコピーには Vulkan Video か AMF の
  インターop が必要）。
- **環境光（IBL）は実行時変換**。`assets/*.hdr` を `stbi_loadf` で読み、
  `IBLPrefilterContext`（`filament-iblprefilter` + `filament-generatePrefilterMipmap` を
  リンク）で equirect → キューブマップ → ラフネス mip 列を **GPU 上で生成**。cmgen と
  ビルド時変換は廃止したので、**HDR を差し替えても再ビルド不要**（再起動のみ）。
  irradiance は指定せず、Filament が反射マップの最下位 mip から導出する。scene.cpp の
  `kEnvironmentHdr`（例 `"studio.hdr"`）と `kEnvironmentIntensity` で設定。読み込み失敗は
  `AssetError` で停止し、**先頭バイトから実際の形式を判定して報告**する（`.exr` を
  `.hdr` にリネームした場合などが一目で分かる）。幅 2048 超はボックスフィルタで縮小
  （8k は float 400MB になり確保に失敗しうる。平均で縮小＝太陽など小さく明るい光源の
  エネルギーを保つ）。ImageLoader.cpp の `STBI_ONLY_*` は
  **ホワイトリスト**なので `STBI_ONLY_HDR` が必須（無いと HDR デコーダが丸ごと
  コンパイルされず、正常な Radiance ファイルが "unknown image type" で弾かれる）。
  加えて stb は署名を `#?RADIANCE\n` と厳密一致で見るため、BOM・CRLF・CR のみの
  ヘッダはメモリ上で正規化してから再読み込みする（バイナリ開始位置が 1 バイトでも
  ずれると全ピクセルが壊れるので注意）。ミップ列は **`generateMipmaps()` を使わず自前で
  ボックスフィルタ生成してアップロード**する（`generateMipmaps` は
  GEN_MIPMAPPABLE/BLIT の usage を要求し、フラグ名と有無が Filament の版で変わる。
  地面テクスチャも同じ理由で自前生成）。**glTF は metallicFactor の既定が 1.0＝金属**で、金属は拡散反射を
  持たないため映り込む環境が無いと影部分が真っ黒になる。これが「glb によって真っ黒」の正体。
- `src/Renderer.{h,cpp}` — 描画エンジン（Filament, headless Vulkan）。下地（デバイス・
  カメラ・ライト2灯＋IBL・共有キューブメッシュ・マテリアル）のみ構築。中身は
  `addShape(ShapeMesh)` / `addGround(halfSize,color)` / `setCamera(eye,target)` で追加。
  箱・床とも lit（`shaded.mat`＝箱用 lit / `ground_lit.mat`＝床用）。箱は影を落とし
  受けもする（以前は unlit＋頂点カラーで焼き込み陰影だったため、転がると陰影が
  向きに追従せず不自然だった）。箱の色は Scene の `setBoxColor`。床テクスチャは
  `assets/textures/ground.png`（stb_image で読み込み。sRGB、ミップマップは自前生成＝
  `generateMipmaps` はこの版で usage フラグ必須のため不可）。画像が無ければ
  コード生成の市松模様にフォールバック。`baseColor` は乗算する色味。
  UV は「1リピート＝`kGroundTile` メートル」でタイリング。readPixels で RGBA 取得。
  エディタ用に**実行時に増減できる形状スロット**（`addShape` / `removeShape`、
  箱と UV 球、削除した番号は空きとして再利用）と**オブジェクトごとの色**
  （`setShapeColor` が初回にそのスロット専用のマテリアルインスタンスを作る。
  共有インスタンスを書き換えると全部の色が変わってしまうため）、
  **ジョイント線**（`setJointLineCount` / `setJointLine`、グラブ線と同じ
  「2頂点1本」の作りを使い回し）を持つ。glTF インスタンスは gltfio に 1 個だけ
  壊す口が無いので、削除時は**スケール 0 に潰して見えなくする**（番号は返らない）。
- `src/GltfLoader.{h,cpp}` — glTF/GLB 読み込み（Filament の gltfio）。`Renderer::addModel()`
  / `setModelTransform()` 経由で使い、gltfio は Renderer の外に漏らさない。CMake が
  gltfio のライブラリ（gltfio_core / uberarchive / dracodec / ktxreader / stb）を検出
  したときだけ有効（`WIZ_HAVE_GLTFIO`）。無い場合もビルドは通り、読み込み時にメッセージを
  出してスキップする。読み込むモデルは scene.cpp の `kModelPath` ほかで指定（既定は空）。
  **表示のみで当たり判定は無い**。
  `kBoxModelPath` を指定すると、動く箱の描画をその glb に差し替える（`createInstancedAsset`
  で 1 アセット N インスタンス＝メッシュ共有）。物理形状は kBoxSize の立方体のままなので、
  モデルは単位立方体に収まるものを選び `kBoxModelScale` で微調整する。読み込みに失敗したら
  自動的に組み込みキューブへフォールバック。モデル使用時は選択ハイライト無し。
  Filament 1.74 Windows 版のライブラリ名は `uberz` ではなく **`uberzlib`**、また
  `shlwapi` のリンクが必要（`utils::Path`）。
- `src/ImageLoader.{h,cpp}` — stb_image で画像を RGBA8 として読む（PNG/JPEG/TGA/BMP）。
- `src/Scene.h` + `src/scene.cpp` — **シーン定義（すべての設定はここ）**。地面と
  4×4×4=64個の 0.5m 箱を物理・描画の両方へ登録し対応付け（物理ID↔描画ID）を保持。
  性能つまみ `kSubsteps`（物理サブステップ）と `kSolverIterations`（ソルバ反復）も
  ここ。マス目サイズ `kGroundSquare` も含め、`build()`（生成＋カメラ）/ `step(dt)`（step のみ。自動リセットは廃止）/ `sync()`（姿勢反映、
  0.5スケール適用）/ `reset()`。格子・間隔・ジッター等の定数も scene.cpp 先頭に集約。
- `src/VideoStreamer.{h,cpp}` — GStreamer パイプライン。`OutputMode` で
  web/None（GStreamer出力なし。ブラウザへは WebRtcStreamer が担当）/ window / stream
  （RTP/UDP）/ rtsp（rtspclientsink）を切替。
- `src/WebRtcStreamer.{h,cpp}` — GStreamer `webrtcbin` でブラウザへ WebRTC 配信。
  コーデックは `scene.cpp` の `kVideoCodec`（VP8 / VP9 / H264）と `kVideoBitrate` で指定。
  VP9 は同画質で VP8 の約半分の帯域だがエンコードが重い（`cpu-used=8` で速度優先）。
  H264 は GPU エンコード（amfh264enc → mfh264enc → x264enc → openh264enc の順で自動選択、
  起動時に `h264 encoder: ...` と表示）。`h264parse config-interval=-1` で SPS/PPS を
  キーフレーム毎に送出（無いと真っ黒になる定番）。**RTP の payload 番号はブラウザの offer
  から動的に取得**（VP8=96 は偶然一致していただけで、VP9/H264 は別番号。固定すると
  ネゴシエーション成立・エラー無しのまま真っ黒になる）。H264 は加えて packetization-mode=1
  の PT を選び、offer の profile-level-id を answer にそのまま返す。
  ブラウザ側は `/stats` のコーデックだけを `setCodecPreferences` で提示。外部メディアサーバ不要。`handleOffer(offerSdp)` で WHEP 風シグナリング
  （SDP offer→answer、非トリクル ICE）。GLib メインループを別スレッドで実行。
  要 GStreamer webrtc/nice プラグイン。**未実機検証**（要調整の可能性）。
- `src/Stats.h` — スレッド間で共有する性能カウンタ（atomic）。物理スレッドが Hz と
  1更新の所要 ms、描画スレッドが fps と renderFrame の ms を書き、`/stats` が JSON で
  返してブラウザ下部のオーバーレイに表示。
- `src/HttpServer.{h,cpp}` — cpp-httplib（別スレッド）。`/` 操作ページ、`/whep` で
  WebRTC シグナリング（`setOfferHandler`）、`/input` で入力受信（`drainCommands`）。
  入力は JSON（`{"cmd":...}`、main で nlohmann/json パース）。既定ポート8080。
  `/favicon.ico` は `web/favicon.ico` をビルド時に実行フォルダへコピーして配信（起動時に
  読み込み、無ければ 404）。拡張子から MIME を判定するので png/svg でも可。
  **視聴は同時1ブラウザのみ**: 最初の `/whep` がトークン（ヘッダ `X-Viewer-Token`）で
  セッションを取得し、`/viewer/ping`（2秒毎、本文=トークン）で維持。2つ目以降は 409 を
  返しブラウザ側でエラー表示＋自動リトライ。`/viewer/leave`（sendBeacon）または6秒
  無応答で解放し、`setViewerGoneHandler` → `WebRtcStreamer::stopSession()` で
  パイプラインを破棄。
- `src/web_client.h` — ブラウザ用フロント（`kIndexHtml`）。`<video>`＋WHEP クライアント
  （`/whep` に SDP offer を POST）、操作は `/input` に JSON POST。フロントはブラウザのみ。
- `src/math_bridge.h` — Chrono → Filament の姿勢変換（四元数から回転行列を手計算）。
- **視聴者がいない間は完全に休む**（web モードのみ、`scene.cpp` の `kIdleWhenUnwatched`）。
  `HttpServer::hasViewer()`（トークン＋ハートビート）で判定し、描画スレッドは 100ms 間隔の
  ポーリングのみ、物理スレッドも停止（アキュムレータもクリアするので復帰時に一気に進まない）。
  WebRTC のパイプラインは視聴者離脱時に破棄済みなのでエンコードも止まる。window/stream/rtsp
  モードは常時動作。
- 物理スレッドは**壁時計駆動の固定タイムステップ**（アキュムレータ方式）。経過した実時間
  ぶんだけ dt 単位で進め、遅れは次のパスで取り戻す。取り戻せる上限は `kMaxCatchUp`（4）で、
  超えたぶんは捨てる＝その間はスローモーションになる。実時間との比は `/stats` の
  `realtime` に出し、ブラウザに `speed 1.00x` として表示（0.95 未満で警告色）。
- **実行時に読むファイルはすべて `assets/`**（exe と同じ場所）。ビルドが
  `build/<config>/../assets/` ではなく `build/assets/` に出力（CMake の `ASSET_DIR`）し、
  中身は `shaded/ground_lit/line.filamat`・`ground.png`・`web/`（index.html, favicon.ico）。
  コード側は `wizengine::assetPath()` を通して解決する（絶対パスと `assets/` 始まりは
  そのまま＝任意の場所のモデルも読める）。scene.cpp の `kBoxModelPath` 等は
  **`assets/` からの相対名**で書く。
- **アセット読み込み失敗は例外で強制停止**（`src/AssetError.{h,cpp}`）。重要なのは
  **チェックリストを持たない**こと: `GltfLoader::add/createInstances`、マテリアル読み込み、
  テクスチャ読み込みという**読む側そのものが `AssetError` を投げる**ので、scene.cpp に
  新しいファイルを追加しても検証漏れが起きない（変数名を列挙する方式は、追加時に
  すり抜けるため廃止）。戻り値でのエラー報告とフォールバックも廃止（呼び出し側が
  無視できてしまうため）。main の起動時チェックに残すのは
  **どの読み込み側も検査しないもの＝`assets/web/index.html` のみ**（HTTP はリクエスト毎に
  読むので、欠けても白紙ページとして後から表面化するため）。.filamat は Renderer 側で
  `requireFile()`＋`AssetError` により検査されるので、起動時リストには載せない
  （二重管理＝同期漏れの再発を避ける）。main が `AssetError` を専用に捕捉し、
  ファイル名・探索ディレクトリ・対処を表示して終了。
- **起動時に実行時ファイルを検証**（main.cpp 冒頭）。`shaded.filamat` /
  `ground_lit.filamat` / `web/index.html` が無ければ**カレントディレクトリを表示して
  終了**（`line.filamat` と `ground.png` は警告のみ）。exe を別の場所から起動したときに
  Filament の奥で無言終了していた問題への対処。あわせて main 全体を try/catch で包み、
  例外を表示してから Enter 待ち（Explorer 起動でコンソールが消えるため）。
- `src/EditorTypes.h` — エディタ文書の型だけを集めたヘッダ（`AppMode` /
  `ShapeKind` / `JointKind` / `BodyDesc` / `JointDesc` / `SimSettings` と、その
  JSON 変換・範囲クランプ）。Chrono も Filament も出てこないので、どのスレッド
  からでもコピーできる。保存フォーマットとブラウザ API のキーはここが唯一の定義。
- `src/EditorState.{h,cpp}` — モード（atomic）、編集操作のキュー、ジョイント一覧、
  シミュレート設定、`assets/scenes` の読み書きと一覧キャッシュ。オブジェクト
  そのものは持たない（実体と並べて Scene が持つ。番号がずれると黙って別の物を
  動かしてしまうため）。
- `src/EditorComponent.{h,cpp}` — ブラウザの `mode` / `edit.*` コマンドを受ける
  SceneComponent。やるのは値の正規化と検証だけで、実体の操作は EditorState の
  キューに積む。ただし物理レート系（Hz・サブステップ・反復・エンベロープ・
  リカバリ）だけは `PhysicsTuning` の atomic に直接書く（System タブと同じ口）。
- `src/main.cpp` — 起動・出力モード選択・**2スレッド**（物理＝`scene.stepPhysics`、
  描画＝main：入力・カメラ・`scene.applyToRenderer`・`renderFrame`）。共有は Scene の
  ポーズ・スナップショット（`poseMutex_`）とオブジェクト一覧（`objectsMutex_`）。
  reset/pause は atomic で物理へ。物理スレッドは毎パス `scene.applyPendingEdits()`
  を呼んでから**モードで分岐**し、エディタなら積分せず `scene.stepEditor()` だけ
  回す。カメラは方位角・仰角オービットで、矢印キーで回す。
- `assets/materials/*.mat` — matc でビルド時に `unlit.filamat` へコンパイル。
- `CMakeLists.txt` — Filament / Chrono / GStreamer のリンク。

## ビルドと実行

```bash
cmake -S . -B build \
  -DChrono_DIR=/path/to/chrono-install/lib/cmake/Chrono
cmake --build build -j
cd build && ./wizengine window     # ローカル表示。配信は: ./wizengine stream 127.0.0.1 5000
```

Filament は `CMakeLists.txt` の FetchContent が OS 別プレビルド `.tgz` を自動
ダウンロードして `FILAMENT_ROOT` を設定する（ソースビルドはしない）。ローカル版を
使うなら `-DFILAMENT_ROOT=...`、版を変えるなら `-DFILAMENT_VERSION=...`。

Chrono は `-DCHRONO_ROOT=<インストール先>`（`ChronoConfig.cmake` を自動探索）か、
`-DChrono_DIR=<ChronoConfig.cmake のあるフォルダ>` を指定する。

**CRT は全体で /MD（動的）に統一**すること。Filament は CRT ごとに別ライブラリ
（`lib/x86_64/{md,mdd,mt,mtd}`）を同梱し、GStreamer 公式ビルドも /MD 前提。Chrono 側の
`USE_MSVC_STATIC_RUNTIME` は **必ずOFF**。ここが食い違うと `std::ios_base::good` の重複定義
（LNK2005）や `type_info::vftable` の未解決になる。WizEngine 側は
`-DWIZ_MSVC_STATIC_RUNTIME=ON/OFF` で明示指定でき、Filament の参照先も自動で追従する。
CRT を変えたときは build フォルダを削除してから再 configure。

物理バックエンドの選択は2段構え。
1. CMake `-DWIZ_USE_MULTICORE=ON`（既定 OFF）… Multicore モジュールを**リンクして使える
   状態にする**だけ。`find_package(Chrono COMPONENTS Multicore)` になる。
2. `scene.cpp` の `kBackend`（`PhysicsBackend::Core` / `::Multicore`）… **実際にどちらを
   使うか**。他のシーン設定と同じ場所で切り替える。
CMake が OFF のまま `kBackend = Multicore` にした場合は、起動時にメッセージを出して
自動的に Core にフォールバックする。Multicore は `ChSystemMulticoreNSC`＋APGD で設定は
`GetSettings()` 経由。**スリープ非対応**なので `asleep` は 0 のまま。起動ログとブラウザの
計測表示に `engine core|multicore` が出る。

Chrono::Multicore（大量剛体の並列化）を使う場合の Chrono 側設定:
`CH_ENABLE_MODULE_MULTICORE=ON` ＋ `Blaze_ROOT_DIR` ＋ `Thrust_DIR`。Thrust は OpenMP
バックエンドで使うので GPU 不要。Chrono 本体（`third_parties/chrono` = 9.0.0）・
Eigen（`third_parties/eigen` = 3.4.0）・Blaze（`third_parties/blaze` = v3.8.2）・
Thrust（`third_parties/thrust` = 1.17.2）はサブモジュールで取得できる
（Thrust は入れ子の cub を含むので `git submodule update --init --recursive`）。
`Thrust_DIR` は `third_parties/thrust/thrust/cmake`（`thrust-config.cmake` が
あるフォルダ）、`EIGEN3_INCLUDE_DIR` / `Blaze_ROOT_DIR` は各サブモジュールの
ルートを指す。

cpp-httplib / nlohmann/json / cgltf / stb は **`third_parties/` の git サブモジュール**
（`git clone --recursive` または `git submodule update --init` で取得。
`.gitmodules` は shallow 指定＝nlohmann/json の巨大な履歴を引かない）。
サブモジュール未取得のクローンでもビルドが止まらないよう、無いものは従来どおり
**単一ヘッダを configure 時にダウンロード**して `build/_deps/single-include/` に置く
フォールバックが残る。オフライン環境ではサブモジュールを取得しておくか、
ヘッダを手動配置すればよい。

受信側:
```bash
gst-launch-1.0 udpsrc port=5000 \
  caps="application/x-rtp, media=video, encoding-name=H264, payload=96" ! \
  rtph264depay ! avdec_h264 ! videoconvert ! autovideosink sync=false
```

配管だけの検証: サーバー役を `videotestsrc ! videoconvert ! x264enc
tune=zerolatency ! rtph264pay ! udpsink host=127.0.0.1 port=5000` に置き換える。

## 最初に潰すべき既知の詰まりどころ（バージョン依存）

1. **Filament のリンク対象と順序** — `CMakeLists.txt` の `FILAMENT_LIBS`。未定義
   シンボルが出たら `${FILAMENT_ROOT}/lib/x86_64/` の実ファイルを見て増減・並べ替え。
2. **Chrono のリンク方式** — 変数方式（`CHRONO_INCLUDE_DIRS` / `CHRONO_LIBRARIES`
   / `CHRONO_CXX_FLAGS` / `CHRONO_LINKER_FLAGS`）を使用。`Chrono::main` 等の
   ターゲットは使わない（この環境の Chrono は提供しない）。Chrono の flags は
   ターゲットに適用済み。
3. **ヘッドレス描画のバックエンド** — Linux で GL コンテキスト生成に失敗する場合は
   `FILAMENT_BACKEND=vulkan ./wizengine ...`、または `Renderer.cpp` の `Engine::create()`
   を `Engine::create(Engine::Backend::VULKAN)` に。GPU + ドライバ必須。
4. **マテリアルのシェーダ言語** — Filament が Vulkan を選ぶ環境では、`matc` を
   `-a all -p desktop` で通しておく（SPIR-V を含めないと "material not built for
   Vulkan (SPIR-V)" で abort）。`CMakeLists.txt` で設定済み。
5. **映像の上下** — `readPixels` の向きはバックエンド依存。Vulkan はトップダウン
   なので `VideoStreamer` で反転しない。OpenGL バックエンドに切替えると上下が逆に
   なるので、その場合は `videoflip method=vertical-flip` を戻す。
6. **Chrono の API 名は 9.0 以降前提**（`ChVector3d`, `ChContactMaterialNSC`,
   `SetGravitationalAcceleration` など）。古い版なら旧名に読み替え。物理は
   `ChSystemNSC`（相補性ソルバ）を使用。硬い拘束として接触を扱うので 1/60 秒の
   タイムステップでも床を貫通しない。SMC（ペナルティ法）は同条件ですり抜けたため
   NSC に戻した。コリジョン系は既定で Bullet（明示設定不要）。
7. **Chrono のメモリ整列** — 全 Chrono オブジェクト（`ChSystem` 含む）を
   `chrono_types::make_shared` で生成する（Eigen/AVX の整列要件）。値メンバや素の
   `new` は避ける。
8. **Debug/Release を Chrono と揃える（重要・実際の落下しない原因だった）** — Chrono を
   Release だけでインストールした場合、WizEngine も必ず **Release** でビルド・実行する。
   Debug アプリ（/MDd）を Release の Chrono（/MD）にリンクすると、リンクは通っても
   CRT 不整合で実行時に未定義動作になり、物理だけが静かに壊れる（描画は動く）。
   実行は `build\Release\wizengine.exe`。VS の「フォルダーを開く」では構成を Release に。
   Debug でも使いたいなら Chrono を Debug でもインストールする。

## 実装済み / 未実装

- 済: エディタモード（配置・プロパティ編集・ジョイント設計・シーンの保存/読込）と
  シミュレートモードの分割。
- 済: イベントグラフ（Node-RED 風のノードエディタ。衝突・開始・タイマーの
  トリガーと、色・力・固定・ライト・カメラ注視のアクション。上の
  「イベントグラフ」の章を参照）。
- 済: ステップ3（姿勢反映）〜6（UDP配信）。
- 未: ステップ7（クライアント→サーバーの入力・制御チャネル。カメラ操作を
  UDP/TCP で受けて `Renderer` にカメラ更新 API を追加）。
- 将来: ハードウェアエンコード（`nvh264enc` 等）、readPixels を避けた GPU 直結、
  物理と描画のスレッド分離（姿勢はダブル/トリプルバッファで受け渡す）。

## 規約

- 重力は −Y にしてあり Filament の Y-up と一致。両エンジンとも右手系なので
  姿勢変換に軸スワップは不要。この前提を崩さないこと。
- 自作の `Renderer` クラスは `namespace wizengine` に入れてある（`filament::Renderer`
  と名前が衝突するため）。`Renderer.cpp` では `using namespace filament;` のまま
  でよいが、`filament::Renderer` のネスト型は `filament::Renderer::ClearOptions`
  のように明示修飾する。新規クラスも衝突を避けるなら `wizengine` に入れる。
- C++17。外部依存の追加は最小限に。ただし Filament のヘッダが designated
  initializer を使うため、MSVC では `Renderer.cpp`（Filament を含む唯一のTU）
  だけ `/std:c++20` でビルドする（`CMakeLists.txt` で設定済み）。他は C++17。
