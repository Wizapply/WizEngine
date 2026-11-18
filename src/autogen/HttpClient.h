#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// 外へ出ていく HTTP(S) の最小の口（AI のシーン生成が LLM / Meshy を呼ぶ）。
//
// 中身は cpp-httplib のクライアント（サーバーと同じライブラリ = 依存を
// 増やさない）。**HTTPS は OpenSSL 付きのビルドでだけ使える**
// （CMake が OpenSSL を見つけたら CPPHTTPLIB_OPENSSL_SUPPORT をターゲット
// 全体に立てる。httplib はヘッダだけのライブラリで、この定義で class の
// 中身が変わるので、TU ごとに立て分けてはいけない = ODR 違反）。無い
// ビルドでは http:// だけ（ローカルの Ollama / LM Studio などには足りる）。
//
// プロキシは環境変数 HTTPS_PROXY / HTTP_PROXY（小文字も）に従い、NO_PROXY と
// localhost / 127.0.0.1 は直接つなぐ。どれも**呼び出したスレッドで止まる**
// 同期呼び出しなので、物理 / 描画 / INPUT のスレッドからは呼ばない
// （SceneGenerator が自分のワーカースレッドで呼ぶ）。
namespace wizengine {
namespace net {

struct Url {
    bool valid = false;
    std::string scheme;  // "http" / "https"
    std::string host;
    int port = 0;
    std::string path;    // "/" から始まる（クエリ込み）。無ければ "/"
};

// "https://host:port/base/path?q" を分ける。scheme が http / https 以外、
// host が空なら valid = false。
Url parseUrl(const std::string& url);

// base（"https://api.openai.com/v1"）の後ろに path（"/chat/completions"）を
// 付ける。スラッシュの重なり・欠けを吸収する。
std::string joinUrl(const std::string& base, const std::string& path);

// このビルドが https:// を扱えるか（OpenSSL 付きか）。
bool httpsSupported();

struct HttpOptions {
    int connectTimeoutSec = 15;
    int readTimeoutSec = 120;
    std::string caFile;  // 空 = OpenSSL / OS の既定の CA
};

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;  // 通信そのものの失敗（英語。ログに出る）
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

using Headers = std::vector<std::pair<std::string, std::string>>;

// method は "GET" / "POST"。body が空でなければ contentType で送る。
// リダイレクトは辿る（Meshy の成果物の URL は署名付きの別ホストへ飛ぶ）。
HttpResponse request(const std::string& method, const std::string& url,
                     const Headers& headers, const std::string& body,
                     const std::string& contentType, const HttpOptions& opt);

// url の中身を path へ書く（一時ファイルに書いてから置き換える = 途中で
// 落ちても壊れたファイルを残さない）。maxBytes を超えたら打ち切って失敗。
// cancel が立ったら打ち切る。成功で ok() = true、body は空。
HttpResponse download(const std::string& url, const std::string& path,
                      std::size_t maxBytes, const HttpOptions& opt,
                      const std::atomic<bool>* cancel);

}  // namespace net
}  // namespace wizengine
