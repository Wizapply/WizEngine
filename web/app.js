// WizEngine web UI logic. Served from web/app.js as a classic script at
// the end of <body>: top-level functions stay global (the inline
// onclick= handlers in index.html rely on that), and the DOM is fully
// parsed by the time it runs. Edit + reload, no rebuild needed.
  // Identifies this browser tab to the server's single-viewer session.
  const token = (crypto.randomUUID && crypto.randomUUID()) ||
                (Date.now() + '-' + Math.random().toString(16).slice(2));
  const statusEl = document.getElementById('link');  // connection line under the stats
  const loadingEl = document.getElementById('loading');
  const loadingMsg = loadingEl.querySelector('span');
  const errorEl = document.getElementById('error');
  const videoEl = document.getElementById('video');
  const hitEl = document.getElementById('hit');
  let heartbeat = null;
  let owner = false;
  // GET パラメータ ?mode=edit（editor でも可）で開いたページは、接続確立後に
  // 一度だけエディタモードへ切り替える。一度きりなのは、その後ユーザーが
  // シミュレートへ切り替えたあとの再接続で勝手にエディタへ戻さないため。
  // （モード切替自体はどのカメラのページからでも許可されている。）
  const wantEditorOnLoad = ['edit', 'editor'].includes(
      new URLSearchParams(location.search).get('mode'));
  let editorModeRequested = false;
  let blockedTries = 0;
  let statsTimer = null;
  let camLinksDone = false;
  // Watchdog knobs: how many one-second ticks without a decoded frame before
  // reconnecting, and how many reconnects have happened (drives the backoff).
  const STALL_TICKS_BEFORE_RETRY = 3;
  let retryCount = 0;
  // Previous byte counter, for the kbps figure.
  let lastBytes = -1, lastBytesAt = 0;
  // Every failure path funnels through here, so a page opened before the
  // server is listening keeps trying instead of stopping at one message.
  function scheduleReconnect(why) {
    const wait = Math.min(500 * Math.pow(2, retryCount++), 8000);
    statusEl.textContent = why + ' - retrying in ' +
                           Math.round(wait / 100) / 10 + 's';
    setTimeout(startWebRTC, wait);
  }

  // Blocked viewers see the error box in place of the video; the controls below
  // stay exactly as they are.
  function setBlocked(blocked) {
    errorEl.classList.toggle('show', blocked);
    videoEl.style.display = blocked ? 'none' : 'block';
    // The loading overlay ("connecting..." + spinner) is an opaque layer over
    // the whole stage, normally dismissed by the first decoded video frame -
    // which a blocked viewer never gets. Without this line the error box
    // renders UNDER the overlay and is invisible (latent since the
    // single-viewer UI was added).
    if (blocked) loadingEl.classList.add('done');
  }

  // Only the browser that owns the session may drive the scene; the others
  // show greyed-out controls. エディタの操作ボタン類も同じ扱い（見ている
  // だけのブラウザがシーンを書き換えられては困る）。
  function setControlsEnabled(on) {
    document.querySelectorAll(
        '#sidebar button.wide, #sidebar .modeBtn, #paneEditor button')
      .forEach((b) => { b.disabled = !on; });
  }
  setControlsEnabled(false);

  // The button always names the action it will perform: "Pause" while the
  // sim runs, "Play" while it is paused. Clicking flips the label at once
  // (the server round-trip is invisible), and the 500 ms stats poll then
  // keeps it truthful - e.g. when someone on another camera page pauses.
  function setPlayLabel(paused) {
    const b = document.getElementById('btnPlay');
    // \ufe0e forces text (not emoji) presentation: the emoji font has
    // taller glyph metrics and made the button grow on "Pause".
    b.textContent = paused ? '\u25b6\ufe0e Play' : '\u23f8\ufe0e Pause';
  }
  function togglePause() {
    send('pause');
    setPlayLabel(document.getElementById('btnPlay')
                     .textContent.indexOf('Play') >= 0 ? false : true);
  }
  // ---- Stream format (resolution / fps / bitrate) --------------------------
  // The server stores the requested format and applies it to the NEXT WebRTC
  // session, so apply = send the command, then reload: the reload releases
  // this session (beacon) and negotiates a fresh one in the new format.
  let streamUiReady = false;
  let versionsShown = false;
  function strKbpsLabel() {
    const kb = document.getElementById('strKbps');
    document.getElementById('strKbpsVal').textContent =
      (kb.value / 1000).toFixed(1) + ' Mbps';
  }
  function applyStream() {
    const [w, h] = document.getElementById('strRes').value.split('x')
      .map(Number);
    send('stream', {
      w: w, h: h,
      fps: parseInt(document.getElementById('strFps').value, 10),
      kbps: parseInt(document.getElementById('strKbps').value, 10),
    });
    // Give the command a moment to land, then restart cleanly.
    setTimeout(() => location.reload(), 400);
  }

  function send(cmd, args) {
    if (!owner) return;
    const msg = Object.assign({ cmd: cmd }, args || {});
    fetch('input', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(msg)
    }).catch(() => {});
  }
  // ---- Input: one code path for mouse, touch and pen ---------------------
  // Pointer Events give the same handlers for every device, so the gestures
  // below are described once and work with a mouse or a finger.
  //
  //   MOUSE                              TOUCH
  //   left drag        = grab object     one finger on an object = grab it
  //   Ctrl + left drag = orbit           one finger on empty space = orbit
  //                                      (the server decides which)
  //   Ctrl + right/mid = pan             two fingers drag = pan
  //   Ctrl + wheel     = zoom            pinch = zoom
  //
  // "Did I hit an object?" is decided by the SERVER: a press always sends a
  // pick, and while the drag continues the server either pulls the object it
  // picked or, if the press hit nothing, orbits the camera instead. Deciding
  // in the browser would need a round trip first, which swallows the start of
  // the gesture - very noticeable on touch.
  const ORBIT_RAD_PER_PX = 0.005;
  const PINCH_ZOOM_PER_PX = 0.004;

  let grabbing = false;
  // Pointer position in normalised device coords, sent while holding an
  // object. Absolute rather than deltas: the server puts the target exactly on
  // this ray, so the object cannot drift away from the pointer.
  let grabNdc = null;
  // Whether the current grab came from a finger. The server only falls back to
  // orbiting when it did; with a mouse the camera stays on Ctrl+drag.
  let grabIsTouch = false;

  let dragMode = null;          // 'orbit' | 'pan' | null
  let accX = 0, accY = 0, accZoom = 0;

  // Live touch points, keyed by pointerId, so two-finger gestures can be
  // measured without guessing which finger moved.
  const touches = new Map();
  let pinchDist = 0;            // distance between two fingers, last frame
  let pinchMid = null;          // midpoint, for two-finger pan

  function toNdc(clientX, clientY) {
    const r = hitEl.getBoundingClientRect();
    return { x: ((clientX - r.left) / r.width) * 2 - 1,
             y: 1 - ((clientY - r.top) / r.height) * 2 };
  }
  function twoFingerState() {
    const [a, b] = [...touches.values()];
    return { dist: Math.hypot(a.x - b.x, a.y - b.y),
             mid: { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 } };
  }

  hitEl.addEventListener('pointerdown', (e) => {
    if (!owner) return;
    hitEl.setPointerCapture?.(e.pointerId);

    if (e.pointerType === 'touch') {
      touches.set(e.pointerId, { x: e.clientX, y: e.clientY });
      if (touches.size === 1) {
        // Try to grab whatever is under the finger. If the server reports no
        // selection, pointermove turns this into an orbit.
        grabIsTouch = true;
        grabNdc = toNdc(e.clientX, e.clientY);
        send('pick', grabNdc);
        send('drag', { x: grabNdc.x, y: grabNdc.y, touch: true });
        grabbing = true;
      } else if (touches.size === 2) {
        // Second finger: this is a camera gesture, not a grab.
        if (grabbing) { grabbing = false; send('release'); }
        dragMode = 'pan';
        const st = twoFingerState();
        pinchDist = st.dist;
        pinchMid = st.mid;
      }
      e.preventDefault();
      return;
    }

    // Mouse / pen.
    if (!e.ctrlKey) {
      if (e.button === 0) {
        grabIsTouch = false;
        grabNdc = toNdc(e.clientX, e.clientY);
        send('pick', grabNdc);
        send('drag', grabNdc);
        grabbing = true;
        e.preventDefault();
      }
      return;
    }
    if (e.button === 0) dragMode = 'orbit';
    else if (e.button === 1 || e.button === 2) dragMode = 'pan';
    else return;
    e.preventDefault();
  });

  hitEl.addEventListener('pointermove', (e) => {
    if (e.pointerType === 'touch') {
      const t = touches.get(e.pointerId);
      if (!t) return;
      const dx = e.clientX - t.x, dy = e.clientY - t.y;
      t.x = e.clientX; t.y = e.clientY;

      if (touches.size >= 2) {
        const st = twoFingerState();
        accZoom += (pinchDist - st.dist) * PINCH_ZOOM_PER_PX;  // pinch = zoom
        if (pinchMid) {
          accX += st.mid.x - pinchMid.x;                       // drag = pan
          accY += st.mid.y - pinchMid.y;
        }
        pinchDist = st.dist;
        pinchMid = st.mid;
        e.preventDefault();
        return;
      }

      if (grabbing) grabNdc = toNdc(e.clientX, e.clientY);
      if (dragMode) { accX += dx; accY += dy; }
      e.preventDefault();
      return;
    }

    // ギズモのハイライト用に、いつでも最新のカーソル位置を控えておく
    // （送るかどうかは下のタイマーが決める）。
    hoverNdc = toNdc(e.clientX, e.clientY);
    if (grabbing) { grabNdc = hoverNdc; return; }
    if (!dragMode) return;
    accX += e.movementX;
    accY += e.movementY;
  });

  function endPointer(e) {
    if (e && e.pointerType === 'touch') {
      touches.delete(e.pointerId);
      if (touches.size === 1) {          // back to one finger: stop the pan
        dragMode = null;
        pinchMid = null;
      }
      if (touches.size > 0) return;      // other fingers still down
    }
    dragMode = null;
    pinchMid = null;
    if (grabbing) {
      grabbing = false;
      send('release');
    }
  }
  hitEl.addEventListener('pointerup', endPointer);
  hitEl.addEventListener('pointercancel', endPointer);
  window.addEventListener('blur', () => endPointer(null));
  window.addEventListener('keyup', (e) => {
    if (e.key === 'Control') endPointer(null);
  });
  // Right-drag must not open the context menu over the video, and a long press
  // must not pop up the touch callout.
  hitEl.addEventListener('contextmenu', (e) => e.preventDefault());

  hitEl.addEventListener('wheel', (e) => {
    if (!e.ctrlKey) return;  // plain wheel scrolls the page as usual
    e.preventDefault();      // Ctrl+wheel would otherwise zoom the page
    accZoom += e.deltaY * 0.0012;
  }, { passive: false });

  setInterval(() => {
    if (grabbing && grabNdc) {
      send('drag', grabIsTouch
        ? { x: grabNdc.x, y: grabNdc.y, touch: true }
        : grabNdc);
    }
    if (dragMode && (accX !== 0 || accY !== 0)) {
      if (dragMode === 'orbit') {
        send('orbit', { dx: -accX * ORBIT_RAD_PER_PX,
                        dy: accY * ORBIT_RAD_PER_PX });
      } else {
        send('pan', { dx: accX, dy: accY });
      }
      accX = 0; accY = 0;
    }
    if (accZoom !== 0) {
      send('zoom', { d: accZoom });
      accZoom = 0;
    }
  }, 16);  // ~60 Hz - drag latency is very visible on the grabbed object

  // パネルとフルスクリーンは常時。W / E / R / X（ギズモ）はエディタモード中
  // だけで、入力欄にフォーカスがあるときは無効 - 数値を打っている最中に
  // モードが変わらないようにするため。それ以外の操作は誤爆を避けて
  // サイドバーのボタンかマウス操作に寄せてある。
  document.addEventListener('keydown', (e) => {
    if (e.repeat) return;
    if (e.key === 'Tab' && !e.ctrlKey && !e.altKey && !e.shiftKey) {
      e.preventDefault();
      toggleSidebar();
      return;
    }
    if ((e.key === 'f' || e.key === 'F') && e.altKey) {
      e.preventDefault();
      toggleFullscreen();
      return;
    }
    if (e.ctrlKey || e.altKey || e.metaKey) return;
    const el = document.activeElement;
    if (el && (el.tagName === 'INPUT' || el.tagName === 'SELECT' ||
               el.tagName === 'TEXTAREA')) {
      return;
    }
    // ノードエディタは画面全体を覆うので、✕ のほかに Esc でも閉じられる
    // ようにしておく（モード不問 - シミュレート中も開けるため）。
    if (e.key === 'Escape' && nodeEdOpen) {
      e.preventDefault();
      closeNodeEditor();
      return;
    }
    if (e.key === 'Escape' && xmlEdOpen) {
      e.preventDefault();
      closeXmlEditor();
      return;
    }
    if (!owner || !sceneData || sceneData.mode !== 'editor') return;
    if (!isEditorCam()) return;  // ギズモが出ないページで切り替えても意味がない
    const key = e.key.toLowerCase();
    if (key === 'w') { e.preventDefault(); setGizmoMode('translate'); }
    else if (key === 'e') { e.preventDefault(); setGizmoMode('rotate'); }
    else if (key === 'r') { e.preventDefault(); setGizmoMode('scale'); }
    else if (key === 'x') {
      e.preventDefault();
      const box = document.getElementById('gzSnap');
      box.checked = !box.checked;
      applyGizmo();
    }
  });
  // Software cursor over the stage (windowed and fullscreen alike): follows
  // the mouse via transform (cheap, no layout). pointer-events:none keeps it
  // from stealing clicks from the hit layer underneath. Outside the stage the
  // normal OS cursor is used, so it is hidden there.
  const cursorEl = document.getElementById('cursor');
  const stageEl = document.getElementById('stage');
  window.addEventListener('pointermove', (e) => {
    if (e.pointerType === 'touch') {  // a finger is its own cursor
      cursorEl.classList.remove('show');
      return;
    }
    const under = document.elementFromPoint(e.clientX, e.clientY);
    // ノードエディタの上は普通のフォーム操作なので OS カーソルに任せる
    // （style.css 側で cursor:none も外している）。
    const overNodeEd = under && under.closest && under.closest('#nodeEd');
    const overStage = !overNodeEd &&
        (document.fullscreenElement || stageEl.contains(under));
    if (!overStage) {
      cursorEl.classList.remove('show');
      return;
    }
    cursorEl.style.transform =
      'translate(' + e.clientX + 'px,' + e.clientY + 'px)';
    cursorEl.classList.toggle('cross', e.ctrlKey);
    cursorEl.classList.add('show');
  });
  // Ctrl toggles camera mode - reflect it in the cursor immediately, not
  // only on the next mouse move.
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Control') cursorEl.classList.add('cross');
  });
  document.addEventListener('keyup', (e) => {
    if (e.key === 'Control') cursorEl.classList.remove('cross');
  });
  window.addEventListener('blur', () => cursorEl.classList.remove('cross'));
  stageEl.addEventListener('pointerleave', () => {
    if (!document.fullscreenElement) cursorEl.classList.remove('show');
    // ビューから出たらギズモのハイライトも消す。どのハンドルにも当たらない
    // 座標を 1 回送るだけでよい（専用のコマンドを増やすほどの話ではない）。
    if (hoverNdc) {
      hoverNdc = null;
      send('hover', { x: 9.0, y: 9.0 });
    }
  });

  document.addEventListener('fullscreenchange', () => {
    cursorEl.classList.remove('show');  // reset when entering/leaving
  });

  // Fullscreen on the video area only, so the controls below stay out of the
  // way. Esc leaves it (handled by the browser).
  function toggleFullscreen() {
    const stage = document.getElementById('stage');
    if (document.fullscreenElement) {
      document.exitFullscreen();
    } else if (stage.requestFullscreen) {
      stage.requestFullscreen().catch(() => {});
    }
  }


  function startHeartbeat() {
    if (heartbeat) return;
    heartbeat = setInterval(() => {
      fetch('viewer/ping', { method: 'POST', body: token })
        .then((r) => { if (r.status === 409) loseSession(); })
        .catch(() => {});
    }, 2000);
  }

  function loseSession() {
    owner = false;
    clearInterval(heartbeat);
    heartbeat = null;
    if (statsTimer) { clearInterval(statsTimer); statsTimer = null; }
    setBlocked(true);
    setControlsEnabled(false);
    statusEl.textContent = 'disconnected';
    setTimeout(startWebRTC, 2000);
  }

  async function startWebRTC() {
    try {
      await connectWebRTC();
    } catch (e) {
      // Thrown before the watchdog exists - typically the server is not
      // listening yet (browser opened during startup) or ICE setup failed.
      scheduleReconnect('cannot reach the server');
    }
  }

  async function connectWebRTC() {
    const pc = new RTCPeerConnection();
    const tx = pc.addTransceiver('video', { direction: 'recvonly' });
    // Offer only the codec the server is actually encoding. Leaving the full
    // list in place lets the browser answer with a different payload than
    // webrtcbin is sending, which shows up as a black picture.
    try {
      const caps = RTCRtpReceiver.getCapabilities('video');
      if (caps && tx.setCodecPreferences) {
        const want = 'video/' + (window.WIZ_CODEC || 'H264');
        let picked = caps.codecs.filter(
          (c) => c.mimeType.toUpperCase() === want.toUpperCase());
        // H264 appears in several variants; the server answers the
        // packetization-mode=1 ones, so prefer those in the offer.
        const pm1 = picked.filter(
          (c) => (c.sdpFmtpLine || '').includes('packetization-mode=1'));
        if (pm1.length) picked = pm1;
        if (picked.length) tx.setCodecPreferences(picked);
      }
    } catch (e) { /* older browsers: leave the default order */ }
    pc.ontrack = (e) => {
      videoEl.srcObject = e.streams[0];
      // autoplay is not guaranteed even when muted (a backgrounded tab, a
      // stricter policy, or a stream attached before the element was ready),
      // and a paused element shows black while the stats happily report a
      // healthy connection. Ask explicitly and retry from the watchdog.
      videoEl.play().catch(() => {});
    };
    pc.oniceconnectionstatechange = () => { statusEl.textContent = pc.iceConnectionState; };

    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    // Wait for ICE gathering, but not forever: on a machine that just booted
    // the network stack can leave gathering pending, and waiting here would
    // hang the page silently. The candidates collected so far are enough for
    // a local connection.
    await new Promise((resolve) => {
      if (pc.iceGatheringState === 'complete') return resolve();
      const done = () => {
        if (pc.iceGatheringState === 'complete') { clearTimeout(t); resolve(); }
      };
      const t = setTimeout(resolve, 2000);
      pc.addEventListener('icegatheringstatechange', done);
    });

    try {
      const resp = await fetch('whep', {
        method: 'POST',
        headers: { 'Content-Type': 'application/sdp', 'X-Viewer-Token': token },
        body: pc.localDescription.sdp
      });
      if (resp.status === 409) {
        pc.close();
        setBlocked(true);
        setControlsEnabled(false);
        statusEl.textContent = 'blocked (another viewer)';
        // Retry fast the first time: after a reload the previous session is
        // released a moment later, so this reconnects almost immediately.
        setTimeout(startWebRTC, blockedTries++ === 0 ? 400 : 2000);
        return;
      }
      if (!resp.ok) {
        pc.close();
        scheduleReconnect('signaling failed (' + resp.status + ')');
        return;
      }
      const answer = await resp.text();
      await pc.setRemoteDescription({ type: 'answer', sdp: answer });
      owner = true;
      blockedTries = 0;
      setBlocked(false);
      setControlsEnabled(true);
      startHeartbeat();
      // ?mode=edit で開かれたページ: 操作権を得た最初の接続でエディタへ。
      if (wantEditorOnLoad && !editorModeRequested) {
        editorModeRequested = true;
        send('mode', { mode: 'editor' });
      }

      // Report what the decoder actually does with the incoming stream. If
      // bytes arrive but framesDecoded stays 0, the browser is dropping the
      // stream (usually a codec/profile mismatch) rather than not receiving it.
      // One timer only - reconnects must not stack them up.
      if (statsTimer) clearInterval(statsTimer);

      // Watchdog. A stream can fail to start for several reasons that all look
      // the same to the user (black video): the first keyframe is lost during
      // the handshake, ICE never completes, or the server was still starting
      // up when we connected. Rather than test for each cause, watch the one
      // thing that matters - are frames being decoded? - and reconnect
      // whenever progress stops. Retries continue with a backoff instead of
      // giving up after one attempt, which is what left the picture black
      // right after a machine or browser start.
      let lastDecoded = -1;
      let stallTicks = 0;
      lastBytes = -1;
      statsTimer = setInterval(async () => {
        let decoded = 0, bytes = 0, dropped = 0, sawVideo = false;
        try {
          const report = await pc.getStats();
          report.forEach((r) => {
            if (r.type === 'inbound-rtp' && r.kind === 'video') {
              sawVideo = true;
              decoded = r.framesDecoded || 0;
              bytes = r.bytesReceived || 0;
              dropped = r.framesDropped || 0;
            }
          });
        } catch (e) { /* connection going away; the checks below handle it */ }

        // What the <video> element itself is doing: frames can decode while
        // the element stays paused or never sizes itself, which looks exactly
        // like a dead stream to the user.
        const painting = videoEl.videoWidth > 0 && !videoEl.paused;
        // The overlay clears on the first painted frame and comes back if the
        // picture is lost, so it doubles as the reconnecting indicator - but
        // it must stay hidden while the blocked-viewer error box is up, or it
        // covers the box again (a blocked viewer never decodes a frame).
        loadingEl.classList.toggle(
            'done',
            (painting && decoded > 0) || errorEl.classList.contains('show'));
        loadingMsg.textContent = sawVideo
          ? (decoded > 0 ? 'starting video…' : 'waiting for the first frame…')
          : 'connecting…';

        // Throughput from the byte counter's own delta, so it reflects what is
        // arriving now rather than the session average.
        const nowMs = performance.now();
        let kbps = 0;
        if (lastBytes >= 0 && nowMs > lastBytesAt) {
          kbps = ((bytes - lastBytes) * 8) / (nowMs - lastBytesAt);  // b/ms = kb/s
        }
        lastBytes = bytes;
        lastBytesAt = nowMs;

        statusEl.innerHTML = sawVideo
          ? (pc.iceConnectionState + ' | <b>' + kbps.toFixed(0) + '</b> kbps' +
             ' | video <b>' + videoEl.videoWidth + 'x' + videoEl.videoHeight +
             '</b>' + (videoEl.paused ? ' (paused)' : '') +
             (dropped ? ' | dropped <b>' + dropped + '</b>' : ''))
          : (pc.iceConnectionState + ' | waiting for video...' +
             (videoEl.srcObject ? '' : ' (no track yet)'));

        // Frames are arriving but nothing is on screen: nudge playback rather
        // than tearing the session down - reconnecting would not help.
        if (decoded > 0 && !painting) {
          if (videoEl.paused) videoEl.play().catch(() => {});
          if (videoEl.srcObject && videoEl.videoWidth === 0) {
            // Re-attaching makes the element pick the stream up again after a
            // suspended tab or a mid-negotiation attach.
            const src = videoEl.srcObject;
            videoEl.srcObject = null;
            videoEl.srcObject = src;
            videoEl.play().catch(() => {});
          }
        }

        const dead = pc.iceConnectionState === 'failed' ||
                     pc.iceConnectionState === 'closed';
        const progressing = decoded > lastDecoded;
        if (progressing && decoded > 0) retryCount = 0;  // stream is healthy
        lastDecoded = decoded;

        // No decoded frames since the last tick counts as a stall - whether
        // that is "never started" or "froze after a while".
        if (!dead && progressing) { stallTicks = 0; return; }
        if (dead || ++stallTicks >= STALL_TICKS_BEFORE_RETRY) {
          clearInterval(statsTimer);
          statsTimer = null;
          try { pc.close(); } catch (e) {}
          owner = false;
          setControlsEnabled(false);
          scheduleReconnect('no picture');
        }
      }, 1000);
    } catch (err) {
      scheduleReconnect('connection error');
    }
  }

  // Live physics tuning: solver cost is roughly substeps x iterations.
  let curIter = 0;
  function bumpIter(delta) {
    const next = Math.max(10, Math.min(2000, curIter + delta));
    send('solver', { iterations: next });
  }

  // Performance overlay: physics thread rate vs render thread rate.
  function fmt(v, digits) { return (v || 0).toFixed(digits === undefined ? 1 : digits); }
  setInterval(() => {
    fetch('stats').then((r) => r.json()).then((s) => {
      if (s.cameraCount !== undefined && !camLinksDone) {
        document.getElementById('camLabel').textContent =
          (s.editorCam !== undefined && s.camera === s.editorCam)
            ? 'Editor Camera'
            : 'camera ' + s.camera + ' of ' + s.cameraCount;
        camLinksDone = true;
      }
      curIter = s.iterations;
      if (s.paused !== undefined) setPlayLabel(s.paused);
      if (s.versions && !versionsShown) {
        versionsShown = true;
        const v = s.versions;
        let html = '<div class="vTitle">WizEngine ' + (v['WizEngine'] || '') +
                   '</div>';
        for (const name of Object.keys(v)) {
          if (name === 'WizEngine') continue;
          html += '<div class="vLib"><span>' + name + '</span><span>' +
                  v[name] + '</span></div>';
        }
        document.getElementById('versions').innerHTML = html;
      }
      if (s.streamW !== undefined && !streamUiReady) {
        streamUiReady = true;
        const res = document.getElementById('strRes');
        const wanted = s.streamW + 'x' + s.streamH;
        // Add the server's current format as an option if the presets miss it.
        if (![...res.options].some((o) => o.value === wanted)) {
          const o = document.createElement('option');
          o.value = wanted;
          o.textContent = s.streamW + ' \u00d7 ' + s.streamH;
          res.insertBefore(o, res.firstChild);
        }
        res.value = wanted;
        document.getElementById('strFps').value = String(s.streamFps);
        const kb = document.getElementById('strKbps');
        kb.value = Math.min(Math.max(s.streamKbps, kb.min), kb.max);
        strKbpsLabel();
      }
      if (s.simTime !== undefined) {
        const t = s.simTime;
        const label = t >= 60
          ? Math.floor(t / 60) + ':' +
            String(Math.floor(t % 60)).padStart(2, '0') + '.' +
            String(Math.floor((t % 1) * 10))
          : t.toFixed(2) + ' s';
        document.getElementById('simTime').textContent = 't = ' + label;
      }
      document.getElementById('iterVal').textContent = s.iterations;
      document.getElementById('subVal').textContent = s.substeps;
      document.getElementById('hzVal').textContent = s.physicsTarget;
      document.getElementById('envVal').textContent = fmt(s.envelope * 1000, 1) + 'mm';
      document.getElementById('recVal').textContent = fmt(s.recovery, 2);
      // エディタ中は物理を回していないので、遅い扱いにしない。
      const editing = s.mode === 'editor';
      const slowPhysics = !editing && s.realtime < 0.95;
      const slowRender = s.renderFps < s.targetFps * 0.9;
      document.getElementById('perf').innerHTML =
        '<span>mode <b>' + (editing ? '✎ editor' : '▶ simulate') + '</b></span>' +
        '<span>joints <b>' + (s.joints || 0) + '</b></span>' +
        '<span class="' + (slowPhysics ? 'warn' : '') + '">physics <b>' +
          fmt(s.physicsHz) + ' Hz</b> (' + fmt(s.physicsMs, 2) + ' ms, x' +
          s.substeps + ' substeps)</span>' +
        '<span class="' + (slowPhysics ? 'warn' : '') + '">speed <b>' +
          fmt(s.realtime, 2) + 'x</b></span>' +
        '<span class="' + (slowRender ? 'warn' : '') + '">render <b>' +
          fmt(s.renderFps) + ' fps</b> (' + fmt(s.renderMs, 2) + ' ms)</span>' +
        '<span>solver <b>' + fmt(s.solverMs, 2) + ' ms</b></span>' +
        '<span>collision <b>' + fmt(s.collisionMs, 2) + ' ms</b></span>' +
        '<span>frame <b>' + fmt(s.frameMs, 2) + ' ms</b></span>' +
        '<span>bodies <b>' + s.bodies + '</b> (asleep <b>' + s.asleep + '</b>)</span>' +
        '<span>target <b>' + s.physicsTarget + ' Hz / ' + s.targetFps + ' fps</b></span>' +
        '<span>engine <b>' + s.engine + '</b></span>' +
        '<span>codec <b>' + s.codec + '</b></span>';
    }).catch(() => {});
  }, 500);

  // Release the session immediately on reload/close so the next page (or the
  // same one after F5) can take over without waiting for the timeout.
  window.addEventListener('pagehide', () => {
    if (owner) navigator.sendBeacon('viewer/leave', token);
  });

  // Ask the server which codec it encodes, then negotiate for exactly that.
  fetch('stats')
    .then((r) => r.json())
    .then((s) => { window.WIZ_CODEC = s.codec; })
    .catch(() => {})
    .finally(() => startWebRTC());

  // ---- Sidebar: hierarchy, cameras, inspector -----------------------------
  // Polls /scene, which reports the cameras (with their highlight colours and
  // what each holds) and the objects with their positions. Clicking an object
  // selects it for THIS page's camera, exactly like clicking it in the view.
  let sceneData = null;
  let objFilter = '';
  // Phones start with the panel closed: the video matters more on a small
  // screen, and the panel is one tap away.
  let sidebarOpen = window.matchMedia('(min-width: 901px)').matches;
  document.getElementById('sidebar').classList.toggle('hidden', !sidebarOpen);

  // ---- Resizable panel ---------------------------------------------------
  // Two splitters: one between the panel and the view (width), one under the
  // hierarchy list (height). Both write a CSS variable, so nothing else in the
  // layout needs to know a drag happened. Pointer events again, so a finger
  // works as well as a mouse.
  const SB_MIN = 170, SB_MAX = 560;
  // A section still has to show its header (and, for Objects, the filter box),
  // so the floor leaves room for those plus a row or two.
  const SECTION_MIN = 64;

  function readSize(name, fallback) {
    const v = parseFloat(
      getComputedStyle(document.documentElement).getPropertyValue(name));
    return Number.isFinite(v) ? v : fallback;
  }
  function setSize(name, px) {
    document.documentElement.style.setProperty(name, px + 'px');
  }
  const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

  function makeSplitter(el, opts) {
    let start = 0, startSize = 0;
    el.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      el.setPointerCapture(e.pointerId);
      el.classList.add('dragging');
      document.body.classList.add(opts.vertical ? 'resizing-col' : 'resizing-row');
      start = opts.vertical ? e.clientX : e.clientY;
      startSize = readSize(opts.varName, opts.fallback);
    });
    el.addEventListener('pointermove', (e) => {
      if (!el.hasPointerCapture(e.pointerId)) return;
      const delta = (opts.vertical ? e.clientX : e.clientY) - start;
      setSize(opts.varName, clamp(startSize + delta, opts.min, opts.max()));
    });
    const stop = (e) => {
      if (e && el.hasPointerCapture(e.pointerId)) {
        el.releasePointerCapture(e.pointerId);
      }
      el.classList.remove('dragging');
      document.body.classList.remove('resizing-col', 'resizing-row');
      try {
        localStorage.setItem(opts.varName, readSize(opts.varName, opts.fallback));
      } catch (err) { /* private mode: sizes just won't persist */ }
    };
    el.addEventListener('pointerup', stop);
    el.addEventListener('pointercancel', stop);

    // Restore the size chosen last time.
    try {
      const saved = parseFloat(localStorage.getItem(opts.varName));
      if (Number.isFinite(saved)) {
        setSize(opts.varName, clamp(saved, opts.min, opts.max()));
      }
    } catch (err) { /* ignore */ }
  }

  makeSplitter(document.getElementById('sbResize'), {
    vertical: true, varName: '--sb-width', fallback: 250,
    min: SB_MIN, max: () => Math.min(SB_MAX, window.innerWidth - 240)
  });
  // One splitter per section: dragging a grip resizes the section directly
  // above it. The sections below simply move, and the last one takes whatever
  // room is left, so no drag can push content out of the panel.
  const SECTION_DEFAULTS = { cameras: 96, objects: 260, events: 132, inspector: 96 };
  for (const grip of document.querySelectorAll('.rowResize[data-resize]')) {
    const name = grip.dataset.resize;
    makeSplitter(grip, {
      vertical: false,
      varName: '--' + name + '-height',
      fallback: SECTION_DEFAULTS[name],
      min: SECTION_MIN,
      // Never let one section grow past the panel itself.
      max: () => Math.max(SECTION_MIN, window.innerHeight - 200)
    });
  }

  // ---- Collapsible sections ---------------------------------------------
  // Each section remembers whether it was open, so the panel comes back the
  // way it was left.
  function toggleSection(name, force) {
    const sec = document.querySelector('.sbSec[data-sec="' + name + '"]');
    if (!sec) return;
    const collapsed = (force === undefined) ? !sec.classList.contains('collapsed')
                                            : force;
    sec.classList.toggle('collapsed', collapsed);
    try { localStorage.setItem('sec:' + name, collapsed ? '1' : '0'); }
    catch (e) { /* private mode: just don't persist */ }
  }
  for (const sec of document.querySelectorAll('.sbSec[data-sec]')) {
    const name = sec.dataset.sec;
    try {
      if (localStorage.getItem('sec:' + name) === '1') toggleSection(name, true);
    } catch (e) { /* ignore */ }
  }

  // 下部アセットパネルの折りたたみ。サイドバーのセクションと同じ流儀で、
  // 状態は localStorage に記憶する（キーも sec: に揃える）。
  function toggleAssets(force) {
    const panel = document.getElementById('assets');
    const collapsed = (force === undefined)
      ? !panel.classList.contains('collapsed')
      : force;
    panel.classList.toggle('collapsed', collapsed);
    try { localStorage.setItem('sec:assets', collapsed ? '1' : '0'); }
    catch (e) { /* private mode: just don't persist */ }
  }
  try {
    if (localStorage.getItem('sec:assets') === '1') toggleAssets(true);
  } catch (e) { /* ignore */ }

  // アセットパネルの高さは textarea と同じ右下のつまみ（CSS の
  // resize:vertical）。ドラッグの結果はブラウザが inline style に書くだけ
  // なので、ResizeObserver で拾って localStorage に覚え、次回復元する。
  {
    const list = document.getElementById('assetList');
    try {
      const h = parseFloat(localStorage.getItem('assets-height'));
      if (Number.isFinite(h) && h >= 56) list.style.height = h + 'px';
    } catch (e) { /* ignore */ }
    if (window.ResizeObserver) {
      new ResizeObserver(() => {
        const h = Math.round(list.getBoundingClientRect().height);
        if (h < 56) return;  // 非表示（0px）や折りたたみ中は覚えない
        try { localStorage.setItem('assets-height', String(h)); }
        catch (e) { /* private mode: sizes just won't persist */ }
      }).observe(list);
    }
  }

  // ---- Events ------------------------------------------------------------
  // A short log of what happened in the scene: who grabbed or released what,
  // and cameras being taken or freed. Derived from the /scene poll rather than
  // pushed by the server, so it costs nothing extra.
  const MAX_EVENTS = 60;
  const events = [];
  let prevSel = null, prevBusy = null;

  function logEvent(msg) {
    const now = new Date();
    const t = String(now.getHours()).padStart(2, '0') + ':' +
              String(now.getMinutes()).padStart(2, '0') + ':' +
              String(now.getSeconds()).padStart(2, '0');
    events.unshift({ t, msg });          // newest first
    if (events.length > MAX_EVENTS) events.pop();
    renderEvents();
  }
  function clearEvents() {
    events.length = 0;
    renderEvents();
  }
  function renderEvents() {
    const el = document.getElementById('evtList');
    if (!el) return;
    el.innerHTML = events.map(e =>
      '<div class="evt"><span class="t">' + e.t + '</span>' +
      '<span class="m">' + e.msg + '</span></div>').join('');
  }

  function noteSceneChanges(s) {
    if (!s || !s.cameras) return;
    const sel = s.cameras.map(c => c.selected);
    const busy = (s.busy || []).slice();
    const label = (i) => cameraName(i) + (i === s.camera ? ' (this)' : '');

    if (prevSel) {
      for (let i = 0; i < sel.length; i++) {
        if (sel[i] === prevSel[i]) continue;
        logEvent(sel[i] >= 0
          ? label(i) + ' grabbed object ' + sel[i]
          : label(i) + ' released object ' + prevSel[i]);
      }
    }
    if (prevBusy) {
      for (let i = 0; i < busy.length; i++) {
        if (busy[i] === prevBusy[i]) continue;
        logEvent(label(i) + (busy[i] ? ' opened by a viewer' : ' closed'));
      }
    }
    prevSel = sel;
    prevBusy = busy;
  }

  // Tabs are just show/hide - all panes stay in the DOM so their controls
  // keep updating from the stats poll even while hidden.
  const TABS = { scene: 'paneScene', editor: 'paneEditor', physics: 'panePhysics' };
  const TAB_BUTTONS = { scene: 'tabScene', editor: 'tabEditor', physics: 'tabPhysics' };
  function showTab(name) {
    for (const key of Object.keys(TABS)) {
      document.getElementById(TABS[key]).hidden = (key !== name);
      document.getElementById(TAB_BUTTONS[key])
              .classList.toggle('active', key === name);
    }
  }

  function toggleSidebar() {
    sidebarOpen = !sidebarOpen;
    document.getElementById('sidebar').classList.toggle('hidden', !sidebarOpen);
  }

  document.getElementById('objFilter').addEventListener('input', (e) => {
    objFilter = e.target.value.trim();
    renderHierarchy();
  });

  function selectObject(index) {
    // Clicking the already-selected entry clears it.
    const mine = myCameraSelection();
    send('select', { index: (mine === index) ? -1 : index });
  }

  function myCameraSelection() {
    if (!sceneData || !sceneData.cameras) return -1;
    const me = sceneData.cameras[sceneData.camera];
    return me ? me.selected : -1;
  }

  function renderCameras() {
    if (!sceneData || !sceneData.cameras) return;
    const el = document.getElementById('camList');
    // Every camera lives on the SAME port under its own path ("/cam0/",
    // "/cam1/", ...); the server sends the list.
    const editing = sceneData.mode === 'editor' && isEditorCam();
    const eSel = myEditorSel();
    const eCam = (sceneData.editorCam !== undefined) ? sceneData.editorCam : 0;
    el.innerHTML = '';
    for (const c of sceneData.cameras) {
      if (c.active === false) continue;  // 「削除」されたスロットは出さない
      const isMe = c.index === sceneData.camera;
      // Only one viewer per camera, so "in use" tells you whether switching
      // there would be refused.
      const busy = sceneData.busy && sceneData.busy[c.index];
      const state = isMe ? 'viewing' : (busy ? 'in use' : 'free');
      const row = document.createElement('div');
      const edSel = eSel.kind === 'camera' && eSel.index === c.index;
      row.className = 'item' + (isMe ? ' sel' : '') + (edSel ? ' edsel' : '');
      row.innerHTML =
        '<span class="dot" style="background:' + c.color + '"></span>' +
        '<span>' + cameraName(c.index) + (isMe ? ' (this)' : '') + '</span>' +
        '<span class="sub' + (busy && !isMe ? ' busy' : '') + '">' +
        state + '</span>';
      // エディタ中は、行のクリック（ページ移動）とは別に ✎（エディタ選択）
      // と 🗑（削除）を常に出す。Editor Camera 自身は選べない・消せない。
      if (owner && editing && c.index !== eCam) {
        const btn = document.createElement('span');
        btn.className = 'camEdit' + (edSel ? ' on' : '');
        btn.textContent = '✎';
        btn.title = edSel ? '選択中（もう一度で解除）'
                          : 'エディタで選択（位置・向きを編集）';
        btn.onclick = (e) => {
          e.stopPropagation();
          selectCamera(c.index);
          // 選択したらインスペクタを開く。編集の続き（数値・削除）が
          // そこにあるので、タブを探させない。
          if (!edSel) showTab('editor');
        };
        row.appendChild(btn);
        const del = document.createElement('span');
        del.className = 'camEdit del';
        del.textContent = '🗑';
        del.title = 'このカメラを削除';
        del.onclick = (e) => { e.stopPropagation(); removeCameraAt(c.index); };
        row.appendChild(del);
      }
      if (!isMe) {
        row.title = 'Switch to this camera';
        // Same tab: leaving this page also releases the viewer session, so the
        // camera we came from frees up immediately.
        row.onclick = () => { location.href = camPath(c.index); };
      }
      el.appendChild(row);
    }
    // 見出し横の ＋（追加）。エディタモード×Editor Camera のページでだけ
    // 出す。削除は各行の 🗑（見出しに － は置かない）。
    const hdr = document.getElementById('camAddDel');
    hdr.hidden = !(owner && editing);
    if (!hdr.hidden) {
      const anyFree = sceneData.cameras.some((c) => c.active === false);
      const add = document.getElementById('camAdd');
      add.disabled = !anyFree;
      add.title = anyFree ? 'カメラを追加'
                          : 'カメラは上限です（' + sceneData.cameras.length +
                            ' - 起動引数 --max-cameras で変更）';
    }
  }

  function renderHierarchy() {
    if (!sceneData || !sceneData.objects) return;
    const all = sceneData.objects;
    const rows = objFilter
      ? all.filter(o => String(o.index).includes(objFilter))
      : all;
    document.getElementById('objCount').textContent =
      rows.length + (rows.length !== all.length ? ' / ' + all.length : '');

    const mine = myCameraSelection();
    const colorOf = (i) => {
      const c = sceneData.cameras[i];
      return c ? c.color : '#888';
    };
    // Cap the DOM: 512+ rows rebuilt twice a second is wasteful, and a list
    // that long is unreadable anyway - filter to narrow it down.
    const shown = rows.slice(0, 200);
    // 形はオブジェクトごとに違う（エディタで箱も球も置けるので）。名前を
    // 付けた物はその名前、付けていない物は「形 + 番号」で表示する。
    const KIND = { box: 'Box', sphere: 'Sphere', model: 'Model' };
    const labelOf = (o) =>
      o.name ? o.name : ((KIND[o.shape] || 'Object') + ' ' + o.index);
    let html = '';
    // Lights first: part of the scene like everything else. エディタモード中の
    // Editor Camera ページではクリックでエディタ選択（Inspector とギズモの
    // 対象）になる。The dot shows the light's colour; the sub line its
    // kind and intensity (lux for sun, lumens otherwise).
    if (sceneData.lights && !objFilter) {
      const kindName = { sun: 'Sun', point: 'Point', spot: 'Spot' };
      const kindIcon = { sun: '☀', point: '💡', spot: '🔦' };
      const fmtI = (v) => v >= 1000 ? Math.round(v / 1000) + 'k' : Math.round(v);
      const canPick = owner && sceneData.mode === 'editor' && isEditorCam();
      const eSel = myEditorSel();
      for (const l of sceneData.lights) {
        const edSel = eSel.kind === 'light' && eSel.index === l.index;
        const label = l.name ? l.name : ('Light ' + l.index);
        const unit = l.kind === 'sun' ? 'lx' : 'lm';
        html += '<div class="item' + (edSel ? ' edsel' : '') + '"' +
                (canPick
                  ? ' onclick="selectLight(' + l.index + ')"' +
                    ' title="クリックで選択（位置・向き・色を編集）"'
                  : ' style="cursor:default"') +
                '><span class="dot" style="background:' + l.color + '"></span>' +
                '<span>' + (kindIcon[l.kind] || '💡') + ' ' + label + '</span>' +
                '<span class="sub">' + (kindName[l.kind] || l.kind) +
                (l.shadows ? '·影' : '') + ' · ' +
                fmtI(l.intensity) + ' ' + unit + '</span></div>';
      }
    }
    for (const o of shown) {
      const isMine = o.index === mine;
      const dot = (o.heldBy >= 0)
        ? '<span class="dot" style="background:' + colorOf(o.heldBy) + '"></span>'
        : '<span class="dot"></span>';
      html += '<div class="item' + (isMine ? ' sel' : '') +
              '" onclick="selectObject(' + o.index + ')">' + dot +
              '<span>' + labelOf(o) + (o.fixed ? ' ⚓' : '') + '</span>' +
              '<span class="sub">' + o.y.toFixed(2) + 'm</span></div>';
    }
    if (rows.length > shown.length) {
      html += '<div class="item" style="cursor:default;opacity:.5">… ' +
              (rows.length - shown.length) + ' more (use the filter)</div>';
    }
    document.getElementById('objList').innerHTML = html;
  }

  // 選択中のオブジェクトの設計値。サーバが選択ぶんだけ丸ごと送ってくれる
  // （objects[] は番号が飛ぶことがあるので、配列の位置では引けない）。
  function renderInspector() {
    const el = document.getElementById('inspector');
    const o = (sceneData && sceneData.selected) ? sceneData.selected : null;
    if (!o) {
      el.textContent = 'nothing selected';
      return;
    }
    const drawn = o.shape === 'model'
      ? ('Model (' + (o.mesh || '?') + ')')
      : (o.shape === 'sphere' ? 'Sphere' : 'Box');
    const size = o.shape === 'box'
      ? [o.size.x, o.size.y, o.size.z].map((v) => v.toFixed(2)).join(' × ')
      : ('⌀ ' + o.size.x.toFixed(2));
    const px = (o.px !== undefined) ? o.px : o.position.x;
    const py = (o.py !== undefined) ? o.py : o.position.y;
    const pz = (o.pz !== undefined) ? o.pz : o.position.z;
    el.innerHTML =
      '<div><span class="k">object</span> #' + o.index +
      (o.name ? ' — ' + o.name : '') + '</div>' +
      '<div><span class="k">drawn as</span> ' + drawn + '</div>' +
      '<div><span class="k">position</span> ' +
      px.toFixed(2) + ', ' + py.toFixed(2) + ', ' + pz.toFixed(2) + '</div>' +
      '<div><span class="k">size</span> ' + size + '</div>' +
      '<div><span class="k">mass</span> ' + o.mass.toFixed(3) + ' kg' +
      (o.fixed ? ' (fixed)' : '') + '</div>' +
      '<div><span class="k">collision</span> ' + o.collision + '</div>';
  }

  // ---- Editor タブ ---------------------------------------------------------
  // サーバ（/scene）が唯一の正。ここでは「送る」と「映す」だけをやる。
  // 入力欄はポーリングのたびに上書きされるが、編集中の欄（フォーカスが
  // 当たっている欄）だけは触らない - でないと数字を打っている途中で
  // 消えてしまう。
  let jointPartner = -1;      // ジョイントの相手 B。-1 = 地面

  function setField(id, value) {
    const el = document.getElementById(id);
    if (!el || el === document.activeElement) return;
    if (el.type === 'checkbox') el.checked = !!value;
    else el.value = value;
  }
  function num(id, fallback) {
    const v = parseFloat(document.getElementById(id).value);
    return Number.isFinite(v) ? v : fallback;
  }
  const round2 = (v) => Math.round((v || 0) * 1000) / 1000;

  function setMode(mode) { send('mode', { mode: mode }); }

  // ---- ギズモ --------------------------------------------------------------
  // 見た目と当たり判定はサーバー側（3D の線として描かれる）。ここでやるのは
  // 設定を送ることと、掴んでいないときのカーソル位置を知らせることだけ。
  function applyGizmo(patch) {
    const g = (sceneData && sceneData.gizmo) || {};
    send('edit.gizmo', Object.assign({
      mode: g.mode || 'translate',
      space: g.space || 'world',
      snap: document.getElementById('gzSnap').checked,
      moveStep: num('gzMoveStep', 0.25),
      rotateStep: num('gzRotStep', 15),
      scaleStep: num('gzScaleStep', 0.1),
      grid: document.getElementById('gzGrid').checked,
      gridStep: num('gzGridStep', 1)
    }, patch || {}));
  }
  function setGizmoMode(mode) { applyGizmo({ mode: mode }); }
  function setGizmoSpace(space) { applyGizmo({ space: space }); }
  function toggleGizmoSpace() {
    const g = (sceneData && sceneData.gizmo) || {};
    setGizmoSpace(g.space === 'local' ? 'world' : 'local');
  }
  // ギズモ設定（スナップ・刻み・グリッド）は普段は隠し、ツールバーの ⚙ で
  // Inspector に開く。Inspector を「選択しているオブジェクトの内容」だけに
  // しておくため。ブラウザ内の見た目の話なのでサーバーには送らない。
  let gizmoSettingsOpen = false;
  function toggleGizmoSettings() {
    gizmoSettingsOpen = !gizmoSettingsOpen;
    if (gizmoSettingsOpen) showTab('editor');  // 隠れたタブの中で開いても気づけない
    renderEditor();
  }

  // どのハンドルを掴めるかは押してみるまで分からない、では使いづらいので、
  // 選択中はカーソル位置を控えめな間隔で送ってサーバーに光らせてもらう。
  // ドラッグ中は drag が同じ情報を運ぶので送らない。ギズモはエディタカメラ
  // にしか出ないので、送るのもそのページだけ。
  let hoverNdc = null;
  setInterval(() => {
    if (!owner || grabbing || dragMode) return;
    if (!sceneData || sceneData.mode !== 'editor' || !isEditorCam()) return;
    // ギズモが出ている対象（オブジェクト / ライト / カメラ）があるときだけ。
    const hasTarget = sceneData.selected ||
        (sceneData.editorSel && sceneData.editorSel.kind !== 'none');
    if (!hasTarget || !hoverNdc) return;
    send('hover', hoverNdc);
  }, 70);

  // アセットパネルの mesh タイル（配列番号 → いまの一覧から名前を引く）。
  function addMeshAsset(i) {
    const m = ((sceneData && sceneData.meshes) || [])[i];
    if (m) addObject('model', m.name);
  }
  function addObject(shape, mesh) {
    const msg = {
      shape: shape,
      size: num('edNewSize', 0.5),
      color: document.getElementById('edNewColor').value,
      mass: 1.0
    };
    // glTF モデルの配置（アセットパネルの mesh タイル）。size は当たり判定の
    // 大きさで、見た目の大きさは文書の <mesh scale> が決める。
    if (mesh) msg.mesh = mesh;
    send('edit.add', msg);
  }

  // 触った項目だけを送る（edit.set は部分更新）。全部まとめて送ると、
  // シミュレート中に質量だけ変えたつもりが「0.5 秒前に表示していた位置」へ
  // 物体を引き戻してしまう - 位置の欄はポーリングで更新されているため。
  function applyEdit(patch) {
    const sel = mySelectedDesc();
    if (!sel) return;
    send('edit.set', Object.assign({ index: sel.index }, patch));
  }
  function applyPos() {
    applyEdit({ position: { x: num('edPX', 0), y: num('edPY', 0),
                            z: num('edPZ', 0) } });
  }
  function applyRot() {
    applyEdit({ rotation: { x: num('edRX', 0), y: num('edRY', 0),
                            z: num('edRZ', 0) } });
  }
  function applySize() {
    const sel = mySelectedDesc();
    if (!sel) return;
    const sx = num('edSX', 0.5);
    // 球は直径ひとつ。X の欄だけを見て 3 成分に配る。
    applyEdit({ size: sel.shape === 'box'
      ? { x: sx, y: num('edSY', 0.5), z: num('edSZ', 0.5) }
      : { x: sx, y: sx, z: sx } });
  }
  function applyMass() { applyEdit({ mass: num('edMass', 1) }); }
  function applyFixed() {
    applyEdit({ fixed: document.getElementById('edFixed').checked });
  }
  function applyColor() {
    applyEdit({ color: document.getElementById('edColor').value });
  }
  function applyName() {
    applyEdit({ name: document.getElementById('edName').value });
  }

  function mySelectedDesc() {
    return (sceneData && sceneData.selected) ? sceneData.selected : null;
  }

  // ---- ライト / カメラ（エディタモード×Editor Camera 専用）-----------------
  // 選択はサーバー（EditorState）が持ち、/scene の editorSel・selectedLight・
  // selectedCamera で返ってくる。ビューのアイコンをクリックしても同じ選択に
  // なる（あちらは pick がサーバー側で判定）。
  function camPath(i) {
    return (sceneData && sceneData.paths && sceneData.paths[i])
      ? sceneData.paths[i] : ('/cam' + i + '/');
  }
  function myEditorSel() {
    return (sceneData && sceneData.editorSel)
      ? sceneData.editorSel : { kind: 'none', index: -1 };
  }
  function selectLight(index) {
    const s = myEditorSel();
    send('select.light',
         { index: (s.kind === 'light' && s.index === index) ? -1 : index });
  }
  function selectCamera(index) {
    const s = myEditorSel();
    send('select.camera',
         { index: (s.kind === 'camera' && s.index === index) ? -1 : index });
  }

  function addLight(kind) { send('edit.light.add', { kind: kind }); }
  function applyLightEdit(patch) {
    const l = sceneData && sceneData.selectedLight;
    if (!l) return;
    send('edit.light.set', Object.assign({ index: l.index }, patch));
  }
  function applyLightName() {
    applyLightEdit({ name: document.getElementById('ltName').value });
  }
  function applyLightPos() {
    applyLightEdit({ position: { x: num('ltPX', 0), y: num('ltPY', 3),
                                 z: num('ltPZ', 0) } });
  }
  function applyLightRot() {
    applyLightEdit({ rotation: { x: num('ltRX', 0), y: num('ltRY', 0),
                                 z: num('ltRZ', 0) } });
  }
  function applyLightColor() {
    applyLightEdit({ color: document.getElementById('ltColor').value });
  }
  function applyLightIntensity() {
    applyLightEdit({ intensity: num('ltIntensity', 300000) });
  }
  function applyLightFalloff() {
    applyLightEdit({ falloff: num('ltFalloff', 25) });
  }
  function applyLightCone() {
    applyLightEdit({ spotInnerDeg: num('ltInner', 25),
                     spotOuterDeg: num('ltOuter', 35) });
  }
  function removeLight() {
    const l = sceneData && sceneData.selectedLight;
    if (l) send('edit.light.remove', { index: l.index });
  }

  function addCamera() { send('edit.camera.add'); }
  function applyCamEdit(patch) {
    const c = sceneData && sceneData.selectedCamera;
    if (!c) return;
    send('edit.camera.set', Object.assign({ index: c.index }, patch));
  }
  function applyCamPos() {
    applyCamEdit({ position: { x: num('cmPX', 0), y: num('cmPY', 2),
                               z: num('cmPZ', 0) } });
  }
  function applyCamRot() {
    applyCamEdit({ rotation: { x: num('cmRX', 0), y: num('cmRY', 0), z: 0 } });
  }
  function removeCameraAt(index) {
    if (!confirm(cameraName(index) +
                 ' を一覧から削除します。よろしいですか？')) return;
    send('edit.camera.remove', { index: index });
  }
  function removeCamera() {  // Inspector の削除ボタン（選択中のカメラ）
    const c = sceneData && sceneData.selectedCamera;
    if (c) removeCameraAt(c.index);
  }
  function openCamPage() {
    const c = sceneData && sceneData.selectedCamera;
    if (c) location.href = camPath(c.index);
  }

  function setJointPartner() {
    const sel = mySelectedDesc();
    if (!sel) return;
    jointPartner = sel.index;
    renderEditor();
  }
  function clearJointPartner() { jointPartner = -1; renderEditor(); }

  function createJoint() {
    const sel = mySelectedDesc();
    if (!sel) return;
    const axis = document.getElementById('edJointAxis').value;
    send('edit.joint.add', {
      kind: document.getElementById('edJointKind').value,
      a: sel.index,
      b: jointPartner,
      ax: axis === 'x' ? 1 : 0,
      ay: axis === 'y' ? 1 : 0,
      az: axis === 'z' ? 1 : 0
    });
  }
  function removeJoint(index) { send('edit.joint.remove', { index: index }); }

  // シミュレート設定（Physics タブ）。物理レートはここでは送らない - Rate
  // セクション（rate コマンド）が受け持ち、サーバー側で保存値にも映る。
  function applySim() {
    send('edit.sim', {
      gravity: num('edGravity', -9.81),
      friction: num('edFriction', 0.6),
      restitution: num('edRestitution', 0),
      linearDamping: num('edLinDamp', 0.15),
      angularDamping: num('edAngDamp', 0.6),
      sleeping: document.getElementById('edSleeping').checked
    });
  }
  function applyCustomRate() {
    const hz = Math.round(num('hzCustom', 60));
    if (hz >= 10 && hz <= 240) send('rate', { hz: hz });
  }

  // ---- World（地面・環境光）--------------------------------------------
  // 触った欄も含めて節の値を丸ごと送る（部分更新はサーバー側が現在値へ
  // 重ねるので、こちらは見えている値をそのまま渡せばよい）。
  function applyGround() {
    send('edit.ground', {
      size: num('edGroundSize', 10),
      visual: num('edGroundVisual', 8),
      texture: document.getElementById('edGroundTex').value.trim(),
      tile: num('edGroundTile', 2),
      color: document.getElementById('edGroundColor').value
    });
  }
  function applyEnvironment() {
    send('edit.environment', {
      hdr: document.getElementById('edEnvHdr').value.trim(),
      intensity: num('edEnvIntensity', 30000)
    });
  }

  function saveScene() {
    const name = document.getElementById('edSceneName').value.trim();
    if (!name) return;
    send('edit.save', { name: name });
  }
  // 読込はアセットパネルのタイルのダブルクリック（assetLoad）が受け持つ。
  // シーンの中身は MuJoCo 風の XML が正（assets/scenes/<名前>.xml）。保存
  // しなくても、いま組んでいる中身は /scene.xml で読める。
  function openSceneXml() { window.open('scene.xml', '_blank'); }

  // ---- シーン XML エディタ（モーダル）------------------------------------
  // /scene.xml のテキストをそのまま編集し、「✓ 適用」で edit.xml として
  // 送る。検証と適用はサーバー側（壊れた XML は適用されず、理由が status に
  // 出る）ので、ここは取得・送信・表示だけ。適用結果はモーダル右上に
  // status をミラーして見せる（送信は非同期でレスポンスに結果が無いため）。
  let xmlEdOpen = false;
  function toggleXmlEditor() { xmlEdOpen ? closeXmlEditor() : openXmlEditor(); }
  function openXmlEditor() {
    xmlEdOpen = true;
    document.getElementById('xmlEd').hidden = false;
    reloadXml();
  }
  function closeXmlEditor() {
    xmlEdOpen = false;
    document.getElementById('xmlEd').hidden = true;
  }
  function reloadXml() {
    fetch('scene.xml')
      .then((r) => r.text())
      .then((t) => { document.getElementById('xmText').value = t; })
      .catch(() => { document.getElementById('xmText').value = ''; });
  }
  function applyXml() {
    send('edit.xml', { text: document.getElementById('xmText').value });
    document.getElementById('xmStatus').textContent = '適用中...';
  }
  function clearScene() {
    if (!confirm('シーンのオブジェクトとジョイントを全部消します。よろしいですか？')) return;
    send('edit.clear');
  }

  const JOINT_LABEL = {
    revolute: 'ちょうつがい', spherical: 'ボール', fixed: '固定',
    prismatic: '直動', distance: '距離'
  };
  // ビューに引く線と同じ色。どの線がどの行かを目で追えるようにする。
  const JOINT_COLOR = {
    revolute: '#ff8c26', spherical: '#f273d9', fixed: '#f2d933',
    prismatic: '#59e6e6', distance: '#99f266'
  };

  // このページのカメラがエディタカメラか。違うページでは Inspector タブを
  // 隠す（シーンを書き換える操作はサーバー側でも同じ判定で弾かれる。
  // モード切替だけはどのページからでも可）。
  function isEditorCam() {
    if (!sceneData) return false;
    const e = (sceneData.editorCam !== undefined) ? sceneData.editorCam : 0;
    return sceneData.camera === e;
  }

  // カメラの表示名。エディタカメラは番号ではなく役割で呼ぶ（一覧・イベント・
  // ラベルの全部で同じ名前になるよう、ここ 1 か所で決める）。
  function cameraName(i) {
    const e = (sceneData && sceneData.editorCam !== undefined)
      ? sceneData.editorCam : 0;
    return i === e ? 'Editor Camera' : ('Camera ' + i);
  }

  function renderEditor() {
    if (!sceneData) return;
    const editing = sceneData.mode === 'editor';
    const editorHere = isEditorCam();

    document.getElementById('tabEditor').style.display =
      editorHere ? '' : 'none';
    if (!editorHere && !document.getElementById('paneEditor').hidden) {
      showTab('scene');  // 隠したタブを開いたままにしない
    }

    // モード切替はどのカメラのページからでもできる（サーバー側も同じ扱い）。
    // Editor Camera は「編集できる」カメラなだけで、モードを握ってはいない。
    const bEdit = document.getElementById('btnModeEdit');
    const bSim = document.getElementById('btnModeSim');
    bEdit.classList.toggle('active', editing);
    bSim.classList.toggle('active', !editing);
    bEdit.disabled = bSim.disabled = !owner;
    if (!editorHere) {
      const eCamIdx =
        (sceneData.editorCam !== undefined) ? sceneData.editorCam : 0;
      bEdit.title = '物理を止めて配置・設計する（編集操作は Editor Camera ' +
        '= /cam' + eCamIdx + '/ のページから）';
    } else {
      bEdit.title = '物理を止めて配置・設計する';
    }

    // エディタ中は時間が進まないので、Play / Reset ボタンとタイムスタンプは
    // 使わない。無効化ではなく行ごと非表示にする。
    document.querySelector('.playRow').style.display = editing ? 'none' : '';
    document.getElementById('simTime').style.display = editing ? 'none' : '';
    document.getElementById('btnPlay').disabled = editing || !owner;

    // ギズモ。モード切替は映像左上のツールバー（Unity のシーンビューと同じ
    // 場所）。ギズモが出るページ = エディタモード中の Editor Camera でだけ
    // 表示し、現在のモードをボタンの点灯で示す。
    const bar = document.getElementById('gizmoBar');
    bar.hidden = !(editing && editorHere);
    bar.querySelectorAll('button').forEach((b) => { b.disabled = !owner; });
    // ライト / カメラを選んでいるあいだ、拡縮に意味は無い（サーバー側は
    // 移動として扱う）。ボタンも無効にして分かるようにしておく。
    {
      const eSel = myEditorSel();
      const nonObject = eSel.kind !== 'none';
      const scaleBtn = document.getElementById('gzScale');
      scaleBtn.disabled = !owner || nonObject;
      scaleBtn.title = nonObject
        ? 'ライト / カメラは拡縮できません' : '拡縮 (R)';
    }
    const gz = sceneData.gizmo;
    if (gz) {
      document.getElementById('gzMove').classList
              .toggle('on', gz.mode === 'translate');
      document.getElementById('gzRot').classList
              .toggle('on', gz.mode === 'rotate');
      document.getElementById('gzScale').classList
              .toggle('on', gz.mode === 'scale');
      // World/Local は 1 個のトグルボタン。今の状態を表示し、押すと反対へ。
      document.getElementById('gzSpace').textContent =
        gz.space === 'local' ? '📦 Local' : '🌐 World';
      setField('gzSnap', gz.snap);
      setField('gzMoveStep', gz.moveStep);
      setField('gzRotStep', gz.rotateStep);
      setField('gzScaleStep', gz.scaleStep);
      setField('gzGrid', gz.grid);
      setField('gzGridStep', gz.gridStep);
    }
    // World 節（地面・環境光）。入力欄は「フォーカス中は触らない」の
    // 既定どおり setField 経由で。
    if (sceneData.ground) {
      const g = sceneData.ground;
      setField('edGroundSize', round2(g.size));
      setField('edGroundVisual', round2(g.visual));
      setField('edGroundTex', g.texture);
      setField('edGroundTile', round2(g.tile));
      setField('edGroundColor', g.color);
    }
    if (sceneData.environment) {
      setField('edEnvHdr', sceneData.environment.hdr);
      setField('edEnvIntensity', Math.round(sceneData.environment.intensity));
    }
    // ギズモ設定の節はツールバーの ⚙ で開閉（エディタモード中だけ意味がある）。
    document.getElementById('gzSettings')
            .classList.toggle('on', gizmoSettingsOpen);
    document.getElementById('secGizmo').hidden =
      !(gizmoSettingsOpen && editing);
    // ノードエディタの開閉状態をツールバーの ⚡ に映す。
    document.getElementById('gzNodes').classList.toggle('on', nodeEdOpen);

    // 選択中のオブジェクト / ライト / カメラ。同時に立つのは 1 つだけ
    // （サーバー側で排他している）。Inspector は選んでいるものの内容だけを
    // 出すので、種類ごとに節を丸ごと入れ替える。
    const sel = mySelectedDesc();
    const lightSel = (editing && sceneData.selectedLight)
      ? sceneData.selectedLight : null;
    const camSel = (editing && sceneData.selectedCamera)
      ? sceneData.selectedCamera : null;
    document.getElementById('secObj').hidden = !!lightSel || !!camSel;
    document.getElementById('secLight').hidden = !lightSel;
    document.getElementById('secCam').hidden = !camSel;

    if (lightSel) {
      const K = { sun: '☀ Sun（平行光）', point: '💡 Point', spot: '🔦 Spot' };
      document.getElementById('ltKind').textContent =
        (K[lightSel.kind] || lightSel.kind) + ' — #' + lightSel.index +
        (lightSel.shadows ? ' ・影あり' : '');
      setField('ltName', lightSel.name || '');
      setField('ltPX', round2(lightSel.position.x));
      setField('ltPY', round2(lightSel.position.y));
      setField('ltPZ', round2(lightSel.position.z));
      setField('ltRX', round2(lightSel.rotation.x));
      setField('ltRY', round2(lightSel.rotation.y));
      setField('ltRZ', round2(lightSel.rotation.z));
      setField('ltColor', lightSel.color);
      setField('ltIntensity', Math.round(lightSel.intensity));
      setField('ltFalloff', round2(lightSel.falloff));
      setField('ltInner', round2(lightSel.spotInnerDeg));
      setField('ltOuter', round2(lightSel.spotOuterDeg));
      document.getElementById('ltIntLabel').textContent =
        lightSel.kind === 'sun' ? '強さ (lx)' : '強さ (lm)';
      // Sun に減衰は無く、Point に向きは無い。円錐は Spot だけ。
      document.getElementById('ltFalloffRow').hidden = lightSel.kind === 'sun';
      document.getElementById('ltRotRow').hidden = lightSel.kind === 'point';
      document.getElementById('ltConeRow').hidden = lightSel.kind !== 'spot';
    }
    if (camSel) {
      document.getElementById('cmLabel').textContent =
        cameraName(camSel.index) + ' — ' + camPath(camSel.index);
      setField('cmPX', round2(camSel.position.x));
      setField('cmPY', round2(camSel.position.y));
      setField('cmPZ', round2(camSel.position.z));
      setField('cmRX', round2(camSel.rotation.x));
      setField('cmRY', round2(camSel.rotation.y));
    }

    document.getElementById('edNoSel').hidden = !!sel;
    document.getElementById('edProps').hidden = !sel;
    if (sel) {
      const sphere = sel.shape !== 'box';
      setField('edName', sel.name || '');
      // 位置は「置いた場所」ではなく今の実際の位置を出す（シミュレート中に
      // 見て分かるほうが役に立つ。編集すればその場所が新しい置き場所になる）。
      setField('edPX', round2(sel.px !== undefined ? sel.px : sel.position.x));
      setField('edPY', round2(sel.py !== undefined ? sel.py : sel.position.y));
      setField('edPZ', round2(sel.pz !== undefined ? sel.pz : sel.position.z));
      setField('edRX', round2(sel.rotation.x));
      setField('edRY', round2(sel.rotation.y));
      setField('edRZ', round2(sel.rotation.z));
      setField('edSX', round2(sel.size.x));
      setField('edSY', round2(sel.size.y));
      setField('edSZ', round2(sel.size.z));
      setField('edMass', round2(sel.mass));
      setField('edFixed', sel.fixed);
      setField('edColor', sel.color);
      document.getElementById('edSizeLabel').textContent =
        sphere ? '直径' : 'スケール';
      document.getElementById('edSizeRow').title = sphere
        ? '直径 (m) — X の欄のみ有効' : '大きさ (m)。倍率ではなく実寸';
      document.getElementById('edSY').disabled = sphere;
      document.getElementById('edSZ').disabled = sphere;
    }

    // Inspector のイベント節（選択中の対象が関わるノードの一覧）。
    if (sel) renderNodesFor('object', sel.index, 'edObjEvents');
    if (lightSel) renderNodesFor('light', lightSel.index, 'edLightEvents');
    if (camSel) renderNodesFor('camera', camSel.index, 'edCamEvents');

    document.getElementById('edJointB').textContent =
      jointPartner < 0 ? '地面' : ('#' + jointPartner);

    // ジョイントも「選択しているオブジェクトの内容」: 節は選択中だけ出し、
    // 一覧はその選択が関わるものに絞る（どのジョイントにも地面でない体が
    // 必ずあるので、どれかを選べば必ず一覧に届く）。
    document.getElementById('secJoint').hidden = !sel;
    const joints = (sceneData.joints || []).filter(
      (j) => sel && (j.a === sel.index || j.b === sel.index));
    document.getElementById('edJointList').innerHTML = joints.length
      ? joints.map((j) => {
          const a = j.a < 0 ? '地面' : '#' + j.a;
          const b = j.b < 0 ? '地面' : '#' + j.b;
          return '<div class="jointItem">' +
            '<span class="dot" style="background:' +
              (JOINT_COLOR[j.kind] || '#888') + '"></span>' +
            '<span>' + (JOINT_LABEL[j.kind] || j.kind) + ' ' + a + ' ↔ ' + b +
            '</span>' +
            '<span class="x" title="削除" onclick="removeJoint(' + j.index +
            ')">✕</span></div>';
        }).join('')
      : '<div class="edHint">このオブジェクトのジョイントはまだありません。</div>';

    // シミュレート設定（Physics タブへ移設済み。全カメラで見える・変えられる）。
    const s = sceneData.sim;
    if (s) {
      setField('edGravity', round2(s.gravity));
      setField('edFriction', round2(s.friction));
      setField('edRestitution', round2(s.restitution));
      setField('edLinDamp', round2(s.linearDamping));
      setField('edAngDamp', round2(s.angularDamping));
      setField('edSleeping', s.sleeping);
      setField('hzCustom', s.hz);
    }

    // シーン名の欄（アセットパネルの操作列）は、空のときだけ今の
    // ファイル名を入れる。保存済みの一覧はアセットパネルのタイルが
    // 受け持つ（ダブルクリックで読込）。
    if (sceneData.sceneFile &&
        !document.getElementById('edSceneName').value) {
      setField('edSceneName', sceneData.sceneFile);
    }
  }

  // ---- 下部アセットリスト ---------------------------------------------------
  // Unity の Project ビュー相当。プリミティブ（Box / 球）はクリックでカメラ
  // 正面に配置、保存済みシーンはダブルクリックで読込（現在の配置が置き換わる
  // ので confirm を挟む）。エディタモード中の Editor Camera ページ専用。
  let assetListKey = '';
  function renderAssets() {
    const panel = document.getElementById('assets');
    const show = !!sceneData && sceneData.mode === 'editor' && isEditorCam();
    panel.hidden = !show;
    if (!show) { assetListKey = ''; return; }

    const files = sceneData.files || [];
    const meshes = sceneData.meshes || [];
    const key = files.join('|') + '@' + (sceneData.sceneFile || '') + '#' +
      meshes.map((m) => m.name).join('|');
    if (key === assetListKey) return;  // 変化が無ければ DOM を組み直さない
    assetListKey = key;

    // シーン名はサーバー側で英数字と _ - に正規化済みなので、そのまま
    // 属性へ埋め込んで安全。
    let html =
      '<div class="asItem" title="クリックでカメラ正面に配置" ' +
      'onclick="addObject(\'box\')"><span class="ico">📦</span>' +
      '<span class="name">Box</span><span class="kind">primitive</span></div>' +
      '<div class="asItem" title="クリックでカメラ正面に配置" ' +
      'onclick="addObject(\'sphere\')"><span class="ico">⚪</span>' +
      '<span class="name">Sphere</span><span class="kind">primitive</span></div>' +
      // ライト。クリックでカメラ正面（Sun は原点上空）に追加され、そのまま
      // 選択されるので、置いた直後にギズモ / Inspector で調整できる。
      '<div class="asItem" title="点光源を追加（全方向に光る）" ' +
      'onclick="addLight(\'point\')"><span class="ico">💡</span>' +
      '<span class="name">Point Light</span><span class="kind">light</span></div>' +
      '<div class="asItem" title="スポットライトを追加（円錐に光る）" ' +
      'onclick="addLight(\'spot\')"><span class="ico">🔦</span>' +
      '<span class="name">Spot Light</span><span class="kind">light</span></div>' +
      '<div class="asItem" title="平行光を追加（太陽。位置は光に影響しない）" ' +
      'onclick="addLight(\'sun\')"><span class="ico">☀&#xFE0E;</span>' +
      '<span class="name">Sun</span><span class="kind">light</span></div>';
    // シーン文書の <asset><mesh/>。クリックでそのモデルのオブジェクトを
    // カメラ正面に配置する。名前は XML 由来（手で書ける）なので HTML には
    // エスケープして出し、onclick へは配列番号だけを埋め込む（名前を JS
    // 文字列に埋め込むと引用符入りの名前で壊れる）。
    const esc = (t) => String(t).replace(/&/g, '&amp;').replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
    meshes.forEach((m, i) => {
      html += '<div class="asItem" title="クリックでカメラ正面に配置（' +
        esc(m.file) + '）" onclick="addMeshAsset(' + i + ')">' +
        '<span class="ico">🧊</span><span class="name">' + esc(m.name) +
        '</span><span class="kind">mesh</span></div>';
    });
    for (const f of files) {
      const cur = f === sceneData.sceneFile;
      html += '<div class="asItem' + (cur ? ' sel' : '') +
        '" title="ダブルクリックで読込" onclick="assetPick(\'' + f + '\')"' +
        ' ondblclick="assetLoad(\'' + f + '\')">' +
        '<span class="ico">🗂&#xFE0E;</span><span class="name">' + f +
        '</span><span class="kind">scene</span></div>';
    }
    document.getElementById('assetList').innerHTML = html;
  }
  function assetPick(name) {
    // シングルクリックは選択だけ: パネル上部のシーン名に入れておく
    // （そのまま 💾 保存すれば上書き、ダブルクリックで読込）。
    const box = document.getElementById('edSceneName');
    if (box) box.value = name;
  }
  function assetLoad(name) {
    if (!confirm('シーン「' + name +
                 '」を読み込みます。現在の配置は置き換わります。')) {
      return;
    }
    send('edit.load', { name: name });
  }

  // ---- ノードエディタ（Node-RED 風のイベント設計）--------------------------
  // グラフの実体はサーバー（/scene の graph = {nodes, wires}）。ここでやるのは
  // 描画と、編集コマンド（edit.node.* / edit.wire.*）の送信だけ。ポーリングが
  // 0.5 秒ごとに上書きしてくるので、ドラッグ中とノード内の入力にフォーカスが
  // あるあいだは DOM を組み直さない（setField の「編集中の欄は触らない」と
  // 同じ発想）。ノードの座標はキャンバス左上原点の px で、サーバーが保存する。
  const NODE_W = 168;   // ノードの幅。ポート（結線の端点）の X 計算に使う
  const PORT_Y = 16;    // 見出し行にあるポートの Y（ノード上端から）
  const NODE_DEF = {
    onCollision:    { icon: '⚡', label: '衝突したら', trigger: true,  target: 'object' },
    onStart:        { icon: '▶',  label: '開始したら', trigger: true,  target: 'none' },
    onTimer:        { icon: '⏱',  label: 'タイマー',   trigger: true,  target: 'none' },
    setColor:       { icon: '🎨', label: '色を変える', trigger: false, target: 'object' },
    impulse:        { icon: '🚀', label: '力を加える', trigger: false, target: 'object' },
    setFixed:       { icon: '📌', label: '固定する',   trigger: false, target: 'object' },
    lightColor:     { icon: '💡', label: 'ライトの色', trigger: false, target: 'light' },
    lightIntensity: { icon: '🔆', label: 'ライトの強さ', trigger: false, target: 'light' },
    cameraLookAt:   { icon: '🎥', label: '注視する',   trigger: false, target: 'camera' },
  };
  let nodeEdOpen = false;
  let ndGraphKey = '';   // 前回組み立てたグラフ+選択肢のキー（変化検知）
  let ndDrag = null;     // ノード移動中 {id, el, offX, offY, x, y}
  let ndWireDrag = null; // 結線中 {from, x1, y1, x2, y2}

  // 名前はユーザーの自由入力なので、innerHTML に混ぜる前に必ず通す。
  const ndEsc = (s) => String(s).replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  }[c]));

  function nodeById(id) {
    const g = sceneData && sceneData.graph;
    return g ? (g.nodes || []).find((n) => n.id === id) || null : null;
  }

  function openNodeEditor() { nodeEdOpen = true; renderNodeEditor(true); }
  function closeNodeEditor() { nodeEdOpen = false; renderNodeEditor(); renderEditor(); }
  function toggleNodeEditor() {
    nodeEdOpen = !nodeEdOpen;
    renderNodeEditor(true);
    renderEditor();
  }

  // 新しいノードの置き場所: 既存の数から素朴に格子へ。重ねて置くと同じ場所に
  // 積もって見つけられないため。
  function ndNewPos() {
    const g = (sceneData && sceneData.graph) || { nodes: [] };
    const i = (g.nodes || []).length;
    return { x: 40 + (i % 4) * (NODE_W + 40), y: 40 + Math.floor(i / 4) * 130 };
  }
  function paletteAdd(kind) {
    send('edit.node.add', Object.assign({ kind: kind }, ndNewPos()));
  }
  // Inspector の「＋」: 対象（target）を送らない = サーバーが「今の選択」を
  // 対象として補完する（EditorComponent の node.add）。
  function addInspectorNode(selectId) {
    const kind = document.getElementById(selectId).value;
    send('edit.node.add', Object.assign({ kind: kind }, ndNewPos()));
    openNodeEditor();  // 追加した結果（と配線のしかた）が見える場所へ
  }
  function ndPatch(id, patch) { send('edit.node.set', Object.assign({ id: id }, patch)); }
  function ndRemoveNode(id) { send('edit.node.remove', { id: id }); }
  function ndRemoveWire(from, to) { send('edit.wire.remove', { from: from, to: to }); }
  function ndVecChange(id, el) {
    const row = el.closest('.ndVec');
    const v = {};
    for (const ax of ['x', 'y', 'z']) {
      v[ax] = parseFloat(row.querySelector('[data-vec="' + ax + '"]').value) || 0;
    }
    ndPatch(id, { vec: v });
  }

  // 対象セレクトの選択肢。一覧（objects / lights / cameras）は /scene の
  // ものをそのまま使う。今の値が一覧に無い（消えた直後など）ときも、勝手に
  // 別の値へ飛ばないようダミーの選択肢として残す。
  function ndObjItems() {
    const KIND = { box: 'Box', sphere: 'Sphere', model: 'Model' };
    return (sceneData.objects || []).map((o) => ({
      v: o.index,
      t: o.name ? o.name : (KIND[o.shape] || 'Obj') + ' ' + o.index
    }));
  }
  function ndLightItems() {
    return (sceneData.lights || []).map((l) => ({
      v: l.index, t: l.name ? l.name : 'Light ' + l.index
    }));
  }
  function ndCamItems() {
    return (sceneData.cameras || []).filter((c) => c.active !== false)
      .map((c) => ({ v: c.index, t: cameraName(c.index) }));
  }
  function ndSelHtml(id, field, items, cur, head) {
    let found = false;
    let h = '<select class="strSel" onchange="ndPatch(' + id + ',{' + field +
            ':parseInt(this.value,10)})">';
    for (const hv of head || []) {
      if (hv[0] === cur) found = true;
      h += '<option value="' + hv[0] + '"' + (cur === hv[0] ? ' selected' : '') +
           '>' + hv[1] + '</option>';
    }
    for (const it of items) {
      if (it.v === cur) found = true;
      h += '<option value="' + it.v + '"' + (cur === it.v ? ' selected' : '') +
           '>' + ndEsc(it.t) + '</option>';
    }
    if (!found) h += '<option value="' + cur + '" selected>? #' + cur + '</option>';
    return h + '</select>';
  }
  const ndRow = (label, inner) =>
    '<div class="ndRow"><span>' + label + '</span>' + inner + '</div>';

  function ndNodeHtml(n, def) {
    const fired = n.fired
      ? '<span class="ndFire" title="発火回数">⚡' + n.fired + '</span>' : '';
    let h = '<div class="ndNode ' + (def.trigger ? 'trig' : 'act') +
            '" data-id="' + n.id + '" style="left:' + n.x + 'px;top:' + n.y + 'px">';
    h += '<div class="ndTitleRow" data-drag="' + n.id + '" title="ドラッグで移動">' +
         '<span>' + def.icon + '</span><span class="ndName">' + def.label + '</span>' +
         fired +
         '<span class="ndDel" title="ノードを削除" onclick="ndRemoveNode(' +
         n.id + ')">✕</span></div>';
    h += '<div class="ndBody">';
    if (def.target === 'object') {
      h += ndRow('対象', ndSelHtml(n.id, 'target', ndObjItems(), n.target,
                                   def.trigger ? [[-1, '(どれでも)']]
                                               : [[-1, '(選んでください)']]));
    } else if (def.target === 'light') {
      h += ndRow('対象', ndSelHtml(n.id, 'target', ndLightItems(), n.target,
                                   [[-1, '(選んでください)']]));
    } else if (def.target === 'camera') {
      h += ndRow('対象', ndSelHtml(n.id, 'target', ndCamItems(), n.target,
                                   [[-1, '(選んでください)']]));
    }
    if (n.kind === 'onCollision') {
      h += ndRow('相手', ndSelHtml(n.id, 'other', ndObjItems(), n.other,
                                   [[-2, '(何でも)'], [-1, '地面']]));
    }
    if (n.kind === 'cameraLookAt') {
      h += ndRow('注視', ndSelHtml(n.id, 'other', ndObjItems(), n.other,
                                   [[-1, '(選んでください)']]));
    }
    if (n.kind === 'onTimer') {
      h += ndRow('間隔 s', '<input class="num" type="number" step="0.1" min="0.05" value="' +
                 n.seconds + '" onchange="ndPatch(' + n.id +
                 ',{seconds:parseFloat(this.value)||1})">');
    }
    if (n.kind === 'setColor' || n.kind === 'lightColor') {
      h += ndRow('色', '<input class="col" type="color" value="' + n.color +
                 '" onchange="ndPatch(' + n.id + ',{color:this.value})">');
    }
    if (n.kind === 'impulse') {
      const vecIn = (ax, val) =>
        '<input class="num" type="number" step="0.5" value="' + val +
        '" title="' + ax.toUpperCase() + ' (m/s)" data-vec="' + ax +
        '" onchange="ndVecChange(' + n.id + ',this)">';
      h += ndRow('Δv', '<span class="ndVec">' + vecIn('x', n.vec.x) +
                 vecIn('y', n.vec.y) + vecIn('z', n.vec.z) + '</span>');
    }
    if (n.kind === 'setFixed') {
      h += ndRow('動作', '<select class="strSel" onchange="ndPatch(' + n.id +
                 ',{value:parseFloat(this.value)})">' +
                 '<option value="1"' + (n.value ? ' selected' : '') + '>固定する</option>' +
                 '<option value="0"' + (!n.value ? ' selected' : '') + '>固定を解除</option>' +
                 '</select>');
    }
    if (n.kind === 'lightIntensity') {
      h += ndRow('強さ', '<input class="num" type="number" step="10000" min="0" value="' +
                 n.value + '" onchange="ndPatch(' + n.id +
                 ',{value:parseFloat(this.value)||0})">');
    }
    h += '</div>';
    // ポート。トリガーは右に出力、アクションは左に入力（Node-RED の向き）。
    h += def.trigger
      ? '<span class="ndPort out" data-out="' + n.id +
        '" title="ここからアクションへドラッグで接続"></span>'
      : '<span class="ndPort in" data-in="' + n.id + '"></span>';
    return h + '</div>';
  }

  function ndWirePath(p1, p2) {
    const dx = Math.max(30, Math.abs(p2.x - p1.x) * 0.5);
    return 'M' + p1.x + ',' + p1.y + ' C' + (p1.x + dx) + ',' + p1.y + ' ' +
           (p2.x - dx) + ',' + p2.y + ' ' + p2.x + ',' + p2.y;
  }

  // 線を引き直す。posOverride があれば（= ドラッグ中）、サーバー座標では
  // なく DOM の現在位置で描く - 動かしている最中も線が付いてくるように。
  function ndDrawWires(posOverride) {
    const g = (sceneData && sceneData.graph) || { nodes: [], wires: [] };
    const pt = (id, out) => {
      const p = posOverride && posOverride[id];
      const n = nodeById(id);
      if (!p && !n) return null;
      const x = p ? p.x : n.x, y = p ? p.y : n.y;
      return { x: x + (out ? NODE_W : 0), y: y + PORT_Y };
    };
    let h = '';
    for (const w of g.wires || []) {
      const p1 = pt(w.from, true), p2 = pt(w.to, false);
      if (!p1 || !p2) continue;
      const d = ndWirePath(p1, p2);
      // 見える線 + 太い透明の当たり判定（細線はクリックできない）。
      h += '<path class="wire" d="' + d + '"/>' +
           '<path class="wireHit" d="' + d + '" onclick="ndRemoveWire(' +
           w.from + ',' + w.to + ')"><title>クリックで切断</title></path>';
    }
    if (ndWireDrag) {
      h += '<path class="wire drag" d="' +
           ndWirePath({ x: ndWireDrag.x1, y: ndWireDrag.y1 },
                      { x: ndWireDrag.x2, y: ndWireDrag.y2 }) + '"/>';
    }
    document.getElementById('ndWires').innerHTML = h;
  }
  function ndRedrawLocal() {
    const pos = {};
    for (const el of document.querySelectorAll('#ndNodes .ndNode')) {
      pos[parseInt(el.dataset.id, 10)] =
        { x: parseFloat(el.style.left), y: parseFloat(el.style.top) };
    }
    ndDrawWires(pos);
  }

  function renderNodeEditor(force) {
    const panel = document.getElementById('nodeEd');
    // Editor Camera のページ専用。モードは問わない: シミュレートを回しながら
    // 発火バッジ（⚡n）で動きを確かめる使い方があるため。編集コマンド自体は
    // サーバー側でもエディタカメラ限定で弾かれる。
    const show = nodeEdOpen && !!sceneData && isEditorCam() && owner;
    panel.hidden = !show;
    if (!show) { ndGraphKey = ''; return; }

    const g = sceneData.graph || { nodes: [], wires: [] };
    // グラフか選択肢（名前・数）が変わったときだけ組み直す。操作中
    // （ドラッグ / ノード内の入力にフォーカス）は据え置き - キーを更新しない
    // ので、操作が終わった次のポーリングで最新版に揃う。
    const optKey =
      (sceneData.objects || []).map((o) => o.index + ':' + (o.name || '')).join(',') +
      '|' + (sceneData.lights || []).map((l) => l.index + ':' + (l.name || '')).join(',') +
      '|' + (sceneData.cameras || []).filter((c) => c.active !== false)
              .map((c) => c.index).join(',');
    const key = JSON.stringify(g) + '@' + optKey;
    if (!force && key === ndGraphKey) return;
    const canvas = document.getElementById('ndCanvas');
    if (ndDrag || ndWireDrag ||
        (canvas.contains(document.activeElement) &&
         document.activeElement !== document.body)) {
      return;
    }
    ndGraphKey = key;

    let h = '';
    for (const n of g.nodes || []) {
      const def = NODE_DEF[n.kind];
      if (!def) continue;
      h += ndNodeHtml(n, def);
    }
    document.getElementById('ndNodes').innerHTML = h ||
      '<div class="ndEmpty">ノードがありません。上のパレット、または ' +
      'Inspector の「イベント ＋」から追加してください。</div>';
    ndDrawWires();
  }

  // ノードの移動と結線。pointer events を #ndNodes に委譲で付ける。
  {
    const canvas = document.getElementById('ndCanvas');
    const nodesEl = document.getElementById('ndNodes');
    const canvasPos = (e) => {
      const r = canvas.getBoundingClientRect();
      return { x: e.clientX - r.left + canvas.scrollLeft,
               y: e.clientY - r.top + canvas.scrollTop };
    };
    nodesEl.addEventListener('pointerdown', (e) => {
      if (!owner) return;
      const out = e.target.closest('[data-out]');
      if (out) {
        const id = parseInt(out.dataset.out, 10);
        const n = nodeById(id);
        if (!n) return;
        ndWireDrag = { from: id, x1: n.x + NODE_W, y1: n.y + PORT_Y,
                       x2: n.x + NODE_W, y2: n.y + PORT_Y };
        nodesEl.setPointerCapture(e.pointerId);
        e.preventDefault();
        ndRedrawLocal();
        return;
      }
      const head = e.target.closest('[data-drag]');
      if (head) {
        const el = head.closest('.ndNode');
        const p = canvasPos(e);
        ndDrag = { id: parseInt(head.dataset.drag, 10), el: el,
                   offX: p.x - parseFloat(el.style.left),
                   offY: p.y - parseFloat(el.style.top) };
        nodesEl.setPointerCapture(e.pointerId);
        e.preventDefault();
      }
    });
    nodesEl.addEventListener('pointermove', (e) => {
      if (ndDrag) {
        const p = canvasPos(e);
        ndDrag.x = Math.max(0, Math.round(p.x - ndDrag.offX));
        ndDrag.y = Math.max(0, Math.round(p.y - ndDrag.offY));
        ndDrag.el.style.left = ndDrag.x + 'px';
        ndDrag.el.style.top = ndDrag.y + 'px';
        ndRedrawLocal();
        return;
      }
      if (ndWireDrag) {
        const p = canvasPos(e);
        ndWireDrag.x2 = p.x;
        ndWireDrag.y2 = p.y;
        ndRedrawLocal();
      }
    });
    const finish = (e) => {
      if (ndDrag) {
        // 動かした結果だけ送る（ドラッグ中はローカルの見た目だけ）。
        if (ndDrag.x !== undefined) {
          send('edit.node.set', { id: ndDrag.id, x: ndDrag.x, y: ndDrag.y });
        }
        ndDrag = null;
        ndGraphKey = '';  // 次のポーリングでサーバーの答え合わせ
        return;
      }
      if (ndWireDrag) {
        // どのアクションノードの上で離してもよい（in ポートちょうどでなくて
        // 可）。向きの検証はサーバー側にもある。
        const el = document.elementFromPoint(e.clientX, e.clientY);
        const nodeEl = el && el.closest ? el.closest('.ndNode') : null;
        if (nodeEl) {
          const id = parseInt(nodeEl.dataset.id, 10);
          const n = nodeById(id);
          if (n && NODE_DEF[n.kind] && !NODE_DEF[n.kind].trigger &&
              id !== ndWireDrag.from) {
            send('edit.wire.add', { from: ndWireDrag.from, to: id });
          }
        }
        ndWireDrag = null;
        ndGraphKey = '';
        ndRedrawLocal();
      }
    };
    nodesEl.addEventListener('pointerup', finish);
    nodesEl.addEventListener('pointercancel', () => {
      ndDrag = null;
      ndWireDrag = null;
      ndGraphKey = '';
    });
  }

  // Inspector の「イベント」節: 選択中の対象（オブジェクト / ライト /
  // カメラ）が関わるノードだけの一覧。対象を持たないトリガー（開始・
  // タイマー）はノードエディタで全体を見る。
  function renderNodesFor(targetKind, index, listId) {
    const el = document.getElementById(listId);
    if (!el) return;
    const g = (sceneData && sceneData.graph) || { nodes: [], wires: [] };
    const rows = (g.nodes || []).filter((n) => {
      const def = NODE_DEF[n.kind];
      return def && def.target === targetKind && n.target === index;
    });
    el.innerHTML = rows.length
      ? rows.map((n) => {
          const def = NODE_DEF[n.kind];
          const wires = (g.wires || [])
            .filter((w) => w.from === n.id || w.to === n.id).length;
          return '<div class="ndItem">' +
            '<span>' + def.icon + ' ' + def.label + '</span>' +
            (n.fired ? '<span class="ndFire" title="発火回数">⚡' + n.fired +
                       '</span>' : '') +
            '<span class="sub">' + wires + ' 接続</span>' +
            '<span class="x" title="ノードを削除" onclick="ndRemoveNode(' +
            n.id + ')">✕</span></div>';
        }).join('')
      : '<div class="edHint">この対象のノードはまだありません。</div>';
  }

  async function pollScene() {
    try {
      const r = await fetch('scene');
      sceneData = await r.json();
      noteSceneChanges(sceneData);
      if (xmlEdOpen) {
        document.getElementById('xmStatus').textContent =
          sceneData.status || '';
      }
      renderCameras();
      renderHierarchy();
      renderInspector();
      renderEditor();
      renderAssets();
      renderNodeEditor();
    } catch (e) { /* server busy or gone; try again next tick */ }
  }
  setInterval(pollScene, 500);
  pollScene();
