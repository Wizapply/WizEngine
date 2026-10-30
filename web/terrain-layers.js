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
  // 層の中の行の見出し（TERRAIN_LAYER_FIELDS の row の番号順。null と、ここに
  // 無い行は層の名前）。行の数は表の row から決める（TERRAIN_LAYER_ROW_COUNT）。
  const TERRAIN_LAYER_ROW_LABELS = [null, '法線', 'マスク'];
  const TERRAIN_LAYER_ROW_COUNT =
    1 + Math.max(...Object.values(TERRAIN_LAYER_FIELDS).map((spec) => spec.row));

  // 塗り分けの層の行（index.html の #edTerrainLayers の中身）。行の数はサーバーの
  // terrain.layers の数（= 層の数。HTML には持たない）で、1 つのテンプレートから作る。
  // 作るのは初回と数が変わったときだけで、欄にフォーカスがある間は作り直さない
  // （打っている途中の値を消さない）。Missing の無効化は外側の fieldset が新しい行にも効く。
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
      const rows = Array.from({ length: TERRAIN_LAYER_ROW_COUNT }, (_, r) => {
        const row = document.createElement('div');
        row.className = 'edRow layerRow';
        const label = document.createElement('span');
        label.textContent = TERRAIN_LAYER_ROW_LABELS[r] || name;
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
        input.onchange = () => applyTerrainLayer(i, field);
        rows[spec.row].appendChild(input);
      }
      box.appendChild(group);
    }
  }
  // 層の欄を 1 つ送る。index = 層の番号（0 から）、field = 変えた欄
  // （TERRAIN_LAYER_FIELDS のキー）で、その 1 つだけを layer{index, <field>}
  // の 1 通ですぐ送る（触っていない欄の値は送らない = 丸めた表示の値や古い値で
  // 上書きしない）。
  function applyTerrainLayer(index, field) {
    // 層は部分更新のキーに混ぜない（サーバーは layer の 1 通を層だけの操作として
    // 扱う）。溜まっている形のキーは先に別の 1 通で送る（層とは独立）。
    const spec = TERRAIN_LAYER_FIELDS[field];
    if (!spec) return;
    const id = spec.id + index;
    const el = document.getElementById(id);
    if (!el) return;
    // 数の欄に打ちかけの文字（「1e」「-」など）があると、ブラウザは value を ''
    // で返す。空と区別できないので送らない（numOrNull で null = 「全体に従う」へ
    // 戻さない。ポーリングが戻す）。
    if (spec.kind !== 'text' && el.validity && el.validity.badInput) return;
    const layer = { index };
    let shown;
    if (spec.kind === 'text') {
      layer[field] = shown = el.value.trim();
    } else {
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
    }
    syncTerrainLayerSent();
    terrainLayerSent[id] = { value: shown, until: Date.now() + TERRAIN_LAYER_HOLD_MS };
    flushTerrain();
    send('edit.terrain', { layer });
    return;
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
        setLayerField(spec.id + i, spec.kind === 'text' ? (v || '')
          : (v === null || v === undefined ? '' : round2(v)));
      }
      // マスクのある層は粗さ・金属の数字を使わない（マスクが勝つ）ので薄く見せる。
      const mask = document.getElementById(TERRAIN_LAYER_FIELDS.mask.id + i);
      const maskRow = mask && mask.closest('.layerRow');
      if (maskRow) maskRow.classList.toggle('masked', !!l.mask);
    });
  }
