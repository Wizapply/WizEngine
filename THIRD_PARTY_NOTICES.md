# Third-party notices

WizEngine のリポジトリ自体にサードパーティのコードは同梱していません
（ヘッダ系ライブラリは `third_parties/` の git サブモジュールとして参照）。
以下は、取得されるもの・リンクされるものの一覧です。

| ライブラリ | ライセンス | 取得・リンク方法 |
|---|---|---|
| [Google Filament](https://github.com/google/filament) | Apache-2.0 | CMake がプレビルド版を自動ダウンロードしてリンク |
| [Project Chrono](https://github.com/projectchrono/chrono) | BSD-3-Clause | サブモジュール `third_parties/chrono`（9.0.0）を利用者がビルドしてリンク |
| [GStreamer](https://gstreamer.freedesktop.org/) | LGPL-2.1+ | システムインストール品に動的リンク |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | MIT | サブモジュール `third_parties/cpp-httplib`（未取得時は単一ヘッダを自動ダウンロード） |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | サブモジュール `third_parties/json`（未取得時は単一ヘッダを自動ダウンロード） |
| [cgltf](https://github.com/jkuhlmann/cgltf) | MIT | サブモジュール `third_parties/cgltf`（未取得時は単一ヘッダを自動ダウンロード） |
| [stb_image](https://github.com/nothings/stb) | Public Domain / MIT (dual) | サブモジュール `third_parties/stb`（未取得時は単一ヘッダを自動ダウンロード） |
| [Eigen](https://eigen.tuxfamily.org/) | MPL-2.0 | サブモジュール `third_parties/eigen`（3.4.0）。Chrono のビルドに使用（ヘッダのみ） |
| [Blaze](https://bitbucket.org/blaze-lib/blaze) | BSD-3-Clause | サブモジュール `third_parties/blaze`（v3.8.2）。Chrono::Multicore のビルドに使用（ヘッダのみ） |
| [Thrust](https://github.com/NVIDIA/thrust) | Apache-2.0 | サブモジュール `third_parties/thrust`（1.17.2、入れ子の cub を含む）。Chrono::Multicore のビルドに使用（ヘッダのみ） |
| [CUB](https://github.com/NVIDIA/cub) | BSD-3-Clause | Thrust の入れ子サブモジュール（ヘッダのみ） |

- GStreamer は LGPL のため**動的リンクのまま**にしてください（本プロジェクトの
  既定構成は動的リンクです）。使用するプラグインによっては追加のライセンス
  （例: x264 は GPL）が絡むため、配布時は選択したエンコーダを確認してください。
- Filament のプレビルド、および H.264 ハードウェアエンコーダ（AMF /
  Media Foundation）はそれぞれのベンダー条件に従います。
