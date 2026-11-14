// Charon web UI: 制御器（ブロック線図）とスコープ。app.js の後ろに読む古典
// スクリプトで、そのトップレベル（send / sceneData / isEditorCam / ndEsc /
// formulaAssets / openFormulaAsset / cleanAssetName / storeJson /
// loadStoredJson）をそのまま使う。ここの関数もグローバルのまま（index.html の
// onclick= と app.js が typeof で確かめてから呼ぶ）。
//
// サーバー側の約束:
//   /scene の formulas.controllers … [{formula, period, enabled}]（文書の値）
//   /scene の formulas.sensors / actuators … 量の名前（候補を組む材料）
//   /scene の control … 実行状態 {running, run, seq, controllers:[{formula, ok,
//                       errors, warnings, inputs, outputs, execs, failures, last}]}
//   /control?since=N  … 記録の差分 {run, seq, series:[{formula, names, rows}]}
//                       rows の 1 行 = [t, 入力..., 出力...]
//   edit.control.add / remove / set、edit.formula.add {template, target}、
//   edit.formula.text {name, text}（数式のテキスト → グラフ）

  // ---- 一覧（World タブの「制御器」節）--------------------------------------
  function controllers() {
    return (sceneData && sceneData.formulas && sceneData.formulas.controllers) || [];
  }
  function controlRun() {
    return (sceneData && sceneData.control) || {};
  }
  function controlParams() {
    return (sceneData && sceneData.formulas && sceneData.formulas.params) || [];
  }
  function isControllerFormula(name) {
    return controllers().some((c) => c.formula === name);
  }
  function controlRunOf(name) {
    return (controlRun().controllers || []).find((c) => c.formula === name) || null;
  }

  // in / out ノードの名前の候補。シーンのオブジェクト・ジョイントの名前と
  // サーバーが渡す量の名前から組む（名前の無い物は信号で指せない）。
  function controlSignalSuggestions(isIn) {
    if (!sceneData || !sceneData.formulas) return [];
    const q = (isIn ? sceneData.formulas.sensors : sceneData.formulas.actuators) || {};
    const out = isIn ? ['time'] : [];
    const objs = (sceneData.objects || []).filter((o) => o.name);
    for (const o of objs) {
      for (const k of q.body || []) out.push('body:' + o.name + '.' + k);
      if (o.vehicle) {
        for (const k of q.vehicle || []) out.push('vehicle:' + o.name + '.' + k);
      }
    }
    for (const j of sceneData.joints || []) {
      if (!j.name) continue;
      for (const k of q.joint || []) out.push('joint:' + j.name + '.' + k);
    }
    if (isIn) {
      for (const p of controlParams()) out.push('param:' + p.name);
    }
    if (!isIn) out.push('scope:');
    return out;
  }

  // PID の雛形を向けるジョイント: モータ付きの最初のもの、無ければ最初のもの。
  function defaultControlJoint() {
    const joints = ((sceneData && sceneData.joints) || []).filter((j) => j.name);
    const motor = joints.find((j) => j.motor && j.motor !== 'none');
    return motor ? motor.name : (joints.length ? joints[0].name : '');
  }

  function controlAdd(name, period) {
    if (!name) return;
    send('edit.control.add', { formula: name, period: period || 0 });
  }
  function controlAddFrom(selectId) {
    const el = document.getElementById(selectId);
    if (el && el.value) controlAdd(el.value, 0);
  }
  function controlRemove(name) {
    send('edit.control.remove', { formula: name });
  }
  function controlSet(name, patch) {
    send('edit.control.set', Object.assign({ formula: name }, patch));
  }
  // 雛形が指す相手: ジョイントならモータ付きの最初のもの、物なら選択中の物
  // （無ければ名前のある最初の固定でない物）。
  function templateTarget(needs) {
    if (needs === 'joint') return defaultControlJoint();
    if (needs === 'body') {
      const sel = typeof mySelectedDesc === 'function' ? mySelectedDesc() : null;
      if (sel && sel.name) return sel.name;
      const o = (sceneData.objects || []).find((x) => x.name && !x.fixed);
      return o ? o.name : '';
    }
    return '';
  }

  // ---- 数式エディタ（#fxEd）-------------------------------------------------
  // 書いた式は edit.formula.text でサーバーへ。サーバーが検証してノードの
  // グラフに起こす（formula/FormulaText.h）。壊れていれば行番号付きの理由が
  // ステータスに出て、何も変わらない。
  let fxOpen = false;
  let fxStatusSeq = -1;
  function openFormulaText(name, isNew) {
    const a = formulaAssets().find((x) => x.name === name);
    document.getElementById('fxName').value = name || 'formula';
    document.getElementById('fxText').value = a && !isNew ? (a.text || '') : '';
    const sel = document.getElementById('fxTemplate');
    sel.innerHTML = '<option value="">雛形を差し込む…</option>' +
      formulaTemplates().filter((t) => t.text).map((t) =>
        '<option value="' + ndEsc(t.id) + '">' + ndEsc(t.label) + '</option>').join('');
    document.getElementById('fxStatus').textContent = '';
    fxStatusSeq = sceneData ? sceneData.statusSeq : -1;
    document.getElementById('fxEd').hidden = false;
    fxOpen = true;
    document.getElementById('fxText').focus();
  }
  function closeFormulaText() {
    document.getElementById('fxEd').hidden = true;
    fxOpen = false;
  }
  function applyFormulaText() {
    const name = cleanAssetName(document.getElementById('fxName').value || '');
    if (!name) return;
    document.getElementById('fxName').value = name;
    fxStatusSeq = sceneData ? sceneData.statusSeq : -1;
    document.getElementById('fxStatus').textContent = '送信中…';
    send('edit.formula.text', { name: name, text: document.getElementById('fxText').value });
  }
  function insertFormulaTemplate(id) {
    const sel = document.getElementById('fxTemplate');
    const t = formulaTemplates().find((x) => x.id === id);
    sel.value = '';
    if (!t) return;
    const ta = document.getElementById('fxText');
    if (ta.value.trim() && !confirm('いまのテキストを雛形「' + t.label + '」で置き換えます。')) return;
    const who = templateTarget(t.needs) || (t.needs === 'joint' ? 'hinge' : 'box');
    ta.value = t.text.split('{joint}').join(who).split('{body}').join(who);
  }
  // サーバーのステータス（適用の結果・式のエラー）を窓の中にも出す。
  function renderFormulaTextStatus() {
    if (!fxOpen || !sceneData) return;
    if (sceneData.statusSeq !== fxStatusSeq) {
      document.getElementById('fxStatus').textContent = sceneData.status || '';
    }
  }

  // 🧮 タイルの右クリックメニューに足す項目（app.js が差し込む）。
  function controlMenuItems(fname) {
    const here = isEditorCam();
    const items = [];
    if (isControllerFormula(fname)) {
      items.push({ label: '■ 制御器から外す', hint: '計算式は残る', disabled: !here,
                   onClick: () => controlRemove(fname) });
    } else {
      items.push({ label: '▶ 制御器にする', hint: 'シミュレート中に回す', disabled: !here,
                   onClick: () => controlAdd(fname, 0) });
    }
    items.push({ label: '📈 スコープで見る', onClick: () => openScope(fname) });
    return items;
  }

  let ctlListKey = '';
  function renderControl() {
    if (!sceneData) return;
    const here = isEditorCam();
    const list = document.getElementById('ctlList');
    const pick = document.getElementById('ctlPick');
    if (!list || !pick) return;
    document.getElementById('ctlAddRow').hidden = !here;
    document.getElementById('ctlNewFormula').hidden = !here;

    const ctl = controllers();
    const run = controlRun();
    const key = JSON.stringify([ctl, (run.controllers || []).map((c) =>
      [c.formula, c.ok, c.errors, c.warnings, c.execs, c.failures]), here,
      formulaAssets().map((f) => f.name)]);
    // 打っている途中の欄（周期）は上書きしない。
    const ae = document.activeElement;
    if (key === ctlListKey || (list.contains(ae) && ae.type === 'number') || ae === pick) {
      return;
    }
    ctlListKey = key;

    const dis = here ? '' : ' disabled';
    list.innerHTML = ctl.length ? ctl.map((c) => {
      const r = controlRunOf(c.formula);
      const n = ndEsc(c.formula);
      let state = '<span class="sub">停止中</span>';
      if (r) {
        state = r.ok
          ? '<span class="sub" title="評価回数（このシミュレート）">' +
            (r.execs || 0) + ' 回' + (r.failures ? ' / 失敗 ' + r.failures : '') + '</span>'
          : '<span class="sub err">エラー</span>';
      }
      // 1 行目: 有効・名前・スコープ・外す。2 行目: 周期と実行状態
      // （サイドバーは狭いので 1 行に詰めると名前が折り返す）。
      let h = '<div class="ndItem ctlItem" data-formula="' + n + '">' +
        '<div class="ctlTop">' +
        '<input type="checkbox" title="有効（外すと止まる。出力も掛けない）"' +
        (c.enabled ? ' checked' : '') + dis +
        ' onchange="controlSet(\'' + n + '\',{enabled:this.checked})">' +
        '<span class="link" title="ノードエディタで開く" onclick="openFormulaAsset(\'' + n +
        '\')">🧮 ' + n + '</span>' +
        '<span class="x" title="スコープ" onclick="openScope(\'' + n + '\')">📈</span>' +
        (here ? '<span class="x" title="制御器から外す（計算式は残る）" onclick="controlRemove(\'' +
                n + '\')">✕</span>' : '') +
        '</div><div class="ctlSub">周期 ' +
        '<input class="num" type="number" min="0" max="10" step="0.001" value="' +
        (c.period || 0) + '" title="実行周期 (s)。0 = 物理の毎ステップ"' + dis +
        ' onchange="controlSet(\'' + n + '\',{period:parseFloat(this.value)||0})"> s' +
        state + '</div></div>';
      if (r && r.errors && r.errors.length) {
        h += '<div class="ctlMsgs err">' + r.errors.map(ndEsc).join('\n') + '</div>';
      }
      if (r && r.warnings && r.warnings.length) {
        h += '<div class="ctlMsgs">' + r.warnings.map(ndEsc).join('\n') + '</div>';
      }
      return h;
    }).join('') : '<div class="edHint">制御器はありません。🧮 計算式を「制御器にする」か、' +
                  '「＋ 新しい計算式」で雛形から作ります。</div>';

    const free = formulaAssets().filter((f) => !isControllerFormula(f.name));
    pick.innerHTML = free.length
      ? free.map((f) => '<option value="' + ndEsc(f.name) + '">🧮 ' + ndEsc(f.name) +
                        '</option>').join('')
      : '<option value="">（計算式がありません）</option>';
  }
  setInterval(() => { renderControl(); renderParams(); renderFormulaTextStatus(); }, 500);

  // ---- パラメータ（式からは param:<名前>）----------------------------------
  // スライダーの値はシミュレート中でもすぐ効く（サーバーは式を作り直さず、
  // 次の物理ステップから新しい値を読む）。値の変更はどのカメラのページから
  // でもでき、追加・削除・範囲の変更はエディタカメラのページだけ。
  let paramListKey = '';
  const paramSendAt = {};
  const paramPending = {};
  function paramIsSwitch(p) {
    return p.min === 0 && p.max === 1 && p.step === 1;
  }
  function paramStep(p) {
    if (p.step > 0) return p.step;
    const span = (p.max - p.min) || 1;
    const raw = span / 200;
    const k = Math.pow(10, Math.floor(Math.log10(raw)));
    return [1, 2, 5, 10].map((m) => m * k).find((v) => v >= raw) || raw;
  }
  // 表示の桁はスライダーの刻みに合わせる（0〜1 なら 0.005 刻みで 3 桁、0〜7000 なら整数）。
  function fmtParam(v, name) {
    const p = controlParams().find((x) => x.name === name);
    const step = p ? paramStep(p) : 0.001;
    return v.toFixed(Math.max(0, Math.min(6, -Math.floor(Math.log10(step)))));
  }
  function renderParams() {
    if (!sceneData) return;
    const list = document.getElementById('paramList');
    if (!list) return;
    const here = isEditorCam();
    document.getElementById('paramAddRow').hidden = !here;
    const params = controlParams();
    const key = JSON.stringify([params, here]);
    // ドラッグ中・入力中の欄は上書きしない（入力欄の「フォーカス中は触らない」と同じ）。
    if (key === paramListKey || list.contains(document.activeElement)) return;
    paramListKey = key;
    const dis = here ? '' : ' disabled';
    list.innerHTML = params.length ? params.map((p) => {
      const n = ndEsc(p.name);
      const title = p.label ? '<span class="paramLabel">' + ndEsc(p.label) + '</span> ' : '';
      if (paramIsSwitch(p)) {
        return '<div class="ndItem ctlItem paramItem" data-param="' + n + '">' +
          '<div class="ctlTop"><label class="paramSwitch"><input type="checkbox"' +
          (p.value >= 0.5 ? ' checked' : '') +
          ' onchange="paramInput(\'' + n + '\', this.checked ? 1 : 0, true)"> ' + title +
          '<code title="式からは param:' + n + ' で読む（オン = 1 / オフ = 0）">param:' + n + '</code></label>' +
          (here ? '<span class="x" title="パラメータを削除" onclick="paramRemove(\'' + n +
                  '\')">✕</span>' : '') + '</div></div>';
      }
      return '<div class="ndItem ctlItem paramItem" data-param="' + n + '">' +
        '<div class="ctlTop">' + title + '<code title="式からは param:' + n + ' で読む">param:' + n + '</code>' +
        '<span class="paramVal" id="pv_' + n + '" title="クリックで値を直接入力"' +
        ' onclick="paramPrompt(\'' + n + '\')">' + fmtParam(p.value, p.name) + '</span>' +
        (here ? '<span class="x" title="パラメータを削除" onclick="paramRemove(\'' + n +
                '\')">✕</span>' : '') + '</div>' +
        '<input type="range" class="paramSlider" min="' + p.min + '" max="' + p.max +
        '" step="' + paramStep(p) + '" value="' + p.value + '"' +
        ' oninput="paramInput(\'' + n + '\', this.value)"' +
        ' onchange="paramInput(\'' + n + '\', this.value, true)">' +
        '<div class="ctlSub">範囲 <input class="num" type="number" value="' + p.min + '"' + dis +
        ' title="最小" onchange="paramSet(\'' + n + '\',{min:parseFloat(this.value)})"> 〜 ' +
        '<input class="num" type="number" value="' + p.max + '"' + dis +
        ' title="最大" onchange="paramSet(\'' + n + '\',{max:parseFloat(this.value)})">' +
        '</div></div>';
    }).join('') : '<div class="edHint">パラメータはありません。式から <code>param:名前</code> で' +
                  '読む値を「＋ パラメータ」で足すと、ここのスライダーで走らせながら変えられます。</div>';
  }
  // スライダーの連続入力は 40 ms に 1 回だけ送り、最後の値は必ず送る。
  function paramInput(name, value, final) {
    const v = parseFloat(value);
    if (!isFinite(v)) return;
    const label = document.getElementById('pv_' + name);
    if (label) label.textContent = fmtParam(v, name);
    const now = performance.now();
    if (final || !paramSendAt[name] || now - paramSendAt[name] > 40) {
      paramSendAt[name] = now;
      clearTimeout(paramPending[name]);
      send('edit.param.set', { name: name, value: v });
    } else {
      clearTimeout(paramPending[name]);
      paramPending[name] = setTimeout(() => paramInput(name, v, true), 45);
    }
  }
  function paramPrompt(name) {
    const p = controlParams().find((x) => x.name === name);
    if (!p) return;
    const v = parseFloat(prompt('param:' + name + ' の値（' + p.min + ' 〜 ' + p.max + '）', String(p.value)));
    if (isFinite(v)) paramInput(name, v, true);
  }
  function paramSet(name, patch) {
    for (const k of Object.keys(patch)) {
      if (!isFinite(patch[k])) return;
    }
    send('edit.param.set', Object.assign({ name: name }, patch));
  }
  function paramRemove(name) {
    if (!confirm('パラメータ「' + name + '」を削除します。式の param:' + name + ' は 0 を読むようになります。')) return;
    send('edit.param.remove', { name: name });
  }
  function paramAdd() {
    const name = prompt('パラメータの名前（英数字と _。式からは param:名前 で読む）', 'gain');
    if (!name) return;
    const min = parseFloat(prompt('最小値', '0'));
    const max = parseFloat(prompt('最大値', '1'));
    if (!isFinite(min) || !isFinite(max)) return;
    send('edit.param.add', { name: name.trim(), min: min, max: max, value: min });
  }

  // ---- スコープ -------------------------------------------------------------
  // 記録はサーバーが持ち（制御器ごとに約 5 万行）、ここは since 以降の差分を
  // 積むだけ。表示は requestAnimationFrame で、データが来たときだけ描く。
  const SCOPE_COLORS = ['#5aa9ff', '#f2a65a', '#6fd08c', '#e86f8a', '#b48cf2',
                        '#e8d36a', '#5fd4d4', '#d98f5f', '#9aa4b5', '#f07fd2'];
  const SCOPE_MAX_POINTS = 60000;  // 1 制御器あたりブラウザに持つ行数
  const SCOPE_HIDDEN_KEY = 'wiz.scope.hidden';
  const scope = {
    open: false, since: 0, run: -1, series: {}, current: '', windowSec: 10,
    paused: false, timer: null, dirty: false, hidden: {},
  };
  {
    const saved = loadStoredJson(SCOPE_HIDDEN_KEY);
    if (saved && typeof saved === 'object') scope.hidden = saved;
  }

  function openScope(formula) {
    const win = document.getElementById('scopeWin');
    win.hidden = false;
    scope.open = true;
    if (formula) scope.current = formula;
    if (!scope.timer) scope.timer = setInterval(scopePoll, 200);
    scopePoll();
    scope.dirty = true;
  }
  function closeScope() {
    document.getElementById('scopeWin').hidden = true;
    scope.open = false;
    if (scope.timer) clearInterval(scope.timer);
    scope.timer = null;
  }
  function scopeSelect(name) {
    scope.current = name;
    scope.dirty = true;
    scopeRenderHead();
  }
  function scopeSetWindow(v) {
    scope.windowSec = parseFloat(v) || 0;
    scope.dirty = true;
  }
  function scopeTogglePause() {
    scope.paused = !scope.paused;
    document.getElementById('scPause').textContent = scope.paused ? '再開' : '停止';
    scope.dirty = true;
  }

  async function scopePoll() {
    if (!scope.open) return;
    try {
      const r = await fetch('control?since=' + scope.since);
      if (!r.ok) return;
      const d = await r.json();
      if (d.run !== scope.run) {
        // シミュレートのやり直し（開始・Reset）: 前の記録は捨てる。
        scope.run = d.run;
        scope.series = {};
      }
      for (const s of d.series || []) {
        let cur = scope.series[s.formula];
        if (!cur || JSON.stringify(cur.names) !== JSON.stringify(s.names)) {
          cur = scope.series[s.formula] = { names: s.names, rows: [] };
        }
        for (const row of s.rows) cur.rows.push(row);
        if (cur.rows.length > SCOPE_MAX_POINTS) {
          cur.rows.splice(0, cur.rows.length - SCOPE_MAX_POINTS);
        }
        if (s.rows.length) scope.dirty = true;
      }
      // 制御器が外れた記録は消す（サーバーの並びが正）。
      const names = (d.series || []).map((s) => s.formula);
      for (const k of Object.keys(scope.series)) {
        if (!names.includes(k)) { delete scope.series[k]; scope.dirty = true; }
      }
      scope.since = d.seq || scope.since;
      if (!scope.current || !scope.series[scope.current]) {
        scope.current = names.length ? names[0] : '';
      }
      scopeRenderHead();
    } catch (e) { /* サーバーが忙しい / 落ちた: 次のティックで */ }
  }

  let scopeHeadKey = '';
  function scopeRenderHead() {
    const sel = document.getElementById('scSeries');
    const names = Object.keys(scope.series);
    const s = scope.series[scope.current];
    const key = names.join(',') + '|' + scope.current + '|' + (s ? s.names.join(',') : '');
    if (key === scopeHeadKey) return;
    scopeHeadKey = key;
    sel.innerHTML = names.length
      ? names.map((n) => '<option value="' + ndEsc(n) + '"' + (n === scope.current ? ' selected' : '') +
                         '>' + ndEsc(n) + '</option>').join('')
      : '<option value="">（記録なし: 制御器を付けてシミュレート）</option>';
    document.getElementById('scCsv').href =
      'control.csv?formula=' + encodeURIComponent(scope.current || '');
    const legend = document.getElementById('scLegend');
    if (!s) { legend.innerHTML = ''; return; }
    legend.innerHTML = s.names.map((n, i) => {
      const id = scope.current + '/' + n;
      return '<label title="' + ndEsc(n) + '"><input type="checkbox" data-sig="' + ndEsc(id) + '"' +
        (scope.hidden[id] ? '' : ' checked') + ' onchange="scopeToggleSignal(this.dataset.sig, this.checked)">' +
        '<span class="sw" style="background:' + SCOPE_COLORS[i % SCOPE_COLORS.length] + '"></span>' +
        ndEsc(scopeShortName(n)) + ' <span class="val" data-val="' + i + '"></span></label>';
    }).join('');
    scope.dirty = true;
  }
  // "joint:arm.angle" → "arm.angle"、"scope:u" → "u"（凡例を短く）。
  function scopeShortName(n) {
    const c = n.indexOf(':');
    return c >= 0 ? n.slice(c + 1) : n;
  }
  function scopeToggleSignal(id, on) {
    if (on) delete scope.hidden[id];
    else scope.hidden[id] = true;
    storeJson(SCOPE_HIDDEN_KEY, scope.hidden);
    scope.dirty = true;
  }

  function niceStep(span, count) {
    const raw = span / Math.max(count, 1);
    const p = Math.pow(10, Math.floor(Math.log10(raw)));
    const m = raw / p;
    return (m < 1.5 ? 1 : m < 3.5 ? 2 : m < 7.5 ? 5 : 10) * p;
  }
  function fmtNum(v) {
    const a = Math.abs(v);
    if (a !== 0 && (a < 1e-3 || a >= 1e5)) return v.toExponential(2);
    return String(+v.toFixed(4));
  }

  function scopeDraw() {
    if (scope.open && (scope.dirty && !scope.paused)) {
      scope.dirty = false;
      scopePaint();
    }
    requestAnimationFrame(scopeDraw);
  }

  requestAnimationFrame(scopeDraw);

  function scopePaint() {
    const cv = document.getElementById('scCanvas');
    const dpr = window.devicePixelRatio || 1;
    const w = cv.clientWidth, h = cv.clientHeight;
    if (w <= 0 || h <= 0) return;
    if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) {
      cv.width = Math.round(w * dpr);
      cv.height = Math.round(h * dpr);
    }
    const g = cv.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    const s = scope.series[scope.current];
    g.font = '11px system-ui, sans-serif';
    if (!s || !s.rows.length) {
      g.fillStyle = '#7b7f88';
      g.fillText('記録がありません（制御器を付けてシミュレートを開始）', 12, 22);
      return;
    }
    const rows = s.rows;
    const tEnd = rows[rows.length - 1][0];
    const tStart = scope.windowSec > 0 ? Math.max(rows[0][0], tEnd - scope.windowSec) : rows[0][0];
    // 表示範囲の先頭（二分探索）。
    let lo = 0, hi = rows.length - 1;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (rows[mid][0] < tStart) lo = mid + 1; else hi = mid;
    }
    const first = Math.max(0, lo - 1);
    const visible = [];
    for (let i = 0; i < s.names.length; ++i) {
      if (!scope.hidden[scope.current + '/' + s.names[i]]) visible.push(i);
    }
    let yMin = Infinity, yMax = -Infinity;
    for (let r = first; r < rows.length; ++r) {
      for (const i of visible) {
        const v = rows[r][i + 1];
        if (v < yMin) yMin = v;
        if (v > yMax) yMax = v;
      }
    }
    if (!isFinite(yMin)) { yMin = -1; yMax = 1; }
    if (yMax - yMin < 1e-9) { yMin -= 0.5; yMax += 0.5; }
    const pad = (yMax - yMin) * 0.06;
    yMin -= pad; yMax += pad;
    const L = 52, R = 8, T = 6, B = 18;
    const pw = w - L - R, ph = h - T - B;
    const span = Math.max(tEnd - tStart, 1e-6);
    const X = (t) => L + (t - tStart) / span * pw;
    const Y = (v) => T + (yMax - v) / (yMax - yMin) * ph;

    // 格子と目盛り
    g.strokeStyle = '#262b34';
    g.fillStyle = '#7b818c';
    g.lineWidth = 1;
    const ys = niceStep(yMax - yMin, Math.max(2, Math.floor(ph / 40)));
    g.textAlign = 'right';
    for (let v = Math.ceil(yMin / ys) * ys; v <= yMax; v += ys) {
      const y = Math.round(Y(v)) + 0.5;
      g.beginPath(); g.moveTo(L, y); g.lineTo(w - R, y); g.stroke();
      g.fillText(fmtNum(v), L - 4, y + 4);
    }
    const ts = niceStep(span, Math.max(2, Math.floor(pw / 80)));
    g.textAlign = 'center';
    for (let t = Math.ceil(tStart / ts) * ts; t <= tEnd; t += ts) {
      const x = Math.round(X(t)) + 0.5;
      g.beginPath(); g.moveTo(x, T); g.lineTo(x, T + ph); g.stroke();
      g.fillText(fmtNum(t) + ' s', x, h - 4);
    }
    if (yMin < 0 && yMax > 0) {
      g.strokeStyle = '#3a414e';
      const y0 = Math.round(Y(0)) + 0.5;
      g.beginPath(); g.moveTo(L, y0); g.lineTo(w - R, y0); g.stroke();
    }

    // 線（横 1 ピクセルに何点も来るときは min/max だけ描く = 速くて形が残る）
    g.save();
    g.beginPath(); g.rect(L, T, pw, ph); g.clip();
    g.lineWidth = 1.5;
    for (const i of visible) {
      g.strokeStyle = SCOPE_COLORS[i % SCOPE_COLORS.length];
      g.beginPath();
      let px = -1, mn = 0, mx = 0, started = false, lastY = 0;
      for (let r = first; r < rows.length; ++r) {
        const x = Math.round(X(rows[r][0]));
        const y = Y(rows[r][i + 1]);
        if (x !== px) {
          if (started && mn !== mx) { g.lineTo(px, mn); g.lineTo(px, mx); g.lineTo(px, lastY); }
          if (!started) { g.moveTo(x, y); started = true; } else g.lineTo(x, y);
          px = x; mn = mx = y;
        } else {
          if (y < mn) mn = y;
          if (y > mx) mx = y;
        }
        lastY = y;
      }
      g.stroke();
    }
    g.restore();

    // 凡例に最新値
    const last = rows[rows.length - 1];
    for (const el of document.querySelectorAll('#scLegend .val')) {
      const i = +el.dataset.val;
      el.textContent = fmtNum(last[i + 1]);
    }
  }

  // 見出しのドラッグで窓を動かす（映像の中に収める）。
  (function scopeDrag() {
    const head = document.getElementById('scHead');
    const win = document.getElementById('scopeWin');
    if (!head || !win) return;
    let sx = 0, sy = 0, ox = 0, oy = 0, drag = false;
    head.addEventListener('pointerdown', (e) => {
      if (e.target.closest('button, select, a, input')) return;
      drag = true;
      sx = e.clientX; sy = e.clientY;
      const r = win.getBoundingClientRect();
      const pr = win.offsetParent.getBoundingClientRect();
      ox = r.left - pr.left; oy = r.top - pr.top;
      head.setPointerCapture(e.pointerId);
      e.preventDefault();
    });
    head.addEventListener('pointermove', (e) => {
      if (!drag) return;
      const pr = win.offsetParent.getBoundingClientRect();
      const x = Math.max(0, Math.min(pr.width - 60, ox + e.clientX - sx));
      const y = Math.max(0, Math.min(pr.height - 30, oy + e.clientY - sy));
      win.style.left = x + 'px'; win.style.top = y + 'px';
      win.style.right = 'auto'; win.style.bottom = 'auto';
    });
    head.addEventListener('pointerup', () => { drag = false; });
    // 大きさを変えたら描き直す。
    if (window.ResizeObserver) new ResizeObserver(() => { scope.dirty = true; }).observe(win);
  })();
