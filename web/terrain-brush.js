// Charon web UI: テレインの筆（映像左上の #terrainBar と、映像の上のポインタ
// 操作）。terrain-files.js の後ろ（最後）に読む古典スクリプトで、app.js
// （send / toNdc / hitEl / hoverNdc / clearHover / touches / NDC_OFFSCREEN /
// owner / sceneData / isEditorCam / myEditorSel / focusInField / storeJson /
// loadStoredJson / readFields / restoreFields）と terrain.js（currentTerrain /
// terrainLayerLabel / terrainTools / terrainIdentity）と terrain-scatter.js
// （currentScatterPrototypes / terrainPrototypeLabel。読み込みはこの後ろだが、
// 呼ぶのは描画のときなので読み終わっている）のトップレベルをそのまま使う。
// ここの関数もグローバルのままにする（index.html の onclick= と app.js の
// renderEditor などが名前で呼ぶため）。
//
// 送るのは edit.terrain.brush {phase, x, y, tool, radius, strength, layer, prototype,
// density, spacing, scaleJitter}（x, y は映像の NDC。後ろの 4 つは 🌲 散布の欄）。
// phase は hover（輪だけ）/ begin / move / end。値の既定値・
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
  // 半径・強さ・塗る層・散布の欄はこのブラウザの localStorage に覚える（道具は
  // 覚えない = 開き直したら選んでいない状態から始まる）。
  const BRUSH_STORE = 'terrain:brush';
  // 筆の欄（送るキー → 欄の id）。欄が空ならそのキーは送らない = サーバーの
  // 既定値。層・原型の select は値が番号（0 始まり）の数なので同じ readFields で
  // 読める。prototype〜scaleJitter は 🌲 散布の欄（サーバーは散布の道具のときだけ
  // 見る）。
  const BRUSH_FIELDS = {
    radius: 'tbRadius', strength: 'tbStrength', layer: 'tbLayer',
    prototype: 'tbProto', density: 'tbDensity', spacing: 'tbSpacing',
    scaleJitter: 'tbJitter',
  };
  // 選択肢がまだ無い間も覚えた値を消さない select の欄（applyBrushValues）。
  const BRUSH_SELECT_KEYS = ['layer', 'prototype'];
  // 確定した（onchange の）筆の欄の値。送るのはこちら（打っている途中の
  // 欄の値を 33 ms ごとに送らない）。
  let brushValues = {};

  // 道具は 'none'（筆を使っていない = 映像のクリックはいつもどおり選択・
  // ギズモ）か、#terrainBar の button[data-tool] のどれか（ボタンの
  // data-tool がサーバーの terrain::brushToolFromName と同じ綴りの唯一の一覧）。
  let brushTool = 'none';
  // 押しているポインタ（押していなければ null）。筆が取った指かの唯一の印で、
  // app.js のタッチの 1 本目もこれを見る（筆が取った指は掴まない）。
  let brushPointerId = null;
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
    renderBrushLayers();
    renderBrushLayerNote();
    renderBrushProtos();
    // 散布の欄（原型・密度・間隔・ばらつき）は 🌲 のときだけ（⌫ は円の中を原型に
    // よらず全部消すので、どれも使わない）。
    const scatterFields = document.getElementById('tbScatter');
    if (scatterFields) scatterFields.hidden = brushTool !== 'scatter';
    bar.querySelectorAll('button, input, select').forEach((el) => { el.disabled = !owner; });
    brushToolButtons().forEach((b) => {
      b.classList.toggle('on', b.dataset.tool === brushTool);
    });
  }
  // サーバーの層の一覧（/scene の terrain.layers。無ければ空）。
  function brushTerrainLayers() {
    const t = sceneData && sceneData.terrain;
    return (t && t.layers) || [];
  }
  // 層 i にテクスチャがあるか（サーバーの値が空でないか。ここで別の判定はしない）。
  function brushLayerHasTexture(i) {
    const l = brushTerrainLayers()[i];
    return !!(l && l.texture);
  }
  // 下地の層の番号（何も塗っていないマスの層 = 🧽 消すが塗る層）。値はサーバーの
  // terrainTools.baseLayer（terrain::kBaseSplatLayer）で、ここには持たない。
  // 届いていなければ null。🖌 の選択肢はこれより後ろの層だけ。
  function brushBaseLayer() {
    const b = terrainTools().baseLayer;
    return Number.isInteger(b) ? b : null;
  }
  // 層の選択欄の説明（知らせが無いときの title）。
  const BRUSH_LAYER_TIP = '🖌 で塗る層（Inspector の『塗り分けの層』の番号）';
  // 🖌 で塗れる層の番号か（下地より後ろで、層の数の内側）。選択肢の作り方と
  // 知らせのための表示用の判定で、塗れるかの最終判断はサーバー（下地や範囲外の
  // 層のスタンプは terrain::usableStamp が弾く）。
  function brushPaintableLayer(v) {
    const base = brushBaseLayer();
    return base !== null && Number.isInteger(v) && v > base &&
        v < brushTerrainLayers().length;
  }
  // 番号の select（層・原型）の選択肢を作り直す: sig（中身の目印）が前回と違う
  // ときだけ、first〜end - 1 の番号を labelFn(番号) の名前で並べ直す。そのときは
  // 選択中（フォーカス中）でも閉じて作り直す（古い選択肢を残さない）。作り直したら
  // true（呼び手は続けて pickBrushSelect で選ぶ）。
  function rebuildBrushSelect(sel, sig, first, end, labelFn) {
    if (sel.dataset.sig === sig) return false;
    if (sel === document.activeElement) sel.blur();
    sel.dataset.sig = sig;
    sel.textContent = '';
    for (let i = first; i < end; i++) {
      const opt = document.createElement('option');
      opt.value = String(i);
      opt.textContent = labelFn(i);
      sel.appendChild(opt);
    }
    return true;
  }
  // 番号の select で選ぶ値: 今の値（brushValues[key]）→ 覚えた値 → fallback の順で、
  // isValid を満たす最初のもの。どれも満たさなければ brushValues[key] を消す
  // （そのキーは送らない）。
  function pickBrushSelect(sel, key, isValid, fallback) {
    const saved = (loadStoredJson(BRUSH_STORE) || {})[key];
    const want = isValid(brushValues[key]) ? brushValues[key]
               : isValid(saved) ? saved : fallback;
    if (isValid(want)) {
      sel.value = String(want);
      brushValues[key] = want;
    } else {
      delete brushValues[key];
    }
  }
  // 🖌 で塗る層の選択肢: 下地の次（層 2）〜N（値は 0 始まりの層の番号）。下地は
  // 🧽 消すが受け持つ（何も塗っていないマスは下地が 100% なので、下地を塗っても
  // 何も変わらない）。数はサーバーの terrain.layers の数（ここには持たない）。
  // テクスチャの無い層は灰色（選べる。全部空でも選べなくならないように）。
  // 作り直すのは層の数・下地の番号・テレイン（terrainIdentity）が変わったとき
  // だけ。そのときは選択中（フォーカス中）でも閉じて作り直す（古い選択肢と層を
  // 残さない）。テクスチャの有無の変化（他の人の操作でも起きる）は作り直さず、
  // 灰色の印だけ付け直す（開いている選択を閉じない）。
  // 選ぶ層は、今の値 → 覚えた値 → 下地の次の層の順（下地・範囲外は下地の次）。
  function renderBrushLayers() {
    const sel = document.getElementById('tbLayer');
    if (!sel) return;
    const layers = brushTerrainLayers();
    const base = brushBaseLayer();
    const sig = base + ':' + layers.length + ':' + terrainIdentity();
    const first = base === null ? layers.length : base + 1;
    if (rebuildBrushSelect(sel, sig, first, layers.length, terrainLayerLabel)) {
      // 塗れる層が無い（層が下地だけ・下地が届いていない）なら選ばない。
      pickBrushSelect(sel, 'layer', brushPaintableLayer, base === null ? null : base + 1);
    }
    // テクスチャの有無の印（毎回。選択肢は作り直さない）。
    for (const opt of sel.options) {
      const none = !brushLayerHasTexture(Number(opt.value));
      opt.classList.toggle('noTex', none);
      opt.title = none ? 'テクスチャがありません' : '';
    }
  }
  // 🌲 で置く原型の選択肢（値は 0 始まりの原型の番号、名前は terrainPrototypeLabel）。
  // 作り直すのは原型の数・テレイン（terrainIdentity）が変わったときだけ。
  // 選ぶ原型は、今の値 → 覚えた値 → 最初の原型の順（一覧が届いていなければ選ばない）。
  function renderBrushProtos() {
    const sel = document.getElementById('tbProto');
    if (!sel) return;
    const protos = currentScatterPrototypes();
    const sig = protos.length + ':' + terrainIdentity();
    if (!rebuildBrushSelect(sel, sig, 0, protos.length,
                            (i) => terrainPrototypeLabel(protos, i))) {
      return;
    }
    const inRange = (v) => Number.isInteger(v) && v >= 0 && v < protos.length;
    pickBrushSelect(sel, 'prototype', inRange, 0);
  }
  // 🖌 で塗れない理由（塗れるなら空）。塗れる層が無い・選んだ層にテクスチャが
  // 無い（こちらは塗れるが見えない）の知らせの文。
  function brushLayerProblem() {
    if (!brushPaintableLayer(brushValues.layer)) {
      return '塗れる層がありません（Inspector の『塗り分けの層』を確認）';
    }
    if (!brushLayerHasTexture(brushValues.layer)) {
      return terrainLayerLabel(brushValues.layer) +
             ' にテクスチャがありません（Inspector の『塗り分けの層』で設定）';
    }
    return '';
  }
  // 🖌 を選んでいる間、#terrainBar の層の選択の隣に知らせを出す（状態から毎回
  // 決める = 同じ層なら出しっぱなし、層やテクスチャが変われば消える）。選択欄の
  // title にも同じ文。選んでいる層にテクスチャが無ければ選択欄も灰色。
  function renderBrushLayerNote() {
    const sel = document.getElementById('tbLayer');
    const note = document.getElementById('tbLayerNote');
    const text = brushTool === 'paint' ? brushLayerProblem() : '';
    if (note) {
      note.hidden = !text;
      if (note.textContent !== '⚠ ' + text) note.textContent = '⚠ ' + text;
      note.title = text;
    }
    if (sel) {
      sel.classList.toggle('noTex', brushPaintableLayer(brushValues.layer) &&
                                    !brushLayerHasTexture(brushValues.layer));
      sel.title = text || BRUSH_LAYER_TIP;
    }
  }
  // 層の選択の確定（onchange）。
  function applyBrushLayer() {
    applyBrushValues();
    renderBrushLayerNote();
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
  // 筆の欄の確定（onchange）。範囲の丸めはサーバー。覚えるのは数の入っている
  // 欄だけ。層・原型の選択肢がまだ無い（一覧が届く前・塗れる層が無い）ときは、
  // 覚えた値を消さずに残す（次に選択肢ができたらそれを選ぶ）。
  function applyBrushValues() {
    brushValues = readFields(BRUSH_FIELDS);
    const store = Object.assign({}, brushValues);
    const saved = loadStoredJson(BRUSH_STORE) || {};
    for (const key of BRUSH_SELECT_KEYS) {
      if (!(key in store) && Number.isInteger(saved[key])) store[key] = saved[key];
    }
    storeJson(BRUSH_STORE, store);
  }
  // 覚えた値があるときだけ欄に入れる（無ければ空 = サーバーの既定値）。
  restoreFields(BRUSH_STORE, BRUSH_FIELDS);
  brushValues = readFields(BRUSH_FIELDS);

  // ---- ポインタ --------------------------------------------------------------
  // #hit の capture 段階で受ける。マウス・ペンでは筆が受け持つ押下を app.js の
  // pick / drag / カメラ操作へ流さない（同じ要素の後の聞き手も止める）。
  // タッチは app.js の指の一覧に入れるため流す（app.js は brushPointerId の
  // 指を掴まない）。Ctrl 付きはいつもどおりカメラ操作、左ボタン以外も素通し。
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
    // 🖌 で塗れる層が無い（選択が空）ならストロークを送らない（サーバーは下地を
    // 塗るスタンプを弾くが、送らなければ輪も出し続けずに済む）。知らせは
    // renderBrushLayerNote が #terrainBar に出している。
    if (brushTool === 'paint' && !brushPaintableLayer(brushValues.layer)) {
      renderBrushLayerNote();
      return;
    }
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
