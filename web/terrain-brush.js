// Charon web UI: テレインの筆（映像左上の #terrainBar と、映像の上のポインタ
// 操作）。terrain.js の後ろに読む古典スクリプトで、app.js（send / toNdc /
// hitEl / hoverNdc / clearHover / touches / NDC_OFFSCREEN / owner / sceneData /
// isEditorCam / myEditorSel / focusInField / storeJson / readFields /
// restoreFields）と terrain.js（currentTerrain / undoTerrain）のトップ
// レベルをそのまま使う。ここの関数もグローバルのままにする（index.html の
// onclick= と app.js の renderEditor などが名前で呼ぶため）。
//
// 送るのは edit.terrain.brush {phase, x, y, tool, radius, strength}（x, y は
// 映像の NDC）。phase は hover（輪だけ）/ begin / move / end。値の既定値・
// 丸めと「エディタモード × エディタカメラ」の守りはサーバー（TerrainComponent
// と terrain::clampBrush）が持つ。入力欄の min / max は表示用。
//
// app.js との分担: 押していない間の hover は app.js の HOVER_MS のタイマーが
// sendBrushHover を呼び、映像から出たときの輪の消去も app.js の pointerleave。
// タッチの指の一覧は app.js の touches で、2 本目の指が触れたら app.js が
// endBrushStroke を呼んでカメラ操作（パン・ピンチ）に渡す。

  // 押している間の move の間隔。止まっていても送る: サーバーは入力が 1 秒
  // 来ないとストロークを閉じ、盛り上げなどは押し続けた時間で効くため。
  const BRUSH_MOVE_MS = 33;
  // 筆の 1 通の送信の期限。返らない送信で順番待ちが止まらないよう、過ぎたら
  // 失敗として次へ進む。
  const BRUSH_SEND_TIMEOUT_MS = 2000;
  // 半径と強さはこのブラウザの localStorage に覚える（道具は覚えない =
  // 開き直したら選んでいない状態から始まる）。
  const BRUSH_STORE = 'terrain:brush';
  // 半径・強さの欄（送るキー → 欄の id）。欄が空ならそのキーは送らない =
  // サーバーの既定値。
  const BRUSH_FIELDS = { radius: 'tbRadius', strength: 'tbStrength' };
  // 確定した（onchange の）半径・強さ。送るのはこちら（打っている途中の
  // 欄の値を 33 ms ごとに送らない）。
  let brushValues = {};

  // 道具は 'none'（筆を使っていない = 映像のクリックはいつもどおり選択・
  // ギズモ）か、#terrainBar の button[data-tool] のどれか（ボタンの
  // data-tool がサーバーの terrain::brushToolFromName と同じ綴りの唯一の一覧）。
  let brushTool = 'none';
  let brushPointerId = null;     // 押しているポインタ（押していなければ null）
  let brushMoveTimer = null;     // 押している間だけ回す move のタイマー

  // 筆が使える状態か: エディタモード × エディタカメラ × テレインを選択中 ×
  // ファイルが読めている（Missing でない）。バーの表示と筆の有効判定の
  // 唯一の定義。
  function brushAvailable() {
    const cur = currentTerrain();
    return !!sceneData && sceneData.mode === 'editor' && isEditorCam() &&
        myEditorSel().kind === 'terrain' && !!cur.name && !cur.missing;
  }
  // 筆がビューのクリックを受け持つか（道具を選んでいて、筆が使える）。
  // app.js のギズモの hover・W / E / R / X・タッチもこれを見る。
  function brushToolActive() {
    return owner && brushTool !== 'none' && brushAvailable();
  }

  // ---- 送信 ------------------------------------------------------------------
  // 筆の送信は 1 本ずつ順に送る（前の送信が終わってから次）。並べて送ると
  // 届く順が入れ替わりうる（すばやいクリックで end が begin より先に着くと、
  // ストロークがサーバーのタイムアウトまで開いたままになる）。begin / end は
  // 必ず積み、move / hover は待ちの末尾が同じ種類なら差し替える（最新の
  // 位置だけ届けばよい。遅い回線で待ちが伸び続けない）。
  const brushQueue = [];
  let brushSending = false;
  function pumpBrushQueue() {
    if (brushSending || brushQueue.length === 0) return;
    brushSending = true;
    send('edit.terrain.brush', brushQueue.shift(), false,
         BRUSH_SEND_TIMEOUT_MS).then(() => {
      brushSending = false;
      pumpBrushQueue();
    });
  }
  // keepalive = ページを離れる途中（pagehide）。待ちを経ずにすぐ送る
  // （順番を待つ間にページが消える）。
  function sendBrush(phase, ndc, keepalive) {
    const p = ndc || NDC_OFFSCREEN;  // 画面の外 = 輪を消す合図
    const args = Object.assign({ phase: phase, x: p.x, y: p.y, tool: brushTool },
                               brushValues);
    if (keepalive) { send('edit.terrain.brush', args, true); return; }
    const last = brushQueue[brushQueue.length - 1];
    if (last && last.phase === phase && (phase === 'move' || phase === 'hover')) {
      brushQueue[brushQueue.length - 1] = args;
    } else {
      brushQueue.push(args);
    }
    pumpBrushQueue();
  }
  // 押していない間の輪（hoverNdc が null なら輪を消す）。app.js の HOVER_MS の
  // タイマーと映像から出たときが呼ぶ。
  function sendBrushHover() {
    if (brushPointerId === null) sendBrush('hover', hoverNdc);
  }

  // ---- ツールバー ------------------------------------------------------------
  function brushToolButtons() {
    return document.querySelectorAll('#terrainBar button[data-tool]');
  }
  // バーの中（ボタン・欄）のフォーカスを外す。
  function blurTerrainBar() {
    const bar = document.getElementById('terrainBar');
    if (bar && bar.contains(document.activeElement)) document.activeElement.blur();
  }
  // 筆が使えるときだけ出す。消えるときは道具も下ろす（隠れた道具が映像の
  // クリックを奪わないように）。renderEditor が毎回呼ぶ。
  function renderTerrainBar() {
    const bar = document.getElementById('terrainBar');
    if (!bar) return;
    const show = brushAvailable();
    bar.hidden = !show;
    if (!show) {
      if (brushTool !== 'none') setBrushTool('none');
      return;
    }
    bar.querySelectorAll('button, input').forEach((el) => { el.disabled = !owner; });
    brushToolButtons().forEach((b) => {
      b.classList.toggle('on', b.dataset.tool === brushTool);
    });
  }
  // 道具を選ぶ。同じボタンをもう一度押す（と Esc）で 'none' = 筆をやめる。
  function setBrushTool(tool) {
    const known = Array.from(brushToolButtons()).some((b) => b.dataset.tool === tool);
    const next = (known && tool !== brushTool) ? tool : 'none';
    if (next === 'none') {
      endBrushStroke();
      // 輪をすぐ消す（待てばサーバーが 1 秒で消すが、それまで残って見える）。
      if (brushTool !== 'none') sendBrush('hover', null);
    } else {
      // 位置はボタンを押したときのもの（バーの角）なので捨てる。映像の上へ
      // 戻った最初の pointermove から輪が出る。道具を書き換える前に呼ぶ:
      // 筆が無かったなら光っていたギズモのハンドルを、別の道具からなら輪を消す。
      clearHover();
    }
    // 押したボタンにフォーカスを残さない（Space / Enter でもう一度押されて
    // 筆が外れる）。
    blurTerrainBar();
    brushTool = next;
    renderTerrainBar();
  }
  // 半径・強さの欄の確定（onchange）。範囲の丸めはサーバー。覚えるのは
  // 数の入っている欄だけ。
  function applyBrushValues() {
    brushValues = readFields(BRUSH_FIELDS);
    storeJson(BRUSH_STORE, brushValues);
  }
  // 覚えた値があるときだけ欄に入れる（無ければ空 = サーバーの既定値）。
  restoreFields(BRUSH_STORE, BRUSH_FIELDS);
  brushValues = readFields(BRUSH_FIELDS);

  // ---- ポインタ --------------------------------------------------------------
  // #hit の capture 段階で受ける。マウス・ペンでは筆が受け持つ押下を app.js の
  // pick / drag / カメラ操作へ流さない（同じ要素の後の聞き手も止める）。
  // タッチは app.js の指の一覧に入れるため流す（app.js は筆の間の 1 本目では
  // 掴まない）。Ctrl 付きはいつもどおりカメラ操作、左ボタン以外も素通し。
  // 位置は app.js の hoverNdc（1 か所）で、押している間はここでも書く
  // （タッチでは app.js が書かない）。
  // ndc = 離した点（pointerup だけが渡す）。サーバーは前の点からそこまでを
  // 塗ってから閉じる。無ければ画面の外 = 閉じるだけ（blur・取り消しなど）。
  function endBrushStroke(keepalive, ndc) {
    if (brushPointerId === null) return;
    brushPointerId = null;
    clearInterval(brushMoveTimer);
    brushMoveTimer = null;
    sendBrush('end', ndc || null, keepalive);
  }
  hitEl.addEventListener('pointerdown', (e) => {
    if (e.pointerType === 'touch') {
      // 2 本目からの指は app.js がカメラ操作にする（筆もそこで終わる）。
      if (touches.size > 0) return;
    } else if (brushPointerId !== null) {
      // 押している間の別のボタン・ペンは捨てる（ストロークを乱さない）。
      e.stopImmediatePropagation();
      e.preventDefault();
      return;
    }
    if (!brushToolActive() || e.button !== 0 || e.ctrlKey) return;
    if (e.pointerType !== 'touch') e.stopImmediatePropagation();
    e.preventDefault();
    // 半径・強さを打ったまま（Enter 無し）で塗り始めたら、欄を外して値を
    // 確定させる（onchange → applyBrushValues が先に走る。Esc も効くように）。
    blurTerrainBar();
    hitEl.setPointerCapture?.(e.pointerId);
    brushPointerId = e.pointerId;
    hoverNdc = toNdc(e.clientX, e.clientY);
    sendBrush('begin', hoverNdc);
    brushMoveTimer = setInterval(() => {
      if (brushToolActive()) sendBrush('move', hoverNdc);
      else endBrushStroke();
    }, BRUSH_MOVE_MS);
  }, true);
  hitEl.addEventListener('pointermove', (e) => {
    if (e.pointerId === brushPointerId) hoverNdc = toNdc(e.clientX, e.clientY);
  }, true);
  // 離した: 離した点を end に添えて 1 回で送る（move と分けると end が先に
  // 届きうる）。app.js の後始末（指の一覧）にもそのまま流す。
  hitEl.addEventListener('pointerup', (e) => {
    if (brushPointerId === null || e.pointerId !== brushPointerId) return;
    hoverNdc = toNdc(e.clientX, e.clientY);
    endBrushStroke(false, hoverNdc);
  }, true);
  // 取り消された: 座標が 0 になるブラウザがあるので位置は使わず閉じるだけ。
  hitEl.addEventListener('pointercancel', (e) => {
    if (brushPointerId === null || e.pointerId !== brushPointerId) return;
    endBrushStroke();
  }, true);
  // ウィンドウを離れた・ページを離れるときは押したままにしない。
  // タブが裏に回ったときも（裏から move を送り続けない）。
  window.addEventListener('blur', () => endBrushStroke());
  window.addEventListener('pagehide', () => endBrushStroke(true));
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) endBrushStroke();
  });
  // Esc で筆をやめる。他の物（モーダル・右クリックメニュー）を閉じた Esc
  // （preventDefault 済み）と、欄に打っている間の Esc では下ろさない。
  document.addEventListener('keydown', (e) => {
    if (e.key !== 'Escape' || brushTool === 'none') return;
    if (e.defaultPrevented || focusInField()) return;
    e.preventDefault();
    setBrushTool('none');
  });
