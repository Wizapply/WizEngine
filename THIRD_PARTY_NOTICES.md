# Third-party notices

Charon（旧コードネーム WizEngine）が使っている外部ソフトウェアとそのライセンスの
一覧です。Charon 自身は [MIT License](LICENSE) です。

版は `CMakeLists.txt` と `.gitmodules` で固定しているものを書いています。
環境によって差し替えた場合（`-DFILAMENT_VERSION=...`、システムの GStreamer
など）は、そちらの版の条文が適用されます。各ライセンスの全文は、それぞれの
配布物に同梱されているものを参照してください（サブモジュールは
`third_parties/<名前>/` の `LICENSE` / `COPYING`）。

## 一覧

| ソフトウェア | 版 | ライセンス | 使い方 |
|---|---|---|---|
| [Project Chrono](https://projectchrono.org/) | 9.0.0 | BSD-3-Clause | 物理エンジン。静的 / 動的リンク（Core、任意で Multicore・Parsers・Modal・Pardiso / MUMPS モジュール） |
| [Eigen](https://eigen.tuxfamily.org/) | 3.4.0 | MPL-2.0（一部のファイルは BSD / LGPL - 既定の設定では MPL-2.0 の範囲のみ） | ヘッダのみ。Chrono の線形代数と `Versions.cpp` の版表示 |
| [Blaze](https://bitbucket.org/blaze-lib/blaze) | 3.8.2 | BSD-3-Clause | ヘッダのみ。Chrono::Multicore のビルドにだけ要る |
| [Thrust](https://github.com/NVIDIA/thrust)（CUB を同梱） | 1.17.2 | Apache-2.0（CUB は BSD-3-Clause） | ヘッダのみ。Chrono::Multicore の OpenMP バックエンド |
| [LuaJIT](https://luajit.org/) | 2.1 | MIT（Lua 5.1 を同梱、MIT） | ノード式の実行エンジン。静的リンク（`-DWIZ_WITH_LUAJIT=OFF` で外せる） |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | MIT | ヘッダのみ。ブラウザ API と設定の JSON |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.15.3 | MIT | ヘッダのみ。操作ページ・WHEP・`/input` の HTTP サーバ |
| [cgltf](https://github.com/jkuhlmann/cgltf) | 1.14 | MIT | ヘッダのみ。glTF の当たり判定用メッシュ読み込み |
| [stb](https://github.com/nothings/stb)（`stb_image.h`） | master | MIT または Public Domain（Unlicense）の選択 | ヘッダのみ。テクスチャと HDR 環境マップの読み込み |
| [Google Filament](https://github.com/google/filament) | 1.74.0 | Apache-2.0 | 描画エンジン。公式プレビルド（`filament` / `gltfio` / `ibl` など）をリンク |
| [GStreamer](https://gstreamer.freedesktop.org/) | 1.x（システムの版） | LGPL-2.1-or-later | 映像のエンコードと配信。動的リンク（core / base / app / webrtc / sdp / video） |

## 実行時に読み込まれる GStreamer プラグイン

Charon は GStreamer のプラグインを**実行時に**名前で探して使います。リンクは
していないので、どれが入っているかは実行環境で決まります。同梱して配布する
ときは、そのプラグインと下位ライブラリのライセンスにも従ってください。

| プラグイン | 下位ライブラリ | ライセンス | 備考 |
|---|---|---|---|
| `webrtcbin` / `nice` | [libnice](https://libnice.freedesktop.org/) | LGPL-2.1 / MPL-1.1 の選択 | WebRTC（ICE / DTLS / SRTP）。`gst-plugins-bad` |
| `vp8enc` / `vp9enc` | [libvpx](https://chromium.googlesource.com/webm/libvpx) | BSD-3-Clause | 既定コーデック（`SceneConfig.h` の `kVideoCodec`） |
| `x264enc` | [x264](https://www.videolan.org/developers/x264.html) | **GPL-2.0-or-later** | H.264 のソフトウェアエンコード。**GPL なので、これを同梱して配布する場合は配布物全体が GPL の条件に従う**。避けたい場合は `openh264enc` かハードウェアエンコーダを使う |
| `openh264enc` | [OpenH264](https://www.openh264.org/)（Cisco） | BSD-2-Clause | Cisco のバイナリを使う場合は同社の特許ライセンス条件（BINARY_LICENSE）に注意 |
| `amfh264enc` | AMD AMF SDK | MIT | AMD GPU のハードウェアエンコード |
| `mfh264enc` | Windows Media Foundation | OS の構成要素 | Windows のハードウェア / ソフトウェアエンコード |
| `rtph264pay` / `rtpvp8pay` / `h264parse` など | GStreamer 本体 | LGPL-2.1-or-later | `gst-plugins-good` / `-bad` |

## Chrono と Filament が同梱しているもの

上の 2 つは自分の配布物の中にさらに外部コードを含みます。代表的なもの:

- **Project Chrono**: Bullet Physics（zlib）、tinyobjloader（MIT）、その他
  `chrono_thirdparty/` 以下（各ディレクトリの LICENSE を参照）。
- **Google Filament**: Draco（Apache-2.0）、meshoptimizer（MIT）、KTX-Software
  （Apache-2.0）、basis_universal（Apache-2.0）、stb（MIT / Public Domain）、
  SPIRV-Cross / glslang（Apache-2.0）ほか。Filament の `third_party/` と
  各リリースの `LICENSE` を参照。

## ライセンス条文（要約ではなく参照先）

- MIT: <https://opensource.org/license/mit>
- BSD-2-Clause: <https://opensource.org/license/bsd-2-clause>
- BSD-3-Clause: <https://opensource.org/license/bsd-3-clause>
- Apache-2.0: <https://www.apache.org/licenses/LICENSE-2.0>
- MPL-2.0: <https://www.mozilla.org/MPL/2.0/>
- LGPL-2.1: <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>
- GPL-2.0: <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>
- zlib: <https://opensource.org/license/zlib>
- Unlicense: <https://unlicense.org/>

この一覧は依存を足したり版を上げたりしたときに更新してください
（`CMakeLists.txt` の `wiz_download_header` / `FILAMENT_VERSION` /
`find_package`、`.gitmodules`、`WebRtcStreamer.cpp` のエンコーダ候補が
出どころです）。
