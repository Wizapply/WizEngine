// Charon web UI: テレインの筆（映像左上の #terrainBar と、映像の上のポインタ
// 操作）。terrain.js の後ろに読む古典スクリプトで、app.js（send / toNdc /
// hitEl / hoverNdc / HOVER_MS / NDC_OFFSCREEN / owner / sceneData /
// isEditorCam / myEditorSel / focusInField / storeJson / readFields /
// restoreFields）と terrain.js（currentTerrain / undoTerrain）のトップ
// レベルをそのまま使う。ここの関数もグローバルのままにする（index.html の
// onclick= と app.js の renderEditor などが名前で呼ぶため）。
//
// 送るのは edit.terrain.brush {phase, x, y, tool, radius, strength}（x, y は
// 映像の NDC）。phase は hover（輪だけ）/ begin / move / end。値の既定値・
// 丸めと「エディタモード × エディタカメラ」の守りはサーバー（TerrainComponent
// と terrain::clampBrush）が持つ。入力欄の min / max は表示用。

  // 押している間の move の間隔。止まっていても送る: サーバーは入力が 1 秒
  // 来ないとストロークを閉じ、盛り上げなどは押し続けた時間で効くため。
  // 押していない間の hover（輪）は app.js の HOVER_MS ごと（輪も入力が
  // 1 秒来ないと消えるので、止まっていても送る）。
  const BRUSH_MOVE_MS = 33;
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
  let brushBarShown = false;     // #terrainBar が出ている（筆が使える）
  let brushPointerId = null;     // 押しているポインタ（押していなければ null）

  // 筆がビューのクリックを受け持つか（道具を選んでいて、バーが出ている）。
  // app.js のギズモの hover もこれを見て止まる。
  function brushToolActive() {
    return owner && brushBarShown && brushTool !== 'none';
  }
  function sendBrush(phase, ndc, keepalive) {
    const p = ndc || NDC_OFFSCREEN;  // 画面の外 = 輪を消す合図
    send('edit.terrain.brush',
         Object.assign({ phase: phase, x: p.x, y: p.y, tool: brushTool }, brushValues),
         keepalive);
  }

  // ---- ツールバー ------------------------------------------------------------
  function brushToolButtons() {
    return document.querySelectorAll('#terrainBar button[data-tool]');
  }
  // 出すのはエディタモード × エディタカメラ × テレインを選択中 × ファイルが
  // 読めている（Missing でない）ときだけ。消えるときは道具も下ろす（隠れた
  // 道具が映像のクリックを奪わないように）。renderEditor が毎回呼ぶ。
  function renderTerrainBar() {
    const bar = document.getElementById('terrainBar');
    if (!bar) return;
    const cur = currentTerrain();
    const show = !!sceneData && sceneData.mode === 'editor' && isEditorCam() &&
        myEditorSel().kind === 'terrain' && !!cur.name && !cur.missing;
    brushBarShown = show;
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
    } else if (brushTool === 'none') {
      // 筆の間はギズモの hover を送らないので、光っていたハンドルを消しておく。
      send('hover', NDC_OFFSCREEN);
    }
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
  // #hit の capture 段階で受け、筆が受け持つ押下は app.js の pick / drag /
  // カメラ操作へ流さない（同じ要素の後の聞き手も止める）。Ctrl 付きは
  // いつもどおりカメラ操作、左ボタン以外も素通し。位置は app.js の hoverNdc
  // （1 か所）で、押している間はここでも書く（タッチでは app.js が書かない）。
  // ndc = 離した点（pointerup だけが渡す）。サーバーは前の点からそこまでを
  // 塗ってから閉じる。無ければ画面の外 = 閉じるだけ（blur・取り消しなど）。
  function endBrushStroke(keepalive, ndc) {
    if (brushPointerId === null) return;
    const id = brushPointerId;
    brushPointerId = null;
    try { hitEl.releasePointerCapture?.(id); } catch (e) { /* もう外れている */ }
    sendBrush('end', ndc || null, keepalive);
  }
  hitEl.addEventListener('pointerdown', (e) => {
    if (brushPointerId !== null) {
      // 押している間の別の指・ボタンは捨てる（ストロークを乱さない）。
      e.stopImmediatePropagation();
      e.preventDefault();
      return;
    }
    if (!brushToolActive() || e.button !== 0 || e.ctrlKey) return;
    e.stopImmediatePropagation();
    e.preventDefault();
    // 半径・強さを打ったまま（Enter 無し）で塗り始めたら、欄を外して値を
    // 確定させる（onchange → applyBrushValues が先に走る。Esc も効くように）。
    const bar = document.getElementById('terrainBar');
    if (focusInField() && bar.contains(document.activeElement)) {
      document.activeElement.blur();
    }
    hitEl.setPointerCapture?.(e.pointerId);
    brushPointerId = e.pointerId;
    hoverNdc = toNdc(e.clientX, e.clientY);
    sendBrush('begin', hoverNdc);
  }, true);
  hitEl.addEventListener('pointermove', (e) => {
    if (e.pointerId === brushPointerId) hoverNdc = toNdc(e.clientX, e.clientY);
  }, true);
  // 離した: 離した点を end に添えて 1 回で送る（move と分けると end が先に
  // 届きうる）。
  hitEl.addEventListener('pointerup', (e) => {
    if (brushPointerId === null || e.pointerId !== brushPointerId) return;
    hoverNdc = toNdc(e.clientX, e.clientY);
    e.stopImmediatePropagation();
    endBrushStroke(false, hoverNdc);
  }, true);
  // 取り消された: 座標が 0 になるブラウザがあるので位置は使わず閉じるだけ。
  hitEl.addEventListener('pointercancel', (e) => {
    if (brushPointerId === null || e.pointerId !== brushPointerId) return;
    e.stopImmediatePropagation();
    endBrushStroke();
  }, true);
  // 映像から出たら輪を消す（押している間はポインタを捕まえているので来ない）。
  // ツールバーの上へ出たときも位置を消す（端に輪を残さない）。
  hitEl.addEventListener('pointerleave', () => {
    if (!brushToolActive() || brushPointerId !== null) return;
    hoverNdc = null;
    sendBrush('hover', null);
  });
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

  // 押している間は move を BRUSH_MOVE_MS ごと、押していない間は映像の上に
  // いれば hover を HOVER_MS ごとに送る（どちらも止まっていても送る）。
  setInterval(() => {
    if (!brushToolActive()) { endBrushStroke(); return; }
    if (brushPointerId !== null) sendBrush('move', hoverNdc);
  }, BRUSH_MOVE_MS);
  setInterval(() => {
    if (brushToolActive() && brushPointerId === null && hoverNdc) {
      sendBrush('hover', hoverNdc);
    }
  }, HOVER_MS);
