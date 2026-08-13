# WizEngine

Headless real-time 3D streaming prototype: **Project Chrono** physics and
**Google Filament** rendering on the server, H.264-encoded with **GStreamer**
and streamed to browsers over **WebRTC** — with a multi-camera, interactive
web UI. (Docs below are in Japanese.)

サーバー側で **Project Chrono**（物理）と **Google Filament**（描画）をヘッドレスに
実行し、フレームを **GStreamer** で H.264 エンコードして **WebRTC** でブラウザへ
配信する、リモートレンダリング／ピクセルストリーミングのプロトタイプです。

## 特徴

- **エディタモード / シミュレートモード**: 物理を止めて配置・設計する
  「エディタ」と、それを走らせる「シミュレート」をブラウザから切り替え。
  ボックス・球の追加／削除／複製、位置・回転・大きさ・質量・色の編集、
  5 種類のジョイント（固定・ちょうつがい・ボール・直動・距離）の設計、
  重力や摩擦などのシミュレート設定、`assets/scenes/*.json` への保存・読込
- **マルチカメラ Web UI**: カメラごとに専用ページ（ポート 8080, 8081, ...）。
  オービット / パン / ズーム、オブジェクトのドラッグ（グラブ）、クリック選択、
  シーン階層サイドバー（カメラ・ライト・オブジェクト）、インスペクタ、
  ソルバーやレートをその場で変えられる物理チューニングパネル
- **物理**: Chrono Core / Multicore（OpenMP 並列）を切り替え可能。既定シーンは
  512 剛体の落下スタック。スリープ、固定タイムステップ + キャッチアップ制御
- **描画**: Filament ヘッドレス（Vulkan / OpenGL）。HDR パノラマ（Radiance .hdr）を
  **起動時に GPU 上でキューブマップ化・プリフィルタ**する IBL —
  ファイル差し替えは再起動だけで反映、ビルド不要
- **シーン定義は 1 ファイル**: グリッド構成・形状・摩擦・カメラ・ライト・
  ストリーミング・ソルバーまで全パラメータが `src/SceneConfig.h` に集約
- **構造化ログ**: 時刻(ms)・レベル・スレッド名・タグ付き、色分け、
  `WIZENGINE_LOG=debug|info|warn|error` でレベル制御
- **CPU 制御**: `--physics-cores` / `--render-cores` / `--physics-threads` で
  物理と描画をコアに固定（Chrono::Multicore の OpenMP ワーカー含む）
- 誰も見ていない間は物理も描画も止まる省電力ゲーティング（web モード）

## アーキテクチャ

```
[input thread]  ブラウザからの JSON コマンド
      └─ Scene::dispatchCommand → SceneComponent 群
         （CameraControl / BoxControl / PhysicsControl / Editor）
[physics thread] 編集キューの適用 → モードで分岐
      ├ Simulate: 固定タイムステップで Chrono を実行 → 姿勢スナップショット
      └ Editor  : 積分せず、掴んだ物の置き直しだけ → 姿勢スナップショット
[main/render thread] スナップショットを Filament に反映 → 描画
      → 非同期 readPixels → GStreamer appsrc → H.264 → WebRTC
[http threads]  カメラごとの HTTP サーバ（UI 配信・シグナリング・stats）
```

オブジェクトの追加・削除は 2 段階で行われます。Chrono を触ってよいのは物理
スレッド、Filament を触ってよいのは描画スレッド、という既存の分担を崩さない
ためです。ブラウザの操作は `EditorState` のキューに積まれ、物理スレッドが
剛体を作り、描画スレッドがその次のフレームでレンダラブルを作ります。

主なソース:

```
src/
  SceneConfig.h            シーンの全パラメータ（まずここを編集）
  scene.cpp / Scene.h      シーン実装・階層 JSON・コンポーネント登録
  CameraObject / LightObject   シーンオブジェクト（atomic 状態 + applyTo）
  BoxController            グラブ（掴んで引っ張る）制御
  EditorTypes.h            エディタ文書の型（剛体・ジョイント・設定 + JSON）
  EditorState              モード・編集キュー・ジョイント・シーンファイル
  EditorComponent          ブラウザの編集コマンド受付（検証と既定値）
  PhysicsControlComponent / PhysicsTuning   ブラウザからの物理チューニング
  PhysicsWorld             Chrono ラッパ（Core / Multicore）
  Renderer                 Filament ヘッドレス描画・ビュー/読み戻し管理
  EnvironmentLoader        HDR → GPU プリフィルタ → IBL
  ImageLoader              stb_image ラッパ（.hdr 診断つき）
  WebRtcStreamer / VideoStreamer / HttpServer   配信と制御
  Log                      構造化コンソールログ
  main.cpp                 引数解析・スレッド起動・物理/描画ループ
web/index.html             ブラウザ UI（ビルド時にコピー、リロードで反映）
assets/
  materials/*.mat          matc でビルド時に .filamat へコンパイル
  textures/ground.png      地面テクスチャ（差し替え可）
  scenes/*.json            エディタで保存したシーン（sample_joints.json 同梱）
  *.hdr / *.glb            環境マップ・モデル（各自配置、git 管理外）
```

## 必要なもの

- C++17 コンパイラ、CMake ≥ 3.21
- **Filament**: CMake が公式プレビルドを自動ダウンロードします（既定 1.74.0、
  `-DFILAMENT_VERSION=` で変更、`-DFILAMENT_ROOT=` でローカル展開品を使用）
- **Project Chrono** ≥ 9.0: ソースからビルドしてインストール（下記）
- **Eigen**（Chrono の必須依存・ヘッダのみ）:
  Windows は展開するだけ、Linux は `sudo apt install libeigen3-dev`
- **GStreamer** runtime + development:
  - Windows: <https://gstreamer.freedesktop.org/download/> の MSVC 64-bit 両 MSI
    （Complete 推奨）。pkg-config（例 `choco install pkgconfiglite`）と
    `PKG_CONFIG_PATH` の設定は `CMakePresets.json` の windows プリセット参照
  - Linux: `libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev` と
    plugins-base / good / ugly（H.264 の `x264enc` は ugly）
  - H.264 エンコーダは AMD AMF → Media Foundation → x264 → openh264 の順で
    自動選択されます（起動ログに採用されたものが出ます）
- cpp-httplib / nlohmann-json / cgltf / stb_image は `third_parties/` の
  **git サブモジュール**として取り込みます:

  ```bash
  git clone --recursive <このリポジトリ>       # 最初から一緒に取得
  git submodule update --init                  # 既存クローンに後から取得
  ```

  サブモジュール未取得でもビルドは可能です（configure 時に従来どおり
  単一ヘッダを自動ダウンロードするフォールバックが働きます）

### Chrono のビルド例（Windows）

```
git clone https://github.com/projectchrono/chrono.git
git -C chrono switch --detach 9.0.0
cmake -S chrono -B chrono_build -G "Visual Studio 17 2022" -A x64 ^
  -DEIGEN3_INCLUDE_DIR=C:/dev/eigen-3.4.0 ^
  -DCMAKE_INSTALL_PREFIX=C:/path/to/WizEngine/third_parties/chrono-install
cmake --build chrono_build --config Release -j
cmake --install chrono_build --config Release
```

`third_parties/` はサードパーティ依存の置き場です。ヘッダ系ライブラリ
（cpp-httplib / json / cgltf / stb）は git サブモジュールとして管理し、
Chrono や Filament のローカルインストール品（`chrono-install` /
`filament-*`）は git 管理外です。
`CMakePresets.json` の windows プリセットは
`third_parties/chrono-install`（と、あれば
`third_parties/filament-v1.74.0-windows`）を自動で参照するので、この場所に
インストールすればプリセットの編集は不要です。Filament のローカルコピーが
無ければ自動ダウンロードにフォールバックします。

Multicore バックエンドを使う場合は Chrono を MULTICORE モジュール付きでビルドし、
本プロジェクトを `-DWIZ_USE_MULTICORE=ON` で構成してください（無ければ自動で
Core にフォールバックします）。

## ビルド

```bash
cmake -S . -B build -DChrono_DIR=/path/to/chrono-install/lib/cmake/Chrono
cmake --build build -j
```

Visual Studio は「フォルダーを開く」で `CMakePresets.json` を読み込めます。
Chrono を `third_parties/chrono-install` に入れていればプリセットはそのまま動きます
（別の場所なら `CHRONO_ROOT` を、GStreamer が既定以外なら `PKG_CONFIG_PATH` を編集）。

> **重要**: Chrono を Release でインストールした場合、本体も **Release** で
> ビルドしてください。Debug/Release 混在は C ランタイム不整合により
> 「リンクは通るが物理だけ動かない」症状になります。

## 実行

```bash
cd build
./wizengine                 # web モード（既定）: http://127.0.0.1:8080/cam0/
./wizengine web 9000        # ポート指定（全カメラが1ポート、パスで分岐）
./wizengine window          # ローカルウィンドウ表示
./wizengine stream 192.168.1.10 5000   # RTP/UDP 配信
./wizengine rtsp rtsp://...            # RTSP 配信
./wizengine --codec av1                   # コーデック指定(h264/h265/av1/vp9)
./wizengine --encoder amfh264device1enc   # エンコーダ要素(=GPU)を指定
./wizengine --max-cameras 8               # カメラスロット数(1-16、既定5)
./wizengine --help          # CPU 固定などのオプション一覧
```

web モードでは単一ポート上の `/cam0/` `/cam1/` `/cam2/` に各カメラのページが
あり（`/` は `/cam0/` へリダイレクト）、各ページは同時に 1 人が操作できます
（サイドバーの Cameras から空きカメラへ移動）。

### ブラウザ操作

| 操作 | マウス | タッチ |
|---|---|---|
| オブジェクトを掴む | 左ドラッグ（物体上） | 1 本指（物体上） |
| オービット | Ctrl + 左ドラッグ / 何もない所を左ドラッグ | 1 本指（空間） |
| パン | Ctrl + 右 or 中ドラッグ | 2 本指ドラッグ |
| ズーム | Ctrl + ホイール | ピンチ |
| サイドバー / 全画面 | Tab / Alt+F | — |

エディタモードでは「掴む」の意味が変わります。力で押すのではなく、掴んだ物が
カーソルにぴったり付いてきて、離した場所が新しい配置になります。離しても選択は
外れないので、そのまま Inspector タブで数値を詰められます。

### エディタモード

サイドバー上部の **✎ エディタ / ▶ シミュレート** で切り替えます。エディタ側では
物理が止まり、シミュレート側に入るとその配置から実行が始まります。戻ると全部が
「置いた場所」へ巻き戻るので、何度でも試せます（`⟲ Reset` も同じ意味です）。
**モードの切り替えはどのカメラのページからでも**行えます。

**シーンを書き換えるエディタ操作は「エディタカメラ」のページ専用です**（既定は
camera 0 = `/cam0/`、`SceneConfig.h` の `kEditorCamera` で変更）。サイドバーの
Cameras 一覧やヘッダでは、このカメラは番号ではなく **Editor Camera** と表示され
ます。配置・ギズモ・ジョイント・保存はそのページからだけ行え、ギズモもその
カメラの選択にだけ出ます。他のカメラはモード切替と見る・選ぶはできますが、
シーンは書き換えられません（Inspector タブ自体が表示されません）。Editor Camera
はあくまで「編集ができる」カメラであって、モードを独占するわけではありません -
複数人で同じシーンを見ながら、編集は 1 人、という分担がそのまま画面になります。

サイドバーのタブは 3 つです:

| タブ | 内容 |
|---|---|
| Scene | 再生/リセット、カメラ・オブジェクト一覧、イベント、選択の要約 |
| Inspector | エディタ操作（下表）。エディタカメラのページにだけ表示 |
| Physics | シミュレート設定（シーンに保存）、ソルバー、レート、接触、配信 |

**Inspector タブ**は Unity と同じく「**選択しているオブジェクトの内容**」だけを
表示します:

| 節 | 内容 |
|---|---|
| 選択オブジェクト | 名前・**Transform（位置 / 回転 / スケール、Unity 風の X/Y/Z 欄）**・質量・固定（土台）・色、複製、削除 |
| ジョイント | 選択中だけ表示。種類と軸を選び、A（現在の選択）と B（既定は地面）を繋ぐ。一覧は**選択が関わるジョイントのみ** |
| ライト | ライト選択中だけ表示。名前・Transform（位置 / 向き）・色・強さ・届く距離・円錐（Spot）、削除 |
| カメラ | カメラ選択中だけ表示。Transform（位置 / 向き）・そのカメラのページへ移動・削除 |
| ギズモ設定 | スナップと刻み、グリッド表示と間隔。普段は隠れていて、**映像左上ツールバーの ⚙ で開閉** |

オブジェクトの新規配置（サイズ・色の初期値）とシーンの**保存 / 全消し**は、
ビュー下の **Assets パネル**上部の操作列にあります（読込はタイルの
ダブルクリック）。

重力・摩擦・反発・減衰・スリープ・物理レートは **Physics タブ**にあります。
どのカメラのページからでも変えられ、変えた値はシーンの保存に含まれます
（Solver / Rate / Contacts で変えた値も同様に保存へ反映されます）。

ジョイントは 5 種類です。**ちょうつがい**（軸まわりの回転だけ）、**ボール**
（位置だけ固定）、**固定**（溶接）、**直動**（軸方向のスライドだけ）、
**距離**（2 点間の距離を保つ）。ビューには種類ごとの色で線が引かれ、エディタ中は
A →軸→ B、シミュレート中は A → B を結びます。

同梱の `sample_joints.json` を読み込むと、ちょうつがい・距離・ボールの 3 種類が
入った小さな仕掛けが出ます（Assets パネルの `sample_joints` タイルを
ダブルクリック）。

### ギズモ

選択したオブジェクトに Unity 風のハンドルが出ます。**ブラウザのオーバーレイでは
なく 3D の線としてシーンに描かれる**ので、映像と必ず同じフレームに乗り、手前の
物にも隠れます。モード切替のボタンは Unity のシーンビューと同じく**映像の
左上に重ねて**表示されます（✥ 移動 / ⟳ 回転 / ⤢ 拡縮、World ⇄ Local の
トグル、⚙ ギズモ設定。エディタモード中のエディタカメラのページのみ）。

| 操作 | 内容 |
|---|---|
| **W / E / R** | 移動 / 回転 / 拡縮 の切り替え（映像左上のボタンでも可） |
| **X** | スナップの ON / OFF（刻みは移動 m・回転 度・寸法 m で指定） |
| 矢印をドラッグ | その軸に沿って移動 |
| 四角をドラッグ | その平面上を移動（2 軸同時） |
| リングをドラッグ | その軸まわりに回転 |
| 軸先の箱 | その軸だけ拡縮（球とモデルは一様） |
| 中央の箱 | 一様に拡縮 |
| World / Local | 軸をワールドに合わせるか、オブジェクトの向きに合わせるか（左上のボタンで切り替え） |

カーソルを乗せるとハンドルが黄色く光ります。ハンドル以外を掴んだときは従来
どおりの自由移動（カメラ平面に沿って動く）なので、ざっくり置いてから
ギズモで詰める、という流れで使えます。ギズモが出るのはエディタカメラ
（既定 camera 0）のビューだけで、他のカメラには映りません（Filament の
レイヤマスクでビュー単位に隠しています）。

エディタ中は **Y=0 に 100×100 m の格子グリッド**も出ます（同じくエディタ
カメラのみ）。作業の目安なので描画は 1 ピクセルの線（軽量）で、原点を通る
2 本は軸の色（X=赤 / Z=青）＝向きの目印になります。表示の ON/OFF と間隔
（0.25〜10 m、既定 1 m）は映像左上の **⚙（ギズモ設定）**から変えられます。グリッドは
物理の床（`kGroundSize`、既定 20 m）より広いことに注意してください -
床の外に置いた物はシミュレートで落下します。

### ライトとカメラの編集

**エディタモード中のエディタカメラのビューには、ライト（黄）とカメラ（水色）の
線画アイコン**が出ます。クリックで選択でき、オブジェクトと同じギズモで
**移動 / 回転**できます（拡縮はありません）。数値は Inspector のライト / カメラ
パネルで詰められます。**編集できるのはエディタモードだけ**で、シミュレート中は
アイコンも出ません。

- **ライト**: Assets パネルの **💡 Point / 🔦 Spot / ☀ Sun** タイルで追加します
  （Point / Spot はカメラ正面の少し上、真下向きで置かれる）。色・強さ・位置・
  向きはいつでも変えられ、**種類と影は作成時に固定**です（変えたいときは削除
  して置き直し）。向きは「回転ゼロ = 真下」。強さの単位は Sun がルクス
  （太陽 ~10 万）、Point / Spot がルーメン（屋外シーンでは数十万が目安）。
  Objects 一覧のライト行のクリックでも選択できます。
- **カメラ**: Cameras 一覧の行の **✎ で選択**（Inspector のカメラパネルが
  開く）、見出し横の **＋ で追加**、**各行の 🗑**（または Inspector の
  削除ボタン）**で削除**できます。✎ / 🗑 はエディタモード中いつでも各行に
  出ています（上限は既定 5。exe 引数 `--max-cameras N` で 1〜16 に変更でき、
  既定値そのものは `SceneConfig.h` の `kMaxCameras`）。
  ページの URL は起動時に上限ぶん用意されますが、**動画ビュー（描画と
  ストリームの実体）は追加した時点で生成**されます。削除すると一覧から
  消えますが、ページの URL 自体は生きています（次の追加で同じ番号とビューを
  再利用）。
  位置と向き（ピッチ / ヨー。ロールは無し）を編集でき、選択中のカメラの
  Inspector からそのカメラのページへ移動できます。**Editor Camera 自身は選択・
  削除できません**（自分の目はオービット操作で動かすため）。
- ライトとカメラは**シーンの保存 / 読込に含まれます**（保存形式 version 2。
  旧形式のシーンを読むとライト・カメラは初期構成に戻ります）。

ビューの下には **Assets パネル**（Unity の Project ビュー相当）が出ます。
Box / Sphere / ライト（Point・Spot・Sun）のタイルをクリックするとカメラ正面に
配置、保存済みシーンのタイルはクリックで名前を選び、**ダブルクリックで読込**
します（現在の配置が置き換わるため確認ダイアログ付き）。見出し下の操作列で、新しく置く
オブジェクトの**初期値（大きさ・色）**と、シーンの **💾 保存 / 🗑 全消し**が
行えます（保存名は英数字と `_ -` のみ）。見出しのクリックで折りたたみ、
textarea と同じ要領で**右下のつまみ**をドラッグすると高さが変わります
（どちらも記憶され、入りきらないタイルは縦スクロールします）。こちらもエディタモード中の Editor Camera ページ
専用です。

形や大きさの変更は、シミュレートを始める瞬間に剛体へ反映されます（エディタ中は
物理を回していないので、スライダーやギズモを動かすたびに剛体を作り直さずに
済みます）。

起動時のモードは `src/SceneConfig.h` の `kStartMode` で変えられます（既定は
`Simulate` ＝従来どおりの挙動）。

## シーンをいじる

`src/SceneConfig.h` がすべての入口です。例:

- 剛体の数・形状: `kNx/kNy/kNz`, `kBodyShape`(Box / Sphere / **ConvexHull** =
  `kBoxModelPath` のメッシュ凸包で衝突), `kBoxSize`
- 静的メッシュ衝突: `kModelCollision = true` で `kModelPath` のモデルが
  トライアングルメッシュの障害物になります（固定ジオメトリ専用）。
  高ポリモデルは物理が固まるので、`kModelCollisionPath` に
  Decimate した衝突専用の低ポリ版（数百〜数千三角形）を指定してください
- ライト: `lightConfigs()`（Directional / Point / Spot、色・強度・影）。
  実行時にも `scene.light(i).setDirection(...)` などで動かせます
- 環境光: `kEnvironmentHdr = "studio.hdr"` — `assets/` に Radiance .hdr を
  置くだけ（<https://polyhaven.com/hdris> の 2k で十分）。リポジトリには
  同梱していないので各自取得してください
- glTF モデル: `kModelPath`（置物）/ `kBoxModelPath`（剛体の見た目差し替え）
- 配信コーデック等（H264 / H265 / AV1 / VP9、ビットレート、GPU色変換）は
  シーンではなくエンジン設定として `main.cpp` 冒頭で定義。起動時は
  `--codec` / `--encoder`、実行中はブラウザの Physics > Stream から変更可
- ソルバー: `kSolverIterations`, `kPhysicsHz`, `kSubsteps` ほか
  （実行中はブラウザの Physics タブからも変更可）
- 配信フォーマットはブラウザからカメラごとに変更可: サイドバー Physics タブの
  **Stream** セクションで解像度 / FPS / ビットレートを選び「Apply & reconnect」
  （再接続で反映。起動時の既定は `kWidth/kHeight/kFps/kVideoBitrate`）

`web/index.html` は実行フォルダの `assets/web/` に直接コピー + リロードで
再ビルドなしに試せます。

## トラブルシューティング

- **起動直後に落ちる / ERROR: asset '...'**: 実行フォルダに `assets/` 一式が
  あるか（ビルドが実行ファイルの隣にコピーします）。`.hdr` は Radiance 形式
  のみ対応 — `.exr` をリネームしたファイルはエラーメッセージ内の診断で分かります
- **Linux ヘッドレスで GL 初期化に失敗**: `FILAMENT_BACKEND=vulkan` を試す
- **`HandleAllocator arena is full` 警告**: `Renderer.cpp` の
  `driverHandleArenaSizeMB`（既定 128）を増やす。シーンの剛体数を大きく
  増やしたときに再発することがあります
- **黒画面のまま**: 起動ログの `webrtc:` 行でエンコーダとペイロード番号を確認

## ライセンス

[MIT License](LICENSE)

依存ライブラリのライセンスは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
を参照してください。
