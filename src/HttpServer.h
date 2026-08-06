#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace httplib {
class Server;
}

// Tiny background HTTP server. Serves the browser control page ("/"), performs
// WebRTC signaling ("/whep": browser POSTs an SDP offer, gets an SDP answer),
// and collects input commands ("/input"). The render loop pulls queued input
// with drainCommands().
//
// Only ONE viewer is allowed at a time. The first browser to negotiate owns the
// session and keeps it alive with "/viewer/ping"; later browsers get HTTP 409
// and show an error. The slot is freed by "/viewer/leave" (sent on reload or
// close) or after kViewerTimeout without a ping.
class HttpServer {
public:
    // webRoot: directory of static files served at "/" ("/" maps to
    // index.html). Read from disk per request, so editing the page and hitting
    // reload is enough - no rebuild. Missing files give a plain 404.
    explicit HttpServer(int port, std::string webRoot = "web");
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void start();  // begin listening on a background thread
    void stop();

    // WebRTC signaling handler: given the browser's SDP offer, return the answer.
    void setOfferHandler(std::function<std::string(const std::string&)> handler);
    // Called when the active viewer goes away (explicit leave or timeout).
    void setViewerGoneHandler(std::function<void()> handler);
    // Supplies the JSON body served at "/stats" (performance counters).
    void setStatsProvider(std::function<std::string()> provider);
    // Served at "/scene": the hierarchy shown in the sidebar.
    void setSceneProvider(std::function<std::string()> provider);

    // True while a browser holds the viewer session (used to idle the engine
    // when nobody is watching). Also expires a viewer that stopped pinging.
    bool hasViewer();

    // Returns and clears the commands received since the last call.
    std::vector<std::string> drainCommands();

private:
    // Frees the session if the owner stopped pinging. Caller holds viewerMutex_.
    // Returns true if a viewer was dropped (the handler is invoked by the caller
    // outside the lock).
    bool expireViewerLocked();

    int port_;
    std::string webRoot_;
    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;

    std::mutex cmdMutex_;
    std::vector<std::string> commands_;

    std::mutex offerMutex_;
    std::function<std::string(const std::string&)> offerHandler_;
    std::function<void()> viewerGoneHandler_;
    std::function<std::string()> statsProvider_;
    std::function<std::string()> sceneProvider_;

    // Single-viewer session state.
    static constexpr std::chrono::seconds kViewerTimeout{6};
    std::mutex viewerMutex_;
    std::string viewerToken_;  // empty = nobody is watching
    std::chrono::steady_clock::time_point viewerLastSeen_{};
};
