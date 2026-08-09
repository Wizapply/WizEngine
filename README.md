# WizEngine

Headless real-time 3D streaming prototype: **Project Chrono** physics and
**Google Filament** rendering on the server, H.264-encoded with **GStreamer**
and streamed to browsers over **WebRTC** — with a multi-camera, interactive
web UI. (Docs below are in Japanese.)

サーバー側で **Project Chrono**（物理）と **Google Filament**（描画）をヘッドレスに
実行し、フレームを **GStreamer** で H.264 エンコードして **WebRTC** でブラウザへ
配信する、リモートレンダリング／ピクセルストリーミングのプロトタイプです。

## 特徴

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
         （CameraControl / BoxControl / PhysicsControl）
[physics thread] 固定タイムステップで Chrono を実行 → 姿勢スナップショット
[main/render thread] スナップショットを Filament に反映 → 描画
      → 非同期 readPixels → GStreamer appsrc → H.264 → WebRTC
[http threads]  カメラごとの HTTP サーバ（UI 配信・シグナリング・stats）
```

主なソース:

```
src/
  SceneConfig.h            シーンの全パラメータ（まずここを編集）
  scene.cpp / Scene.h      シーン実装・階層 JSON・コンポーネント登録
  CameraObject / LightObject   シーンオブジェクト（atomic 状態 + applyTo）
  BoxController            グラブ（掴んで引っ張る）制御
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
- cpp-httplib / nlohmann-json / stb_image はビルド時に単一ヘッダとして
  自動ダウンロードされます（クローン不要）

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

`third_parties/` はローカル依存の置き場です（git 管理外）。
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
  `--codec` / `--encoder`、実行中はブラウザの System > Stream から変更可
- ソルバー: `kSolverIterations`, `kPhysicsHz`, `kSubsteps` ほか
  （実行中はブラウザの System タブからも変更可）
- 配信フォーマットはブラウザからカメラごとに変更可: サイドバー System タブの
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
