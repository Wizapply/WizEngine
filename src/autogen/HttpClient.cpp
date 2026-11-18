#include "autogen/HttpClient.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

#include <httplib.h>

namespace wizengine {
namespace net {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string envValue(const char* upper, const char* lowerName) {
    if (const char* v = std::getenv(upper); v && *v) return v;
    if (const char* v = std::getenv(lowerName); v && *v) return v;
    return {};
}

// NO_PROXY（"localhost,.example.com,10.0.0.1"）に当たるか。末尾一致で見る
// （".example.com" も "example.com" も example.com の下を全部含む）。
bool bypassProxy(const std::string& host) {
    const std::string h = lower(host);
    if (h == "localhost" || h == "127.0.0.1" || h == "::1" || h == "[::1]") return true;
    const std::string list = lower(envValue("NO_PROXY", "no_proxy"));
    std::size_t start = 0;
    while (start <= list.size()) {
        std::size_t end = list.find(',', start);
        if (end == std::string::npos) end = list.size();
        std::string item = list.substr(start, end - start);
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front())))
            item.erase(item.begin());
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back())))
            item.pop_back();
        if (item == "*") return true;
        if (!item.empty()) {
            if (item.front() == '.') item.erase(item.begin());
            if (h == item ||
                (h.size() > item.size() &&
                 h.compare(h.size() - item.size(), item.size(), item) == 0 &&
                 h[h.size() - item.size() - 1] == '.')) {
                return true;
            }
        }
        start = end + 1;
    }
    return false;
}

// プロキシの URL（"http://user:pass@proxy:8080"）を httplib のクライアントへ。
void applyProxy(httplib::Client& cli, const Url& target) {
    if (bypassProxy(target.host)) return;
    const std::string proxy = target.scheme == "https"
                                  ? envValue("HTTPS_PROXY", "https_proxy")
                                  : envValue("HTTP_PROXY", "http_proxy");
    if (proxy.empty()) return;
    std::string rest = proxy;
    if (const auto p = rest.find("://"); p != std::string::npos) rest = rest.substr(p + 3);
    if (const auto slash = rest.find('/'); slash != std::string::npos) rest.resize(slash);
    std::string user, pass;
    if (const auto at = rest.rfind('@'); at != std::string::npos) {
        const std::string cred = rest.substr(0, at);
        rest = rest.substr(at + 1);
        const auto colon = cred.find(':');
        user = cred.substr(0, colon);
        if (colon != std::string::npos) pass = cred.substr(colon + 1);
    }
    int port = 8080;
    std::string host = rest;
    if (const auto colon = rest.rfind(':'); colon != std::string::npos) {
        host = rest.substr(0, colon);
        port = std::atoi(rest.c_str() + colon + 1);
    }
    if (host.empty() || port <= 0) return;
    cli.set_proxy(host, port);
    if (!user.empty()) cli.set_proxy_basic_auth(user, pass);
}

// 呼ぶ前に「この URL へつなげるか」を見て、だめなら理由（英語）を返す。
std::string unusable(const Url& u, const std::string& raw) {
    if (!u.valid) return "invalid URL '" + raw + "'";
    if (u.scheme == "https" && !httpsSupported()) {
        return "https is not available in this build (configure with OpenSSL "
               "to enable it, or use an http:// endpoint)";
    }
    return {};
}

std::unique_ptr<httplib::Client> makeClient(const Url& u, const HttpOptions& opt) {
    const std::string origin =
        u.scheme + "://" + u.host + ":" + std::to_string(u.port);
    auto cli = std::make_unique<httplib::Client>(origin);
    cli->set_connection_timeout(opt.connectTimeoutSec, 0);
    cli->set_read_timeout(opt.readTimeoutSec, 0);
    cli->set_write_timeout(opt.connectTimeoutSec, 0);
    cli->set_follow_location(true);
    // 呼び出し側が渡すのは組み立て済みの URL（署名付き URL の %xx を含む）。
    // ここでもう一度エンコードすると % が %25 になって署名が壊れる。
    cli->set_url_encode(false);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    cli->enable_server_certificate_verification(true);
    if (!opt.caFile.empty()) cli->set_ca_cert_path(opt.caFile);
#endif
    applyProxy(*cli, u);
    return cli;
}

httplib::Headers toHeaders(const Headers& in) {
    httplib::Headers h;
    for (const auto& kv : in) h.emplace(kv.first, kv.second);
    return h;
}

}  // namespace

Url parseUrl(const std::string& url) {
    Url u;
    const auto sep = url.find("://");
    if (sep == std::string::npos) return u;
    u.scheme = lower(url.substr(0, sep));
    if (u.scheme != "http" && u.scheme != "https") return u;
    std::string rest = url.substr(sep + 3);
    const auto slash = rest.find_first_of("/?");
    std::string authority = rest.substr(0, slash);
    u.path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (!u.path.empty() && u.path[0] == '?') u.path = "/" + u.path;
    if (const auto at = authority.rfind('@'); at != std::string::npos) {
        authority = authority.substr(at + 1);  // 資格情報は URL に書かせない
    }
    u.port = u.scheme == "https" ? 443 : 80;
    if (!authority.empty() && authority[0] == '[') {  // [::1]:8080
        const auto close = authority.find(']');
        if (close == std::string::npos) return u;
        u.host = authority.substr(0, close + 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') {
            u.port = std::atoi(authority.c_str() + close + 2);
        }
    } else if (const auto colon = authority.rfind(':'); colon != std::string::npos) {
        u.host = authority.substr(0, colon);
        u.port = std::atoi(authority.c_str() + colon + 1);
    } else {
        u.host = authority;
    }
    u.valid = !u.host.empty() && u.port > 0 && u.port < 65536;
    return u;
}

std::string joinUrl(const std::string& base, const std::string& path) {
    std::string b = base;
    while (!b.empty() && b.back() == '/') b.pop_back();
    if (path.empty()) return b;
    return path.front() == '/' ? b + path : b + "/" + path;
}

bool httpsSupported() {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    return true;
#else
    return false;
#endif
}

HttpResponse request(const std::string& method, const std::string& url,
                     const Headers& headers, const std::string& body,
                     const std::string& contentType, const HttpOptions& opt) {
    HttpResponse out;
    const Url u = parseUrl(url);
    out.error = unusable(u, url);
    if (!out.error.empty()) return out;
    auto cli = makeClient(u, opt);
    const auto h = toHeaders(headers);
    httplib::Result res =
        method == "POST"
            ? cli->Post(u.path, h, body, contentType.empty() ? "application/json"
                                                             : contentType)
            : cli->Get(u.path, h);
    if (!res) {
        out.error = "request to " + u.host + " failed: " +
                    httplib::to_string(res.error());
        return out;
    }
    out.status = res->status;
    out.body = std::move(res->body);
    return out;
}

HttpResponse download(const std::string& url, const std::string& path,
                      std::size_t maxBytes, const HttpOptions& opt,
                      const std::atomic<bool>* cancel) {
    HttpResponse out;
    const Url u = parseUrl(url);
    out.error = unusable(u, url);
    if (!out.error.empty()) return out;

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".part";
    std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
    if (!file) {
        out.error = "cannot write '" + tmp + "'";
        return out;
    }

    auto cli = makeClient(u, opt);
    std::size_t received = 0;
    bool tooLarge = false;
    int status = 0;
    httplib::Result res = cli->Get(
        u.path, httplib::Headers{},
        [&](const httplib::Response& r) {
            status = r.status;
            // リダイレクトの途中（3xx）は httplib が辿る。最後の応答が
            // 2xx でなければ中身を受けない（エラーページを .glb にしない）。
            return r.status >= 200 && r.status < 300;
        },
        [&](const char* data, std::size_t len) {
            if (cancel && cancel->load()) return false;
            received += len;
            if (received > maxBytes) {
                tooLarge = true;
                return false;
            }
            file.write(data, std::streamsize(len));
            return bool(file);
        });
    file.close();

    auto fail = [&](std::string why) {
        fs::remove(tmp, ec);
        out.error = std::move(why);
        return out;
    };
    if (cancel && cancel->load()) return fail("download cancelled");
    if (tooLarge) {
        return fail("download exceeds " + std::to_string(maxBytes / (1024 * 1024)) +
                    " MB");
    }
    if (!res) {
        if (status != 0 && (status < 200 || status >= 300)) {
            out.status = status;
            return fail("download from " + u.host + " returned HTTP " +
                        std::to_string(status));
        }
        return fail("download from " + u.host + " failed: " +
                    httplib::to_string(res.error()));
    }
    out.status = res->status;
    if (out.status < 200 || out.status >= 300) {
        return fail("download returned HTTP " + std::to_string(out.status));
    }
    if (received == 0) return fail("download returned an empty body");
    fs::rename(tmp, path, ec);
    if (ec) {
        // Windows は既存ファイルへの rename が失敗する版があるので消してから。
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
        if (ec) return fail("cannot move download to '" + path + "': " + ec.message());
    }
    return out;
}

}  // namespace net
}  // namespace wizengine
