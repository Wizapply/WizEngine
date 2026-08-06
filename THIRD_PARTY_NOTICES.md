# Third-party notices

WizEngine のリポジトリ自体にサードパーティのコードは同梱していません。
以下は、ビルド時に自動取得されるもの・リンクされるものの一覧です。

| ライブラリ | ライセンス | 取得・リンク方法 |
|---|---|---|
| [Google Filament](https://github.com/google/filament) | Apache-2.0 | CMake がプレビルド版を自動ダウンロードしてリンク |
| [Project Chrono](https://github.com/projectchrono/chrono) | BSD-3-Clause | 利用者がソースからビルドしてリンク |
| [GStreamer](https://gstreamer.freedesktop.org/) | LGPL-2.1+ | システムインストール品に動的リンク |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | MIT | ビルド時に単一ヘッダを自動ダウンロード |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | ビルド時に単一ヘッダを自動ダウンロード |
| [stb_image](https://github.com/nothings/stb) | Public Domain / MIT (dual) | ビルド時に単一ヘッダを自動ダウンロード |
| [Eigen](https://eigen.tuxfamily.org/) | MPL-2.0 | Chrono 経由（ヘッダのみ） |

- GStreamer は LGPL のため**動的リンクのまま**にしてください（本プロジェクトの
  既定構成は動的リンクです）。使用するプラグインによっては追加のライセンス
  （例: x264 は GPL）が絡むため、配布時は選択したエンコーダを確認してください。
- Filament のプレビルド、および H.264 ハードウェアエンコーダ（AMF /
  Media Foundation）はそれぞれのベンダー条件に従います。
