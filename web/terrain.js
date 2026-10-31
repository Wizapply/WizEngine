// Charon web UI: テレイン欄（Inspector の secTerrain 節 = 選択したテレイン、XML の <terrain>）。app.js の後ろに読む
// 古典スクリプトで、app.js のトップレベル（send / num / setField / round2 /
// sceneData など）をそのまま使う。ここの関数もグローバルのままにする
// （index.html の onchange= と app.js の renderEditor などが名前で呼ぶため）。
// 層の欄は terrain-layers.js、1 回きりの道具は terrain-tools.js、ファイルの操作は
// terrain-files.js、筆は terrain-brush.js（どれもこの後ろに読む）。
// 描き直し（renderTerrainSection）は後ろの renderTerrainLayers（terrain-layers.js）と
// buildTerrainAutoPaintRows / terrainAutoPaintDefaults（terrain-tools.js）を呼ぶ。

  // ---- テレイン（選択オブジェクトの secTerrain 節）---------------------------
  // テレインはシーンに 1 つのオブジェクト。Objects 一覧の Terrain 行で選ぶと
  // この節が出る（renderEditor が出し入れ）。追加はアセットパネルの Terrain
  // タイル（新規）か .terrain のタイル、削除は節の「シーンから外す」。
  // 地面と違い、変えたキーだけを送る（サーバーは部分更新）。形のキー
  // （全幅・解像度・高さ）は 1 回で格子と全チャンクの作り直しが走るので、
  // 欄の change（確定時）だけで送り、さらにスピンボタンの連打を 1 回に
  // まとめる（短い間引き）。表面と置き場所（位置・回転 = フレームの差し替え
  // だけ）のキーは軽いのですぐ送る（溜まっている形のキーも一緒に）。
  // ノイズの値はここに混ぜない（「ノイズで生成」のボタンが generate で送る）。
  const TERRAIN_SHAPE_KEYS = ['size', 'resolution', 'height'];
  const TERRAIN_SHAPE_DELAY_MS = 300;
  let terrainPatch = {};
  let terrainTimer = null;
  // 送信待ちを送る。extra は同じ 1 通に足すキー（ノイズで生成の generate =
  // サーバーが部分更新を先に当てる。2 通に分けると届く順番が入れ替わりうる。
  // 応答を待ってから次を送れば順番は保たれる = import / export はこちら）。
  // 送ったときはその Promise（送るものが無ければ undefined）。
  function flushTerrain(extra, keepalive) {
    if (terrainTimer) { clearTimeout(terrainTimer); terrainTimer = null; }
    const patch = Object.assign(terrainPatch, extra || {});
    terrainPatch = {};
    if (Object.keys(patch).length === 0) return undefined;
    return send('edit.terrain', patch, keepalive);
  }
  // 送信待ちの形のキーを捨てる。シーンの読込・XML 適用・全消しの前に呼ぶ:
  // 残すと、前のシーンで触った値が新しく読んだシーンのテレインに後から当たる。
  function discardTerrain() {
    if (terrainTimer) { clearTimeout(terrainTimer); terrainTimer = null; }
    terrainPatch = {};
  }
  // ページを離れるときは送信待ちを捨てずに送る（触った値が黙って消えない
  // ように）。通常の fetch は離脱で打ち切られうるので keepalive を付ける。
  window.addEventListener('pagehide', () => flushTerrain(null, true));
  // 今のテレインの見分け（data と guid）。層の欄の守り（terrain-layers.js）と筆の
  // 層の選択肢（terrain-brush.js）が「テレインが差し替わったか」をこれで見る。
  function terrainIdentity() {
    const t = (sceneData && sceneData.terrain) || {};
    return (t.data || '') + '|' + (t.guid || '');
  }
  // key = 送るキー。層の欄は terrain-layers.js の applyTerrainLayer が送る。
  function applyTerrain(key) {
    const t = (sceneData && sceneData.terrain) || {};
    const pick = (id) => document.getElementById(id);
    // 欄が空・数でないときは今の値（無ければ送らない）。
    let value;
    switch (key) {
      case 'size': value = num('edTerrainSize', t.size); break;
      case 'resolution': value = Math.round(num('edTerrainRes', t.resolution)); break;
      case 'height': value = num('edTerrainHeight', t.height); break;
      case 'pos': {
        const p = t.pos || {};
        value = { x: num('edTerrainPX', p.x), y: num('edTerrainPY', p.y),
                  z: num('edTerrainPZ', p.z) };
        if (!Number.isFinite(value.x) || !Number.isFinite(value.y) ||
            !Number.isFinite(value.z)) return;
        break;
      }
      case 'rot': {
        const r = t.rot || {};
        value = { x: num('edTerrainRX', r.x), y: num('edTerrainRY', r.y),
                  z: num('edTerrainRZ', r.z) };
        if (!Number.isFinite(value.x) || !Number.isFinite(value.y) ||
            !Number.isFinite(value.z)) return;
        break;
      }
      case 'color': value = pick('edTerrainColor').value; break;
      case 'roughness': value = num('edTerrainRough', t.roughness); break;
      case 'metallic': value = num('edTerrainMetal', t.metallic); break;
      default: return;
    }
    if (typeof value === 'number' && !Number.isFinite(value)) return;
    if (value === undefined) return;
    terrainPatch[key] = value;
    // 高さには「形を保つ」を同じ 1 通で付ける。読むのは高さの欄を変えた時点の
    // チェック（間引きの間に切り替えても、この変更の指示は変わらない）。サーバーは
    // テレインに覚えないので、高さを送らない通には付けない。
    if (key === 'height') {
      const keep = pick('edTerrainKeep');
      terrainPatch.keepShape = !!(keep && keep.checked);
    }
    if (TERRAIN_SHAPE_KEYS.includes(key)) {
      if (terrainTimer) clearTimeout(terrainTimer);
      terrainTimer = setTimeout(() => flushTerrain(), TERRAIN_SHAPE_DELAY_MS);
    } else {
      flushTerrain();
    }
  }
  // 層の表示名（i = 0 始まりの層の番号）。名前の付け方はサーバーが持つ
  // （/scene の terrain.layers[i].label = ステータスの文と同じ名前）。層の欄
  // （terrain-layers.js）・自動塗りの行（terrain-tools.js）・筆の層の選択
  // （terrain-brush.js）はここを通す。
  function terrainLayerLabel(i) {
    const l = ((sceneData && sceneData.terrain && sceneData.terrain.layers) || [])[i];
    return (l && l.label) || '';
  }
  // テレインの道具の既定（テレインごとに変わらない値。/scene のトップレベルの
  // terrainTools = {baseLayer, autoPaintDefaults}。無ければ空）。
  function terrainTools() {
    return (sceneData && sceneData.terrainTools) || {};
  }
  // 節の出し入れは renderEditor（選択が terrain のときだけ）。ここは中身。
  function renderTerrainSection() {
    if (!sceneData) return;
    const t = sceneData.terrain;
    const cur = currentTerrain();
    const label = document.getElementById('edTerrainLabel');
    if (label) {
      label.textContent = '⛰ ' + (cur.name || '(名前なし)') +
        (cur.dirty ? ' *（未保存）' : '') + (cur.missing ? ' ／ ファイルなし' : '');
    }
    // ファイルが無い・読めない（Missing）: サーバーの文を出し、欄をまとめて
    // 無効にする（fieldset 1 つ。子の disabled 属性は触らない）。Missing に
    // 変わる前に触った値の送信待ちも捨てる（サーバーは断り、残すと
    // terrainUnsaved が誤って確認を出す）。
    const missing = cur.missing;
    if (missing) discardTerrain();
    const note = document.getElementById('edTerrainMissing');
    if (note) {
      note.textContent = missing;
      note.hidden = !missing;
    }
    const fields = document.getElementById('edTerrainFields');
    if (fields) fields.disabled = !!missing;
    if (!t) return;
    // 送信待ちの欄は上書きしない（間引きの間にポーリングが来ても戻さない）。
    const set = (key, id, v) => { if (!(key in terrainPatch)) setField(id, v); };
    set('size', 'edTerrainSize', round2(t.size));
    set('resolution', 'edTerrainRes', t.resolution);
    set('height', 'edTerrainHeight', round2(t.height));
    const p = t.pos || {};
    set('pos', 'edTerrainPX', round2(p.x));
    set('pos', 'edTerrainPY', round2(p.y));
    set('pos', 'edTerrainPZ', round2(p.z));
    const r = t.rot || {};
    set('rot', 'edTerrainRX', round2(r.x));
    set('rot', 'edTerrainRY', round2(r.y));
    set('rot', 'edTerrainRZ', round2(r.z));
    // 塗り分けの層（.terrain の LAYR。サーバーの terrain.layers の順 = 層の番号）。
    renderTerrainLayers(t.layers || []);
    // 自動塗りの行（層の数と既定の帯はサーバーの値。欄の値はこのブラウザが持つ）。
    buildTerrainAutoPaintRows(terrainAutoPaintDefaults());
    if (t.color) set('color', 'edTerrainColor', t.color);
    set('roughness', 'edTerrainRough', round2(t.roughness));
    set('metallic', 'edTerrainMetal', round2(t.metallic));
  }

  // ---- 今のテレイン ----------------------------------------------------------
  // テレインの名前の一致は大文字小文字を区別しない（サーバーの
  // terrain::sameTerrainName と同じ規則。名前は英数字と _ - だけ）。
  function sameTerrainName(a, b) {
    return String(a).toLowerCase() === String(b).toLowerCase();
  }
  // 今のテレインの名前（無ければ ''）と、保存していない変更があるか。
  // missing はファイルが無い・読めないときのサーバーの文（空 = 正常）。
  // そのとき name は XML に残っている名前のまま（地形は出ていない）。
  function currentTerrain() {
    const t = sceneData && sceneData.hasTerrain ? sceneData.terrain : null;
    return { name: (t && t.data) || '', dirty: !!(t && sceneData.terrainDirty),
             missing: (sceneData && sceneData.terrainMissing) || '' };
  }
  // name のテレインが「開いている」か。判定はサーバーの terrainOpen
  // （isCurrentTerrain と同じ意味 = 読めている今のテレインの名前）だけを見る。
  // Missing は開いていない扱いなので、ファイルを戻してダブルクリックすれば
  // 開き直せる。
  function isOpenTerrain(name) {
    const open = (sceneData && sceneData.terrainOpen) || '';
    return !!open && sameTerrainName(name, open);
  }
  // 取り替えると失う変更があるか（サーバーの未保存 + ここで確定待ちの欄）。
  // テレインが無ければ失うものは無い（確定待ちの欄は黙って捨ててよい -
  // 新規は既定の大きさで作る仕様）。
  function terrainUnsaved() {
    const cur = currentTerrain();
    return !!cur.name && (cur.dirty || Object.keys(terrainPatch).length > 0);
  }
