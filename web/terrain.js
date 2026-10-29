// Charon web UI: テレイン欄（Inspector の secTerrain 節 = 選択したテレイン、XML の <terrain>）。app.js の後ろに読む
// 古典スクリプトで、app.js のトップレベル（send / num / setField / round2 /
// sceneData / owner など）をそのまま使う。ここの関数もグローバルのままにする
// （index.html の onchange= と app.js の renderEditor などが名前で呼ぶため）。

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
  // 層の欄で送った値（キーは欄の id、値は {value, until}）。サーバーの値が
  // 送った値になるまで（届かなければ TERRAIN_LAYER_HOLD_MS まで）、ポーリングで
  // 欄を上書きしない（適用は物理スレッドなので、送った直後の /scene はまだ古い）。
  // 丸められた・断られたときは期限で手放し、サーバーの値に戻る。
  const TERRAIN_LAYER_HOLD_MS = 3000;
  // 守りはテレインごと: 別のテレインを開いた・新規にした（data か guid が
  // 変わった）ら捨てる（前のテレインへ送った値で新しいテレインの欄を止めない）。
  let terrainLayerSent = {};
  let terrainLayerSentFor = '';
  // 今のテレインの見分け（data と guid）。層の欄の守りと筆の層の選択肢
  // （terrain-brush.js）が「テレインが差し替わったか」をこれで見る。
  function terrainIdentity() {
    const t = (sceneData && sceneData.terrain) || {};
    return (t.data || '') + '|' + (t.guid || '');
  }
  // 守りを今のテレインのものにする（見分けが変わっていたら空にする）。読む前・
  // 書く前に必ず通す（判定はここ 1 か所）。
  function syncTerrainLayerSent() {
    const who = terrainIdentity();
    if (who !== terrainLayerSentFor) {
      terrainLayerSent = {};
      terrainLayerSentFor = who;
    }
  }
  // 層の欄 id にサーバーの値 v を入れる（送信待ちの間は触らない）。
  function setLayerField(id, v) {
    syncTerrainLayerSent();
    const sent = terrainLayerSent[id];
    if (sent) {
      if (sent.value !== v && Date.now() < sent.until) return;
      delete terrainLayerSent[id];
    }
    setField(id, v);
  }
  // 塗り分けの層の欄の表（キー = サーバーの terrain.layers[i] と layer{...} の
  // キー）。id は欄の id の頭（後ろに層の番号）、kind は値の扱い:
  // text = パス（前後の空白を落として送る）、num = 数（空 = 送らない）、
  // numOrNull = 数か null（空 = null = テレイン全体の値に従う）。row は層の中の
  // 何行目か（0 = 層 N、1 = 法線、2 = マスク）で、表の順に左から並ぶ。範囲
  // （min / max）は持たない: 範囲はサーバーが丸める（1 か所）。
  const TERRAIN_LAYER_FIELDS = {
    texture: { id: 'edTerrainLayerTex', kind: 'text', row: 0, cls: 'layerPath',
               placeholder: 'textures/ground.png',
               tip: 'テクスチャ（assets/ からの相対パス。空 = 単色）' },
    tile: { id: 'edTerrainLayerTile', kind: 'num', row: 0, cls: 'layerNum', step: '0.5',
            tip: 'タイル（テクスチャ 1 リピートのメートル。法線・マスクも同じ）' },
    normal: { id: 'edTerrainLayerNormal', kind: 'text', row: 1, cls: 'layerPath',
              placeholder: 'textures/ground_nor_gl.png',
              tip: '法線マップ（OpenGL 向き = 緑が画像の上）。空 = 平ら' },
    normalScale: { id: 'edTerrainLayerNScale', kind: 'num', row: 1, cls: 'layerNum',
                   step: '0.1', tip: '法線の強さ（1 = 原寸、0 = 平ら）' },
    mask: { id: 'edTerrainLayerMask', kind: 'text', row: 2, cls: 'layerPath',
            placeholder: 'textures/ground_arm.png',
            tip: 'マスク: R = 遮蔽、G = 粗さ、B = 金属（polyhaven の arm）。' +
                 'あれば右の数字は使わない' },
    roughness: { id: 'edTerrainLayerRough', kind: 'numOrNull', row: 2, cls: 'layerNum',
                 step: '0.05', placeholder: '全体', tip: '粗さ。空 = テレイン全体の値' },
    metallic: { id: 'edTerrainLayerMetal', kind: 'numOrNull', row: 2, cls: 'layerNum',
                step: '0.05', placeholder: '全体', tip: '金属。空 = テレイン全体の値' },
  };
  // 層の中の行の見出し（TERRAIN_LAYER_FIELDS の row の番号順。0 行目は層の名前）。
  const TERRAIN_LAYER_ROW_LABELS = [null, '法線', 'マスク'];
  // key = 送るキー。'layer' のときだけ index = 層の番号（0 から）、field =
  // 変えた欄（TERRAIN_LAYER_FIELDS のキー）で、その 1 つだけを layer{index, <field>}
  // の 1 通ですぐ送る（触っていない欄の値は送らない = 丸めた表示の値や古い値で
  // 上書きしない）。
  function applyTerrain(key, index, field) {
    const t = (sceneData && sceneData.terrain) || {};
    const pick = (id) => document.getElementById(id);
    // 欄が空・数でないときは今の値（無ければ送らない）。
    const n = (id, cur) => num(id, cur);
    let value;
    switch (key) {
      case 'layer': {
        // 層は部分更新のキーに混ぜない（サーバーは layer の 1 通を層だけの操作として
        // 扱う）。溜まっている形のキーは先に別の 1 通で送る（層とは独立）。
        const spec = TERRAIN_LAYER_FIELDS[field];
        if (!spec) return;
        const id = spec.id + index;
        const el = pick(id);
        if (!el) return;
        const layer = { index };
        let shown;
        if (spec.kind === 'text') {
          layer[field] = shown = el.value.trim();
        } else {
          const v = n(id, NaN);
          if (Number.isFinite(v)) {
            layer[field] = v;
            shown = round2(v);
          } else if (spec.kind === 'numOrNull' && el.value.trim() === '') {
            layer[field] = null;  // 空 = テレイン全体の値に従う
            shown = '';
          } else {
            return;  // 空・数でない = 送らない（ポーリングが戻す）
          }
        }
        syncTerrainLayerSent();
        terrainLayerSent[id] = { value: shown, until: Date.now() + TERRAIN_LAYER_HOLD_MS };
        flushTerrain();
        send('edit.terrain', { layer });
        return;
      }
      case 'size': value = n('edTerrainSize', t.size); break;
      case 'resolution': value = Math.round(n('edTerrainRes', t.resolution)); break;
      case 'height': value = n('edTerrainHeight', t.height); break;
      case 'pos': {
        const p = t.pos || {};
        value = { x: n('edTerrainPX', p.x), y: n('edTerrainPY', p.y),
                  z: n('edTerrainPZ', p.z) };
        if (!Number.isFinite(value.x) || !Number.isFinite(value.y) ||
            !Number.isFinite(value.z)) return;
        break;
      }
      case 'rot': {
        const r = t.rot || {};
        value = { x: n('edTerrainRX', r.x), y: n('edTerrainRY', r.y),
                  z: n('edTerrainRZ', r.z) };
        if (!Number.isFinite(value.x) || !Number.isFinite(value.y) ||
            !Number.isFinite(value.z)) return;
        break;
      }
      case 'color': value = pick('edTerrainColor').value; break;
      case 'roughness': value = n('edTerrainRough', t.roughness); break;
      case 'metallic': value = n('edTerrainMetal', t.metallic); break;
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
  // 塗り分けの層の行（index.html の #edTerrainLayers の中身）。行の数はサーバーの
  // terrain.layers の数（= 層の数。HTML には持たない）で、1 つのテンプレートから作る。
  // 作るのは初回と数が変わったときだけで、欄にフォーカスがある間は作り直さない
  // （打っている途中の値を消さない）。Missing の無効化は外側の fieldset が新しい行にも効く。
  // 層の表示名（i = 0 始まりの層の番号）。名前の付け方はサーバーが持つ
  // （/scene の terrain.layers[i].label = ステータスの文と同じ名前）。層の欄・
  // 自動塗りの行・筆の層の選択（terrain-brush.js）はここを通す。
  function terrainLayerLabel(i) {
    const l = ((sceneData && sceneData.terrain && sceneData.terrain.layers) || [])[i];
    return (l && l.label) || '';
  }
  // テレインの道具の既定（テレインごとに変わらない値。/scene のトップレベルの
  // terrainTools = {baseLayer, autoPaintDefaults}。無ければ空）。
  function terrainTools() {
    return (sceneData && sceneData.terrainTools) || {};
  }
  function terrainAutoPaintDefaults() { return terrainTools().autoPaintDefaults; }
  const TERRAIN_LAYER_TIP = '層はこのテレインの .terrain に保存され、どのシーンでも同じ';
  const TERRAIN_TILE_TIP = 'タイル 4 m 以上が配信に優しい';
  // 層ごとに 1 つの箱（.layerGroup）に 3 行（.edRow.layerRow: 層 N = テクスチャ /
  // タイル、法線 = パス / 強さ、マスク = パス / 粗さ / 金属）。行の中身は
  // TERRAIN_LAYER_FIELDS から作る。
  function buildTerrainLayerRows(count) {
    const box = document.getElementById('edTerrainLayers');
    if (!box || box.childElementCount === count) return;
    if (box.contains(document.activeElement)) return;
    const head = document.getElementById('edTerrainLayersHead');
    if (head) head.title = TERRAIN_LAYER_TIP + '。' + TERRAIN_TILE_TIP;
    box.textContent = '';
    for (let i = 0; i < count; i++) {
      const name = terrainLayerLabel(i);
      const group = document.createElement('div');
      group.className = 'layerGroup';
      const rows = TERRAIN_LAYER_ROW_LABELS.map((rowLabel) => {
        const row = document.createElement('div');
        row.className = 'edRow layerRow';
        const label = document.createElement('span');
        label.textContent = rowLabel || name;
        label.title = name + '。' + TERRAIN_LAYER_TIP;
        row.appendChild(label);
        group.appendChild(row);
        return row;
      });
      for (const field in TERRAIN_LAYER_FIELDS) {
        const spec = TERRAIN_LAYER_FIELDS[field];
        const input = document.createElement('input');
        input.id = spec.id + i;
        input.className = (spec.kind === 'text' ? 'txt ' : 'num ') + spec.cls;
        input.type = spec.kind === 'text' ? 'text' : 'number';
        if (spec.step) input.step = spec.step;
        if (spec.placeholder) input.placeholder = spec.placeholder;
        input.title = name + ' の' + spec.tip + '。' +
          (field === 'tile' ? TERRAIN_TILE_TIP : TERRAIN_LAYER_TIP);
        input.onchange = () => applyTerrain('layer', i, field);
        rows[spec.row].appendChild(input);
      }
      box.appendChild(group);
    }
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
    const layers = t.layers || [];
    buildTerrainLayerRows(layers.length);
    layers.forEach((l, i) => {
      for (const field in TERRAIN_LAYER_FIELDS) {
        const spec = TERRAIN_LAYER_FIELDS[field];
        const v = l[field];
        // パスは文字列、数は丸めて、null（全体に従う）・無い値は空の欄。
        setLayerField(spec.id + i, spec.kind === 'text' ? (v || '')
          : (v === null || v === undefined ? '' : round2(v)));
      }
      // マスクのある層は粗さ・金属の数字を使わない（マスクが勝つ）ので薄く見せる。
      const mask = document.getElementById(TERRAIN_LAYER_FIELDS.mask.id + i);
      const maskRow = mask && mask.closest('.layerRow');
      if (maskRow) maskRow.classList.toggle('masked', !!l.mask);
    });
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
  }  // 取り替えると失う変更があるか（サーバーの未保存 + ここで確定待ちの欄）。
  // テレインが無ければ失うものは無い（確定待ちの欄は黙って捨ててよい -
  // 新規は既定の大きさで作る仕様）。
  function terrainUnsaved() {
    const cur = currentTerrain();
    return !!cur.name && (cur.dirty || Object.keys(terrainPatch).length > 0);
  }
  // 名前に使える文字が 1 つも無かったときの知らせ（cleanAssetName の結果が空）。
  const TERRAIN_NAME_RULE = '名前に使えるのは英数字と _ - だけです';

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
    const count = document.getElementById('edTerrainAutoRows').childElementCount;
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

  // ---- テレインのタイル（アセットパネル）------------------------------------
  // 一覧は /scene の terrainFiles（assets/terrains/*.terrain の見出し）。
  // 今のテレインは .sel、保存していない変更があれば名前に *。まだファイルの
  // 無い新規（保存前）は一覧に無いので、今のテレインのぶんを先頭に足す。
  // Missing は足さない（ファイルが無い。読めないファイルは一覧にあれば出る）。
  // 単クリックは何もしない（開くのはダブルクリック、右クリックはメニュー）。
  function terrainFileList() {
    return (sceneData && sceneData.terrainFiles) || [];
  }
  // name の .terrain ファイルが一覧にあるか（保存前の新規のタイルは無い）。
  function hasTerrainFile(name) {
    return terrainFileList().some((f) => sameTerrainName(f.name, name));
  }
  function terrainTileEntries() {
    const files = terrainFileList();
    const cur = currentTerrain();
    const list = files.slice();
    if (isOpenTerrain(cur.name) && !hasTerrainFile(cur.name)) {
      const t = sceneData.terrain;
      list.unshift({ name: cur.name, resolution: t.resolution, size: t.size,
                     height: t.height });
    }
    return { list: list, cur: cur };
  }
  function terrainTilesKey() {
    const e = terrainTileEntries();
    return e.list.map((f) => f.name + ':' + f.resolution + ':' + f.size + ':' +
                             f.height).join('|') +
      '@' + e.cur.name + (e.cur.dirty ? '*' : '') + (isOpenTerrain(e.cur.name) ? '' : '!');
  }
  function renderTerrainTiles() {
    const e = terrainTileEntries();
    let html = '';
    for (const f of e.list) {
      const cur = isOpenTerrain(f.name);
      const dirty = cur && e.cur.dirty;
      const title = '解像度 ' + f.resolution + ' ／ 全幅 ' + round2(f.size) +
        ' m ／ 高さ ' + round2(f.height) + ' m' + (dirty ? '（未保存）' : '') +
        '\nダブルクリック: 開く ／ 右クリック: メニュー（名前の変更・削除）';
      // 名前は属性から読む（JS 文字列に埋め込まない）。
      html += '<div class="asItem' + (cur ? ' sel' : '') +
        '" data-terrain="' + ndEsc(f.name) + '" title="' + ndEsc(title) +
        '" ondblclick="openTerrain(this.dataset.terrain)">' +
        '<span class="ico">⛰&#xFE0E;</span><span class="name">' + ndEsc(f.name) +
        (dirty ? ' *' : '') + '</span><span class="kind">terrain</span></div>';
    }
    return html;
  }

  // ---- 名前の変更（タイルのその場編集）--------------------------------------
  // .name を input に差し替え、Enter / ページ内の別の場所へフォーカスが
  // 移ったら確定、Esc で取消。ウィンドウごとフォーカスが外れたとき（別の
  // アプリへ切り替えた）は確定せず、戻ったら入力欄へフォーカスを返す。
  // 編集中は renderAssets が DOM を組み直さない（terrainRenameActive）。
  let terrainRenameFinish = null;  // 編集中だけ: finish(commit)
  function terrainRenameActive() {
    return terrainRenameFinish !== null;
  }
  // 取り消す（アセットパネルを隠すとき。renderAssets が呼ぶ）。
  function cancelTerrainRename() {
    if (terrainRenameFinish) terrainRenameFinish(false);
  }
  function startTerrainRename(tile) {
    const span = tile && tile.querySelector('.name');
    if (!span || terrainRenameActive()) return;
    const from = tile.dataset.terrain;
    const input = document.createElement('input');
    input.type = 'text';
    input.className = 'asRename';
    input.value = from;
    input.maxLength = ASSET_NAME_MAX;
    input.title = '英数字と _ - だけ。Enter で確定、Esc で取消';
    let done = false;
    const finish = (commit) => {
      if (done) return;  // Enter の後、input を外したときの blur でもう一度来る
      done = true;
      terrainRenameFinish = null;
      const to = cleanAssetName(input.value);
      input.replaceWith(span);
      if (!commit) return;
      if (!to) { showAssetStatus(TERRAIN_NAME_RULE); return; }
      // 同じ名前・既にある名前・正規化はサーバーが判定してステータスに出す。
      if (to !== from) send('edit.terrain', { rename: { from: from, to: to } });
    };
    input.addEventListener('keydown', (ev) => {
      // Tab も確定。document の Tab（サイドバーの開閉）へは流さない。
      if (ev.key === 'Enter' || ev.key === 'Tab') {
        ev.preventDefault();
        ev.stopPropagation();
        finish(true);
      } else if (ev.key === 'Escape') {
        ev.preventDefault();
        finish(false);
      }
    });
    input.addEventListener('blur', () => {
      if (!document.hasFocus()) {
        window.addEventListener('focus', () => { if (!done) input.focus(); },
                                { once: true });
        return;
      }
      finish(true);
    });
    span.replaceWith(input);
    terrainRenameFinish = finish;
    input.focus();
    input.select();
  }

  // ---- メニューと操作 --------------------------------------------------------
  // タイルの右クリック。メニューを出してから選ぶまでにポーリングで一覧が
  // 組み直されうるので、タイルは選んだ時点で名前から引き直す。
  function terrainMenuItems(name) {
    const isOpen = isOpenTerrain(name);
    // シーンが参照している名前（Missing も含む = サーバーの refersToTerrain）。
    const cur = currentTerrain();
    const referred = !!cur.name && sameTerrainName(name, cur.name);
    const simulating = !!sceneData && sceneData.mode !== 'editor';
    const hasFile = hasTerrainFile(name);
    const tileNow = () => Array.from(
      document.querySelectorAll('#assetList [data-terrain]'))
      .find((el) => el.dataset.terrain === name);
    return [
      { head: '⛰ ' + name },
      { label: '✎ 名前の変更', onClick: () => startTerrainRename(tileNow()) },
      { label: '開く', hint: isOpen ? '開いています' : 'ダブルクリック',
        disabled: isOpen, onClick: () => openTerrain(name) },
      // 複製はファイルから作る（保存前の新規のタイルにはファイルが無い）。シミュレート
      // 中は削除と同じく受けない。最後の守りはサーバー（Scene::duplicateTerrain と
      // TerrainLibrary::duplicate）。
      { label: '⧉ 複製', disabled: simulating || !hasFile,
        hint: simulating ? 'エディタモードでだけ複製できます'
          : !hasFile ? '保存してから複製できます' : '',
        onClick: () => duplicateTerrain(name) },
      { sep: true },
      // シーンが参照しているテレイン（保存前でファイルの無いタイル・Missing も）
      // とシミュレート中は消せない。最後の守りはサーバー（Scene::deleteTerrain）。
      { label: '🗑 削除', danger: true, disabled: referred || simulating,
        hint: simulating ? 'エディタモードでだけ削除できます'
          : referred ? 'シーンが使っているテレインは削除できません' : '',
        onClick: () => deleteTerrain(name) },
    ];
  }
  // ごみ箱へ移す（OK のときだけ送る）。一覧はサーバーのキャッシュから
  // 次のポーリングで消える。
  function deleteTerrain(name) {
    if (!confirm('テレイン「' + name + '」をごみ箱へ移しますか？\n' +
                 'このテレインを使っているシーンは、開くとテレインが表示されなく' +
                 'なります（ファイルを戻せば元に戻ります。ローカルのドライブなら、' +
                 'ごみ箱から戻せます）')) {
      return;
    }
    send('edit.terrain', { delete: name });
  }
  // 取り替える前の確認。戻り値は送るキーに足すもの（null = やめる）。
  // OK を受けたときだけ discard:true を付ける: サーバーは未保存の変更があれば
  // discard 無しの open / new を断るので、ここの dirty 表示が古くても（生成の
  // 直後など）黙って失われない（断られたら次のポーリングの後にここで聞く）。
  function confirmReplaceTerrain(action) {
    if (!terrainUnsaved()) return {};
    if (!confirm('テレイン「' + currentTerrain().name +
                 '」の保存していない変更を捨てて、' + action + '。')) {
      return null;
    }
    return { discard: true };
  }
  // 新しいテレイン（平ら、既定の大きさ）。今のテレインがあれば取り替える
  // （古いファイルは消えない）。名前が既にあればサーバーが _2 などを付ける。
  function newTerrain() {
    const name = prompt('新しいテレインの名前（英数字と _ -）',
                        (sceneData && sceneData.sceneFile) || 'terrain');
    const clean = name === null ? '' : cleanAssetName(name);
    if (name !== null && !clean) showAssetStatus(TERRAIN_NAME_RULE);
    const extra = clean
      ? confirmReplaceTerrain('新しいテレイン「' + clean + '」を作ります') : null;
    if (!extra) return;
    discardTerrain();
    // 作ったテレインはサーバーが選択にする（Inspector に欄が出る）。
    send('edit.terrain', Object.assign({ new: clean }, extra));
  }
  // シーンから外す（.terrain ファイルは残る）。保存していない変更があれば
  // 捨てる確認を出す（サーバーは外すときに未保存の変更を捨てる）。
  function removeTerrainFromScene() {
    const cur = currentTerrain();
    if (terrainUnsaved()) {
      if (!confirm('テレイン「' + cur.name + '」の保存していない変更を捨てて、' +
                   'シーンから外します（ファイルは残ります）。')) {
        return;
      }
    }
    discardTerrain();
    send('edit.terrain', { enabled: false });
  }
  // 開く（確認は confirmReplaceTerrain、最後の守りはサーバー）。
  function openTerrain(name) {
    if (!name || terrainRenameActive()) return;
    // 開いている（Missing なら開き直す）: シーンのテレインを選ぶだけ。
    if (isOpenTerrain(name)) { selectTerrain(); return; }
    const extra = confirmReplaceTerrain('「' + name + '」を開きます');
    if (!extra) return;
    discardTerrain();
    send('edit.terrain', Object.assign({ open: name }, extra));
  }
  // 複製して開く（新しい名前 _2 などと背番号はサーバーが決める。確認は
  // confirmReplaceTerrain、最後の守りはサーバー）。
  function duplicateTerrain(name) {
    if (!name || terrainRenameActive()) return;
    const extra = confirmReplaceTerrain('「' + name + '」を複製して開きます');
    if (!extra) return;
    discardTerrain();
    send('edit.terrain', Object.assign({ duplicate: name }, extra));
  }

  // ---- 一覧の取り直し --------------------------------------------------------
  // エクスプローラで .terrain を足した・消した後にタブへ戻ったら、サーバーに
  // assets/terrains/ を読み直させる。focus と visibilitychange は続けて来るので
  // 1 回にまとめる。編集できるのはエディタカメラのページだけなので、そこでだけ。
  const TERRAIN_REFRESH_GAP_MS = 1000;
  let terrainRefreshAt = 0;
  function refreshTerrainFiles() {
    if (!isEditorCam()) return;
    const now = Date.now();
    if (now - terrainRefreshAt < TERRAIN_REFRESH_GAP_MS) return;
    terrainRefreshAt = now;
    send('edit.terrain', { refresh: true });
    // 共通イベント（assets/events/）の一覧も同じ時に取り直す。
    send('edit.event.refresh', {});
  }
  window.addEventListener('focus', refreshTerrainFiles);
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible') refreshTerrainFiles();
  });
