<p align="center">
  <img src="web/logo.png" alt="Charon" width="260">
</p>

# Charon

**Charon** は、サーバー側で物理シミュレーション（Project Chrono）と描画
（Google Filament）をヘッドレスに実行し、映像を WebRTC でブラウザへ配信する
リアルタイム 3D エンジンです。ブラウザからシーンを組み立て、走らせ、
保存できます。専用クライアントは要りません。

Headless real-time 3D engine: Chrono physics + Filament rendering on the
server, streamed to browsers over WebRTC with an interactive web editor.
Codename "WizEngine". (Docs below are in Japanese.)

現在の版: **Charon 1.0.0 "WizEngine"**（WizEngine は 1.x のコードネーム。
名前空間・実行ファイル `wizengine`・シーン文書のルート要素 `<wizengine>` も
この名前です）。

## 目次

1. [できること](#できること)
2. [構成](#構成)
3. [ビルド](#ビルド)
4. [実行](#実行)
5. [ブラウザでの使い方](#ブラウザでの使い方)
6. [シーン文書（XML）](#シーン文書xml)
7. [物理](#物理)
8. [描画](#描画)
9. [設定の場所](#設定の場所)
10. [トラブルシューティング](#トラブルシューティング)
11. [ライセンス](#ライセンス)

## できること

| 領域 | 内容 |
|---|---|
| エディタ | ブラウザ上で箱・球・glTF モデル・ライト・カメラを置き、Unity 風のギズモで移動 / 回転 / 拡縮。物理を止めた「エディタ」と走らせる「シミュレート」を切り替え、止めれば配置は元に戻る |
| 物理 | Project Chrono。14 種のジョイント（可動範囲・モータ・ばね・破断付き）、ボディごとの材質・衝突レイヤ・初速、FEA ケーブル、ソフトボディ（質点ばね）、レイキャスト式の車両、URDF / OpenSim / ADAMS の取込 |
| バックエンド | Chrono Core と Multicore（OpenMP 並列）をシーンの中身で自動選択 |
| イベント | Node-RED 風のノードエディタで「衝突したら色を変える」などを設計。名前付きアセットとしてオブジェクトに付け回せる |
| 描画 | Filament（Vulkan / OpenGL、ヘッドレス）。PBR 材質、PCSS 影、SSAO、SSR、ブルーム、被写界深度、物理カメラの露出、ACES トーンマップ、HDR 環境マップ（起動時に GPU でプリフィルタ） |
| 配信 | GStreamer + WebRTC。H.264（GPU エンコード対応）/ H.265 / AV1 / VP9。カメラごとに独立したページと視聴セッション |
| 保存形式 | MuJoCo（MJCF）風の XML 1 ファイル。テキストエディタで読め、ブラウザからも直接編集できる |

## 構成

```
[入力スレッド]   ブラウザからの JSON コマンド → Scene::dispatchCommand
[物理スレッド]   編集キューの適用 → Simulate なら Chrono を固定ステップで実行
                                   → Editor なら掴んだ物の置き直しだけ
[描画スレッド]   姿勢スナップショットを Filament へ → 描画 → 非同期 readPixels
                 → GStreamer → WebRTC
[HTTP スレッド]  UI 配信・WebRTC シグナリング・/scene・/stats
```

Chrono を触るのは物理スレッド、Filament を触るのは描画スレッドだけです。
ブラウザの操作はキューに積まれ、物理スレッドが剛体を作り、描画スレッドが次の
フレームで見た目を作ります。

主なディレクトリ:

```
src/
  main.cpp        起動・引数・2 スレッドのループ
  core/           ログ・版・アセットエラー・CPU 固定
  physics/        PhysicsWorld（Chrono ラッパ）、glTF 凸包、モデル取込
  render/         Renderer（Filament）、glTF・HDR・画像の読み込み
  streaming/      HTTP サーバ、WebRTC、GStreamer
  document/       シーン文書の型と XML の読み書き（エンジン非依存）
  scene/          Scene（実体管理）、EditorState、SceneConfig.h（既定値）
  components/     エディタ・ギズモ・物理設定・車両・プレハブ
  vehicle/        車両モデルとノード式（純粋な数値ライブラリ）
web/              ブラウザ UI（index.html / style.css / app.js / logo.png）
assets/           実行時に読むもの（materials / textures / scenes）
third_parties/    依存ライブラリのサブモジュール
```

## ビルド

### 必要なもの

| 依存 | 入手 |
|---|---|
| C++17 コンパイラ、CMake 3.21 以上 | Windows は Visual Studio 2022、Linux は GCC / Clang + Ninja |
| Project Chrono 9.0 | ソースからビルドしてインストール（下記）。ソースは `third_parties/chrono` |
| Eigen 3.4 | `third_parties/eigen`（ヘッダのみ。Linux は `libeigen3-dev` でも可） |
| Google Filament 1.74 | CMake が公式プレビルドを自動ダウンロード（`-DFILAMENT_ROOT=` でローカル品） |
| GStreamer 1.x | Windows: 公式 MSVC 64-bit の runtime と development 両方の MSI。Linux: `libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev` と plugins-base / good / bad / ugly |
| LuaJIT、nlohmann/json、cpp-httplib、cgltf、stb | `third_parties/` のサブモジュール（無ければ configure 時に単一ヘッダを自動取得） |

サブモジュールは最初に取得しておきます:

```bash
git clone --recursive <このリポジトリ>
# 既存のクローンなら
git submodule update --init --recursive
```

### Chrono のビルド（Windows の例）

リポジトリのルートで実行します。

```
cmake -S third_parties/chrono -B chrono_build -G "Visual Studio 17 2022" -A x64 ^
  -DEIGEN3_INCLUDE_DIR=%CD%/third_parties/eigen ^
  -DCMAKE_INSTALL_PREFIX=%CD%/third_parties/chrono-install
cmake --build chrono_build --config Release -j
cmake --install chrono_build --config Release
```

Multicore モジュール（大量の剛体・ソフトボディの並列化）も使うなら、
Blaze と Thrust を足します:

```
cmake -S third_parties/chrono -B chrono_build -G "Visual Studio 17 2022" -A x64 ^
  -DEIGEN3_INCLUDE_DIR=%CD%/third_parties/eigen ^
  -DCH_ENABLE_MODULE_MULTICORE=ON ^
  -DBlaze_ROOT_DIR=%CD%/third_parties/blaze ^
  -DThrust_DIR=%CD%/third_parties/thrust/thrust/cmake ^
  -DCMAKE_INSTALL_PREFIX=%CD%/third_parties/chrono-install
```

Chrono 側の `USE_MSVC_STATIC_RUNTIME` は OFF のまま（/MD）にしてください。
Filament と GStreamer が /MD 前提なので、ここが食い違うとリンクエラーになります。

### Charon のビルド

```bash
cmake -S . -B build -DChrono_DIR=/path/to/chrono-install/lib/cmake/Chrono
cmake --build build -j
```

Visual Studio では「フォルダーを開く」で `CMakePresets.json` が読まれます。
Chrono を `third_parties/chrono-install` に入れていればプリセットはそのまま
使えます（別の場所なら `CHRONO_ROOT`、GStreamer が既定以外なら
`PKG_CONFIG_PATH` を編集）。

CMake オプション:

| オプション | 既定 | 内容 |
|---|---|---|
| `WIZ_USE_MULTICORE` | OFF | Chrono::Multicore をリンクする（Chrono 側も MULTICORE 付きが必要） |
| `WIZ_WITH_LUAJIT` | ON | ノード式を LuaJIT で実行（OFF なら C++ のインタプリタ） |
| `WIZ_WITH_CHRONO_PARSERS` | OFF | URDF / OpenSim / ADAMS の取込 |
| `WIZ_WITH_CHRONO_MODAL` | OFF | モーダル解析 |
| `WIZ_WITH_PARDISO` / `WIZ_WITH_MUMPS` | OFF | 直接法ソルバ |

> **重要**: Chrono を Release でインストールしたら、Charon も **Release** で
> ビルド・実行してください。Debug / Release が混ざると「リンクは通るが物理
> だけ動かない」状態になります。実行ファイルは `build/Release/wizengine.exe`
> です。

## 実行

```bash
cd build
./wizengine                      # web モード（既定）: http://127.0.0.1:8080/cam0/
./wizengine web 9000             # HTTP ポートを指定
./wizengine window               # ローカルウィンドウに表示
./wizengine stream 192.168.1.10 5000   # RTP/UDP 配信
./wizengine rtsp rtsp://...      # RTSP 配信
./wizengine --help               # オプション一覧
```

主なオプション:

| オプション | 内容 |
|---|---|
| `--codec h264\|h265\|av1\|vp9` | 配信コーデック（既定 h264。使えなければ h264 に戻る） |
| `--encoder <要素名>` | GStreamer のエンコーダ要素を指定（複数 GPU の選択に） |
| `--max-cameras N` | カメラのスロット数（1〜16、既定 5） |
| `--physics-cores "0-11"` / `--render-cores "12-15"` | 物理と描画のスレッドを CPU コアに固定 |
| `--physics-threads N` | ソルバのスレッド数 |

環境変数 `WIZENGINE_LOG=debug|info|warn|error` でログのレベルを変えられます。

web モードでは `/cam0/`、`/cam1/`、… がカメラごとのページです（`/` は
`/cam0/` へ）。1 ページを同時に見られるのは 1 ブラウザで、誰も見ていない間は
物理も描画も止まります。

## ブラウザでの使い方

### 基本操作

| 操作 | マウス | タッチ |
|---|---|---|
| オブジェクトを掴む | 物体の上で左ドラッグ | 物体の上で 1 本指 |
| オービット | Ctrl + 左ドラッグ、または空間を左ドラッグ | 空間を 1 本指 |
| パン | Ctrl + 右 / 中ドラッグ | 2 本指ドラッグ |
| ズーム | Ctrl + ホイール | ピンチ |
| サイドバー / 全画面 | Tab / Alt+F | ☰ ボタン |

### 2 つのモード

映像上部のヘッダーにある **✎ エディタ / ▶ シミュレート** で切り替えます。

- **エディタ**: 物理が止まります。掴んだ物はカーソルに付いてきて、離した
  場所が新しい配置になります。離しても選択は残るので、そのまま Inspector で
  数値を詰められます。
- **シミュレート**: いまの配置から Chrono が走ります。エディタに戻すと全部が
  置いた場所へ戻るので、何度でも試せます（⟲ Reset も同じ意味）。

モードの切り替えはどのカメラのページからでもできます。ただし**シーンを書き
換える操作（配置・ギズモ・ジョイント・保存）は Editor Camera（既定 `/cam0/`）の
ページ専用**です。他のカメラは見る・選ぶだけで、Inspector タブも出ません。
複数人で同じシーンを見ながら、編集は 1 人、という分担です。

### サイドバーのタブ

| タブ | 内容 |
|---|---|
| Scene | 再生 / リセット、カメラ・ライト・オブジェクトの一覧、選択の要約 |
| Inspector | World 節（地面・環境光・描画設定）と、選択している物の内容。Editor Camera のページだけ |
| Physics | 重力・摩擦・ソルバ・レート・接触モデル・バックエンド表示・配信設定 |

Inspector は Unity と同じく「選択している物の内容」だけを出します:

| 選択 | 節 |
|---|---|
| オブジェクト | Transform（位置 / 回転 / スケール）・質量・固定・色・材質、物性（摩擦・反発・粘着・衝突レイヤ・重力・初速・定常荷重）、ジョイント、ケーブル、ソフトボディ、車両、プレハブ、付いているイベント |
| ライト | 位置 / 向き・色・強さ・届く距離・円錐角 |
| カメラ | 位置 / 向き、そのページへ移動、削除 |

### Assets パネル（ビューの下）

Unity の Project ビューに相当します。エディタモード中の Editor Camera ページに
出ます。

| タイル | 操作 |
|---|---|
| Box / Sphere / Soft Box / Soft Ball / Stairs / Point / Spot / Sun | クリックでカメラ正面に配置 |
| メッシュ（文書の `<asset><mesh>`） | クリックで配置 |
| ⚡ イベント / 🧮 計算式 / 🧩 プレハブ | クリックで開く、右クリックで作成・付ける・削除 |
| 保存済みシーン | ダブルクリックで読込（確認あり） |
| 📄 XML | いまのシーンの XML をその場で編集して適用 |
| 📥 取込 | URDF / OpenSim / ADAMS を今のシーンに足す（Parsers 付きビルド） |

見出し下の操作列に、新規オブジェクトの初期値（大きさ・色）、シーン名、
**💾 保存**、**🗑 全消し** があります。

### ギズモ

選択したオブジェクトに Unity 風のハンドルが出ます。3D の線としてシーンに
描くので、映像と同じフレームに乗り、手前の物にも隠れます。モード切替は
映像左上のツールバー（✥ 移動 / ⟳ 回転 / ⤢ 拡縮、World ⇄ Local、⚙ 設定）。

| キー / 操作 | 内容 |
|---|---|
| W / E / R | 移動 / 回転 / 拡縮 |
| X | スナップの ON / OFF（刻みは ⚙ で） |
| 矢印 / 四角 / リング / 軸先の箱 / 中央の箱 | 軸移動 / 平面移動 / 回転 / 軸拡縮 / 一様拡縮 |

エディタ中は Y=0 に 100×100 m のグリッドも出ます（⚙ で表示と間隔）。物理の
床（`<ground size>`、既定 ±10 m）より広いので、床の外に置いた物はシミュレートで
落ちます。

### ライトとカメラ

エディタモード中の Editor Camera のビューには、ライト（黄）とカメラ（水色）の
線画アイコンが出ます。クリックで選択し、ギズモで移動 / 回転できます。

- ライトは Assets パネルの Point / Spot / Sun タイルで追加。種類と影は作成時に
  決まり、色・強さ・位置・向きは後から変えられます。強さの単位は Sun が
  ルクス、Point / Spot がルーメン。
- カメラは Scene タブの Cameras 一覧の ＋ で追加、行の 🗑 で削除、
  ダブルクリックでそのページへ移動、目のアイコンで小窓に表示
  （Chrome / Edge は Picture-in-Picture）。Editor Camera 自身は選べません。

### イベント（ノードエディタ）

ツールバーの ⚡ でノードエディタが開きます（画面の 9 割のウィンドウ、
Esc で閉じる）。

- **トリガー**: 衝突したら（新しく触れた瞬間だけ）、開始したら、タイマー、
  掴んだら、ジョイントが破断したら。
- **アクション**: 色を変える、力を加える、速度を与える、固定 / 解除、
  引き寄せる、ライトの色 / 強さ、カメラの注視、モータの目標。
- 中身は**名前付きのイベントアセット**で、オブジェクトかシーン全体に付けて
  初めて動きます。同じアセットを複数のオブジェクトに付け回せます。
- 対象を書かないノードは、実行時に「付けたオブジェクト」や「触れた相手」を
  指します。
- マウスで掴んだ物を引き寄せる動きも既定アセット `pickup` の仕事です。外せば
  掴んでも動きません。
- 実行はシミュレート中だけ。発火したノードには ⚡n のバッジが付きます。
  アクションが変えた色・固定は、止めると元に戻ります。

## シーン文書（XML）

シーンの中身（配置・モデル・ジョイント・イベント・ライト・カメラ・描画設定）は
`assets/scenes/<名前>.xml` の 1 ファイルです。💾 保存で書き、タイルの
ダブルクリックで読み、起動時は `SceneConfig.h` の `kStartupScene`（既定
`default`）を読みます。書式は MuJoCo（MJCF）に寄せてあります。

```xml
<wizengine model="sample" version="4">
  <option gravity="0 -9.81 0" rate="60" substeps="2" iterations="60"/>
  <asset>
    <mesh name="apple" file="apple.glb" scale="1"/>
  </asset>
  <worldbody>
    <environment hdr="studio.hdr" intensity="30000" skybox="true"/>
    <ground size="10" visual="8" texture="textures/ground.png" tile="2"/>
    <light name="key" type="spot" pos="1.5 4 -2" euler="35 -20 0"/>
    <camera name="cam0" target="0 1 0" azimuth="37.8" elevation="19.5" radius="12"/>
    <body name="post" pos="0 1 0" fixed="true">
      <geom type="box" size="0.1 1 0.1" mass="20" rgba="0.42 0.45 0.5 1"/>
    </body>
    <body name="arm" pos="1 2 0">
      <geom type="mesh" mesh="apple" size="0.1" mass="0.2"/>
    </body>
  </worldbody>
  <equality>
    <joint name="hinge" type="hinge" body1="arm" body2="post"
           anchor="0 1.9 0" axis="0 0 1"/>
  </equality>
</wizengine>
```

- `<geom size>` は MuJoCo と同じ**半分の寸法**（box は各辺の半分、sphere と
  mesh は半径）。角度は**度**、色は `rgba`（リニア値）。
- `body1` / `body2` は名前でも番号でも書け、`world`（-1）が地面。
- 打ち間違いは読み飛ばして警告になります（コンソールに内容、ステータスに
  件数）。手書きの XML はそこで答え合わせできます。
- 読むだけなら `/cam0/scene.xml` でいまの中身が取れます。
- 旧形式の `.json` は読み込みのみ。保存は常に `.xml`。

同梱のシーン:

| ファイル | 内容 |
|---|---|
| `default.xml` | 起動時のシーン（箱の山と球） |
| `sample_joints.xml` | ちょうつがい・距離・ボールの小さな仕掛け |
| `mechanisms.xml` | モータ・可動範囲・ばね・歯車・破断・材質・衝突レイヤ・初速・無重力 |
| `flexible.xml` | FEA ケーブル・ブッシュ・定常荷重（Core 専用機能） |
| `softbody.xml` | ソフトボディ |
| `vehicle.xml` | 車両（ソフトタイヤ、ノード式のタイヤモデル） |
| `photoreal.xml` | 材質と描画設定の見本 |

## 物理

### バックエンドの自動選択

Chrono の系はシーンの中身で自動的に決まります。いま何で動いていて、なぜかは
Physics タブの「バックエンド」と画面下の `engine` に出ます。

1. Multicore が扱えない機能（FEA ケーブル、ブッシュ / 定常荷重、線形・直接法
   ソルバ、HHT / Newmark、モーダル解析）を使っていれば **Core**。
2. それが無く、ソフトボディがあるか剛体が 200 個以上なら **Multicore**
   （`WIZ_USE_MULTICORE` 付きのビルドのとき）。
3. どちらでもなければ既定の **Core**。

Core は Chrono の全機能が使えてスリープも効き、数十個の剛体なら Multicore より
速いか同等です。Multicore は接触が大量に立つ場面で効きます。切り替えは
シミュレート開始・設定変更・読込の時点で行われます。しきい値は
`SceneConfig.h` の `kMulticoreForSoftBodies` / `kMulticoreMinBodies` です。

### ジョイント

固定・ちょうつがい・ボール・直動・距離・自在継手・円筒・平面・点‐線・点‐面・
歯車・ねじ・ばね・ブッシュの 14 種。ちょうつがい / 直動には可動範囲と
モータ（速度・位置・力）、どの拘束にも破断（反力がしきい値を超えたら外れ、
`onJointBreak` が発火）を付けられます。シミュレート中は反力が一覧に出ます。

### Physics タブで選べるもの

重力 3 成分、積分器（Euler / 射影 / 陰解法 / 台形則 / HHT / Newmark）、
接触ソルバ（BB / APGD / PSOR / Jacobi / ADMM / PMINRES / MINRES と直接法の
SparseLU / SparseQR / Pardiso / MUMPS）、材質の合成方式、接触モデル（NSC /
SMC）、モーダル解析の本数。線形・直接法のソルバは接触モデルが SMC のときだけ
使えます。ケーブルのある系ではソルバが自動で ADMM になります。

### その他

- **ケーブル**: Chrono の FEA（ANCF）。Inspector の「ケーブル」節か `<cable>` で
  張り、節点の球で床や物と接触します。
- **ソフトボディ**: 質点ばね方式。箱 / 球の格子（1 軸 2〜8 個）を陰解法の
  ばねで結びます。Inspector の「ソフトボディ」節か `<soft>`。
- **車両**: レイキャスト式の車輪とグラフ型のパワートレイン。W / S / A / D /
  Space で運転。タイヤの式はノードエディタ（🧮）で差し替えられます。
- **プレハブ**: 見た目の部品の集合。階段（Stairs）や車体の飾りがこれです。
  右クリック「プレハブを編集」で部品を動かせます。
- **取込**: URDF / OpenSim / ADAMS のボディ・当たり形状・ジョイントを足します
  （見た目のメッシュは読みません）。

## 描画

Inspector の World 節「描画」がシーンの `<visual>` を編集します。Draft /
Standard / Photo の 3 ボタンで一括切替、下の欄で個別調整（影の種類と解像度・
MSAA・AO・ブルーム・反射・ビネット・被写界深度・露出・トーンマップ）。
オブジェクトの材質（粗さ / 金属 / 反射率 / クリアコート / 自己発光）は
選択オブジェクトの「材質」欄です。

いちばん効くのは環境マップです。`assets/` に .hdr を置いて
`<environment hdr="studio.hdr" intensity="25000" skybox="true"/>` と書くと、
映り込みと背景が一致します。HDR はリポジトリに含まれていないので、
<https://polyhaven.com/hdris> などから 2k の Radiance 形式（.hdr）を 1 つ
置いてください。無ければ一様な環境光で起動します。

配信の解像度・FPS・ビットレートは Physics タブの Stream 節でカメラごとに
変えられます。

## 設定の場所

| 変えたいもの | 場所 |
|---|---|
| シーンの中身 | `assets/scenes/*.xml`（エディタで保存、または手で編集） |
| 起動時のシーン・モード、カメラ数、ソルバの既定値、Multicore のしきい値 | `src/scene/SceneConfig.h` |
| 製品名・版・コードネーム | `src/core/Versions.h` |
| 配信コーデックの既定 | `src/main.cpp` 冒頭（起動時は `--codec` / `--encoder`） |
| CPU コアの割り当て | 起動引数（`--physics-cores` など） |
| ブラウザ UI | `web/`（ビルドが `assets/web/` へコピー。リロードで反映、再ビルド不要） |

## トラブルシューティング

| 症状 | 見るところ |
|---|---|
| 起動直後に落ちる / `ERROR: asset '...'` | 実行フォルダに `assets/` 一式があるか。`.hdr` は Radiance 形式のみ（`.exr` のリネームはエラー内の診断で分かる） |
| 物理だけ動かない | Debug / Release の混在。Chrono と同じ構成でビルドする |
| シミュレート開始で落ちる・エディタへ戻る | コンソールの `LOGE` 行に Chrono の例外理由が出る。3 回続くと物理を止めて描画だけ続ける |
| 黒画面のまま | 起動ログの `webrtc:` 行でエンコーダとペイロード番号を確認。GStreamer の webrtc / nice プラグインが要る |
| Linux ヘッドレスで GL 初期化に失敗 | `FILAMENT_BACKEND=vulkan` を試す |
| `HandleAllocator arena is full` 警告 | `Renderer.cpp` の `driverHandleArenaSizeMB`（既定 128）を増やす |
| 拘束の付いた物が消える・NaN | 起動時の `diag step` 行とジョイントごとの反力ログを見る |

## ライセンス

Charon は [MIT License](LICENSE) です。依存ライブラリのライセンスは
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を参照してください。
