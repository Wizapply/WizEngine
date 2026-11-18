#pragma once

#include <string>
#include <vector>

#include "autogen/AiConfig.h"
#include "scene/SceneSystemConfig.h"

// 起動設定（config.json）。**実行環境・システムに関わる設定はすべてここ**:
// 出力モードとポート・映像（解像度・コーデック・ビットレート）・CPU コア・
// ログ・カメラのスロット・起動時のシーンとモード・物理バックエンドの選び方・
// AI の接続先と API キー。シーンの中身（配置・モデル・物理の手触り）は
// assets/scenes/*.xml と SceneConfig.h のままで、ここには来ない。
//
// 読む順（後ろが勝つ）:
//   1. 各構造体の既定値（このヘッダ / SceneSystemConfig.h / AiConfig.h）
//   2. config.json（カレントディレクトリ = assets/ と同じ場所。--config で別の
//      ファイル）。書いていないキーは 1 のまま = 足りないキーがあっても動く
//   3. exe 引数（--physics-cores / --max-cameras / --codec / 位置引数の
//      モードとポートなど。従来の起動スクリプトがそのまま効く）
// API キーは config.json の apiKey が空のときだけ環境変数から補う
// （autogen::fillAiKeysFromEnvironment）。
//
// 読み方の約束（シーン文書と同じ考え方）:
//   * // と /* */ のコメントを書いてよい（JSONC。同梱の config.json は説明付き）
//   * 知らないキー・型の違う値・範囲外の値は**警告して既定値 / 丸めた値**で続ける
//     （打ち間違いを動かして首をかしげる前に気付けるように、全部ログに出す）
//   * JSON として壊れているときだけ失敗（行と列付き）。起動は止める - 壊れた
//     設定で黙って既定値のまま動くと、どの設定が効いているのか分からなくなる
namespace wizengine {

struct AppConfig {
    struct Server {
        // web = ブラウザ（WebRTC）/ window = ローカルの窓 / stream = RTP/UDP /
        // rtsp = RTSP サーバーへ送る。
        std::string mode = "web";
        int httpPort = 8080;               // ブラウザの UI（全カメラで 1 ポート）
        std::string streamHost = "127.0.0.1";  // mode = stream の送り先
        int streamPort = 5000;
        std::string rtspUrl = "rtsp://127.0.0.1:8554/wiz";  // mode = rtsp
    } server;

    struct Video {
        // 描画解像度。ブラウザは拡大して表示するので、描画・エンコード・配信を
        // 軽く保つならここを下げる（1080p は 720p の約 2.2 倍の画素）。
        int width = 1920;
        int height = 1080;
        int fps = 60;                      // 描画のペース（物理のレートとは別）
        std::string codec = "h264";        // h264 / h265 / av1 / vp9
        std::string encoder;               // GStreamer の要素名（空 = 自動）
        int bitrate = 8000000;             // bit/s（ブラウザからカメラごとに下げられる）
        bool gpuConvert = true;            // RGBA -> NV12 を GPU で
    } video;

    struct Cpu {
        std::string physicsCores;          // "0-11" / "0,2,4" / 空 = 指定なし
        std::string renderCores;
        int physicsThreads = 0;            // 0 = 物理コアの数（無指定ならコア数）
    } cpu;

    std::string logLevel = "info";         // debug / info / warn / error

    SceneSystemConfig scene;               // "scene" + "physics"
    autogen::AiConfig ai;                  // "ai"
};

struct ConfigLoad {
    bool found = false;                    // ファイルがあったか
    std::string path;                      // 読んだ（探した）パス
    std::string error;                     // JSON が壊れていた（英語、行と列付き）
    std::vector<std::string> warnings;     // 英語（LOGW に流す）
};

// text を cfg に重ねる（書いてあるキーだけ上書き）。壊れた JSON なら false。
bool parseAppConfig(const std::string& text, AppConfig& cfg,
                    std::vector<std::string>& warnings, std::string& error);

// ファイルから。無ければ found = false で cfg は既定値のまま（失敗ではない）。
ConfigLoad loadAppConfig(const std::string& path, AppConfig& cfg);

}  // namespace wizengine
