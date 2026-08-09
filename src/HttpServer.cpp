#include "HttpServer.h"

#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <utility>

namespace {

// Content type from the file extension, so the browser knows what it got.
std::string mimeForPath(const std::string& path) {
    const auto dot = path.find_last_of('.');
    std::string ext = (dot == std::string::npos) ? "" : path.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(::tolower(c));
    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "js") return "text/javascript; charset=utf-8";
    if (ext == "css") return "text/css; charset=utf-8";
    if (ext == "json") return "application/json";
    if (ext == "png") return "image/png";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "gif") return "image/gif";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "ico") return "image/x-icon";
    return "application/octet-stream";
}

// Reads a file under root. Returns false when missing or when the path tries
// to escape the web root. `resolved` reports the file actually opened - the
// caller needs it for the content type, since "/" carries no extension and
// would otherwise be served as a download instead of a page.
bool readWebFile(const std::string& root, std::string rel, std::string& out,
                 std::string& resolved) {
    if (rel.empty() || rel == "/") rel = "/index.html";
    if (rel.find("..") != std::string::npos) return false;
    std::ifstream in(root + rel, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
    resolved = rel;
    return true;
}

// Tokens arrive as a request body (sendBeacon can't set headers), so trim.
std::string trimmed(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}
}  // namespace

constexpr std::chrono::seconds HttpServer::kViewerTimeout;

HttpListener::HttpListener(int port)
    : port_(port), server_(std::make_unique<httplib::Server>()) {}

HttpListener::~HttpListener() { stop(); }

void HttpListener::mount(HttpServer& camera, const std::string& prefix) {
    camera.registerRoutes(*server_, prefix);
    if (firstPrefix_.empty()) firstPrefix_ = prefix;
}

void HttpListener::start() {
    // "/" lands on the first camera - the page itself never links to "/",
    // so a plain redirect is all the root needs to do.
    if (!firstPrefix_.empty()) {
        const std::string target = firstPrefix_ + "/";
        server_->Get("/", [target](const httplib::Request&,
                                   httplib::Response& res) {
            res.status = 302;
            res.set_header("Location", target);
        });
    }
    thread_ = std::thread([this] { server_->listen("0.0.0.0", port_); });
}

void HttpListener::stop() {
    if (server_) server_->stop();
    if (thread_.joinable()) thread_.join();
}

HttpServer::HttpServer(std::string webRoot) : webRoot_(std::move(webRoot)) {}

void HttpServer::registerRoutes(httplib::Server& srv,
                                const std::string& prefix) {
    // "/cam0" (no trailing slash) redirects to "/cam0/": the page uses
    // RELATIVE fetch URLs ("whep", "stats", ...), which only resolve inside
    // the camera's path when the document URL ends with the slash.
    srv.Get(prefix, [prefix](const httplib::Request&, httplib::Response& res) {
        res.status = 301;
        res.set_header("Location", prefix + "/");
    });

    // Static files: "/" -> web/index.html, plus anything else under web/.
    // Read per request so page edits need only a browser reload.
    auto serveStatic = [this, prefix](const httplib::Request& req,
                                      httplib::Response& res) {
        // "/cam0/style.css" -> "/style.css" under the web root.
        std::string body, resolved;
        if (!readWebFile(webRoot_, req.path.substr(prefix.size()), body,
                         resolved)) {
            res.status = 404;
            res.set_content("not found", "text/plain");
            return;
        }
        res.set_header("Cache-Control", "no-store, no-cache, must-revalidate");
        res.set_content(body, mimeForPath(resolved).c_str());
    };
    srv.Get(prefix + "/", serveStatic);
    srv.Get(prefix + R"(/(favicon\.ico|.*\.(?:html|js|css|png|svg|jpg|jpeg|gif|ico|json)))",
                 serveStatic);

    // WebRTC signaling (WHEP-style): body is the browser's SDP offer, the
    // response is the SDP answer. Rejected with 409 while another browser owns
    // the session.
    srv.Post(prefix + "/whep", [this](const httplib::Request& req,
                                  httplib::Response& res) {
        const std::string token = trimmed(req.get_header_value("X-Viewer-Token"));
        if (token.empty()) {
            res.status = 400;
            res.set_content("missing viewer token", "text/plain");
            return;
        }

        bool dropped = false;
        {
            std::lock_guard<std::mutex> lock(viewerMutex_);
            dropped = expireViewerLocked();
            if (!viewerToken_.empty() && viewerToken_ != token) {
                res.status = 409;  // someone else is watching
                res.set_content("another viewer is connected", "text/plain");
                return;
            }
            viewerToken_ = token;
            viewerLastSeen_ = std::chrono::steady_clock::now();
        }
        if (dropped) {
            std::function<void()> gone;
            {
                std::lock_guard<std::mutex> lock(offerMutex_);
                gone = viewerGoneHandler_;
            }
            if (gone) gone();
        }

        std::function<std::string(const std::string&)> handler;
        {
            std::lock_guard<std::mutex> lock(offerMutex_);
            handler = offerHandler_;
        }
        if (!handler) {
            res.status = 503;
            return;
        }
        const std::string answer = handler(req.body);
        if (answer.empty()) {
            // Negotiation failed - free the slot so the next attempt can retry.
            std::lock_guard<std::mutex> lock(viewerMutex_);
            if (viewerToken_ == token) viewerToken_.clear();
            res.status = 500;
            return;
        }
        res.set_content(answer, "application/sdp");
    });

    // Heartbeat from the active viewer; body is its token.
    srv.Post(prefix + "/viewer/ping", [this](const httplib::Request& req,
                                         httplib::Response& res) {
        const std::string token = trimmed(req.body);
        std::lock_guard<std::mutex> lock(viewerMutex_);
        expireViewerLocked();
        if (viewerToken_.empty() || viewerToken_ != token) {
            res.status = 409;
            return;
        }
        viewerLastSeen_ = std::chrono::steady_clock::now();
        res.set_content("ok", "text/plain");
    });

    // Sent on reload/close (navigator.sendBeacon); body is the token.
    srv.Post(prefix + "/viewer/leave", [this](const httplib::Request& req,
                                          httplib::Response& res) {
        const std::string token = trimmed(req.body);
        bool released = false;
        {
            std::lock_guard<std::mutex> lock(viewerMutex_);
            if (!viewerToken_.empty() && viewerToken_ == token) {
                viewerToken_.clear();
                released = true;
            }
        }
        if (released) {
            std::function<void()> gone;
            {
                std::lock_guard<std::mutex> lock(offerMutex_);
                gone = viewerGoneHandler_;
            }
            if (gone) gone();
        }
        res.set_content("ok", "text/plain");
    });

    // Scene hierarchy for the sidebar. Copy the provider under the lock, then
    // call it outside - same pattern as /stats, so a slow provider never
    // blocks whoever is swapping it.
    srv.Get(prefix + "/scene", [this](const httplib::Request&,
                                  httplib::Response& res) {
        std::function<std::string()> provider;
        {
            std::lock_guard<std::mutex> lock(offerMutex_);
            provider = sceneProvider_;
        }
        res.set_content(provider ? provider() : "{}", "application/json");
    });

    // Performance counters for the browser overlay.
    srv.Get(prefix + "/stats", [this](const httplib::Request&, httplib::Response& res) {
        std::function<std::string()> provider;
        {
            std::lock_guard<std::mutex> lock(offerMutex_);
            provider = statsProvider_;
        }
        if (!provider) {
            res.status = 503;
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(provider(), "application/json");
    });

    // Input command (button press / key), JSON body.
    srv.Post(prefix + "/input",
                  [this](const httplib::Request& req, httplib::Response& res) {
                      {
                          std::lock_guard<std::mutex> lock(cmdMutex_);
                          commands_.push_back(req.body);
                      }
                      res.set_content("ok", "text/plain");
                  });
}

HttpServer::~HttpServer() = default;

bool HttpServer::expireViewerLocked() {
    if (viewerToken_.empty()) return false;
    const auto now = std::chrono::steady_clock::now();
    if (now - viewerLastSeen_ < kViewerTimeout) return false;
    viewerToken_.clear();
    return true;
}

bool HttpServer::hasViewer() {
    bool dropped = false;
    bool present = false;
    {
        std::lock_guard<std::mutex> lock(viewerMutex_);
        dropped = expireViewerLocked();
        present = !viewerToken_.empty();
    }
    if (dropped) {
        std::function<void()> gone;
        {
            std::lock_guard<std::mutex> lock(offerMutex_);
            gone = viewerGoneHandler_;
        }
        if (gone) gone();
    }
    return present;
}

void HttpServer::setOfferHandler(
    std::function<std::string(const std::string&)> handler) {
    std::lock_guard<std::mutex> lock(offerMutex_);
    offerHandler_ = std::move(handler);
}

void HttpServer::setViewerGoneHandler(std::function<void()> handler) {
    std::lock_guard<std::mutex> lock(offerMutex_);
    viewerGoneHandler_ = std::move(handler);
}

void HttpServer::setStatsProvider(std::function<std::string()> provider) {
    std::lock_guard<std::mutex> lock(offerMutex_);
    statsProvider_ = std::move(provider);
}

void HttpServer::setSceneProvider(std::function<std::string()> provider) {
    std::lock_guard<std::mutex> lock(offerMutex_);
    sceneProvider_ = std::move(provider);
}

std::vector<std::string> HttpServer::drainCommands() {
    std::lock_guard<std::mutex> lock(cmdMutex_);
    std::vector<std::string> out;
    out.swap(commands_);
    return out;
}
