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

// Per-camera HTTP endpoint. Serves the browser control page, performs WebRTC
// signaling ("<prefix>/whep": browser POSTs an SDP offer, gets an SDP answer),
// and collects input commands ("<prefix>/input"). The render loop pulls
// queued input with drainCommands().
//
// An HttpServer no longer listens by itself: every camera is mounted under a
// path prefix ("/cam0", "/cam1", ...) on ONE shared HttpListener, so a single
// port carries all cameras. The web page uses relative URLs, so the same
// static files work under every prefix.
//
// Only ONE viewer per camera at a time. The first browser to negotiate owns
// the session and keeps it alive with "<prefix>/viewer/ping"; later browsers
// get HTTP 409 and show an error. The slot is freed by "<prefix>/viewer/leave"
// (sent on reload or close) or after kViewerTimeout without a ping.
class HttpServer {
public:
    // webRoot: directory of static files served at "<prefix>/" (which maps to
    // index.html). Read from disk per request, so editing the page and hitting
    // reload is enough - no rebuild. Missing files give a plain 404.
    explicit HttpServer(std::string webRoot = "web");
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Registers this camera's routes on the shared listener under `prefix`
    // (e.g. "/cam0" - no trailing slash). Called by HttpListener::mount.
    void registerRoutes(httplib::Server& srv, const std::string& prefix);

    // WebRTC signaling handler: given the browser's SDP offer, return the answer.
    void setOfferHandler(std::function<std::string(const std::string&)> handler);
    // Called when the active viewer goes away (explicit leave or timeout).
    void setViewerGoneHandler(std::function<void()> handler);
    // Supplies the JSON body served at "<prefix>/stats" (performance counters).
    void setStatsProvider(std::function<std::string()> provider);
    // Served at "<prefix>/scene": the hierarchy shown in the sidebar.
    void setSceneProvider(std::function<std::string()> provider);
    // Served at "<prefix>/scene.xml": シーンの中身そのもの（保存されるのと
    // 同じ MJCF 風の XML）。ブラウザで開けば、いま組んでいるシーンが
    // どういう文書になっているかがそのまま読める。
    void setSceneXmlProvider(std::function<std::string()> provider);

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

    std::string webRoot_;

    std::mutex cmdMutex_;
    std::vector<std::string> commands_;

    std::mutex offerMutex_;
    std::function<std::string(const std::string&)> offerHandler_;
    std::function<void()> viewerGoneHandler_;
    std::function<std::string()> statsProvider_;
    std::function<std::string()> sceneProvider_;
    std::function<std::string()> sceneXmlProvider_;

    // Single-viewer session state.
    static constexpr std::chrono::seconds kViewerTimeout{6};
    std::mutex viewerMutex_;
    std::string viewerToken_;  // empty = nobody is watching
    std::chrono::steady_clock::time_point viewerLastSeen_{};
};

// The one listening socket all cameras share. mount() each camera under its
// prefix, then start(). "/" redirects to the first mounted camera.
class HttpListener {
public:
    explicit HttpListener(int port);
    ~HttpListener();

    HttpListener(const HttpListener&) = delete;
    HttpListener& operator=(const HttpListener&) = delete;

    void mount(HttpServer& camera, const std::string& prefix);
    void start();  // begin listening on a background thread
    void stop();
    int port() const { return port_; }

private:
    int port_;
    std::string firstPrefix_;
    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
};
