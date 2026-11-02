// Charon web UI: テレインの塗り分けの層の欄（Inspector の secTerrain 節の #edTerrainLayers。
// 欄の表と、層だけの行の扱い = 見出しのツールチップ・マスクのある層を薄く見せる）。
// 行の組み立て・値の表示と送信・送った値をポーリングで戻さない守りは terrain-rows.js の
// 共通部品。app.js・terrain.js・terrain-rows.js の後ろに読む古典スクリプトで、それらの
// トップレベル（round2 / terrainLayerLabel / buildIndexedRows / fillIndexedRows など）を
// そのまま使う。ここの関数もグローバルのままにする（terrain.js の
// renderTerrainSection が名前で使うため）。

  // 塗り分けの層の欄の表。層の中の行の順に並び、行ごとに見出し（label。null =
  // 層の名前）と、その行に左から並ぶ欄（fields）を持つ。欄のキー = サーバーの
  // terrain.layers[i] と layer{...} のキー。id は欄の id の頭（後ろに層の番号）、
  // kind は値の扱い: text = パス（前後の空白を落として送る）、num = 数（空 =
  // 送らない）、numOrNull = 数か null（空 = null = テレイン全体の値に従う）。
  // 層の欄は決め事として範囲（min / max）を持たない: サーバーが丸め（1 か所）、
  // 空は全体に従う。Inspector の固定の欄（粗さなど）と筆の欄の範囲は別の仕組みで、
  // サーバーの limits を下限は data-limit、上限は data-limit-max で受ける（app.js の
  // applyFieldLimits）。
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
  const TERRAIN_LAYER_FIELDS = indexedFields(TERRAIN_LAYER_ROWS);

  // 塗り分けの層の行（index.html の #edTerrainLayers の中身）。行の数はサーバーの
  // terrain.layers の数（= 層の数。HTML には持たない）で、作り方（作り直す時期・
  // フォーカス中は作り直さない）は terrain-rows.js の buildIndexedRows。
  const TERRAIN_LAYER_TIP = '層はこのテレインの .terrain に保存され、どのシーンでも同じ';
  const TERRAIN_TILE_TIP = 'タイル 4 m 以上が配信に優しい';
  // 層ごとのマスクの行（mask の欄がある .layerRow。添字 = 層の番号）。行を
  // 作り直すたびに作り直し、renderTerrainLayers が .masked を付け外す。
  let terrainLayerMaskRows = [];
  // 層ごとに 1 つの箱（.layerGroup）に 3 行（.edRow.layerRow: 層 N = テクスチャ /
  // タイル、法線 = パス / 強さ、マスク = パス / 粗さ / 金属）。行の中身は
  // TERRAIN_LAYER_ROWS から作り、変えた欄は layer{index, <field>} で送る。
  function buildTerrainLayerRows(count) {
    buildIndexedRows('edTerrainLayers', count, TERRAIN_LAYER_ROWS, terrainLayerLabel, 'layer', {
      tip: TERRAIN_LAYER_TIP,
      fieldTip: (field) => (field === 'tile' ? TERRAIN_TILE_TIP : TERRAIN_LAYER_TIP),
      onRebuild: () => {
        const head = document.getElementById('edTerrainLayersHead');
        if (head) head.title = TERRAIN_LAYER_TIP + '。' + TERRAIN_TILE_TIP;
        terrainLayerMaskRows = [];
      },
      onRow: (i, r, row) => {
        if ('mask' in r.fields) terrainLayerMaskRows[i] = row;
      },
    });
  }
  // 層の欄の中身（layers = サーバーの terrain.layers。順 = 層の番号）。行を
  // そろえてから値を入れ、マスクのある層の行を薄く見せる。
  function renderTerrainLayers(layers) {
    buildTerrainLayerRows(layers.length);
    fillIndexedRows(TERRAIN_LAYER_FIELDS, layers);
    layers.forEach((l, i) => {
      // マスクのある層は粗さ・金属の数字を使わない（マスクが勝つ）ので薄く見せる。
      const maskRow = terrainLayerMaskRows[i];
      if (maskRow) maskRow.classList.toggle('masked', !!l.mask);
    });
  }
