// Charon web UI: 制御盤。Scene タブの Events の下の「🎛 制御盤（コントローラ）」で
// 別ウィンドウを開き、制御器のパラメータ（スライダー / スイッチ）・メーター
// （scope:* の現在値）・グラフ（文書の <control><chart>）を並べる。
//
// app.js / control.js の後ろに読む古典スクリプト。窓の中身はこのページの JS が
// 直接組み立てる（window.open('') の空のページは同じオリジンなので DOM を触れる）
// = 窓の側にはスクリプトも状態も無く、サーバーへの送信もデータの取得もこのページ
// の send / fetch を使う。窓の中のボタンは addEventListener で結ぶ（onclick=
// 文字列は窓の側のグローバルで評価されて関数が見えないため）。
//
// サーバー側の約束:
//   /scene の formulas.params … [{name, value, min, max, label, step}]
//   /scene の formulas.charts … [{title, signals:["計算式/入出力の名前"], span}]
//   /control?since=N        … 記録の差分（スコープと同じ。since は窓ごとに別）
//   edit.param.set（どのカメラからでも可）、edit.chart.add / set / remove（エディタ
//   カメラのページのみ。サーバーが判定）

  const PANEL_COLORS = ['#5aa9ff', '#f2a65a', '#6fd08c', '#e86f8a', '#b48cf2',
                        '#e8d36a', '#5fd4d4', '#d98f5f', '#9aa4b5', '#f07fd2'];
  const PANEL_MAX_ROWS = 300000;   // 1 制御器あたり窓に持つ行数の上限
  const PANEL_ALL_SEC = 120;       // span = 0（全部）のグラフが参照する記録の長さ
  const panel = {
    win: null, timer: null, since: 0, run: -1, data: {}, polling: false,
    paramsKey: '', metersKey: '', chartsKey: '', editing: -1, meterEls: {}, edit: false,
    chartEls: [], dirty: true,
  };

  const PANEL_HTML = `<!doctype html>
<html lang="ja"><head><meta charset="utf-8"><title>制御盤 - Charon</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin: 0; background: #14171c; color: #d7dce4; font: 13px system-ui, sans-serif; }
  header { position: sticky; top: 0; z-index: 2; display: flex; align-items: center; gap: 10px;
           padding: 8px 12px; background: #1c2027; border-bottom: 1px solid #2c323c; }
  header .ttl { font-weight: 600; font-size: 14px; }
  header .scn { color: #9aa3b0; }
  header .state { font-size: 12px; padding: 2px 8px; border-radius: 10px; background: #2a2f38; }
  header .state.run { background: #1f4a2f; color: #8fe0a8; }
  header .sp { flex: 1; }
  button { background: #2a303a; color: #d7dce4; border: 1px solid #3a414d; border-radius: 4px;
           padding: 4px 9px; cursor: pointer; font: inherit; }
  button:hover { background: #343b47; }
  button:disabled { opacity: .45; cursor: default; }
  section { padding: 10px 12px 4px; }
  h2 { margin: 0 0 8px; font-size: 12px; font-weight: 600; color: #9aa3b0; letter-spacing: .03em; }
  .hint { color: #7d8693; font-size: 12px; }
  .params { display: grid; grid-template-columns: repeat(auto-fill, minmax(230px, 1fr)); gap: 8px; }
  .pcard { background: #1c2027; border: 1px solid #2c323c; border-radius: 6px; padding: 8px 10px; }
  .pcard .top { display: flex; align-items: baseline; gap: 6px; }
  .pcard .lbl { font-weight: 600; }
  .pcard .nm { color: #7d8693; font-size: 11px; font-family: ui-monospace, monospace; display: block; }
  .pcard .val { margin-left: auto; font-size: 20px; color: #f2c66d; font-variant-numeric: tabular-nums; }
  .pcard input[type=range] { width: 100%; margin: 8px 0 2px; }
  .pcard .rng { display: flex; justify-content: space-between; color: #6f7884; font-size: 11px; }
  .sw { display: inline-flex; align-items: center; gap: 8px; cursor: pointer; margin-top: 6px; }
  .sw input { display: none; }
  .sw .knob { width: 40px; height: 22px; border-radius: 11px; background: #3a414d; position: relative;
              transition: background .15s; }
  .sw .knob::after { content: ''; position: absolute; left: 3px; top: 3px; width: 16px; height: 16px;
                     border-radius: 50%; background: #d7dce4; transition: left .15s; }
  .sw input:checked + .knob { background: #3d8b5a; }
  .sw input:checked + .knob::after { left: 21px; }
  .meters { display: grid; grid-template-columns: repeat(auto-fill, minmax(140px, 1fr)); gap: 6px; }
  .meter { background: #1c2027; border: 1px solid #2c323c; border-radius: 6px; padding: 6px 8px; }
  .meter .nm { font-size: 11px; color: #9aa3b0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .meter .v { font-size: 18px; font-variant-numeric: tabular-nums; color: #e4e8ef; }
  .meter .f { font-size: 10px; color: #6f7884; }
  .charts { display: grid; grid-template-columns: repeat(auto-fill, minmax(420px, 1fr)); gap: 10px;
            align-items: start; }
  .chart { background: #1c2027; border: 1px solid #2c323c; border-radius: 6px; padding: 6px 8px; }
  .chart .head { display: flex; align-items: center; gap: 6px; }
  .chart .head .t { font-weight: 600; flex: 1; }
  .chart .head button { padding: 1px 6px; font-size: 12px; }
  .chart canvas { width: 100%; height: 190px; display: block; margin-top: 4px; }
  .chart .lg { display: flex; flex-wrap: wrap; gap: 4px 12px; margin-top: 4px; font-size: 11.5px; }
  .chart .lg span.c { display: inline-block; width: 10px; height: 3px; margin-right: 4px; vertical-align: middle; }
  .chart .lg b { font-weight: 500; color: #f2c66d; margin-left: 4px; font-variant-numeric: tabular-nums; }
  .edit { margin-top: 6px; padding: 6px; background: #161a20; border: 1px solid #2c323c; border-radius: 4px; }
  .edit .row { display: flex; gap: 6px; align-items: center; margin-bottom: 6px; }
  .edit input[type=text] { flex: 1; background: #0f1216; color: #d7dce4; border: 1px solid #3a414d;
                           border-radius: 3px; padding: 3px 6px; }
  .edit input[type=number] { width: 80px; background: #0f1216; color: #d7dce4; border: 1px solid #3a414d;
                             border-radius: 3px; padding: 3px 6px; }
  .edit .grp { margin: 4px 0; }
  .edit .grp .g { color: #9aa3b0; font-size: 11px; margin: 4px 0 2px; }
  .edit label { display: inline-flex; align-items: center; gap: 3px; margin: 1px 10px 1px 0; font-size: 12px; }
  button.on { background: #3d5f8b; border-color: #5a83b8; }
  .pedit { margin-top: 8px; padding-top: 6px; border-top: 1px dashed #333a45; font-size: 12px; }
  .pedit .row { display: flex; align-items: center; gap: 5px; margin-bottom: 5px; flex-wrap: wrap; }
  .pedit input[type=text] { flex: 1; min-width: 80px; }
  .pedit input[type=text], .pedit input[type=number] { background: #0f1216; color: #d7dce4;
      border: 1px solid #3a414d; border-radius: 3px; padding: 2px 5px; font: inherit; }
  .pedit input[type=number] { width: 64px; }
  .pedit .sp { flex: 1; }
  .pedit button { padding: 1px 7px; font-size: 12px; }
  .pcard.add { border-style: dashed; }
  .params.editing { grid-template-columns: repeat(auto-fill, minmax(290px, 1fr)); }
  .pedit label { display: inline-flex; align-items: center; gap: 4px; margin-left: 8px; }
  .pcard.add h3 { margin: 0 0 6px; font-size: 12px; color: #9aa3b0; font-weight: 600; }
</style></head>
<body>
<header>
  <span class="ttl">🎛 制御盤</span><span class="scn" id="scn"></span>
  <span class="state" id="state"></span><span class="sp"></span>
  <button id="bSim" title="シミュレートを始める">▶ シミュレート</button>
  <button id="bStop" title="エディタへ戻る（止める）">■ 停止</button>
  <button id="bReset" title="置いた姿勢から始め直す">⟲ Reset</button>
  <button id="bEdit" title="制御盤を編集する（エディタモードのみ）: パラメータの追加・表示名・範囲・刻み・並び、グラフの追加・編集">✎ 編集</button>
  <button id="bSave" title="シーンを保存する（制御盤の構成もシーンの XML に入る）">💾 保存</button>
</header>
<section><h2>パラメータ（式からは param:名前）</h2><div id="params" class="params"></div></section>
<section><h2>メーター（scope: の現在値）</h2><div id="meters" class="meters"></div></section>
<section><h2>グラフ <button id="bAddChart" title="グラフを追加（シーンに保存される）">＋ グラフ</button></h2>
  <div id="charts" class="charts"></div></section>
</body></html>`;

  function openControlPanel() {
    if (panel.win && !panel.win.closed) { panel.win.focus(); return; }
    const w = window.open('', 'charonControlPanel', 'width=1000,height=780,resizable=yes,scrollbars=yes');
    if (!w) {
      alert('ポップアップがブロックされました。このサイトのポップアップを許可してください。');
      return;
    }
    panel.win = w;
    w.document.open();
    w.document.write(PANEL_HTML);
    w.document.close();
    panel.since = 0; panel.run = -1; panel.data = {};
    panel.paramsKey = panel.metersKey = panel.chartsKey = '';
    panel.editing = -1; panel.dirty = true;
    const $ = (id) => w.document.getElementById(id);
    $('bSim').addEventListener('click', () => setMode('simulate'));
    $('bStop').addEventListener('click', () => setMode('editor'));
    $('bReset').addEventListener('click', () => send('reset'));
    $('bAddChart').addEventListener('click', panelAddChart);
    $('bEdit').addEventListener('click', () => {
      panel.edit = !panel.edit;
      panel.paramsKey = panel.chartsKey = '';  // 組み直す
      if (!panel.edit) panel.editing = -1;
    });
    $('bSave').addEventListener('click', panelSave);
    if (!panel.timer) panel.timer = setInterval(panelTick, 120);
    panelTick();
  }
  function closeControlPanel() {
    if (panel.win && !panel.win.closed) panel.win.close();
    panel.win = null;
    if (panel.timer) clearInterval(panel.timer);
    panel.timer = null;
  }
  // このページを離れたら窓も閉じる（窓の中身はこのページの JS が描いているため）。
  window.addEventListener('beforeunload', closeControlPanel);

  function panelOpen() { return panel.win && !panel.win.closed; }

  function panelTick() {
    if (!panelOpen()) { closeControlPanel(); return; }
    panelPoll();
    panelRenderHead();
    panelRenderParams();
    panelRenderCharts();
    panelRenderMeters();
    if (panel.dirty) { panel.dirty = false; panelPaint(); }
  }

  // ---- データ（/control の差分。スコープとは別の since）---------------------
  async function panelPoll() {
    if (panel.polling) return;
    panel.polling = true;
    try {
      // 追いつくまで続けて取る（1 回 4000 行まで）。
      for (let k = 0; k < 8; ++k) {
        const r = await fetch('control?since=' + panel.since);
        if (!r.ok) break;
        const d = await r.json();
        if (d.run !== panel.run) { panel.run = d.run; panel.data = {}; }
        let got = 0;
        for (const s of d.series || []) {
          let cur = panel.data[s.formula];
          if (!cur || JSON.stringify(cur.names) !== JSON.stringify(s.names)) {
            cur = panel.data[s.formula] = { names: s.names, rows: [] };
          }
          for (const row of s.rows) cur.rows.push(row);
          got += s.rows.length;
        }
        const live = (d.series || []).map((s) => s.formula);
        for (const f of Object.keys(panel.data)) if (!live.includes(f)) delete panel.data[f];
        panel.since = d.seq || panel.since;
        if (got) panel.dirty = true;
        if (got < 3000) break;
      }
      panelTrim();
    } catch (e) { /* 次のティックで */ }
    panel.polling = false;
  }
  // 持つ長さ = その制御器を参照するグラフの最大の表示幅（全部 = PANEL_ALL_SEC）。
  function panelTrim() {
    const keep = {};
    for (const c of panelCharts()) {
      for (const id of c.signals || []) {
        const f = id.slice(0, id.indexOf('/'));
        const want = c.span > 0 ? c.span : PANEL_ALL_SEC;
        keep[f] = Math.max(keep[f] || 0, want);
      }
    }
    for (const f of Object.keys(panel.data)) {
      const rows = panel.data[f].rows;
      if (!rows.length) continue;
      const sec = (keep[f] || 1) + 1;
      const tEnd = rows[rows.length - 1][0];
      let cut = lowerBound(rows, tEnd - sec);
      cut = Math.max(cut, rows.length - PANEL_MAX_ROWS);
      if (cut > 0) rows.splice(0, cut);
    }
  }
  function lowerBound(rows, t) {
    let lo = 0, hi = rows.length;
    while (lo < hi) { const m = (lo + hi) >> 1; if (rows[m][0] < t) lo = m + 1; else hi = m; }
    return lo;
  }
  function panelCharts() {
    return (sceneData && sceneData.formulas && sceneData.formulas.charts) || [];
  }
  // 選べる信号: 制御器の入出力（走らせる前でもコンパイル結果から）+ 記録の列。
  function panelSignalChoices() {
    const out = {};
    const run = (sceneData && sceneData.control && sceneData.control.controllers) || [];
    for (const c of run) out[c.formula] = (c.inputs || []).concat(c.outputs || []);
    for (const f of Object.keys(panel.data)) {
      const have = out[f] || [];
      for (const n of panel.data[f].names) if (!have.includes(n)) have.push(n);
      out[f] = have;
    }
    return out;
  }
  function shortSig(n) { const c = n.indexOf(':'); return c >= 0 ? n.slice(c + 1) : n; }
  function lastValue(id) {
    const slash = id.indexOf('/');
    const st = panel.data[id.slice(0, slash)];
    if (!st || !st.rows.length) return null;
    const col = st.names.indexOf(id.slice(slash + 1));
    return col < 0 ? null : st.rows[st.rows.length - 1][col + 1];
  }

  // ---- 見出し -----------------------------------------------------------------
  function panelRenderHead() {
    const d = panel.win.document;
    const sim = sceneData && sceneData.mode === 'simulate';
    d.getElementById('scn').textContent = (sceneData && sceneData.sceneFile) || '(無題のシーン)';
    const st = d.getElementById('state');
    st.textContent = sim ? '● シミュレート中' : '■ エディタ（停止中）';
    st.classList.toggle('run', !!sim);
    d.getElementById('bSim').disabled = !!sim;
    d.getElementById('bStop').disabled = !sim;
    // 編集はエディタモード × エディタカメラのページだけ（サーバーも edit.* を
    // エディタカメラのページ以外から受けない）。シミュレートに入ったら抜ける。
    if (panel.edit && !panelCanEdit()) {
      panel.edit = false;
      panel.editing = -1;
      panel.paramsKey = panel.chartsKey = '';
    }
    const be = d.getElementById('bEdit');
    be.hidden = !panelCanEdit();
    be.textContent = panel.edit ? '✓ 編集を終える' : '✎ 編集';
    be.classList.toggle('on', panel.edit);
    d.getElementById('bSave').hidden = !panelEditing();
    d.getElementById('bAddChart').hidden = !panelEditing();
  }
  function panelCanEdit() {
    return isEditorCam() && !!sceneData && sceneData.mode !== 'simulate';
  }
  function panelEditing() { return panel.edit && panelCanEdit(); }
  function panelSave() {
    let name = (sceneData && sceneData.sceneFile) || '';
    if (!name) {
      name = panel.win.prompt('シーンの名前（英数字と _ -）', 'my_scene');
      if (!name) return;
    }
    send('edit.save', { name: name.trim() });
  }

  // ---- パラメータ -------------------------------------------------------------
  function panelRenderParams() {
    const d = panel.win.document;
    const box = d.getElementById('params');
    const params = controlParams();
    const editing = panelEditing();
    const key = JSON.stringify([params.map((p) => [p.name, p.min, p.max, p.step, p.label]), editing]);
    // 構造が同じなら値だけ直す（触っている欄は触らない）。
    if (key === panel.paramsKey || (editing && box.contains(d.activeElement) &&
                                     d.activeElement.closest('.pedit, .add'))) {
      for (const p of params) {
        const el = box.querySelector('[data-p="' + p.name + '"]');
        if (!el || el.contains(d.activeElement)) continue;
        const inp = el.querySelector('input');
        if (paramIsSwitch(p)) inp.checked = p.value >= 0.5;
        else { inp.value = p.value; el.querySelector('.val').textContent = fmtParam(p.value, p.name); }
      }
      return;
    }
    panel.paramsKey = key;
    box.innerHTML = '';
    box.classList.toggle('editing', editing);
    if (!params.length && !editing) {
      box.innerHTML = '<div class="hint">パラメータはありません（エディタモードで「✎ 編集」から' +
                      '足せます）。</div>';
      return;
    }
    for (const p of params) {
      const card = d.createElement('div');
      card.className = 'pcard';
      card.dataset.p = p.name;
      const label = p.label || p.name;
      if (paramIsSwitch(p)) {
        card.innerHTML = '<div class="top"><span class="lbl"></span></div><span class="nm"></span>' +
          '<label class="sw"><input type="checkbox"><span class="knob"></span><span class="st"></span></label>';
        const inp = card.querySelector('input');
        inp.checked = p.value >= 0.5;
        const showState = () => { card.querySelector('.st').textContent = inp.checked ? 'オン' : 'オフ'; };
        showState();
        inp.addEventListener('change', () => { showState(); paramInput(p.name, inp.checked ? 1 : 0, true); });
      } else {
        card.innerHTML = '<div class="top"><span class="lbl"></span>' +
          '<span class="val"></span></div><span class="nm"></span><input type="range"><div class="rng"><span></span><span></span></div>';
        const inp = card.querySelector('input');
        inp.min = p.min; inp.max = p.max; inp.step = paramStep(p); inp.value = p.value;
        const val = card.querySelector('.val');
        val.textContent = fmtParam(p.value, p.name);
        const rng = card.querySelectorAll('.rng span');
        rng[0].textContent = fmtParam(p.min, p.name);
        rng[1].textContent = fmtParam(p.max, p.name);
        inp.addEventListener('input', () => {
          val.textContent = fmtParam(parseFloat(inp.value), p.name);
          paramInput(p.name, inp.value);
        });
        inp.addEventListener('change', () => paramInput(p.name, inp.value, true));
      }
      card.querySelector('.lbl').textContent = label;
      card.querySelector('.nm').textContent = 'param:' + p.name;
      if (editing) card.appendChild(panelParamEditor(d, p));
      box.appendChild(card);
    }
    if (editing) box.appendChild(panelParamAddCard(d));
  }

  // 編集モードのカードの下半分: 表示名・範囲・刻み・スイッチ・並び・削除。
  // 欄を離れた（change）ときに送る = 打っている途中の値は送らない。
  function panelParamEditor(d, p) {
    const ed = d.createElement('div');
    ed.className = 'pedit';
    ed.innerHTML =
      '<div class="row">表示名 <input type="text" class="el" placeholder="（名前のまま）"></div>' +
      '<div class="row">範囲 <input type="number" class="emin"> 〜 <input type="number" class="emax"></div>' +
      '<div class="row">刻み <input type="number" class="estep" min="0" title="0 = 自動（範囲の約 1/200）">' +
      '<label><input type="checkbox" class="esw"> スイッチ（オン / オフ）</label></div>' +
      '<div class="row"><span class="sp"></span><button class="ml" title="前へ">◀</button>' +
      '<button class="mr" title="後ろへ">▶</button><button class="del" title="削除">✕ 削除</button></div>';
    const q = (c) => ed.querySelector(c);
    q('.el').value = p.label || '';
    q('.emin').value = p.min;
    q('.emax').value = p.max;
    q('.estep').value = p.step || 0;
    const sw = paramIsSwitch(p);
    q('.esw').checked = sw;
    for (const c of ['.emin', '.emax', '.estep']) q(c).disabled = sw;
    const set = (patch) => send('edit.param.set', Object.assign({ name: p.name }, patch));
    q('.el').addEventListener('change', () => set({ label: q('.el').value }));
    q('.emin').addEventListener('change', () => {
      const v = parseFloat(q('.emin').value); if (isFinite(v)) set({ min: v });
    });
    q('.emax').addEventListener('change', () => {
      const v = parseFloat(q('.emax').value); if (isFinite(v)) set({ max: v });
    });
    q('.estep').addEventListener('change', () => {
      const v = parseFloat(q('.estep').value); if (isFinite(v) && v >= 0) set({ step: v });
    });
    q('.esw').addEventListener('change', () => {
      if (q('.esw').checked) set({ min: 0, max: 1, step: 1, value: p.value >= 0.5 ? 1 : 0 });
      else set({ step: 0 });
    });
    q('.ml').addEventListener('click', () => set({ move: -1 }));
    q('.mr').addEventListener('click', () => set({ move: 1 }));
    q('.del').addEventListener('click', () => {
      if (!panel.win.confirm('パラメータ「' + (p.label || p.name) + '」を削除します。式の param:' +
                             p.name + ' は 0 を読むようになります。')) return;
      send('edit.param.remove', { name: p.name });
    });
    return ed;
  }
  // 編集モードの末尾のカード: 新しいパラメータ。
  function panelParamAddCard(d) {
    const card = d.createElement('div');
    card.className = 'pcard add';
    card.innerHTML = '<h3>＋ パラメータを追加</h3><div class="pedit" style="border:0;margin:0;padding:0">' +
      '<div class="row">名前 <input type="text" class="an" placeholder="英数字と _（式からは param:名前）"></div>' +
      '<div class="row">表示名 <input type="text" class="al" placeholder="（省略可）"></div>' +
      '<div class="row">範囲 <input type="number" class="amin" value="0"> 〜 ' +
      '<input type="number" class="amax" value="1"></div>' +
      '<div class="row"><label><input type="checkbox" class="asw"> スイッチ（オン / オフ）</label>' +
      '<span class="sp"></span><button class="ok">追加</button></div></div>';
    const q = (c) => card.querySelector(c);
    q('.asw').addEventListener('change', () => {
      q('.amin').disabled = q('.amax').disabled = q('.asw').checked;
    });
    q('.ok').addEventListener('click', () => {
      const name = q('.an').value.trim();
      if (!/^[A-Za-z0-9_-]+$/.test(name)) {
        panel.win.alert('名前は英数字と _ - で付けてください（式からは param:名前 で読みます）。');
        return;
      }
      if (controlParams().some((p) => p.name === name)) {
        panel.win.alert('同じ名前のパラメータがあります: ' + name);
        return;
      }
      const msg = { name: name, label: q('.al').value };
      if (q('.asw').checked) Object.assign(msg, { min: 0, max: 1, step: 1, value: 0 });
      else {
        const lo = parseFloat(q('.amin').value), hi = parseFloat(q('.amax').value);
        if (!isFinite(lo) || !isFinite(hi)) return;
        Object.assign(msg, { min: lo, max: hi, value: lo });
      }
      send('edit.param.add', msg);
      q('.an').value = ''; q('.al').value = '';
      panel.win.document.activeElement.blur();
    });
    return card;
  }

  // ---- メーター（scope: の現在値）----------------------------------------------
  function panelRenderMeters() {
    const d = panel.win.document;
    const box = d.getElementById('meters');
    const ids = [];
    for (const f of Object.keys(panel.data)) {
      for (const n of panel.data[f].names) if (n.startsWith('scope:')) ids.push(f + '/' + n);
    }
    const key = ids.join('|');
    if (key !== panel.metersKey) {
      panel.metersKey = key;
      panel.meterEls = {};
      box.innerHTML = ids.length ? '' : '<div class="hint">シミュレートすると、制御器の ' +
                                        'scope: の値がここに出ます。</div>';
      for (const id of ids) {
        const m = d.createElement('div');
        m.className = 'meter';
        const slash = id.indexOf('/');
        m.innerHTML = '<div class="nm"></div><div class="v">-</div><div class="f"></div>';
        m.querySelector('.nm').textContent = shortSig(id.slice(slash + 1));
        m.querySelector('.f').textContent = id.slice(0, slash);
        box.appendChild(m);
        panel.meterEls[id] = m.querySelector('.v');
      }
    }
    for (const id of ids) {
      const v = lastValue(id);
      panel.meterEls[id].textContent = v === null ? '-' : fmtNum(v);
    }
  }

  // ---- グラフ -------------------------------------------------------------------
  function panelAddChart() {
    const title = panel.win.prompt('グラフの名前', 'グラフ');
    if (title === null) return;
    panel.editing = panelCharts().length;  // 届いたら編集を開く
    send('edit.chart.add', { title: title, signals: [], span: 10 });
  }
  function panelRenderCharts() {
    const d = panel.win.document;
    const box = d.getElementById('charts');
    const charts = panelCharts();
    const here = panelEditing();
    const editing = d.querySelector('.edit');
    if (editing && editing.contains(d.activeElement)) return;  // 編集の入力中は組み直さない
    const key = JSON.stringify([charts, here, panel.editing,
                                panel.editing >= 0 ? panelSignalChoices() : 0]);
    if (key === panel.chartsKey) return;
    panel.chartsKey = key;
    box.innerHTML = '';
    panel.chartEls = [];
    if (!charts.length) {
      box.innerHTML = '<div class="hint">グラフはありません' +
                      (here ? '（「＋ グラフ」で追加。シーンと一緒に保存されます）。' : '。') + '</div>';
    }
    charts.forEach((c, i) => {
      const card = d.createElement('div');
      card.className = 'chart';
      card.innerHTML = '<div class="head"><span class="t"></span>' +
        (here ? '<button data-a="left" title="前へ">◀</button><button data-a="right" title="後ろへ">▶</button>' +
                '<button data-a="edit" title="信号・表示幅を編集">⚙</button>' +
                '<button data-a="del" title="グラフを削除">✕</button>' : '') +
        '</div><canvas></canvas><div class="lg"></div>';
      card.querySelector('.t').textContent = (c.title || 'グラフ') +
        (c.span > 0 ? '  （' + c.span + ' s）' : '  （全部）');
      const act = (a, fn) => { const b = card.querySelector('[data-a="' + a + '"]'); if (b) b.addEventListener('click', fn); };
      act('left', () => send('edit.chart.set', { index: i, move: -1 }));
      act('right', () => send('edit.chart.set', { index: i, move: 1 }));
      act('edit', () => { panel.editing = panel.editing === i ? -1 : i; panel.chartsKey = ''; });
      act('del', () => {
        if (!panel.win.confirm('グラフ「' + (c.title || '') + '」を削除します。')) return;
        if (panel.editing === i) panel.editing = -1;
        send('edit.chart.remove', { index: i });
      });
      const lg = card.querySelector('.lg');
      const vals = [];
      (c.signals || []).forEach((id, k) => {
        const it = d.createElement('span');
        it.innerHTML = '<span class="c"></span><span class="n"></span><b></b>';
        it.querySelector('.c').style.background = PANEL_COLORS[k % PANEL_COLORS.length];
        it.querySelector('.n').textContent = shortSig(id.slice(id.indexOf('/') + 1));
        it.title = id;
        lg.appendChild(it);
        vals.push(it.querySelector('b'));
      });
      if (!(c.signals || []).length) lg.innerHTML = '<span class="hint">信号がありません（⚙ で選ぶ）</span>';
      if (here && panel.editing === i) card.appendChild(panelChartEditor(d, c, i));
      box.appendChild(card);
      panel.chartEls.push({ canvas: card.querySelector('canvas'), chart: c, vals: vals });
    });
    panel.dirty = true;
  }
  function panelChartEditor(d, c, i) {
    const ed = d.createElement('div');
    ed.className = 'edit';
    ed.innerHTML = '<div class="row">名前 <input type="text" class="et"> 表示幅 ' +
      '<input type="number" class="es" min="0" step="0.1" title="秒。0 = 記録の全部"> s</div>' +
      '<div class="sigs"></div><div class="row"><button class="ok">適用</button>' +
      '<button class="no">閉じる</button></div>';
    ed.querySelector('.et').value = c.title || '';
    ed.querySelector('.es').value = c.span;
    const sigs = ed.querySelector('.sigs');
    const choices = panelSignalChoices();
    const fs = Object.keys(choices);
    if (!fs.length) sigs.innerHTML = '<div class="hint">制御器がありません。</div>';
    for (const f of fs) {
      const g = d.createElement('div');
      g.className = 'grp';
      g.innerHTML = '<div class="g"></div>';
      g.querySelector('.g').textContent = '🧮 ' + f;
      for (const n of choices[f]) {
        const id = f + '/' + n;
        const l = d.createElement('label');
        l.innerHTML = '<input type="checkbox"><span></span>';
        l.querySelector('input').checked = (c.signals || []).includes(id);
        l.querySelector('input').dataset.id = id;
        l.querySelector('span').textContent = n;
        g.appendChild(l);
      }
      sigs.appendChild(g);
    }
    ed.querySelector('.ok').addEventListener('click', () => {
      const picked = [];
      // 選んだ順ではなく、今の並び（既にある信号の順）+ 新しく足した物の順にする。
      for (const id of c.signals || []) {
        const box = sigs.querySelector('input[data-id="' + id + '"]');
        if (box && box.checked) picked.push(id);
      }
      for (const box of sigs.querySelectorAll('input:checked')) {
        if (!picked.includes(box.dataset.id)) picked.push(box.dataset.id);
      }
      send('edit.chart.set', {
        index: i, title: ed.querySelector('.et').value, signals: picked,
        span: parseFloat(ed.querySelector('.es').value) || 0,
      });
      panel.editing = -1;
      panel.chartsKey = '';
    });
    ed.querySelector('.no').addEventListener('click', () => { panel.editing = -1; panel.chartsKey = ''; });
    return ed;
  }

  function panelPaint() {
    for (const ce of panel.chartEls) panelPaintChart(ce);
  }
  function panelPaintChart(ce) {
    const cv = ce.canvas;
    const w = cv.clientWidth, h = cv.clientHeight;
    if (w <= 0 || h <= 0) return;
    const dpr = panel.win.devicePixelRatio || 1;
    if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) {
      cv.width = Math.round(w * dpr);
      cv.height = Math.round(h * dpr);
    }
    const g = cv.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    g.fillStyle = '#12151a';
    g.fillRect(0, 0, w, h);
    const c = ce.chart;
    const sig = (c.signals || []).map((id, k) => {
      const slash = id.indexOf('/');
      const st = panel.data[id.slice(0, slash)];
      const col = st ? st.names.indexOf(id.slice(slash + 1)) : -1;
      return { st: st, col: col < 0 ? -1 : col + 1, color: PANEL_COLORS[k % PANEL_COLORS.length], k: k };
    }).filter((s) => s.st && s.col > 0 && s.st.rows.length);
    // 凡例の現在値
    (c.signals || []).forEach((id, k) => {
      const v = lastValue(id);
      if (ce.vals[k]) ce.vals[k].textContent = v === null ? '' : fmtNum(v);
    });
    g.font = '11px system-ui, sans-serif';
    if (!sig.length) {
      g.fillStyle = '#6f7884';
      g.fillText('記録なし（シミュレートすると出ます）', 10, 18);
      return;
    }
    let tEnd = -Infinity, tFirst = Infinity;
    for (const s of sig) {
      tEnd = Math.max(tEnd, s.st.rows[s.st.rows.length - 1][0]);
      tFirst = Math.min(tFirst, s.st.rows[0][0]);
    }
    const span = c.span > 0 ? c.span : Math.max(1e-3, tEnd - tFirst);
    const t0 = tEnd - span;
    const L = 46, R = 8, T = 8, B = 18;
    const pw = w - L - R, ph = h - T - B;
    // 画素の列ごとの min / max（間引き）と縦軸の範囲。
    let lo = Infinity, hi = -Infinity;
    const cols = sig.map((s) => {
      const mn = new Float64Array(pw).fill(Infinity), mx = new Float64Array(pw).fill(-Infinity);
      const rows = s.st.rows;
      for (let i = lowerBound(rows, t0); i < rows.length; ++i) {
        const v = rows[i][s.col];
        if (!isFinite(v)) continue;
        const x = Math.min(pw - 1, Math.max(0, Math.floor((rows[i][0] - t0) / span * pw)));
        if (v < mn[x]) mn[x] = v;
        if (v > mx[x]) mx[x] = v;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
      }
      return { mn: mn, mx: mx, color: s.color };
    });
    if (!isFinite(lo)) { lo = 0; hi = 1; }
    if (hi - lo < 1e-9) { lo -= 1; hi += 1; }
    const pad = (hi - lo) * 0.06;
    lo -= pad; hi += pad;
    const Y = (v) => T + ph - (v - lo) / (hi - lo) * ph;
    // 目盛り
    g.strokeStyle = '#262b33'; g.fillStyle = '#7d8693'; g.lineWidth = 1;
    const ys = niceStep(hi - lo, 5);
    for (let v = Math.ceil(lo / ys) * ys; v <= hi; v += ys) {
      const y = Math.round(Y(v)) + 0.5;
      g.beginPath(); g.moveTo(L, y); g.lineTo(w - R, y); g.stroke();
      g.fillText(fmtNum(Math.abs(v) < ys * 1e-6 ? 0 : +v.toPrecision(6)), 2, y + 4);
    }
    const xs = niceStep(span, 6);
    for (let t = Math.ceil(t0 / xs) * xs; t <= tEnd; t += xs) {
      const x = Math.round(L + (t - t0) / span * pw) + 0.5;
      g.beginPath(); g.moveTo(x, T); g.lineTo(x, T + ph); g.stroke();
      g.fillText(fmtNum(+t.toPrecision(6)) + ' s', x + 2, h - 5);
    }
    // 線（列ごとに min〜max の縦線 + 隣の列への線）
    for (const cl of cols) {
      g.strokeStyle = cl.color; g.lineWidth = 1.4;
      g.beginPath();
      let started = false;
      for (let x = 0; x < pw; ++x) {
        if (!isFinite(cl.mn[x])) continue;
        const px = L + x + 0.5;
        if (!started) { g.moveTo(px, Y(cl.mn[x])); started = true; } else g.lineTo(px, Y(cl.mn[x]));
        if (cl.mx[x] !== cl.mn[x]) g.lineTo(px, Y(cl.mx[x]));
      }
      g.stroke();
    }
  }
