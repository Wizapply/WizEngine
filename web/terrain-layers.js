// Charon web UI: テレインの塗り分けの層の欄（Inspector の secTerrain 節の #edTerrainLayers。
// 欄の表・行の組み立て・値の表示と送信・送った値をポーリングで戻さない守り）。app.js と
// terrain.js の後ろに読む古典スクリプトで、app.js（send / num / setField / round2）と
// terrain.js（terrainIdentity / terrainLayerLabel / flushTerrain）のトップレベルを
// そのまま使う。ここの関数もグローバルのままにする（terrain.js の
// renderTerrainSection が名前で使うため）。

  // 層の欄で送った値（キーは欄の id、値は {value, until}）。サーバーの値が
  // 送った値になるまで（届かなければ TERRAIN_LAYER_HOLD_MS まで）、ポーリングで
  // 欄を上書きしない（適用は物理スレッドなので、送った直後の /scene はまだ古い）。
  // 丸められた・断られたときは期限で手放し、サーバーの値に戻る。
  const TERRAIN_LAYER_HOLD_MS = 3000;
  // 守りはテレインごと: 別のテレインを開いた・新規にした（data か guid が
  // 変わった）ら捨てる（前のテレインへ送った値で新しいテレインの欄を止めない）。
  let terrainLayerSent = {};
  let terrainLayerSentFor = '';
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
  // 塗り分けの層の欄の表。層の中の行の順に並び、行ごとに見出し（label。null =
  // 層の名前）と、その行に左から並ぶ欄（fields）を持つ。欄のキー = サーバーの
  // terrain.layers[i] と layer{...} のキー。id は欄の id の頭（後ろに層の番号）、
  // kind は値の扱い: text = パス（前後の空白を落として送る）、num = 数（空 =
  // 送らない）、numOrNull = 数か null（空 = null = テレイン全体の値に従う）。
  // 層の欄は決め事として範囲（min / max）を持たない: サーバーが丸め（1 か所）、
  // 空は全体に従う。Inspector の固定の欄（粗さなど）の下限は別の仕組みで、
  // サーバーの limits を data-limit で受ける（app.js の applyFieldLimits）。
  const TERRAIN_LAYER_ROWS = [
    { label: null, fields: {
      texture: { id: 'edTerrainLayerTex', kind: 'text', cls: 'layerPath',
                 placeholder: 'textures/ground.png',
                 tip: 'テクスチャ（assets/ からの相対パス。空 = 単色）' },
      tile: { id: 'edTerrainLayerTile', kind: 'num', cls: 'layerNum', step: '0.5',
              tip: 'タイル（テクスチャ 1 リピートのメートル。法線・マスクも同じ）' },
    } },
    { label: '法線', fields: {
      normal: { id: 'edTerrainLayerNormal', kind: 'text', cls: 'layerPath',
                placeholder: 'textures/ground_nor_gl.png',
                tip: '法線マップ（OpenGL 向き = 緑が画像の上）。空 = 平ら' },
      normalScale: { id: 'edTerrainLayerNScale', kind: 'num', cls: 'layerNum',
                     step: '0.1', tip: '法線の強さ（1 = 原寸、0 = 平ら）' },
    } },
    { label: 'マスク', fields: {
      mask: { id: 'edTerrainLayerMask', kind: 'text', cls: 'layerPath',
              placeholder: 'textures/ground_arm.png',
              tip: 'マスク: R = 遮蔽、G = 粗さ、B = 金属（polyhaven の arm）。' +
                   'あれば右の数字は使わない' },
      roughness: { id: 'edTerrainLayerRough', kind: 'numOrNull', cls: 'layerNum',
                   step: '0.05', placeholder: '全体', tip: '粗さ。空 = テレイン全体の値' },
      metallic: { id: 'edTerrainLayerMetal', kind: 'numOrNull', cls: 'layerNum',
                  step: '0.05', placeholder: '全体', tip: '金属。空 = テレイン全体の値' },
    } },
  ];
  // キーで欄を引くための一覧（TERRAIN_LAYER_ROWS から作る写し。欄を足すのは
  // TERRAIN_LAYER_ROWS だけ）。
  const TERRAIN_LAYER_FIELDS = Object.assign({}, ...TERRAIN_LAYER_ROWS.map((r) => r.fields));
  // 欄が数か（text 以外 = num / numOrNull）。
  function isNumberField(spec) {
    return spec.kind !== 'text';
  }

  // 塗り分けの層の行（index.html の #edTerrainLayers の中身）。行の数はサーバーの
  // terrain.layers の数（= 層の数。HTML には持たない）で、1 つのテンプレートから作る。
  // 作るのは初回と数が変わったときだけで、欄にフォーカスがある間は作り直さない
  // （打っている途中の値を消さない）。Missing の無効化は外側の fieldset が新しい行にも効く。
  const TERRAIN_LAYER_TIP = '層はこのテレインの .terrain に保存され、どのシーンでも同じ';
  const TERRAIN_TILE_TIP = 'タイル 4 m 以上が配信に優しい';
  // 層ごとのマスクの行（mask の欄がある .layerRow。添字 = 層の番号）。行を
  // 作り直すたびに作り直し、renderTerrainLayers が .masked を付け外す。
  let terrainLayerMaskRows = [];
  // 層ごとに 1 つの箱（.layerGroup）に 3 行（.edRow.layerRow: 層 N = テクスチャ /
  // タイル、法線 = パス / 強さ、マスク = パス / 粗さ / 金属）。行の中身は
  // TERRAIN_LAYER_ROWS から作る。
  function buildTerrainLayerRows(count) {
    const box = document.getElementById('edTerrainLayers');
    if (!box || box.childElementCount === count) return;
    if (box.contains(document.activeElement)) return;
    const head = document.getElementById('edTerrainLayersHead');
    if (head) head.title = TERRAIN_LAYER_TIP + '。' + TERRAIN_TILE_TIP;
    box.textContent = '';
    terrainLayerMaskRows = [];
    for (let i = 0; i < count; i++) {
      const name = terrainLayerLabel(i);
      const group = document.createElement('div');
      group.className = 'layerGroup';
      for (const r of TERRAIN_LAYER_ROWS) {
        const row = document.createElement('div');
        row.className = 'edRow layerRow';
        const label = document.createElement('span');
        label.textContent = r.label || name;
        label.title = name + '。' + TERRAIN_LAYER_TIP;
        row.appendChild(label);
        for (const field in r.fields) {
          const spec = r.fields[field];
          const input = document.createElement('input');
          input.id = spec.id + i;
          input.className = (isNumberField(spec) ? 'num ' : 'txt ') + spec.cls;
          input.type = isNumberField(spec) ? 'number' : 'text';
          if (spec.step) input.step = spec.step;
          if (spec.placeholder) input.placeholder = spec.placeholder;
          input.title = name + ' の' + spec.tip + '。' +
            (field === 'tile' ? TERRAIN_TILE_TIP : TERRAIN_LAYER_TIP);
          input.onchange = () => applyTerrainLayer(i, field);
          row.appendChild(input);
        }
        if ('mask' in r.fields) terrainLayerMaskRows[i] = row;
        group.appendChild(row);
      }
      box.appendChild(group);
    }
  }
  // 層の欄を 1 つ送る。index = 層の番号（0 から）、field = 変えた欄
  // （TERRAIN_LAYER_FIELDS のキー）で、その 1 つだけを layer{index, <field>}
  // の 1 通ですぐ送る（触っていない欄の値は送らない = 丸めた表示の値や古い値で
  // 上書きしない）。
  function applyTerrainLayer(index, field) {
    const spec = TERRAIN_LAYER_FIELDS[field];
    if (!spec) return;
    const id = spec.id + index;
    const el = document.getElementById(id);
    if (!el) return;
    // 数の欄に打ちかけの文字（「1e」「-」など）があると、ブラウザは value を ''
    // で返す。空と区別できないので送らない（numOrNull で null = 「全体に従う」へ
    // 戻さない。ポーリングが戻す）。
    if (isNumberField(spec) && el.validity && el.validity.badInput) return;
    const layer = { index };
    let shown;
    if (isNumberField(spec)) {
      const v = num(id, NaN);
      if (Number.isFinite(v)) {
        layer[field] = v;
        shown = round2(v);
      } else if (spec.kind === 'numOrNull' && el.value.trim() === '') {
        layer[field] = null;  // 空 = テレイン全体の値に従う
        shown = '';
      } else {
        return;  // 空・数でない = 送らない（ポーリングが戻す）
      }
    } else {
      layer[field] = shown = el.value.trim();
    }
    syncTerrainLayerSent();
    terrainLayerSent[id] = { value: shown, until: Date.now() + TERRAIN_LAYER_HOLD_MS };
    // 層は部分更新のキーに混ぜない（サーバーは layer の 1 通を層だけの操作として
    // 扱う）。溜まっている形のキーは先に別の 1 通で送る（層とは独立）。
    flushTerrain();
    send('edit.terrain', { layer });
  }
  // 層の欄の中身（layers = サーバーの terrain.layers。順 = 層の番号）。行を
  // そろえてから値を入れ、マスクのある層の行を薄く見せる。
  function renderTerrainLayers(layers) {
    buildTerrainLayerRows(layers.length);
    layers.forEach((l, i) => {
      for (const field in TERRAIN_LAYER_FIELDS) {
        const spec = TERRAIN_LAYER_FIELDS[field];
        const v = l[field];
        // パスは文字列、数は丸めて、null（全体に従う）・無い値は空の欄。
        setLayerField(spec.id + i, isNumberField(spec)
          ? (v === null || v === undefined ? '' : round2(v)) : (v || ''));
      }
      // マスクのある層は粗さ・金属の数字を使わない（マスクが勝つ）ので薄く見せる。
      const maskRow = terrainLayerMaskRows[i];
      if (maskRow) maskRow.classList.toggle('masked', !!l.mask);
    });
  }
