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
  // show greyed-out controls.
  function setControlsEnabled(on) {
    document.querySelectorAll('#sidebar button.wide')
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
  function send(cmd, args) {
    if (!owner) return;
    const msg = Object.assign({ cmd: cmd }, args || {});
    fetch('/input', {
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

    if (grabbing) { grabNdc = toNdc(e.clientX, e.clientY); return; }
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

  // Two shortcuts only. Everything else (reset, pause, camera) is a control in
  // the sidebar or a mouse gesture, so no bare letter or arrow key can fire an
  // action by accident while working in the view.
  document.addEventListener('keydown', (e) => {
    if (e.repeat) return;
    if (e.key === 'Tab' && !e.ctrlKey && !e.altKey && !e.shiftKey) {
      e.preventDefault();
      toggleSidebar();
    } else if ((e.key === 'f' || e.key === 'F') && e.altKey) {
      e.preventDefault();
      toggleFullscreen();
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
    const overStage = document.fullscreenElement ||
        stageEl.contains(document.elementFromPoint(e.clientX, e.clientY));
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
      fetch('/viewer/ping', { method: 'POST', body: token })
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
        const want = 'video/' + (window.WIZ_CODEC || 'VP8');
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
      const resp = await fetch('/whep', {
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
    fetch('/stats').then((r) => r.json()).then((s) => {
      if (s.cameraCount !== undefined && !camLinksDone) {
        document.getElementById('camLabel').textContent =
          'camera ' + s.camera + ' of ' + s.cameraCount;
        camLinksDone = true;
      }
      curIter = s.iterations;
      if (s.paused !== undefined) setPlayLabel(s.paused);
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
      const slowPhysics = s.realtime < 0.95;
      const slowRender = s.renderFps < s.targetFps * 0.9;
      document.getElementById('perf').innerHTML =
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
    if (owner) navigator.sendBeacon('/viewer/leave', token);
  });

  // Ask the server which codec it encodes, then negotiate for exactly that.
  fetch('/stats')
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
    const label = (i) => 'camera ' + i + (i === s.camera ? ' (this)' : '');

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

  // Tabs are just show/hide - both panes stay in the DOM so their controls
  // keep updating from the stats poll even while hidden.
  function showTab(name) {
    document.getElementById('paneScene').hidden = (name !== 'scene');
    document.getElementById('panePhysics').hidden = (name !== 'physics');
    document.getElementById('tabScene').classList.toggle('active', name === 'scene');
    document.getElementById('tabPhysics').classList.toggle('active', name === 'physics');
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
    // The server sends the real ports: busy ones are skipped at startup, so
    // they are not necessarily consecutive.
    const base = location.port ? parseInt(location.port, 10) : 80;
    const portOf = (i) => (sceneData.ports && sceneData.ports[i] !== undefined)
      ? sceneData.ports[i]
      : (base - sceneData.camera + i);   // older server: assume consecutive
    el.innerHTML = '';
    for (const c of sceneData.cameras) {
      const isMe = c.index === sceneData.camera;
      // Only one viewer per camera, so "in use" tells you whether switching
      // there would be refused.
      const busy = sceneData.busy && sceneData.busy[c.index];
      const state = isMe ? 'viewing' : (busy ? 'in use' : 'free');
      const row = document.createElement('div');
      row.className = 'item' + (isMe ? ' sel' : '');
      row.innerHTML =
        '<span class="dot" style="background:' + c.color + '"></span>' +
        '<span>Camera ' + c.index + (isMe ? ' (this)' : '') + '</span>' +
        '<span class="sub' + (busy && !isMe ? ' busy' : '') + '">' +
        state + '</span>';
      if (!isMe) {
        row.title = 'Switch to this camera';
        // Same tab: leaving this page also releases the viewer session, so the
        // camera we came from frees up immediately.
        row.onclick = () => {
          location.href =
            'http://' + location.hostname + ':' + portOf(c.index) + '/';
        };
      }
      el.appendChild(row);
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
    const kind = sceneData.model ? 'Model' : 
                 (sceneData.shape === 'sphere' ? 'Sphere' : 'Box');
    let html = '';
    // Lights first: part of the scene like everything else, but not
    // selectable - selecting drives the grab controller, which only knows
    // physics bodies. The dot shows the light's colour; the sub line its
    // type and intensity (lux for directional, lumens otherwise).
    if (sceneData.lights && !objFilter) {
      const typeName = { directional: 'Directional', point: 'Point', spot: 'Spot' };
      const fmtI = (v) => v >= 1000 ? Math.round(v / 1000) + 'k' : Math.round(v);
      for (const l of sceneData.lights) {
        const unit = l.type === 'directional' ? 'lx' : 'lm';
        html += '<div class="item" style="cursor:default">' +
                '<span class="dot" style="background:' + l.color + '"></span>' +
                '<span>Light ' + l.index + (l.shadows ? ' ☀' : '') + '</span>' +
                '<span class="sub">' + (typeName[l.type] || l.type) + ' · ' +
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
              '<span>' + kind + ' ' + o.index + '</span>' +
              '<span class="sub">' + o.y.toFixed(2) + 'm</span></div>';
    }
    if (rows.length > shown.length) {
      html += '<div class="item" style="cursor:default;opacity:.5">… ' +
              (rows.length - shown.length) + ' more (use the filter)</div>';
    }
    document.getElementById('objList').innerHTML = html;
  }

  function renderInspector() {
    const el = document.getElementById('inspector');
    const mine = myCameraSelection();
    if (!sceneData || mine < 0) {
      el.textContent = 'nothing selected';
      return;
    }
    const o = sceneData.objects[mine];
    if (!o) { el.textContent = 'nothing selected'; return; }
    const kind = sceneData.model
      ? ('Model (' + sceneData.model + ')')
      : (sceneData.shape === 'sphere' ? 'Sphere' : 'Box');
    el.innerHTML =
      '<div><span class="k">object</span> #' + o.index + '</div>' +
      '<div><span class="k">drawn as</span> ' + kind + '</div>' +
      '<div><span class="k">position</span> ' +
      o.x.toFixed(2) + ', ' + o.y.toFixed(2) + ', ' + o.z.toFixed(2) + '</div>' +
      '<div><span class="k">collision</span> ' +
      (sceneData.shape === 'sphere' ? 'sphere' : 'box') + '</div>';
  }

  async function pollScene() {
    try {
      const r = await fetch('/scene');
      sceneData = await r.json();
      noteSceneChanges(sceneData);
      renderCameras();
      renderHierarchy();
      renderInspector();
    } catch (e) { /* server busy or gone; try again next tick */ }
  }
  setInterval(pollScene, 500);
  pollScene();
