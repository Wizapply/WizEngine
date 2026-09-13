# CLAUDE.md

Claude Code 向けのプロジェクト前提メモ。作業開始時にまず読むこと。

## 言語

- 応答・説明・コミットメッセージはすべて日本語で行う。
- コード内のコメントも日本語で書く。
- ファイル名・ディレクトリ名・識別子・APIは英語のまま（従来どおり）。
- **コンソールに出るログは英語**（LOG* マクロの本文と、そこへ流れる診断
  文字列 - XML のパースエラー・シーン文書の warnings・ファイル I/O の
  reason）。ブラウザに出す操作結果のステータス（`setStatus` の
  「保存しました」等）は日本語のまま。

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
  -> toFilament()  [MathBridge.h]  四元数/位置 -> mat4f
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
- **編集は必ずキュー経由**（`src/scene/EditorState.{h,cpp}`）。ブラウザ → INPUT スレッドが
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
  停止時は `removeAllJoints()`。種類は `PhysicsWorld::JointType` の 13 種
  （下の「Chrono の拘束・物性」の章）。基本 5 種は Chrono の
  `ChLinkLockLock` / `...Revolute` / `...Spherical` / `...Prismatic` /
  `ChLinkDistance`。**Revolute と Prismatic はリンク座標系の Z 軸が基準**なので、
  ユーザー指定のワールド軸を Z に合わせる四元数を作って渡している
  （直動モータ `ChLinkMotorLinear*` と PointLine だけは X 軸基準 =
  `quatFromXAxis`）。文書のジョイント → `JointSpec` の変換（度 → rad）は
  `SceneInternal.h` の `toJointSpec` 1 か所。
- **`objectsMutex_` は「一覧の構造」だけを守る**。書くのは物理スレッドだけなので
  そのスレッドは読むときにロック不要。他のスレッド（HTTP の階層 JSON、INPUT の
  選択、RENDER の反映）は `Scene::lockObjects()` を取る。`applyToRenderer()` は
  丸ごとロックを持つので、その中から呼ばれる `onRender` はロックを取らない
  （取ると自分自身で詰まる）。ロック順は **objects → editor → poses** の一方向。
- **保存/読込は `assets/scenes/*.xml`**（`Scene::document()` / `loadDocument()`）。
  中身は **MuJoCo(MJCF) 風の XML** で、下の「シーン文書（XML）」の章が唯一の
  定義。保存時にオブジェクト番号を詰め、ジョイント・イベントノードの参照も
  付け替える。名前は英数字と `_ -` だけに正規化（保存先を assets/scenes に
  固定するため）。旧 `*.json`（version 1〜3）は**読み込みだけ**できる。
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
  エディタカメラ自身は選べない）。文書では `<worldbody>` の `<light>` /
  `<camera>`（旧 JSON の version 2 で加わった節）。ライトを 1 つも書かない
  文書は読み込み時に初期構成へリセットする。
- **System タブで変えた値も保存に映る**。PhysicsControlComponent は
  rate / substeps / solver / envelope / recovery を PhysicsTuning に書くとき、
  同じ値を EditorState の SimSettings にもミラーする。シーン保存はそちらを
  書き出すので、怠ると「見ている物理」と「保存される物理」が食い違う。
- **映像エリアの上にヘッダー**（`#viewHead`）: サイドバーの表示/非表示（☰、
  Tab キーと同じ）・シーンタイトル（sceneFile、無題は "(無題のシーン)"）・
  カメラ名・エディタ⇄シミュレートのモードボタン。**モードボタンはここの
  1 組だけ**（サイドバー上部にあったものは廃止。以前の浮きボタン #sbOpen も
  このヘッダーに置き換えた）。**サイドバーを畳む口もここの ☰ だけ**
  （サイドバー内にあった ‹ ＝ #sbClose は廃止。畳んだ瞬間に一緒に消える
  ボタンなので、開く側は結局 ☰ が受け持っていた）。サイドバーのヘッダは
  「WizEngine / Version 1.0.0 (Charon)」の表記のみで、カメラ名は映像ヘッダーが
  受け持つ。
- ブラウザ側のタブは **Scene / Inspector / Physics**（`web/index.html` +
  `app.js` の `renderEditor()`。Inspector の内部 id は `tabEditor` /
  `paneEditor` のまま）。**カメラの入口は Scene タブの Cameras 一覧**で、
  **行クリックの意味はモードで変わる**（Unity のヒエラルキーと同じ感覚）:
  エディタモード×エディタカメラのページでは**クリック = エディタ選択**
  （ビューにギズモが出て移動 / 回転。もう一度で解除。数値・削除は選択で
  出る Inspector の「選択カメラ」節）、追加は見出しの ＋、ページ移動は
  ダブルクリック、映像の確認は行の目アイコン。それ以外では従来どおり
  クリック = ページ移動。**Inspector に常設のカメラ一覧は置かない** -
  Inspector は「選択しているものの内容」だけ、という整理（一覧と選択の
  二重表示になるため一度置いて廃止した）。行の目アイコン（👁 の絵文字は
  フォントによって出ないため **インライン SVG**、app.js の `EYE_SVG`）は
  そのカメラの映像を小窓で開く（`openCamPopup`）。**Chrome / Edge 116+ は
  Document Picture-in-Picture** - アドレスバーの無い常時前面の小窓に、
  このページが張った 2 本目の WHEP 接続の video だけを入れる（`openCamPip` /
  `pipConnect`。トークンは別で、視聴セッションはカメラごとに独立。PiP は
  ブラウザ全体で 1 窓なので保持も 1 台ぶん）。非対応ブラウザは従来の
  ポップアップ（`openCamWindow`、URL は `?popup=1` でサイドバー・アセット
  パネル・モード切替を CSS で隠す。**window.open のアドレスバーは仕様上
  消せない** - なりすまし対策で URL 表示が強制されるため、消したい環境では
  PiP 対応ブラウザを使う）。視聴はカメラごとに 1 ブラウザなので、既に誰かが
  見ているカメラは「視聴中」表示で再試行する。**シミュレートへ切り替えると
  窓は閉じる**（`closeCamPopups`、元のページを離れるときも）- 編集のための
  窓なので、走らせる段になったら残さない。Inspector には**常設の World 節**（`secWorld`）が
  あり、地面（床の広さ・見える広さ・テクスチャ・タイル・色）と環境光
  （HDR・強さ）を編集できる（`edit.ground` / `edit.environment`、部分更新で
  物理スレッドが `setGroundAndEnvironment` を呼ぶ。パスは `assetFileAllowed`
  の関所を通り、不正なら現状維持）。Inspector の選択オブジェクトは **Unity 風の
  Transform 行**（`.trRow`、位置 / 回転 / スケールに X/Y/Z の色タグ。id は
  edPX〜edSZ のまま、スケールは倍率ではなく実寸 m）。**ギズモのモード切替
  （✥/⟳/⤢ と World⇄Local）と設定の ⚙ は映像左上のツールバー**（`#gizmoBar`、
  Unity のシーンビュー左上相当）に重ねて出す: `#hit`(z=1) より上の z=2 に
  置いてクリックを受け、エディタモード×Editor Camera のページでだけ
  `renderEditor()` が表示する。**Inspector は「選択しているオブジェクトの
  内容」だけ**: Transform・質量・固定・色に加え、**付いているイベント
  アセットの一覧**（付ける / 外す。World 節にはシーン全体ぶん）と、ジョイント節も
  選択時のみ表示し、一覧は選択が関わるものに絞る（どのジョイントにも地面でない体が
  必ずあるので、どれかを選べば必ず一覧に届く）。ギズモの数値設定（スナップ・
  刻み・グリッド）は普段隠れた `secGizmo` 節で、ツールバー右端の ⚙
  （`gzSettings` → `toggleGizmoSettings()`）が開閉する。ブラウザ内の表示
  状態だけの話なのでサーバーには送らない。
  ビューの下には**アセットパネル**（`#assets`、
  `renderAssets()`）: プリミティブ（Box/球）と文書の `<asset>` にある
  メッシュはクリックで配置、**イベントアセット（⚡）はクリックで
  ノードエディタに開く**（「✨ 新しいイベント」で作成）、保存済み
  シーンはダブルクリックで読込（confirm 付き。読込は現在の配置を置き換える
  ため）。見出し下の操作列（`.asBar`）に、新規オブジェクトの初期値
  （edNewSize / edNewColor）とシーン名・💾保存・🗑全消し（旧 Inspector の
  配置・シーン節の移設先。読込ボタンと保存済み select は廃止＝タイルが
  受け持つ）。エディタモード中の Editor Camera ページにだけ出る。見た目と見出しはサイド
  バーの流儀（見出しクリックで折りたたみ）。高さは textarea と同じ右下の
  つまみ（CSS の `resize: vertical`。ドラッグ結果はブラウザが inline style に
  書くだけなので、ResizeObserver で拾って localStorage に記憶・復元する）。
  入りきらないタイルは縦スクロール（**ホイールが効くのはクリックしてアクティブに
  した間だけ**。載せただけの間は一覧を `overflow: hidden` にしてホイールを
  ブラウザにページへ流させる - 一覧の `overscroll-behavior: contain` が溢れて
  いなくてもホイールを食うため。contain はアクティブ中だけ付ける: hidden も
  スクロールコンテナなので、付けたままだと実機でホイールが止まる。JS の
  scrollBy で動かすとなめらかスクロールが効かないので CSS の切替だけで
  済ませる）。シミュレート設定（重力・摩擦・反発・減衰・スリープ）
  は Physics タブに置く。入力欄は 500ms ポーリングで上書きされるが、
  **フォーカス中の欄だけは触らない**（打っている途中の数字が消えるため）。

## シーン文書（MuJoCo 風の XML）

**シーンの中身は XML が正**。エディタが保存するのも、起動時に読むのも、
`/scene.xml` が返すのも同じ文書で、経路は 1 本しかない:

```
Scene（Chrono / Filament の実体） <-> SceneDocument <-> XML テキスト
```

- **書式は MJCF（MuJoCo）に寄せてある**（`src/document/SceneDocument.h` の先頭に全体像）。

  ```xml
  <wizengine model="sample_joints" version="4">
    <option gravity="0 -9.81 0" rate="60" substeps="2" iterations="60" .../>
    <visual>
      <quality shadowMap="2048" cascades="3" shadow="pcss" msaa="4" .../>
      <postprocess ssao="true" bloom="0.08" ssr="true" vignette="0.25" .../>
      <exposure aperture="16" shutter="125" sensitivity="100"/>
      <grading tonemap="aces" contrast="1.05" saturation="1.02" .../>
    </visual>
    <asset>
      <mesh name="apple" file="apple2.glb" scale="1"/>
      <event name="pickup">
        <node id="1" type="onGrab" pos="40 40" target="-1"/>
        <node id="2" type="grabPull" pos="300 40" target="-1" value="1"/>
        <wire from="1" to="2"/>
      </event>
    </asset>
    <worldbody>
      <environment hdr="studio.hdr" intensity="30000" skybox="true"/>
      <ground size="10" visual="8" texture="textures/ground.png" tile="2"
              roughness="0.9"/>
      <light name="key" type="spot" pos="1.5 4 -2" euler="35 -20 0" .../>
      <camera name="cam0" target="0 1 0" azimuth="37.8" elevation="19.5" radius="12"/>
      <body name="post" pos="0 1 0" euler="0 0 0" fixed="true">
        <geom type="box" size="0.1 1 0.1" mass="20" rgba="0.42 0.45 0.5 1"
              roughness="0.3" metallic="1"/>
      </body>
      <body name="a1" pos="1 2 0">
        <geom type="mesh" mesh="apple" size="0.1" mass="0.2"/>
        <event name="blink"/>
      </body>
    </worldbody>
    <equality>
      <joint name="hinge" type="hinge" body1="arm" body2="post" anchor="0 1.9 0" axis="0 0 1"/>
    </equality>
    <events>
      <event name="pickup"/>
    </events>
  </wizengine>
  ```

- **MuJoCo に合わせた点**: `<geom size>` は**半分の寸法**（box は各辺の半分、
  sphere / mesh は半径。`BodyDesc` は辺の長さで持つのでここで 1/2 する）、色は
  `rgba="r g b a"`（リニア値）、角度は全部**度**、`body1` / `body2` は名前でも
  番号でも書けて `world` と -1 が地面。`<option timestep>` で書かれていたら
  Hz に直して読む。glTF モデルは MJCF と同じく **`<asset>` の `<mesh>` に
  宣言して `<geom type="mesh" mesh="名前">` で参照**する。`file` は assets/
  からの**相対パスのみ**（`..` と絶対パスは警告して弾く）。`scale` はモデル
  単位 → m の素の倍率で**見た目だけ**を決め、当たり判定は geom の
  size / collision（既定はモデルの凸包、読めなければ球）。宣言の無い名前は
  警告して球で描く。**凸包の質量は `mass` そのもの**（`PhysicsWorld::
  addConvexHull` は密度ではなく質量を受ける。凸包の体積はモデルの大きさ
  次第で size とは無関係なので、密度から出すと 1 m のモデルに size 0.05 を
  書いた「りんご」が 100 kg 超になり、触れた物を弾き飛ばした）。Chrono は
  凸包を**体積重心へ寄せる**（ボディの原点 = 重心）ので、寄せた量を
  `GameObject::hullCenter` に持ち、RENDER スレッドが glTF の原点を同じだけ
  逆にずらして重ねる（原点が底にあるモデルが当たり判定から浮かない）。**地面と環境光も文書が持つ**: worldbody 直下の単一要素
  `<ground size visual texture tile rgba>`（size = 物理の床の半寸法、visual =
  見える地面の半寸法。texture は assets/ 相対、空 = 市松模様）と
  `<environment hdr intensity>`（hdr は assets/ 相対、空 = 環境マップ無し）。
  節を書かない文書は既定値（`GroundDesc` / `EnvironmentDesc`）で開く。
  **違う点**はイベント（`<asset>` の `<event>` = 中身、`<body>` の
  `<event name/>` とルートの `<events>` = 付け先）と `<ground>` /
  `<environment>` が WizEngine の拡張であることと、`<worldbody>` の直下しか
  見ないこと（MJCF の入れ子 body は親からの相対姿勢なので、姿勢を合成せずに
  平らに落とすと物が別の場所に出る）。
- **XML の実装は自前**（`src/document/SceneXml.{h,cpp}`、依存なし）。要素・属性・入れ子と
  コメント・実体参照だけの部分集合で、テキストノードは持たない（値は全部属性）。
  属性は書いた順に出て、長い要素は要素名の下へ揃えて折り返す＝保存ファイルの
  差分が読める。**読み取りは失敗しない**（欠けた属性・型違いは既定値）。壊れた
  XML だけが行番号付きのエラーになる。ただし「黙って別の意味になる」内容
  （未知の節・種類名の打ち間違い・見つからない body 参照・入れ子 body・
  零ベクトルの軸・不正なワイヤーなど）は **warnings に文で集まる**
  （`fromXml` / `parseXml` の第 4 引数）。シーン読込はこれを LOGW に全件、
  ステータスに件数で出す。ノード id は省略可（読み込みが空き番号を採番）。
- **節・属性の増やし方は `SceneDocument.h` 冒頭の「拡張の手順」が正**。要点:
  読み込みは必ず既定値付きで書く（＝属性を足しても古い文書はそのまま読める、
  新しい文書を古い版が読んでも壊れない）。`kSceneDocVersion` を上げるのは
  **読めなくなる変更をしたときだけ**。節を足したら fromXml の「知っている節の
  一覧」（未知の節を警告する箇所）にも名前を足す。
- **値の型は EditorTypes.h のまま**。`SceneDocument`（`src/document/SceneDocument.{h,cpp}`）は
  その入れ物で、XML と 1 対 1。「節が無い」と「空の節」を区別するために
  `hasSim` / `hasLights` / `hasCameras` / `hasEvents` を持つ（ライトを書かない
  文書は初期構成の 2 灯で開く＝手書きの最小 XML が真っ暗にならない。イベントを
  書かない文書は既定の pickup で開く＝掴んでも動かないシーンにならない）。
- **旧 JSON（version 1〜3）は読み込みだけ**（`fromLegacyJson`）。同じ名前の
  `.xml` が無いときだけ `.json` を探す。保存は常に `.xml` なので、一度保存すれば
  そのシーンは XML に移る（`.json` は上書きしない）。一覧（`sceneFiles()`）は
  拡張子を落とした名前で、新旧が両方あってもタイルは 1 個。保存名の正規化は
  `EditorState::sanitizeSceneName` の 1 か所（ファイル名・文書の model・
  sceneFile 表示が同じ文字列になる - 別々に正規化するとタイルの選択表示が
  外れる）。
- **`/scene.xml` でいまの中身が読める**（保存しなくてよい）。`Scene::document()`
  はオブジェクト一覧のロックを自分で取るので、物理スレッド（保存）と HTTP
  スレッド（この口）の両方から呼べる。ロック順は他と同じ **objects → editor**。
- **ブラウザから XML を直接編集して適用できる**。アセットパネルの「📄 XML」が
  モーダルエディタ（`#xmlEd`、ノードエディタと同じ .ndBack/.ndWin の流儀）を
  開き、「✓ 適用」が `edit.xml`（`{text}`）として送る。**検証は INPUT
  スレッド**（EditorComponent が parseXml。壊れた XML はキューに積まず理由を
  ステータスへ）、**適用は物理スレッド**（テキストをそのまま Op で運び、
  もう一度パースして loadDocument。警告は読込と同じくログ + 件数）。上限
  1MB。適用はファイルに書かない（保存は従来どおり 💾）。エディタカメラの
  ページ限定（他の edit.* と同じガード）。
- **起動時に読むシーンは `SceneConfig.h` の `kStartupScene`**（既定
  "default" = 同梱の `assets/scenes/default.xml`）。シーンの中身（配置・
  モデル・ジョイント・イベント）はコードではなく文書が持つ - 以前
  Scene.cpp にあった**格子の自動生成は廃止**した。読めなければ警告を出して
  空のシーン（地面のみ）で起動する（止めない）。空文字列 = 常に空で起動。

## フォトリアル描画（`<visual>` と材質）

**Filament は物理ベース（PBR）のレンダラなので、写実的な絵は出せる。**
出るかどうかを決めるのは engine ではなく、渡す 3 つ: **材質**（表面が光を
どう返すか）・**光**（実単位の強さと環境マップ）・**フィルム**（露出と
トーンマップ）。以前はこの 3 つが全部固定値だった（roughness 0.75 の誘電体・
背景は無地・後処理なし）ので、絵が「CG っぽい」のは当然だった。いまはどれも
**シーン文書の値**になっている。詳しい答えと限界は `docs/photorealism.md`。

```
シーン文書 <visual> / <geom> の材質
  -> editor::RenderDesc / MaterialDesc      （document/EditorTypes.h）
  -> wizengine::RenderSettings / ShapeMaterial（scene/RenderBridge.h が変換）
  -> Filament の View / Camera / ColorGrading / LightManager / MaterialInstance
```

- **`<visual>` は「どう撮るか」**（MuJoCo の `<visual>` と同じ置き場、中身は
  Filament の語彙）。小節は 4 つ: `<quality>`（影の種類 pcf / dpcf / pcss /
  vsm・解像度・カスケード・接地影・msaa・taa・fxaa）、`<postprocess>`
  （ssao・bloom・ssr・dof・vignette、enabled=false で後処理ごと切る）、
  `<exposure>`（aperture / shutter / sensitivity = 写真と同じ 3 つ）、
  `<grading>`（tonemap・contrast・saturation・temperature・tint）。
  **節を書かない文書は既定値 = 従来の絵**（後処理は全部オフ、露出も
  Filament の既定と同じ f/16・1/125・ISO100、トーンマップは ACES legacy）。
- **プリセットの定義は C++ に 1 か所**（`editor::renderPreset` の draft /
  standard / photo）。ブラウザの Inspector「描画」のボタンは名前だけを送り
  （`edit.render` の `preset`）、続けて個別の値を上書きできる（部分更新）。
  UI 側に数値を持たせない = 画面とサーバーの食い違いが起きない。
- **材質は `<geom>` / `<part>` の属性**（`rgba` の隣）: `roughness`
  `metallic` `reflectance` `clearcoat` `clearcoatRoughness` `emissive`。
  **既定値と同じ値は保存時に書かない** - 全部書くと、材質を触っていない
  シーンのファイルまで 6 属性ぶん太って差分が読めなくなる。読む側は必ず
  既定値付き（＝古い文書はそのまま従来の見た目で開く）。
- **色と材質は別の口**（`setShapeColor` / `setShapeMaterial`）。イベントの
  SetColor や掴んだときのハイライトは**色だけ**を差し替えるので、材質が
  巻き込まれない。スロットごとのマテリアルインスタンスは色と材質の両方を
  覚えていて（`ShapeSlot::color` / `material`）、作り直すときに入れ直す
  （`.mat` にパラメータの既定値は書けないので、入れ忘れ = 真っ黒な鏡）。
- **露出は実単位で効く**。ライトが lux（平行光）/ lumen（点・スポット）
  なので、F 値を半分にすれば 2 段明るくなる。「暗いから intensity を上げる」
  ではなく「露出を開ける」で合わせられる＝写真と同じ勘が使える。
- **`<environment skybox="true">` で環境マップを背景にも出す**。背景と
  映り込みが同じキューブマップになる（プリフィルタ済みの mip 0 を使う）＝
  一致するので、金属や光沢のある物が「そこにある」ように見える。写真らしさが
  いちばん安く上がるスイッチ。HDR を読み直さずに切り替えられる
  （`Renderer::setSkyboxEnabled`）。
- **地面も材質を持つ**（`<ground roughness metallic>`）。既定は従来どおりの
  艶消し（0.9）。0.2 くらいにすると濡れた路面のようになり、SSR と組み合わせ
  ると物が映り込む。
- **適用は RENDER スレッド**。設計値を持つのは Scene（`render_` + dirty、
  地面・環境光とまったく同じ流儀）で、Filament を触るのは
  `applyToRenderer` の末尾、**オブジェクト一覧のロックを外してから**
  （トーンマップの LUT を焼くことがあるため）。
- **ビューは遅延生成される**（カメラの追加・初訪問）ので、`addView` も
  同じ設定を当てる - でないとページを開いた順で絵が変わる。
- **色作り（ColorGrading）は LUT を焼く**ので、関係する 5 つの値が変わった
  ときだけ作り直す。差し替えは「全ビューへ新しい方を渡してから古い方を
  壊す」順（使用中の LUT を消さない）。
- **影の設定はライト側**（Filament の `LightManager::ShadowOptions`）。
  `addLight` の生成時に入れるほか、`setRenderSettings` が
  `setShadowOptions` で既にあるライトへも流し込む＝シーンを読み直さずに
  影の解像度や種類を変えられる。カスケードは平行光だけの概念なので、点・
  スポットには 1 を渡す。
- **重さの目安**（1080p・GPU 依存）: MSAA 4x > PCSS 影 > SSR > SSAO >
  ブルーム > FXAA。TAA は安いが**速く動く剛体に残像**が出るので Photo
  プリセットでも既定オフ（静止した絵を撮るときだけ on）。後処理を全部
  切りたければ `<postprocess enabled="false">`。
- **テストは `tests/document/`**（Chrono / Filament 不要。`cmake -S
  tests/document -B build-document-test && cmake --build build-document-test
  && ./build-document-test/document_test`）: `<visual>` と材質の XML 往復、
  既定値どおりに開くこと、打ち間違いが警告になること、ブラウザ API の
  部分更新とクランプ、同梱シーンが警告 0 件で読めること。**文書に節や属性を
  足したらここにも 1 行足す**（車両の `vehicle_test` と同じ扱い）。
- **見本シーンは `assets/scenes/photoreal.xml`**（金属の粗さ違い・誘電体の
  粗さ違い・クリアコートの塗装・自己発光 + Photo プリセット）。環境マップ
  （`assets/studio.hdr`）はリポジトリに入っていないので、polyhaven などから
  2k の .hdr を 1 つ置くと金属が「何かを映す」ようになる（無くても起動する）。
- **未**: 材質テクスチャ（baseColor / 法線 / ラフネスマップ。いまは 1 物体
  1 色）、ガラス（屈折・透過）、面光源、スクリーン空間を超える GI（Filament
  はパストレーサではない）。glTF モデルは gltfio が持つ自前のマテリアルで
  描かれるので、`<geom>` の材質属性は**組み込みメッシュ（箱・球・円柱・
  ソフトボディ）とプレハブの部品にだけ**効く（モデル側の材質はファイルの
  ぶんが使われる）。

## 車両（`src/vehicle/` + `src/components/VehicleComponent.{h,cpp}`）

NWH Vehicle Physics 2 の構成に倣った**グラフ型パワートレイン + 車輪ごとの
WheelController**。`<body>` に `<vehicle>` 節を書いたオブジェクトが車体になり
（`BodyDesc::hasVehicle` / `vehicle`）、シミュレート中に VehicleComponent が
サス力・タイヤ力・空気抵抗を Chrono の車体へ「点に掛かる力」として積む
（`PhysicsWorld::applyForceAtPoint`）。サンプルは `assets/scenes/vehicle.xml`。

- **`src/vehicle/` は Chrono も Filament も知らない純粋な数値ライブラリ**
  （自前の `Vec3` / `Quat`）。境界の VehicleComponent だけが詰め替える。
  だから `tests/vehicle/`（小さな 6 自由度の剛体積分器つき）で単体で回せる:
  `cmake -S tests/vehicle -B build-vehicle-test && cmake --build build-vehicle-test
  && ./build-vehicle-test/vehicle_test`（静置・0-100 km/h・制動・定常旋回・
  後退・XML の往復を検査。`--csv` で時系列）。**モデルを触ったら必ずこれを回す**。
- **車輪は剛体にしない（レイキャスト式）**。車体の取り付け点から下へレイを
  飛ばして縮み量を決め、ばね・ダンパの力を取り付け点へ、タイヤの縦横力を
  接地点へ掛ける（`WheelController`）。**レイは車輪の面内の扇**（`<tire
  rays>`、既定 9 本、±60°）: 各当たり点に「半径 r の円が触れる」ときの
  中心の高さ c = L cosθ − √(r² − (L sinθ)²) を縮みに直し、一番高い円を採る
  ので、段差の縁に円として乗り上げる（真下 1 本だと中心が縁を越えた瞬間に
  段の高さぶん縮みが飛んで車が跳ねる。`rays="1"` で旧来の 1 本）。法線は
  当たり点 → 円の中心（縁では斜め = 乗り越える向きにタイヤ力が働く）。
  サス力は静止荷重の 8 倍で頭打ち（1 ステップの衝撃にしない安全弁）。
  VehicleComponent は車体（箱）が何かに触れていたら 1 秒に 1 回 LOGI
  （"chassis is in contact with ..."）- 車輪はレイで接触しないので、出たら
  車体が段や壁に当たっている、の切り分け用。地面は物理の床と同じ **y = 0 の平面**
  として見るのに加え、**シーンの剛体（箱・球。ソフトボディと自分自身は
  除く）にも当てる**（VehicleComponent が `GroundQuery` を組む。当たり判定は
  `VehicleMath.h` の `rayHitsOrientedBox` / `rayHitsSphereSurface`、レイの
  始点が箱の中なら無視 = 車体と重なった箱を地面と取り違えない）ので、
  階段や坂の上を走れる。レイキャストなので乗った物に力は返さない（固定の
  段差向き）。**プレハブの collide 部品にも当てる**（下の「プレハブ」の章。
  物理の複合形状と同じ形なので、レイと接触で段の位置が食い違わない。
  一覧は `prefabVersion` が変わったときだけコピー）。**階段は専用の
  オブジェクトではなくプレハブ**: アセットパネルの **🪜 Stairs**（`edit.add`
  の `shape="stairs"`）は、プレハブ `stairs`（`builtinStairsPrefab` = 全段を
  `collide="true"` の箱の部品として並べたもの。原点は**全体が収まる箱の
  中心**、各段は床から段の高さまでの中実の箱）が無ければ `prefab.add`
  （`parts` 付き）で作り、それを付けた **geom の無い固定ボディ**
  （`ShapeKind::None`、下記）を `add` する（`BodyDesc::prefab` を JSON でも
  受けるようにした）。置いたあとは「プレハブを編集」で段を動かせる。
- **車輪と車体の飾りの描画はプレハブ**（下の「プレハブ」の章）。
  VehicleComponent は車輪の**車体ローカル**の姿勢（`VehicleModel::
  wheelLocalPoses`: 取り付け点・下がり・舵・回転角）を `visMutex_` 越しに
  渡す（`wheelLocalPoses()`）だけで、PrefabComponent が `Scene::latestPose`
  の車体姿勢に重ねる - 物理スナップショットと同じ姿勢から組むので車体と
  ずれない。エディタ中は設計値から静的な沈み込みを見積もった位置
  （`designWheelPoses`）。タイヤは Renderer の `ShapeMesh::Cylinder`
  （軸 X の単位円柱、車両のために追加）。`<tire width>` は見た目だけ。
- **時計が 2 つ**（NWH と同じ）。接地とサス（`updateContact`）は物理 1 ステップに
  1 回、パワートレインとタイヤ力の更新は `<vehicle ticks>`（既定 10）回の
  反復。エンジン・クラッチの慣性は小さくトルクは大きい硬い系なので、
  物理 60Hz で 1 回積分すると暴れる。`ticks` は SimSettings の substeps とは
  別（前者はパワートレインの硬さ、後者は接触の精度）。
- **クラッチとデフのロックは陰解法**（`Powertrain::tick` / `lockTorque`）:
  2 質点の相対速度に対する粘性要素 `T = k Δω / (1 + k dt (1/J1 + 1/J2))` を
  容量でクランプする。どんな剛性でも安定なので、Locked デフは k = 1e6 で
  「剛結」にできる。角速度は車輪 → エンジンへ戻し（駆動輪の平均 × 総減速比）、
  トルクはエンジン → 車輪へ流す（センターデフは bias、軸デフは半分ずつ +
  ロック）。車輪の ω を積分するのは VehicleModel（タイヤの反作用を持つ側）。
- **タイヤは Pacejka の簡易 Magic Formula**（B, C, E。D = μ Fz）＋摩擦楕円＋
  緩和長（力の一次遅れ）。停車付近の振動は「滑りの向きに逆らわず、この
  ティックで滑り速度を反転させない」範囲（`fxLo..fxHi` / `fyLo..fyHi`）で
  消す。範囲は緩和長の前に掛ける（後に掛けると状態に逆向きの力が溜まり、
  停車後に押し戻す）。ブレーキと転がり抵抗は
  クーロン的に「止められるなら止める」クランプ（符号反転でばたつかせない）。
- **滑りにくくする 3 点セット**: タイヤ既定は μ = 1.2・B = 12（ゲーム寄りに
  高め）、**速度感応ステアリング**（`steerSpeed`、その速度で最大舵角が半分。
  キーボードのフル舵で高速に前輪が飽和しないため）、**トラクション
  コントロール**（`tcs`、駆動輪のスリップ比がこれを超えたらスロットルを絞る。
  絞りは速く、戻しはゆっくり）。どれも `<vehicle>` の属性で、0 で無効。
- **自動変速は駆動系の回転数で判断**（クラッチが滑っている間のエンジン回転は
  当てにならない）。変速中はトルクを切り、クラッチも切る。停車が 0.3 s
  続いた状態で S を踏み続けるとリバースに入り、その間は W / S の役割が
  入れ替わる（キーボードで自然に運転できるように）。
- **入力は `drive` コマンド**（`{throttle, brake, steer, handbrake}`、どのカメラ
  からでも可 - 運転はシーンの書き換えではない）。ブラウザは W / S / A / D /
  Space を 50ms ごとに送る（シミュレート中のみ。エディタ中は W / E / R が
  ギズモ）。複数の車両があれば全部が同じ入力で走る。計測値は `/stats` の
  `vehicle`（速度・rpm・ギア・クラッチ）でオーバーレイの「car」欄に出る。
- **文書の `<vehicle>`**（`vehicle/VehicleXml.{h,cpp}`）: `<engine>` `<clutch>`
  `<gearbox>` `<center>` と `<axle>`（左右 2 輪。`<tire>` / `<suspension>` は
  軸ごと、`<vehicle>` 直下に書けば全軸の既定値）。座標は**前 = -Z、右 = +X**
  で、axle の z は「前向きの距離」。読み込みは既定値付き、駆動軸が 1 本も無い・
  未知の節は warnings。値の範囲は `clampVehicle`。Inspector の「車両」節で
  できるのは「車両にする / やめる」とタイヤ式の付け外しだけで、サス・
  エンジン・ギアの数値は 📄 XML で編集する。
- **実行状態はエディタに戻ると捨てる**（`onEditorStep`）。次のシミュレート
  開始で設計値から作り直すので、XML を適用すればそのまま効く。
- **ソフトタイヤ**（`src/vehicle/SoftTire.{h,cpp}`、`<tire soft="true"
  stiffness damping segments rows iterations>`）: 車輪は剛体ではない
  （レイキャスト）ので、シーンのソフトボディ（Chrono の粒子）は車輪に
  使えない。代わりに **タイヤの見た目（円柱の部品）を質点ばねの変形
  メッシュにする**: ハブ（半径 35%、皿状にへこむ）・リムのフランジ
  （60%）・中心のキャップは運動学的（車輪の姿勢そのもの）で、トレッドの
  粒子は空気圧（**ハブへの**径方向。フランジへ張ると内側へ押し込まれた
  とき鏡像の位置で釣り合ってへこんだままになる = 双安定）・周方向・幅方向・
  対角線・曲げのばねで結ばれ、接地点の平面に押し戻される（摩擦なし =
  トレッドは地面の上を滑る。潰れだけが要る）。**リムは硬い境界**（トレッドは
  フランジの半径より内側へ入れない）で、物理側の潰れの上限も半径の 25%
  （接地面がリムより内側に来ない）。だから離せば必ず丸に戻る。
  境界と接地面が食い違ったら接地面が勝つ（順序: リム → 地面）。
  **空気圧**（`<tire pressure="320000">`、基準の絶対圧 Pa、0 で切る）: 閉じた
  メッシュが囲む体積 V を毎ステップ発散定理で出し、静止体積 V0 との比から
  圧力の増分 p = p0 (V0/V − 1)（等温変化、下限 −p0）を求める。これを
  **粒子ごとの径方向の陰解法ばね**として同じガウス・ザイデル反復に入れる:
  剛性 k_i = p_abs A_tot / V × |A_i|（一様に膨らむモードの線形化、A_i は
  粒子の面積ベクトル = 隣接三角形の面積 × 法線 / 3 の和）、自然長 = 静止
  半径 + p V / (p_abs A_tot)（圧力ぶんの膨らみ）。力として陽に足すと
  結合剛性の分母で釣り合いが縮んで膨らみがほぼ消えるので、この形にした。
  潰すほど急に硬くなり、離せば張り戻る（ゴムの復元）。**この「体積比 →
  圧力」はノード式で差し替えられる**（`<tire pressureFormula="名前">`、
  入出力の約束は `TireFormula.h` の `pressureFormulaInputs / Outputs`:
  ratio / rate / p0 / load / deflection / radius → pressure。既定グラフは
  `defaultPressureFormulaGraph()`、`vehicle_test --dump-pressure-formula` で
  XML が出る。`edit.formula.add` の `template="pressure"`、🧮 タイルの右
  クリック「空気圧式に付ける」、Inspector の軸ごとの「空気圧 / 空気圧式」。
  失敗したステップは組み込みで代用して `formulaFailures()` に数える）。**メッシュは閉じた面**（帯 + 両側の扇）にして
  ある - 穴が開いていると向こう側の内面が背面カリングで透けて見える。
  ソフト形状のマテリアルは両面描画（`setCullingMode(NONE)`）で、フラスタム
  カリングも切ってある（`addSoftShape`）。ばねは PhysicsWorld の
  ソフトボディと同じ陰解法（1 本ずつ implicit Euler をガウス・ザイデル）
  なので発散せず、静止位置から半径の 50% を超えるずれは引き戻す（安全弁）。
  **メッシュは車体に力を返さない**（見た目）。物理への影響は
  WheelController の**径方向ばね**だけ: サスと直列に縮み（同じ力を分け
  合うので縮みは剛性の逆比、`deflection()`。上限は半径の 25%）、接地点は
  車輪中心から `radius − deflection` の距離になる = 車体がそのぶん低く座る。
  VehicleModel が step の末尾で各輪の `SoftTire::step`（中心・回転 =
  `wheelRotation`・接地面）を回し、`WheelLocalPose::softMesh`（粒子の
  ワールド位置、shared_ptr）と `deflection` で描画側へ渡す。**描画は
  PrefabComponent**: 円柱 + 車輪ソケットの部品は、その軸の `<tire soft>`
  なら `Renderer::addSoftShape` の変形メッシュになり（部品の寸法・位置は
  使わず、タイヤの半径・幅・分割と車輪の姿勢で描く。色は部品の色）、毎
  フレーム `softlattice::buildSurfaceMesh`（任意の位相版）で頂点と法線を
  組む。走らせていないとき（softMesh が null）は `SoftTire::restParticles`
  の静止形状を車輪の姿勢に置く。Inspector の「車両」節に軸ごとの
  「ソフトタイヤ」チェックと「タイヤ剛性」があり、`edit.vehicle.tire` の
  `soft` / `stiffness` キー（formula と同じく送られたキーだけ変える）。
  サンプルの `assets/scenes/vehicle.xml` は両軸とも soft。テストは
  `vehicle_test` の 8（接地で底が地面に揃う・浮くと丸に戻る・直列ばねで
  潰れのぶん低く座る・XML 往復）。

### ノード式（`src/vehicle/Formula.{h,cpp}` + LuaJIT）

研究者が計算式をノードで組み替えるための仕組み。いま差し替えられるのは
**タイヤの「スリップ → 力」**（`TireFormula.h` に入出力の約束）。ユーザーは
Lua を見ない: ノード → Lua ソース → LuaJIT、はエンジンの中で閉じている。

- **型は 3 つ**: `FormulaGraphDesc`（文書の値 = ノード + ワイヤー）、
  `FormulaProgram`（検証・整列済みの線形命令列。入出力は名前で結び、lag /
  integrate に状態スロットを割る）、`FormulaInstance`（車輪 1 本ぶんの実行口）。
  実行口は **C++ の参照インタプリタ**（`FormulaProgram::interpret`）と
  **LuaJIT**（`LuaFormula.h`）の 2 つで、同じ命令列から作るので結果は一致する
  （`vehicle_test` の 7 が 3 実装の差 0 を検査）。LuaJIT の無いビルド
  （`WIZ_HAVE_LUAJIT` 未定義）はインタプリタに落ちる（遅いが同じ結果）。
- **語彙は全部スカラー**（`Formula.h` の冒頭に一覧）。in / out / const / dt、
  四則と min max pow atan2 gt lt、単項の neg abs sqrt sin cos tan atan exp log
  sign、clamp / lerp / select、折れ線 curve、状態を持つ lag / integrate、
  車両向けの magic（Pacejka）と ellipse（摩擦楕円、出力 2）。未接続の入力は
  0（警告）、循環はエラー（式は動かず組み込みで代用）。**陰解法の部品
  （クラッチ・デフ）はノードに分解しない** - 不安定になるので C++ に残す。
- **Lua の作法**（`LuaFormula.cpp`）: Lua ステートは物理スレッドに 1 つ
  （VehicleComponent が持つ）。1 グラフ = 1 関数 `function(I, O, S, dt)` で、
  ノードは関数のローカル変数になる = トレース JIT が境界を消す。入出力と
  状態は Lua 側で確保した FFI の double 配列で、C++ はそのアドレス（intptr →
  double で運ぶ）に直接読み書きする。**ティック内でテーブルも文字列も作ら
  ない**（GC を起こさない）。呼び出しは `lua_rawgeti` × 4 + `lua_pcall`。
  LuaJIT は `third_parties/luajit`（v2.1、サブモジュール）。`cmake/LuaJIT.cmake`
  がライブラリを探し、無ければ configure 時にビルドする（Linux は
  `make BUILDMODE=static`、Windows は `msvcbuild.bat static` = VS の開発者環境
  が要る）。`-DWIZ_WITH_LUAJIT=OFF` で外せる。
- **計算式はシーンのアセット**（イベントアセットと同じ扱い）。文書では
  `<asset>` の `<formula name>`（`FormulaXml.h`。`<node id type name params pos>`
  と `<wire from fromPort to port>`）で、`<tire formula="名前">` が名前で参照
  する。`<vehicle>` の中に書いた `<formula>`（旧置き場）も読めるが、読込時に
  シーンのアセットへ移す（次の保存で `<asset>` に出る）。本体は
  `EditorState::formulaAssets()`（`formulaVersion()` は式かタイヤの参照が
  変わるたびに進み、VehicleComponent がこれを見て**車両モデルを作り直す** =
  シミュレート中の編集もすぐ効く。走行状態は一度リセットされる）。
  既定の Magic Formula をノードで組んだものが
  `defaultTireFormulaGraph()`（`vehicle_test --dump-formula` で XML が出る。
  `assets/scenes/vehicle.xml` に貼ってある。`edit.formula.add` の
  `template="tire"` で同じものが作れる）。
- **UI**: アセットパネルの **🧮 タイル**（クリックでノードエディタ、右クリックで
  作成 / 削除 / 選択中の車両の全軸に付ける）、**ノードエディタは ⚡ と共用**
  （見出しの選択が「⚡ イベント」「🧮 計算式」の 2 グループ、`ndKind` で
  切替。計算式ノードは左に入力ポート・右に出力ポートが**行ごと**に並び、
  座標は app.js の `FPORT_Y0` / `FPORT_DY` と CSS の `.ndPortRow` の高さで
  揃える。同じ入力に繋ぐと張り替え、循環はサーバーが弾く）、**Inspector の
  「車両」節**（「車両として扱う」チェック = `edit.vehicle.enable`、軸ごとの
  タイヤ式の選択 = `edit.vehicle.tire`）。コマンドは edit.formula.add /
  remove、edit.fnode.add / set / remove、edit.fwire.add / remove で、
  すべて EditorComponent → Op → 物理スレッド（イベントと同じ配管）。入力 `fxLo fxHi fyLo fyHi`（力の
  許される範囲: 滑りの向きに逆らわず、滑り速度を反転させない大きさまで）は
  緩和長の**前**にクランプする - 組み込みも同じ順序で、状態が範囲を超えて
  溜まらない（順序を変える・符号を見ないと、制動で溜まった逆向きの力が
  停車後に車を押し戻す）。
- **失敗は組み込みで代用**: 式が NaN や実行時エラーを出したティックは
  組み込みのタイヤで計算し、`formulaFailures()` に数える。VehicleComponent が
  1 回だけ LOGW。安全のため出力にももう一度 ±cap を掛ける。
- **未**: タイヤ以外（サス・エンジン曲線）の差し替え口、vec3 型、ノードの
  値のライブ表示（デバッグバッジ）。

## Chrono の拘束・物性（`PhysicsWorld::JointSpec` / `BodyOptions`）

Chrono が持っていて WizEngine が使っていなかった機能のうち、既存の
`PhysicsWorld` の口を広げるだけで入るもの（「A」の一群）。文書の定義は
`SceneDocument.h` 冒頭、値の型は `EditorTypes.h`、Chrono への変換は
`SceneInternal.h`（`toJointSpec` / `toBodyOptions`）に 1 か所ずつ。見本は
`assets/scenes/mechanisms.xml`。

- **ジョイント 13 種**（`JointKind` / `JointType`）。基本 5 種に加えて
  Universal（`ChLinkUniversal`、axis = シャフト）、Cylindrical、Planar
  （axis = 面の法線）、PointLine（axis = 線）、PointPlane、Gear
  （`ChLinkLockGear`、`ratio` と 2 本目のシャフト `anchor2` / `axis2`。
  シャフト座標系はボディローカルへ手計算で直す `worldToBodyLocal`）、Screw
  （`ChLinkLockScrew`、`pitch` m/回転）、Spring（`ChLinkTSDA`、拘束ではなく
  力。地面側の取り付け点はアンカー、ボディ側は中心）。文書の type 名は
  `weld / hinge / ball / slide / distance / universal / cylindrical / planar /
  pointline / pointplane / gear / screw / spring`。歯車とねじはヘッダを
  `__has_include` で見て、無い版では「作らない」で済ませる（Distance と同じ）。
- **可動範囲 `range="lo hi"`**（hinge / cylindrical は度、slide は m。
  `ChLinkLock::LimitRz()` / `LimitZ()`）。**モータ `motor="speed|position|
  force" target=".."`**（hinge / slide のみ。`ChLinkMotorRotationSpeed /
  Angle / Torque`、`ChLinkMotorLinearSpeed / Position / Force`。拘束ごと
  置き換わるので range と併用不可 = 読み込みが警告して range を落とす。
  目標値は `ChFunctionConst` で持ち、`setJointMotorTarget` が実行中に
  書き換える = イベントの `setMotor`）。**ばね `stiffness damping`**
  （spring 種類は 2 点間、hinge は `ChLinkRSDA` を拘束に添える、slide は
  中心間の TSDA）。**破断 `breakforce`**（`step()` の末尾 `checkJointBreaks`
  が `GetReaction2()` の力の大きさを見て `RemoveLink`。外れた番号は
  `takeBrokenJoints()` → Scene が文書の番号に直して `onJointBreak`
  トリガーへ。線も消える）。
- **反力の計測**（`jointReaction`。ばねはばね力）。Scene が毎ステップ
  `jointStats_`（poseMutex_ の下）へ写し、`/scene` の joints[] に `force /
  torque / broken`、UI の一覧に出る。モータの出力トルクもこれ。
- **ボディごとの接触物性 `<geom friction restitution rolling cohesion>`**
  （`SurfaceDesc`、負 = シーン設定に従う = 書かない）。指定のあるボディだけ
  専用の `ChContactMaterialNSC` を持ち（`materialFor`）、シーン設定の変更は
  「シーン任せ」の欄に追従する（`setSurfaceMaterial` が再解決）。共有材質の
  ボディを実行中に専用へ変えるのは形状に焼き込まれていて無理なので、
  `setBodySurface` が false を返し Scene が `physDirty` で作り直す。
  **合成方式 `<option combine="min|average|max">`**（`CombineStrategy`。
  Chrono の `ChContactMaterialCompositionStrategy` の仮想関数を上書き。
  `override` は付けない - 版によって無い関数があるため）。
- **衝突レイヤ `<geom layer="0..7" nocollide="1 3">`**（Chrono の衝突
  ファミリ。剛体が 0〜7、**ソフトボディの粒子は 8〜15**（以前の 1〜14 から
  移した）。Bullet は片側が拒めば当たらないので片方に書けば足りる。変更は
  衝突モデルの作り直し = `physDirty`）。当たらない相手とは接触も生まれない
  ので衝突トリガーも鳴らない。
- **接触の力と法線**（`activeContacts()`。`ReportContactCallback` の
  react_forces と plane_coord の X 軸。`activeContactPairs()` はここから
  導出）。`onCollision` ノードの `minforce`（value）と `normal`（vec =
  対象から相手へ向く法線と 60° 以内。零 = 問わない）がこれで絞る。vec の
  既定 (0,5,0) は impulse 用なので、`clampNode` が衝突ノードでは零へ倒す。
- **重力 3 成分 `<option gravity="x y z">`**（`SimSettings::gravityX / Z`、
  `setGravity`）と**ボディ別の重力オフ `<geom gravity="false">`**
  （`SetUseGravity` / `SetNoGravity` を SFINAE。Multicore は自前の積分で
  効かない版がある）。
- **積分器 `<option integrator="euler|projected|implicit|trapezoidal">`**
  （NSC で組める 4 つだけ。HHT / Newmark は滑らかな系向け）と**ソルバ
  `solver="bb|apgd|psor|jacobi|minres"`**。どちらも Core のみ（Multicore は
  `setIntegrator` / `setSolver` が false を返し、既定以外を頼まれたときだけ
  1 回警告）。ソルバを替えたあとに反復回数を入れ直す順序に注意。
- **初速 `<geom velocity="x y z" angvel="x y z">`**（m/s, deg/s。
  `restoreAuthoredPoses` が置き直しの直後に `setBodyVelocity`。Reset でも
  同じ）と**イベントの `setVelocity`**（vec、value = 1 で加算）。
- **円柱部品の当たり判定**（`ExtraShape::cylinder`、`ChCollisionShapeCylinder`。
  Chrono の円柱は Z 軸なので部品の X 軸へ回す。固定の持ち主では 1 部品だけ
  の `addFrame` にする - 単独の円柱ボディの口は無い）と**三角メッシュの
  当たり判定 `<geom type="mesh" collision="trimesh">`**（`ShapeKind::Trimesh`
  は当たり判定だけの値。`ChCollisionShapeTriangleMesh`、`MeshCollision` の
  `loadCollisionTriangles`。凹形状の器・トンネル用で**固定の物に向く**。
  無い版・読めない場合は凸包へ倒す）。車輪のレイ（VehicleComponent）は
  円柱部品には当たらない（箱 / 球のみ、従来どおり）。
- **ジョイントの番号参照**（`onJointBreak` / `setMotor` の target、
  `NodeTargetKind::Joint`）。ジョイントを消したら `pruneJointNodes` が
  そのノードを消し、後ろの番号を 1 つ前へずらす。保存では端点の消えた
  ジョイントが落ちるので `jointRemap` で付け替える。
- **ブラウザ側**: Inspector の「物性」節（摩擦・反発・転がり・粘着・レイヤ・
  当てないレイヤ・重力・初速）、ジョイント節の追加行（`renderJointForm` が
  種類に合う行だけ出す。一覧の行クリックでフォームへ読込、✎ で
  `edit.joint.set`）、Physics タブの重力 X/Z・積分器・ソルバ・合成。
  コマンドは edit.set の新キー（surface / layer / nocollide / gravity /
  velocity / angularVelocity）、edit.joint.add の追加キー、edit.joint.set。
- **Multicore バックエンドでの扱い**（`kBackend` の既定は Multicore）。
  Chrono::Multicore は自前の積分で、リンクが `IntLoadResidual_F` で足す力
  （`ChLinkTSDA` / `ChLinkRSDA` / トルク・力モータ）を拾わず、速度モータ
  （`ChLinkMotor*Speed`）は専用の一覧に登録されて `RemoveLink` で外れない
  （停止 → 再開で解放済みのポインタを触る）。そのため Multicore では
  **ばねとトルク / 力モータを手計算の力積**（`JointRec::Manual`、
  `applyManualJoints` を `DoStepDynamics` の前に。ソフトボディのばねと
  同じ流儀）にし、**速度モータは角度 / 位置モータ + ランプ関数**
  （`ChFunctionRamp`、傾き = 速度。目標変更は今の角度から引き直す）で
  作る。Core では Chrono のリンクをそのまま使う。**可動範囲も Multicore は
  手計算**: Chrono の `ChLinkLimit`（片側拘束）は Multicore で範囲に
  当たった瞬間に系全体が NaN になった（実機で確認: 初速付きの振り子が
  ±60° に達する step 7 で全ジョイントの反力が NaN）。そちらでは接触と
  同じ速度レベルの力積（範囲を超えて進む相対角速度 / 速度だけを打ち消し、
  めり込みは 1 ステップ 20% で押し戻す）にする。Core は `ChLinkLimit`。
- **破断は `RemoveLink` ではなく `SetDisabled`**（`checkJointBreaks`）。走行中に
  リンクを外して `Setup()` を呼ぶと Multicore のデータマネージャが崩れ、
  拘束の付いた物が全部 NaN になった（実機で確認: 1 ステップ目に距離拘束が
  325 N を返して破断 → 5 ステップ後に全 NaN）。無効化したリンクは停止時の
  `removeAllJoints` がまとめて外す。開始直後 10 ステップは反力が収束途中で
  跳ねるので破断を見ない（`kWarmupSteps`）。
- **ボディ別の重力オフは手計算**。Chrono 9.0 には `SetNoGravity` /
  `SetUseGravity` が無い（SFINAE が両方落ちた）ので、`step()` が重力ぶんの
  速度を毎ステップ打ち消す（両バックエンド共通）。
- **衝突ファミリは `AddBody` の前に付ける**（`prepareBody`）。登録後に
  `SetFamily` すると Bullet はモデルを Remove / Add し直し、Multicore の
  衝突系は Remove が未実装で例外を投げる（ソフトボディの粒子が先に
  この順序だった理由）。
- **物理スレッドは例外を捕まえる**（main.cpp の `guarded`）。Chrono の
  例外はそれまでプロセスごと落として原因が残らなかった。捕まえたら
  `LOGE` とステータスに理由を出してエディタへ戻し、3 回続いたら物理を
  止めて描画だけ続ける。「シミュレート開始で落ちる」の切り分けはまず
  コンソールのこの行を見る。
- **未検証**: この一群は Chrono を持たない環境で書いた。文書層（XML / JSON
  の往復・警告）は `g++ -fsyntax-only` と往復テストで通したが、
  `PhysicsWorld.cpp` / Scene は Chrono 9.0 のヘッダ名を前提にした
  SFINAE で書いてあり、実機ビルドでの確認が要る。特に
  `ChContactMaterialCompositionStrategy` の仮想関数名、`ChLinkLock::LimitRz()`
  の戻り型、`ChBody::SetUseGravity` の有無。

## ソフトボディ（`src/scene/SoftLattice.{h,cpp}` + `PhysicsWorld::addSoftBody` + `Renderer::addSoftShape`）

**質点ばね方式**。Chrono の FEA モジュールは使わず、小さな球の剛体
（粒子）を格子状に並べて隣どうしをばねで結ぶ。`<body>` に `<soft .../>`
を書いたオブジェクト（`BodyDesc::hasSoft` / `soft` = `ed::SoftDesc`）が
対象で、形（箱 / 球）と大きさ（size）が格子の外形、mass が全粒子の合計。
サンプルは `assets/scenes/softbody.xml`、UI はアセットパネルの **🫧 Soft
Box / Soft Ball** タイルと Inspector の「ソフトボディ」節（チェックで
剛体⇄ソフトを切替、数値は edit.set の `soft` キー 1 個で部分更新）。

- **格子は `softlattice::build(desc)` の純粋な計算**（Chrono も Filament も
  知らない）。1 軸 `res` 個（2〜8 = 8〜512 粒子）で、箱は各辺を n 等分した
  セルの中心、球は同じ格子を殻ごとに球へ写したもの（spherified cube。殻
  |u|∞ = c が半径 c の球殻へ - 内側の点にそのまま表面用の式を使うと角が
  はみ出る）。当たり半径はセルの半分より少し小さく（0.46 h）、一番外の
  粒子の中心は外形から半径ぶん内側 = **粒子の球の外側がちょうど外形**。
  ばねは構造（隣）・せん断（面と立方体の対角線）・曲げ（1 個おき）の 3 種。
  **硬さは弾性率相当 E (Pa)** で持ち、k = E × 格子間隔にするので解像度を
  変えても材料の硬さが変わらない。減衰は減衰比 ζ（c = 2ζ√(k m/2)）。
  表面は「どれかの添字が端」の粒子を頂点にした 6 面の三角形メッシュ
  （頂点は面をまたいで共有、法線は隣接面の平均 = 角が少し丸い）。
- **PhysicsWorld は粒子を普通のボディとして持ち、代表番号 1 個で操作を
  受ける**（`addSoftBody` の戻り値 = 最初の粒子の physId）。`bodyTransform`
  は粒子群に当てはめた剛体姿勢（重心 + 相関行列の SVD による最小二乗の
  回転）、`placeBody` / `setBodyPose` は静止形状のまま置き直し、
  `setBodyFixed` / `disableBody` は全粒子、`applyForce` は合計質量で速度変化
  を出して全粒子へ等しく、`bodyMass` は合計、`bodyVelocity` は平均、
  `activeContactPairs` は粒子の接触を代表番号で報告。**Scene 側は剛体と同じ
  physId 1 個で扱う**（掴む・イベントの力・固定・衝突トリガー・ギズモ・
  選択の当たり判定がそのまま効く）。ジョイントは代表粒子に付く（限界）。
  `GameObject::lattice`（shared_ptr<const Lattice>）が「ソフトである」印で、
  `snapshot()` が `latestSoft_` に粒子位置を積む。
- **ばねは `step()` の先頭で速度に織り込む**（`solveSoftSprings`。車両の
  「点に掛かる力」と同じく DoStepDynamics の前に速度を直接書く）。1 本ずつ
  implicit Euler で解く: v' = (v − dt k x / m) / (1 + dt (c + dt k) / m)、
  m は換算質量。これをガウス・ザイデルで `iterations` 回。1 本ずつが無条件
  安定なので**どんな硬さでも発散しない**（硬さが dt に対して大きいときは
  「自然長へ射影する拘束」に近づき、反復不足で形が崩れることはある =
  硬くするなら rate / substeps / iterations を上げる）。Chrono の ChLinkTSDA
  を使わないのは、BB / APGD の反復ソルバは剛性行列を持たず結局陽解法に
  なるのと、Core / Multicore で同じ結果にするため。
- **同じソフトボディの粒子どうしは衝突しない**（衝突ファミリ 8〜15 を順に
  割り当て（0〜7 は剛体の衝突レイヤ）、`SetFamily` + `DisallowCollisionsWith`。旧 API 名にも SFINAE で
  落ち、無ければ粒子どうしも当たるが半径を小さくしてあるので静止では
  触れない）。**粒子は眠らせない**（一部だけ眠るとばねの相手が動いても
  起きず形が固まる。`setSleepingEnabled` が粒子を飛ばす）。
- **描画は形状スロットの 1 つ**（`Renderer::addSoftShape` = 自前の頂点 /
  インデックスバッファを持つ ShapeSlot。色・ハイライト・削除は
  `setShapeColor` / `setBoxHighlighted` / `removeShape` の同じ口）。
  `applyToRenderer` が毎フレーム `softlattice::buildSurface`（粒子中心 +
  法線 × 半径）で頂点と法線を組み、ワールド座標のまま
  `setSoftShapeVertices` へ（姿勢行列は使わない。AABB も頂点から毎回）。
- **作り直しのタイミング**: Inspector の変更（soft の切替・数値・寸法・
  質量）はその場で `rebuildBody`（見た目も粒子から組むので、遅らせると
  大きさが変わらない）。ギズモの拡縮ドラッグ（`resizeObject`）は
  `softRebuildTimer`（0.35 s）で落ち着いてから `stepEditor` が作り直す
  （ドラッグごとに粒子を捨てると退場ボディが溜まる）。
- **文書**: `<soft res stiffness damping shear bend iterations/>`（全部既定値
  付き、`<soft/>` だけでも有効）。メッシュ形状に書くと箱として読む（警告）。
  ブラウザ API は objects[] の `soft: true` と selected の `soft {enabled,
  res, ...}` + `softParticles` / `softSprings`。
- **テストは `tests/softbody/`**（Chrono / Filament 不要。`cmake -S
  tests/softbody -B build-softbody-test && cmake --build build-softbody-test
  && ./build-softbody-test/softbody_test`）: 格子の整合性と法線の向き、
  `<soft>` の XML / JSON 往復、陰解法ばねの安定性。**格子や式を触ったら
  必ず回す**（車両の `vehicle_test` と同じ扱い）。
- **未**: 部分的な固定（上面だけ留める等）、粒子以外の当たり形状、
  破断・塑性、他のソフトボディへのジョイント、四面体メッシュ。

## プレハブ（`src/components/PrefabComponent.{h,cpp}` + `scene/PrefabDefaults` + `scene/PrefabFrame.h`）

Unity の prefab に相当する**見た目の部品の集合**。`EditorTypes.h` の
`PrefabDesc`（名前 + `PartDesc` の配列）で、文書では `<asset>` の
`<prefab name>` と `<body>` の `<prefab name/>`（付け先）。部品は
**物理ボディではない**（質量は元の `<geom>` のまま。当たり判定は既定で無く、
`collide` を立てた箱 / 球だけ付け先の形に足される - 下記）ので、
Multicore の制約にも MJCF の入れ子 body の姿勢合成にも触れない。
**部品だけの物**は geom を持たないボディ（`ShapeKind::None`、文書では
`<geom>` を書かず `<body mass="...">`。MJCF の geom 無し body）に付ける:
自分の見た目も当たり判定も無く、`PhysicsWorld::addFrame`（質量は指定、慣性は
collide 部品の外接箱、当たり判定は collide 部品だけ）が実体で、原点は好きな
場所（階段は全体が収まる箱の中心）に置ける。ソフトにはできない（clampBody
が箱へ倒す）。**ビューのクリック（`Scene::pickBoxAt`）は部品にも当たる**
（車体に固定の箱 / 球 / 円柱。付いていない車両は組み込みの見た目）ので、
段やキャビンをクリックしても持ち主が選べる。

- **部品**は種類（box / sphere / cylinder / mesh）・親ローカルの位置と回転・
  大きさ（Box は各辺、Sphere は直径 x、Cylinder は長さ x と直径 y、Mesh は
  倍率 x）・色・**socket**を持つ。socket は `wheel:<軸>:<L|R>` で、その部品は
  車両のその車輪の姿勢（縮み・舵・回転）に付いていく。空 = 車体に固定。
- **描画は PrefabComponent（RENDER）**。付いていない車両は
  `builtinCarPrefab()`（キャビン・フロントガラス・ライト・タイヤとスポーク、
  寸法比）を**暗黙のプレハブ**として描く - 以前 VehicleComponent が直接
  描いていた飾りはこれに移した。車輪の姿勢は VehicleComponent の
  `wheelLocalPoses()` スナップショット。glTF の部品はシーンの `<mesh>`
  アセットを名前で使う（`Scene::meshModelId` が遅延読込）。
- **プレハブ編集モード**（Unity のプレハブモード）: オブジェクトを**右クリック
  →「🧩 プレハブを編集」**（階層一覧の行でもビューでも）。プレハブが無ければ
  いま見えている組み込みの見た目から作って付ける（`edit.prefab.open`、名前は
  `<オブジェクト名>_prefab`）。状態は `EditorState::prefabEditObject()`
  （-1 = 通常）で、その間は **ビューでそのオブジェクトの部品だけが選べ**
  （GizmoComponent の pick が部品の中心を投影して拾う。外の物は選べない）、
  選択は `SelKind::Part`（index = 部品番号）、ギズモの対象は「部品」
  （Target type 3）。ドラッグは `Scene::movePart / rotatePartWorld / resizePart`
  で、ワールドの姿勢を **`PrefabFrame.h` が親フレーム（車体 / ソケット）の
  ローカルへ直す**。回転は四元数のまま渡す（オイラーで往復すると親の回転で
  崩れる）。Inspector は他の節を隠して部品の一覧と数値（`secPrefab`）に
  なり、映像ヘッダーに「◀ 戻る」（`edit.prefab.close`、Esc でも）。
  シミュレートに入る・持ち主が消える・プレハブが消えると自動で抜ける。
- **当たり判定 `collide`**（文書は `<part collide="true"/>`、Inspector の部品
  の「当たり判定」チェック。箱 / 球で socket が空の部品だけ - `clampPart` が
  他を false に落とし、読み込みは警告する）。`Scene::collisionShapes` が
  `PhysicsWorld::ExtraShape` に直す。**固定の持ち主なら部品ごとに別の固定
  ボディ**（`addBox / addSphere` を普通に呼ぶ = 「固定の箱を並べた階段」と
  同じ物理で、Core / Multicore とも実績のある経路。番号は
  `GameObject::childPhysIds`、接触ペアは `PhysicsWorld::setAlias` で持ち主の
  番号に寄せる。持ち主を動かしたら `rebuildChildren` が physDirty で作り
  直す）、**動く持ち主なら `addBox / addSphere / addConvexHull / addFrame` の
  複合形状**（`ChBody::AddCollisionShape` + `ChCollisionShapeBox / Sphere`。
  ヘッダの無い版は `__has_include` で見た目だけに落として警告）として本体の
  形に足す。質量・慣性は本体の geom のまま（動く物に付けるなら本体の geom を
  重心に置く）。Chrono は形を
  後から変えられないので、collide 部品の追加 / 変更 / 削除と付け外し・
  プレハブの削除は持ち主を `physDirty`（`markPrefabUsersDirty`。エディタ中は
  シミュレート開始でまとめて、シミュレート中は即 `rebuildBody`）。車輪の
  レイ（VehicleComponent）も同じ部品に当たる。`prefab.add` は `parts` 付きで
  中身ごと作れる（🪜 Stairs が使う）。
- コマンド: edit.prefab.open / close / attach / detach / add / remove、
  edit.part.add / set / remove（prefab 省略時は編集中のもの）、select.part。
  すべて EditorComponent → Op → 物理スレッド（イベントと同じ配管）。
  アセットパネルの **🧩 タイル**は右クリックで「選択オブジェクトに付ける /
  付けて編集 / 削除」。
- **未**: 円柱 / メッシュ / ソケット付き部品の当たり判定、ビュー上のドラッグ
  以外の複数選択、部品の複製。

## ギズモ（`src/components/GizmoComponent.{h,cpp}`）

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
- **オイラー角と四元数の変換は `SceneMath.h` に 1 か所だけ**
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

## イベントアセット（ノードベースのイベント設計）

映像上のノードエディタ（ギズモバーの ⚡、Node-RED 風）で組む「トリガー →
アクション」。**中身（ノードとワイヤー）は名前付きの「イベントアセット」で、
オブジェクトかシーン全体に付けて初めて動く**（Unity のスクリプト資産と同じ
関係）。1 つのオブジェクトに何本でも付けられ、同じアセットを複数の
オブジェクトに付け回せる。

**エンジンには「マウスで掴んだら動く」も焼き込んでいない**。掴みの計算
（対象・カーソルの指す点）は `Scene::pointerGrab` が出すだけで、実際に
引き寄せるのは既定シーンに付いているイベントアセット `pickup`
（onGrab → grabPull）。外せば掴んでも動かなくなり、差し替えれば挙動を
ノードで書き換えられる。イベントの節を持たない文書と「🗑 全消し」は
`Scene::resetEventsToDefaults()` がこの 2 ノードを作って付ける（ライトを
1 灯も書かない文書が初期構成で開くのと同じ扱い）。

- **型は EditorTypes.h**（`NodeKind` / `NodeDesc` / `WireDesc` /
  `EventAssetDesc`）。トリガーは衝突（OnCollision）・開始（OnSimStart）・
  タイマー（OnTimer）・掴み（OnGrab）、アクションは色（SetColor）・力
  （ApplyImpulse）・固定（SetFixed）・引き寄せ（GrabPull）・ライトの色/強さ
  （SetLight*）・カメラ注視（CameraLookAt）。ワイヤーはトリガー →
  アクションの 1 段だけ（連鎖なし）。target の指す種別は
  `nodeTargetKind(kind)` が唯一の定義（object / light は保存で番号が詰まるので、
  詰め替え・掃除が全部ここで分岐する）。
- **対象を書かない（target = -1）が既定**。実際の相手は実行時に決まる:
  ①番号を書いてあればそれ ②トリガーが渡してきた物（OnGrab が掴んだ物・
  OnCollision が触れた物）③そのアセットを付けたオブジェクト。定義は
  `Scene::graphTarget` の 1 か所で、これがあるからアセットを付け回せる。
  ライトとカメラは付け先になれない（付けるのはオブジェクトかシーン全体）
  ので、番号を必ず明示する。
- **アセット本体は EditorState**（ジョイントと同じ mutex 流儀）。ノード id は
  **アセットの中で**一意で、削除しても再利用しない（ワイヤーが別のノードを
  指し直すため）。別のアセットとは番号が重なってよいので、**編集コマンドは
  必ずアセット名を伴う**（edit.node.* / edit.wire.* の `asset`）。作成・削除・
  付け外しは edit.event.add / remove / attach / detach。付け先はオブジェクト側が
  `BodyDesc::events`（Scene が持つ = 保存で番号が詰まっても付け替え不要）、
  シーン全体は `EditorState::worldEvents`。
- **実行は物理スレッド**（`Scene::runEventGraph`、`physics_.step()` の直後 =
  そのステップの接触を見る）。走るのは「付いているアセット」の数だけの
  **実体**（`GraphRuntime::Instance` = アセット + 付け先）。毎ステップのロックを
  避けるため、`EditorState::graphVersion()`（変更ごとに進む版番号。オブジェクトの
  増減と付け外しでも `bumpGraphVersion()` で進む）が変わったときだけ一覧を
  コピーする。タイマー等の実行状態は **（アセット名, 付け先, ノード id）で引く**
  ので、シミュレート中の編集で他のノードの状態がリセットされない。
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
- **掴みトリガー（OnGrab）は毎ステップ・カメラごと**に発火し、掴んだ物と
  カーソルの指す点を文脈として渡す。受けた GrabPull がサーボ
  （F = m・(kp・e − kd・v)、加速度は上限で頭打ち）を毎ステップ掛け、
  引っぱり線（`Scene::setGrabLine`）も引く。ばね定数は `BoxController::Config`
  （SceneConfig.h）で、ノードの value はその倍率。**エディタモードの
  置き直しは今までどおり C++**（BoxControlComponent::onEditorStep）: 配置作業は
  シミュレートではないので、スクリプトの有無に左右させない。
- **対象が消えたノードは掃除**（`pruneGraphForRemoved`、ジョイントの掃除と
  同じ判断）。OnCollision の相手フィルタだけが消えたときはノードを残して
  「何でも」(-2) に戻す。**番号を書いていないノード（target = -1）は触らない**:
  付け先が消えても、アセットは他の相手に付けられる部品として残る。
- **文書では `<asset>` の `<event name>`**（中身）と、`<body>` の
  `<event name/>` / ルートの `<events>` の `<event name/>`（付け先）。保存で
  オブジェクト・ライトの番号を詰めるのに合わせて target / other も付け替える
  （ライトにも remap 表が要る）。読込はジョイントと同じく base / lightBase
  ぶんずらす。**旧形式**（`<events>` の直下に `<node>` / `<wire>` を並べた文書、
  旧 JSON version 3 の nodes / wires）は "events" という名前のアセットに入れて
  ワールドへ付ける。イベントの節が 1 つも無い文書は既定構成（pickup）で開く。
- **UI は /scene の `events`**（`{assets:[{name,nodes,wires}], world:[名前]}`。
  発火回数 fired 付き＝ノードの ⚡ バッジ。オブジェクトに付いているぶんは
  objects / selected の `events`）をポーリングで描く。アセットは**アセット
  パネルの ⚡ タイル**（クリックでノードエディタがそのアセットに切り替わる）と
  **ノードエディタ見出しの選択**で選び、**付け外しは Inspector の「イベント」**
  （オブジェクト＝選択中の物、World 節＝シーン全体）。ライト / カメラの
  Inspector は「そのライト / カメラを対象にしているノード」の読み取り専用一覧。
  **作成・削除・付ける・ノードの追加は右クリック（コンテキストメニュー）**が
  入口: アセットパネルの何もない所 =「新しいイベント」、⚡ タイルの上 =
  開く / 付ける / 削除、ノードエディタのキャンバス = 押した場所にノードを追加
  （トリガー / アクションの一覧）、ノードの上 = そのノードを削除、Inspector の
  イベント行 = 開く / 外す。見出しにボタンを並べる方式は種類が増えるほど
  読めなくなるので、置く場所で決まる操作は置きたい場所で選ぶ（実体は app.js の
  `showContextMenu`、CSS は `#ctxMenu`）。
  グラフと選択肢が変わったときだけ DOM を組み直し、
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

**`src/` は担当ごとのフォルダに分かれ、`src/` が include のルート**
（`#include "core/Log.h"` のようにフォルダ名から書く。同じフォルダでも省略しない）:

```
src/
  main.cpp       起動・引数解析・2 スレッドのループ
  core/          Log, AssetError, Versions, Stats, CpuAffinity, PortScan
                 （エンジン非依存の土台）
  physics/       PhysicsWorld（Chrono）, MeshCollision（glTF 凸包）, PhysicsTuning
  render/        Renderer（Filament）, GltfLoader, EnvironmentLoader, ImageLoader
  streaming/     HttpServer, WebRtcStreamer, VideoStreamer（GStreamer）
  document/      EditorTypes, SceneDocument, SceneXml（シーン文書。エンジン非依存）
  scene/         Scene（Scene.cpp / SceneEdit.cpp / SceneEvents.cpp /
                 SceneSerialize.cpp + 私的ヘッダ SceneInternal.h）, SceneConfig,
                 EditorState, GameObject, CameraObject, BoxController,
                 SceneComponent, PrefabDefaults, PrefabFrame, SceneMath,
                 MathBridge, RenderBridge
  components/    EditorComponent, GizmoComponent, PhysicsControlComponent,
                 StreamControlComponent, VehicleComponent, PrefabComponent
  vehicle/       車両モデルとノード式（純粋な数値ライブラリ。tests/vehicle が使う）
tests/vehicle/   車両モデルの単体テスト（-DWIZ_BUILD_TESTS=ON で本体からも回せる）
tests/document/  シーン文書の単体テスト（同上。Chrono も Filament も要らない）
cmake/           LuaJIT.cmake（LuaJIT の検出・ビルド）
web/             ブラウザ UI（ビルド時に assets/web/ へコピー）
assets/          実行時に読むもの（materials / textures / scenes）
third_parties/   サブモジュール（Chrono, Eigen, Blaze, Thrust, LuaJIT, json, httplib, cgltf, stb）
```

- **版番号とコードネームは `src/core/Versions.h` が唯一の定義**
  （`kVersionMajor/Minor/Patch` = 1.0.0、`kCodename` = "Charon"）。CMake の
  `project(VERSION)` には持たせない（2 か所に書くと必ずずれる）。起動ログの
  1 行目（`WizEngine 1.0.0 "Charon"`）、`/stats` の `versions.WizEngine`（素の
  semver）と `versions.Codename`、ブラウザのサイドバー見出しと About 節
  （Physics タブ）がここから出る。コードネームは major 版ごと（1.x = Charon）。
- **CPU コアの固定**（`src/core/CpuAffinity.{h,cpp}`、Windows / Linux 両対応）。設定は **exe 引数**（`--physics-cores "0-11"` / `--render-cores "12-15"` /
  `--physics-threads N`、`--help` で一覧）。**SceneConfig.h には置かない**（scene はユーザーが
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
- `src/physics/PhysicsWorld.{h,cpp}` — 物理エンジン（Chrono）。重力・接触・材質の設定のみ。
  `addBox(...)` で剛体追加、`step` / `bodyTransform(id)` / `setBodyPose(id,...)`。
  エディタ用に `placeBody`（起こすための落下速度を与えない置き直し）、
  `setBodyFixed`、`disableBody`（削除相当。当たり判定を切って地面の下へ退避し、
  番号は残す。**Multicore では当たり判定のフラグを触らない** - その衝突系の
  `Remove()` は未実装で、Chrono 9 は "not yet implemented" を出して例外を
  投げるため。固定 + 退避だけで無効化する）、`setGravityY`、そして `addJoint` / `removeAllJoints` を持つ。
  **固定ボディには力を掛けない**（`applyForce` / `applyForceAtPoint` が
  `IsFixed` で弾き、`setBodyFixed(true)` は `ForceToRest` で速度も捨てる）:
  固定の物は位置を積分しないが足した速度は残り、**Multicore の接触拘束は
  固定ボディの速度も右辺に入れる**（= 動く床）ので、固定の台を掴んで毎
  ステップ速度を積むと、台は動かないまま上の物が全部押し出されて発散した。
  Core は非アクティブな変数を無視するので出ない（Chrono 9.0.0 の
  `ChConstraintTuple.h`）。掴み（GrabPull）は `bodyFixed` を見て引っぱり線
  ごと諦める。
  **ボディの追加は必ず `registerBody` を通す**: Chrono 9 は衝突モデルを
  最初のステップ（`Initialize` → `BindAll`）でまとめて登録し、**それ以降に
  `AddBody` したボディは Core / Multicore とも衝突系へ渡さない**。
  `bindCollision` が衝突系の初期化済みを見て `BindItem` を呼ぶ
  （エディタで足した箱が床を抜ける・シーン読込で作り直した床に何も乗らない、
  の正体だった）。
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
  irradiance は指定せず、Filament が反射マップの最下位 mip から導出する。**指定は
  シーン文書の `<environment hdr intensity>`**（worldbody 直下の単一要素。節を
  書かない文書は既定 = studio.hdr。`hdr=""` は環境マップ無し = 一様アンビエント、
  `Renderer::clearEnvironment`）。差し替えは重い（デコード + GPU プリフィルタ）ので
  Scene が desc + dirty で持ち、**applyToRenderer がロックの外で適用**する。
  読み込み失敗は `AssetError` だが、環境光はシーン文書の内容（手で書ける）なので
  **Scene が捕まえて警告に留める**（前の環境のまま続行 - 起動は止めない）。
  ローダは**先頭バイトから実際の形式を判定して報告**する（`.exr` を
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
- `src/render/Renderer.{h,cpp}` — 描画エンジン（Filament, headless Vulkan）。下地（デバイス・
  カメラ・ライト2灯＋IBL・共有キューブメッシュ・マテリアル）のみ構築。中身は
  `addShape(ShapeMesh)` / `addGround(halfSize,color)` / `setCamera(eye,target)` で追加。
  箱・床とも lit（`shaded.mat`＝箱用 lit / `ground_lit.mat`＝床用）。箱は影を落とし
  受けもする（以前は unlit＋頂点カラーで焼き込み陰影だったため、転がると陰影が
  向きに追従せず不自然だった）。床の広さ・テクスチャ・タイル・色味は**シーン文書の
  `<ground>` が持ち**（stb_image で読み込み。sRGB、ミップマップは自前生成＝
  `generateMipmaps` はこの版で usage フラグ必須のため不可）、画像が無い・読めない
  ときはコード生成の市松模様に落として警告する。`addGround` は 2 回目以降の
  呼び出しで前の床を壊して作り直す＝シーン読込による実行時の差し替えに対応
  （Scene::syncGround が dirty を見て呼ぶ）。物理の床（`<ground size>` の半寸法）は
  物理スレッドが作り直す（`Scene::rebuildGroundBody`。ジョイントの「ワールド側」
  番号もここで更新）。readPixels で RGBA 取得。
  エディタ用に**実行時に増減できる形状スロット**（`addShape` / `removeShape`、
  箱と UV 球と円柱（車輪用、軸 X）、削除した番号は空きとして再利用）と**オブジェクトごとの色**
  （`setShapeColor` が初回にそのスロット専用のマテリアルインスタンスを作る。
  共有インスタンスを書き換えると全部の色が変わってしまうため）、
  **ジョイント線**（`setJointLineCount` / `setJointLine`、グラブ線と同じ
  「2頂点1本」の作りを使い回し）を持つ。glTF は `loadModel`（原型、同じ
  パスは 1 回だけ）+ `addModelInstance` / `releaseModelInstance`（実体）。
  gltfio は実体を 1 個だけ壊せないので、release は**スケール 0 で隠して
  同じモデルの空き番号として再利用**する。
- `src/render/GltfLoader.{h,cpp}` — glTF/GLB 読み込み（Filament の gltfio）。
  **「原型 + 実体」の 2 段**: `loadModel(path)` がファイルを 1 回だけ読んで
  モデル番号を返し（同じパスはキャッシュ）、`createInstance(model)` が実体を
  何個でも作る（メッシュ・マテリアル・テクスチャは原型と共有 =
  `createInstancedAsset` + `createInstance`）。`releaseInstance` はスケール 0 で
  隠して同じモデルの空きに回す（gltfio に実体を 1 個だけ壊す口が無いため）。
  gltfio は Renderer の外に漏らさない。シーン文書の `<asset><mesh/>` が
  そのままこの語彙で、どのモデルをどの剛体で使うかは XML が決める。CMake が
  gltfio のライブラリ（gltfio_core / uberarchive / dracodec / ktxreader / stb）を検出
  したときだけ有効（`WIZ_HAVE_GLTFIO`）。無いビルドでは loadModel が
  AssetError を投げ、Scene 側が球で描いて警告する。
  Filament 1.74 Windows 版のライブラリ名は `uberz` ではなく **`uberzlib`**、また
  `shlwapi` のリンクが必要（`utils::Path`）。
- `src/render/ImageLoader.{h,cpp}` — stb_image で画像を RGBA8 として読む（PNG/JPEG/TGA/BMP）。
- `src/scene/Scene.h` + `src/scene/Scene*.cpp` — **シーンの実体管理**。実装は担当ごとに
  `Scene.cpp`（構築・ステップ・組み込みコンポーネント・描画への反映）・
  `SceneEdit.cpp`（エディタ操作）・`SceneEvents.cpp`（掴みとイベントグラフ）・
  `SceneSerialize.cpp`（文書との変換と `/scene` の JSON）に分かれ、共通の
  下準備（インクルード・小さな変換関数）は私的ヘッダ `SceneInternal.h`。オブジェクト
  （GameObject: 設計値 + 物理ID + 描画ID）・ライト・カメラ・メッシュアセット
  （`MeshAsset`: 文書の宣言 + Renderer のモデル番号 + 凸包のキャッシュ）を
  持ち、文書（SceneDocument）との相互変換・編集操作の適用・スレッド間の
  同期を行う。**マウスの掴み**（`pointerGrab` = 対象とカーソルの指す点、
  掴んだ時点の奥行きを覚える）と**引っぱり線**もここ: エディタの置き直しと
  イベントの引き寄せで同じ答えが要るため（動かすかどうかはイベントアセット
  次第）。**シーンの中身は持たない**（配置・モデル・ジョイントは
  assets/scenes/*.xml。`build()` は地面・ライト・カメラの初期化と
  kStartupScene の読み込みだけ）。エンジン側の既定値は `SceneConfig.h`。
- `src/streaming/VideoStreamer.{h,cpp}` — GStreamer パイプライン。`OutputMode` で
  web/None（GStreamer出力なし。ブラウザへは WebRtcStreamer が担当）/ window / stream
  （RTP/UDP）/ rtsp（rtspclientsink）を切替。
- `src/streaming/WebRtcStreamer.{h,cpp}` — GStreamer `webrtcbin` でブラウザへ WebRTC 配信。
  コーデックは `SceneConfig.h` の `kVideoCodec`（VP8 / VP9 / H264）と `kVideoBitrate` で指定。
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
- `src/core/Stats.h` — スレッド間で共有する性能カウンタ（atomic）。物理スレッドが Hz と
  1更新の所要 ms、描画スレッドが fps と renderFrame の ms を書き、`/stats` が JSON で
  返してブラウザ下部のオーバーレイに表示。
- `src/streaming/HttpServer.{h,cpp}` — cpp-httplib（別スレッド）。`/` 操作ページ、`/whep` で
  WebRTC シグナリング（`setOfferHandler`）、`/input` で入力受信（`drainCommands`）。
  入力は JSON（`{"cmd":...}`、main で nlohmann/json パース）。既定ポート8080。
  `/favicon.ico` は `web/favicon.ico` をビルド時に実行フォルダへコピーして配信（起動時に
  読み込み、無ければ 404）。拡張子から MIME を判定するので png/svg でも可。
  **視聴は同時1ブラウザのみ**: 最初の `/whep` がトークン（ヘッダ `X-Viewer-Token`）で
  セッションを取得し、`/viewer/ping`（2秒毎、本文=トークン）で維持。2つ目以降は 409 を
  返しブラウザ側でエラー表示＋自動リトライ。`/viewer/leave`（sendBeacon）または6秒
  無応答で解放し、`setViewerGoneHandler` → `WebRtcStreamer::stopSession()` で
  パイプラインを破棄。
- `web/` — ブラウザ用フロント（`index.html` / `style.css` / `app.js`）。ビルドが
  `assets/web/` にコピーし、HttpServer がそこから配信する（リロードで反映、
  再ビルド不要）。`<video>`＋WHEP クライアント（`/whep` に SDP offer を POST）、
  操作は `/input` に JSON POST。フロントはブラウザのみ。
- `src/scene/MathBridge.h` — Chrono → Filament の姿勢変換（四元数から回転行列を手計算）。
- `src/scene/RenderBridge.h` — 文書の値 → レンダラの語彙（材質 `MaterialDesc`
  → `ShapeMaterial`、描画設定 `RenderDesc` → `RenderSettings`）。Scene と
  PrefabComponent の両方が使うので Scene の私的ヘッダには置かない
  （同じ変換を 2 か所に書くと、値を足したとき片方だけ直して黙って食い違う）。
- **視聴者がいない間は完全に休む**（web モードのみ、`SceneConfig.h` の `kIdleWhenUnwatched`）。
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
  そのまま）。シーン文書の `<mesh file>` は **`assets/` からの相対名のみ**
  （`..` と絶対パスは読み込みで弾く - 文書は手で書けるため）。
- **アセット読み込み失敗は例外で強制停止**（`src/core/AssetError.{h,cpp}`）。重要なのは
  **チェックリストを持たない**こと: `GltfLoader::loadModel`、マテリアル読み込み、
  テクスチャ読み込みという**読む側そのものが `AssetError` を投げる**ので、シーン文書に
  新しいファイルを足しても検証漏れが起きない（変数名を列挙する方式は、追加時に
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
- `src/document/EditorTypes.h` — エディタ文書の型だけを集めたヘッダ（`AppMode` /
  `ShapeKind` / `JointKind` / `BodyDesc` / `JointDesc` / `NodeDesc` /
  `EventAssetDesc` / `SimSettings` と、その JSON 変換・範囲クランプ）。Chrono も Filament も出てこないので、どのスレッド
  からでもコピーできる。保存フォーマットとブラウザ API のキーはここが唯一の定義。
- `src/document/SceneXml.{h,cpp}` — 依存の無い最小 XML DOM（読み書き）。シーン文書の
  ためだけの部分集合で、要素・属性・入れ子とコメント・実体参照まで。整形出力は
  属性の順を保ち、長い要素を折り返す。
- `src/document/SceneDocument.{h,cpp}` — シーン文書の値型（`SceneDocument`）と、その
  **MuJoCo 風 XML** への変換。保存フォーマットの定義はここ 1 か所（旧 JSON の
  取り込み `fromLegacyJson` も同居）。上の「シーン文書（XML）」の章を参照。
- `src/scene/EditorState.{h,cpp}` — モード（atomic）、編集操作のキュー、ジョイント一覧、
  **イベントアセット**（名前 + ノード + ワイヤー。付け先はシーン全体ぶんだけ
  ここが持ち、オブジェクトに付いたぶんは `BodyDesc::events`）、
  シミュレート設定、`assets/scenes` の読み書き（`.xml` が正、`.json` は
  読み込みのみ）と一覧キャッシュ。オブジェクト
  そのものは持たない（実体と並べて Scene が持つ。番号がずれると黙って別の物を
  動かしてしまうため）。
- `src/components/EditorComponent.{h,cpp}` — ブラウザの `mode` / `edit.*` コマンドを受ける
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
2. `SceneConfig.h` の `kBackend`（`PhysicsBackend::Core` / `::Multicore`）… **実際にどちらを
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

LuaJIT（ノード式の実行エンジン、上の「ノード式」の章）も `third_parties/luajit`
のサブモジュール。cpp-httplib / nlohmann/json / cgltf / stb は **`third_parties/` の git サブモジュール**
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
9. **最初のステップ後に追加したボディに当たり判定が無い** — Chrono 9 の
   `ChSystem::AddBody` / `ChSystemMulticore::AddBody` は衝突モデルを衝突系に
   登録しない（登録は初期化時の `BindAll` の 1 回だけ）。実行中に足したボディは
   `GetCollisionSystem()->BindItem(body)` を自分で呼ぶ
   （`PhysicsWorld::bindCollision`）。形は見えるのに床を抜ける、が症状。

## 実装済み / 未実装

- 済: エディタモード（配置・プロパティ編集・ジョイント設計・シーンの保存/読込）と
  シミュレートモードの分割。
- 済（実機未検証）: Chrono の拘束・物性の拡張（13 種のジョイント・可動範囲・
  モータ・ばね・破断・反力計測、ボディ別の接触物性と合成方式、衝突レイヤ、
  接触の力 / 法線によるトリガー、重力 3 成分とボディ別重力、積分器 / ソルバの
  選択、初速、円柱部品と三角メッシュの当たり判定。上の「Chrono の拘束・
  物性」の章）。
- 済: シーン文書の MuJoCo 風 XML 化（`assets/scenes/*.xml`、`/scene.xml`、
  `kStartupScene`。上の「シーン文書（XML）」の章を参照）。
- 済: glTF モデルの文書化（`<asset><mesh/>` + `<geom type="mesh" mesh=...>`。
  配置も含め、シーンの中身はコードから文書へ全面移行。Scene.cpp の
  格子自動生成・kBoxModelPath プール・置物 kModelPath は廃止し、既定シーンは
  `assets/scenes/default.xml`）。
- 済（試作）: 車両シミュレーション（グラフ型パワートレイン + レイキャスト式
  車輪、車輪の描画、ソフトタイヤ、シーンの剛体への接地。上の「車両」の章）。
- 済: イベントアセット（Node-RED 風のノードエディタ。衝突・開始・タイマー・
  掴みのトリガーと、色・力・固定・引き寄せ・ライト・カメラ注視のアクション。
  ノードは名前付きアセットにまとめ、オブジェクト / シーン全体に何本でも
  付けられる。マウスで掴んだ物を動かすのも既定アセット `pickup` の仕事で、
  エンジンには焼き込んでいない。上の「イベントアセット」の章を参照）。
- 済: フォトリアル描画（シーン文書の `<visual>` = 影 PCSS / MSAA / TAA /
  AO / ブルーム / スクリーン空間反射 / 被写界深度 / ビネット・物理カメラの
  露出・トーンマップと色調整、`<geom>` / `<part>` の PBR 材質、
  `<ground roughness metallic>`、`<environment skybox>`。上の「フォトリアル
  描画」の章と `docs/photorealism.md`）。
- 済: ステップ3（姿勢反映）〜6（UDP配信）。
- 未: ステップ7（クライアント→サーバーの入力・制御チャネル。カメラ操作を
  UDP/TCP で受けて `Renderer` にカメラ更新 API を追加）。
- 未: 材質テクスチャ（baseColor / 法線 / ラフネスマップ）、ガラス（屈折・
  透過）、面光源。Filament はラスタライザなので、スクリーン空間を超える
  GI（パストレース相当）は原理的に出せない - 必要ならオフラインの
  レンダラへ文書を渡す（`docs/photorealism.md` の「限界」）。
- 将来: ハードウェアエンコード（`nvh264enc` 等）、readPixels を避けた GPU 直結、
  物理と描画のスレッド分離（姿勢はダブル/トリプルバッファで受け渡す）。

## 規約

- 重力は −Y にしてあり Filament の Y-up と一致。両エンジンとも右手系なので
  姿勢変換に軸スワップは不要。この前提を崩さないこと。
- 自作の `Renderer` クラスは `namespace wizengine` に入れてある（`filament::Renderer`
  と名前が衝突するため）。`Renderer.cpp` では `using namespace filament;` のまま
  でよいが、`filament::Renderer` のネスト型は `filament::Renderer::ClearOptions`
  のように明示修飾する。新規クラスも衝突を避けるなら `wizengine` に入れる。
- **出力引数を持つ関数の戻り値と、その出力引数を同じ呼び出しに並べない**
  （`f(hits(.., dist, normal), dist, normal)` の一行書き）。引数の評価順は
  未規定で、MSVC は右から左に評価するため、値渡しの `dist` が判定前の古い
  値でコピーされる。GCC のテストでは通り、Windows の実機でだけ壊れる
  （車輪のレイが前の部品の距離を使って階段で車が跳ねた）。判定の戻り値を
  一度変数に受けてから使う。
- C++17。外部依存の追加は最小限に。ただし Filament のヘッダが designated
  initializer を使うため、MSVC では `Renderer.cpp`（Filament を含む唯一のTU）
  だけ `/std:c++20` でビルドする（`CMakeLists.txt` で設定済み）。他は C++17。
