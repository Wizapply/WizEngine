// Charon web UI: テレインの 1 回きりの道具（Inspector の secTerrain 節のノイズで生成・
// 取り消し・自動塗り・ハイトマップの取り込みと書き出し）。app.js と terrain.js の
// 後ろに読む古典スクリプトで、app.js（send / round2 / sceneData / readFields /
// fillFields / restoreFields / storeJson / loadStoredJson）と terrain.js
// （flushTerrain / discardTerrain / currentTerrain / terrainTools / terrainLayerLabel）の
// トップレベルをそのまま使う。ここの関数もグローバルのままにする（index.html の
// onclick= と terrain.js の renderTerrainSection などが名前で呼ぶため）。
// 取り消し（undoTerrain）は secTerrain 節のほか、index.html の #terrainBar の ↶
// からも呼ばれる。

  // ---- ノイズで生成と取り消し（secTerrain 節）-------------------------------
  // 値は送るときだけ使い、テレインには覚えない（サーバーは送られなかった
  // キーを既定値にし、整数の丸めもする）。次に開いたときのために、この
  // ブラウザの localStorage に覚えておく。欄が空ならそのキーは送らない。
  const TERRAIN_NOISE_FIELDS = {
    seed: 'edTerrainSeed', octaves: 'edTerrainOct', gain: 'edTerrainGain',
    lacunarity: 'edTerrainLac', scale: 'edTerrainScale'
  };
  const TERRAIN_NOISE_STORE = 'terrain:noise';
  function generateTerrainNoise() {
    const g = readFields(TERRAIN_NOISE_FIELDS);
    storeJson(TERRAIN_NOISE_STORE, g);
    // 確定待ちの形（全幅など）も同じ 1 通で送る（サーバーが先に当ててから生成）。
    flushTerrain({ generate: g });
  }
  // 確定待ちの欄は捨ててから取り消す: 取り消すのはビューに見えている直前の
  // 編集で、まだ送っていない値はビューに出ていない。先に送ると形の変更が
  // 取り消しの記録を消すので、このボタンが何もできなくなる。
  function undoTerrain() {
    discardTerrain();
    send('edit.terrain', { undo: true });
  }
  // 読めなければ空 = 既定値のまま。
  restoreFields(TERRAIN_NOISE_STORE, TERRAIN_NOISE_FIELDS);

  // ---- 自動塗り（secTerrain 節）---------------------------------------------
  // ノイズで生成と同じく、値は送るときだけ使い（テレインには覚えない）、この
  // ブラウザの localStorage に覚える（欄・チェックを変えたその時に。押さずに
  // リロードしても消えない）。既定の帯はここに持たない: 行の数・薄い
  // 数字（placeholder）・チェックの初期値は、サーバーの terrainTools.autoPaintDefaults
  // から取る。空の欄のキーは送らない = サーバーの既定の帯。
  // 層ごとの欄（キー → id の頭。id は頭 + 層の番号）。
  const TERRAIN_AUTOPAINT_FIELDS = {
    slopeMin: 'edTerrainAutoSlopeMin', slopeMax: 'edTerrainAutoSlopeMax',
    heightMin: 'edTerrainAutoHeightMin', heightMax: 'edTerrainAutoHeightMax'
  };
  const TERRAIN_AUTOPAINT_SOFT = { softness: 'edTerrainAutoSoft' };
  const TERRAIN_AUTOPAINT_STORE = 'terrain:autopaint';
  function terrainAutoPaintDefaults() { return terrainTools().autoPaintDefaults; }
  // 層 i の欄の表（readFields / fillFields に渡す形）。
  function autoPaintRowFields(i) {
    const map = {};
    for (const key in TERRAIN_AUTOPAINT_FIELDS) map[key] = TERRAIN_AUTOPAINT_FIELDS[key] + i;
    return map;
  }
  // 層の行（index.html の #edTerrainAutoRows の中身）を既定の帯 defaults
  // （{softness, layers:[...]}）の層の数だけ作る。作るのは初回と数が変わったとき
  // だけで、欄にフォーカスがある間は作り直さない（層の行と同じ）。欄の値は覚えた
  // もの、チェックは覚えたもの（無ければ既定）。
  const TERRAIN_AUTOPAINT_TIP = {
    slopeMin: '傾きの下限（度、0 = 水平）', slopeMax: '傾きの上限（度、90 = 垂直）',
    heightMin: '高さの下限（テレインの位置 Y からの m）',
    heightMax: '高さの上限（テレインの位置 Y からの m）'
  };
  function buildTerrainAutoPaintRows(defaults) {
    const box = document.getElementById('edTerrainAutoRows');
    const layers = (defaults && defaults.layers) || [];
    if (!box || box.childElementCount === layers.length) return;
    if (box.contains(document.activeElement)) return;
    const saved = loadStoredJson(TERRAIN_AUTOPAINT_STORE) || {};
    const savedLayers = Array.isArray(saved.layers) ? saved.layers : [];
    box.textContent = '';
    layers.forEach((d, i) => {
      const name = terrainLayerLabel(i);
      const row = document.createElement('div');
      row.className = 'autoRow';
      const label = document.createElement('label');
      label.title = name + ' を塗るか（外すと、この層は自動塗りで使わない）';
      const on = document.createElement('input');
      on.type = 'checkbox';
      on.id = 'edTerrainAutoOn' + i;
      const s = savedLayers[i] || {};
      on.checked = typeof s.enabled === 'boolean' ? s.enabled : !!d.enabled;
      // 外した層の行は欄を薄くする（欄は無効にしない = 値は打てる）。
      row.classList.toggle('off', !on.checked);
      on.onchange = () => {
        row.classList.toggle('off', !on.checked);
        storeTerrainAutoPaint();
      };
      label.append(on, ' ' + name);
      row.appendChild(label);
      const map = autoPaintRowFields(i);
      for (const key in map) {
        const input = document.createElement('input');
        input.id = map[key];
        input.className = 'num';
        input.type = 'number';
        input.step = key.startsWith('slope') ? '5' : '1';
        // 範囲（min / max）は持たない: サーバーが丸める（1 か所）。
        input.placeholder = Number.isFinite(d[key]) ? String(round2(d[key])) : '';
        input.title = name + ' の' + TERRAIN_AUTOPAINT_TIP[key] + '。空 = 薄い数字（既定）';
        input.onchange = () => storeTerrainAutoPaint();
        row.appendChild(input);
      }
      box.appendChild(row);
      fillFields(s, map);
    });
    const soft = document.getElementById(TERRAIN_AUTOPAINT_SOFT.softness);
    if (soft && defaults && Number.isFinite(defaults.softness))
      soft.placeholder = String(round2(defaults.softness));
  }
  // 欄の値（送る形 = {softness?, layers:[{enabled, slopeMin?, ...}]}）。
  function readTerrainAutoPaint() {
    const rules = readFields(TERRAIN_AUTOPAINT_SOFT);
    // 欄が無ければ層は 0 個（storeTerrainAutoPaint は覚えずに抜ける）。
    const box = document.getElementById('edTerrainAutoRows');
    const count = box ? box.childElementCount : 0;
    rules.layers = [];
    for (let i = 0; i < count; i++) {
      const band = readFields(autoPaintRowFields(i));
      band.enabled = !!document.getElementById('edTerrainAutoOn' + i).checked;
      rules.layers.push(band);
    }
    return rules;
  }
  // 欄・チェックの確定（onchange）で覚える。rules = 読んだばかりの値（省けば
  // 欄から読む）。行がまだ無い（層の数が届く前）ときは覚えない（空の layers で
  // 覚えた値を消さない）。
  function storeTerrainAutoPaint(rules) {
    const r = rules || readTerrainAutoPaint();
    if (r.layers.length === 0) return;
    storeJson(TERRAIN_AUTOPAINT_STORE, r);
  }
  function autoPaintTerrain() {
    const rules = readTerrainAutoPaint();
    storeTerrainAutoPaint(rules);
    // 確定待ちの形（全幅など）も同じ 1 通で送る（サーバーが先に当ててから塗る）。
    flushTerrain({ autopaint: rules });
  }
  restoreFields(TERRAIN_AUTOPAINT_STORE, TERRAIN_AUTOPAINT_SOFT);

  // ---- ハイトマップの取り込みと書き出し（secTerrain 節）----------------------
  // サーバーの import / export は部分更新を当てない（ファイルの分岐で終わる）ので、
  // 確定待ちの形（全幅など）は先に 1 通で送り、届いてから次を送る（取り込みは
  // その形に合わせ、書き出しはその格子を書く）。ファイル名と上下反転は
  // このブラウザに記憶する。
  const TERRAIN_HEIGHTMAP_STORE = 'terrain:heightmap';
  function sendAfterTerrainPatch(args) {
    Promise.resolve(flushTerrain()).then(() => send('edit.terrain', args));
  }
  function importTerrainHeightmap() {
    const fileEl = document.getElementById('edTerrainHmFile');
    const flipEl = document.getElementById('edTerrainHmFlip');
    const file = fileEl.value.trim();
    const flip = !!flipEl.checked;
    // 空なら覚えも送りもしない（覚えていたパスを空で上書きしない）。
    if (!file) { fileEl.focus(); return; }
    storeJson(TERRAIN_HEIGHTMAP_STORE, { file: file, flip: flip });
    sendAfterTerrainPatch({ import: { file: file, flip: flip } });
  }
  // 書き出し先が既にあれば（/scene の heightmapFiles）上書きを確かめ、OK なら
  // overwrite を付ける（サーバーは overwrite の無い上書きを断る）。確かめた
  // テレインの名前も添える（その間に開き替え・改名されたらサーバーが断る）。
  function exportTerrainHeightmap(fmt) {
    const name = currentTerrain().name;
    const exists = !!((sceneData && sceneData.heightmapFiles) || {})[fmt];
    if (exists && !confirm(name + '.' + fmt + ' は既にあります。上書きしますか？')) return;
    sendAfterTerrainPatch({ export: fmt, terrain: name, overwrite: exists });
  }
  (function restoreTerrainHeightmap() {
    const saved = loadStoredJson(TERRAIN_HEIGHTMAP_STORE);
    if (!saved) return;
    const fileEl = document.getElementById('edTerrainHmFile');
    const flipEl = document.getElementById('edTerrainHmFlip');
    if (fileEl && typeof saved.file === 'string') fileEl.value = saved.file;
    if (flipEl) flipEl.checked = saved.flip === true;
  })();
