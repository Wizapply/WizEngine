// Charon web UI: テレインの散布物の原型の欄（Inspector の secTerrain 節の #edScatterProtos と
// 配置の数 #edScatterCount）。欄の表と描き直しだけを持ち、行の組み立て・値の表示と
// 送信・送った値の守りは terrain-rows.js の共通部品。app.js・terrain.js・terrain-rows.js
// の後ろ（terrain-brush.js の後ろ）に読む古典スクリプトで、それらのトップレベル
// （sceneData / indexedFields / buildIndexedRows / fillIndexedRows）をそのまま使う。
// ここの関数もグローバルのままにする（terrain.js の renderTerrainSection と terrain-brush.js の
// 原型の選択が名前で使うため）。

  // 原型の欄の表（作りは terrain-rows.js の冒頭）。欄のキー = サーバーの
  // terrain.scatter.prototypes[i] と scatter{...} のキー。範囲（倍率・幹の寸法）は
  // ブラウザに持たない: サーバーが丸め（1 か所）、丸めた値がポーリングで戻る。
  const TERRAIN_SCATTER_ROWS = [
    { label: null, fields: {
      file: { id: 'edScatterFile', kind: 'text', cls: 'layerPath',
              placeholder: 'models/tree.glb',
              tip: 'モデル（assets/ からの相対パス。空 = 代役の幹と冠で描く）' },
      scale: { id: 'edScatterScale', kind: 'num', cls: 'layerNum', step: '0.1',
               tip: '倍率（モデルの見た目と単位の換算。幹には掛からない。配置ごとのばらつきはこれに掛かる）' },
    } },
    { label: '幹', fields: {
      collide: { id: 'edScatterCollide', kind: 'bool', cls: 'layerCheck', text: '当たり判定',
                 tip: '幹の当たり判定（鉛直の円柱 1 本）' },
      trunkRadius: { id: 'edScatterTrunkR', kind: 'num', cls: 'layerNum', step: '0.05',
                     tip: '幹の半径（m、倍率 1 のとき。配置ごとのばらつきだけが掛かる）' },
      trunkHeight: { id: 'edScatterTrunkH', kind: 'num', cls: 'layerNum', step: '0.5',
                     tip: '幹の高さ（m、倍率 1 のとき。配置ごとのばらつきだけが掛かる）' },
    } },
  ];
  // キーで欄を引くための一覧（TERRAIN_SCATTER_ROWS から作る写し。欄を足すのは
  // TERRAIN_SCATTER_ROWS だけ）。
  const TERRAIN_SCATTER_FIELDS = indexedFields(TERRAIN_SCATTER_ROWS);
  const TERRAIN_SCATTER_TIP = '原型はこのテレインの .terrain に保存され、どのシーンでも同じ';

  // 原型の一覧（scatter = サーバーの terrain.scatter。無ければ空）。散布物から
  // 原型の配列を取り出すのはここだけ。
  function terrainScatterPrototypes(scatter) {
    return (scatter && scatter.prototypes) || [];
  }
  // 今の /scene（sceneData.terrain.scatter）の原型の一覧。terrain-brush.js の 🌲 の
  // 原型の選択が使う。
  function currentScatterPrototypes() {
    const t = sceneData && sceneData.terrain;
    return terrainScatterPrototypes(t && t.scatter);
  }
  // 原型の表示名（protos = terrainScatterPrototypes の配列、i = 0 始まりの原型の
  // 番号）。名前の付け方はサーバーが持つ（prototypes[i].label = ステータスの文と
  // 同じ名前）。
  function terrainPrototypeLabel(protos, i) {
    const p = protos[i];
    return (p && p.label) || '';
  }
  // 原型の欄と配置の数（scatter = サーバーの terrain.scatter。無ければ欄を空にする）。
  // 行の数は原型の数（サーバーの配列の長さ）、配置の上限もサーバーの max。
  function renderTerrainScatterSection(scatter) {
    const protos = terrainScatterPrototypes(scatter);
    buildIndexedRows('edScatterProtos', protos.length, TERRAIN_SCATTER_ROWS,
                     (i) => terrainPrototypeLabel(protos, i), 'scatter', {
      tip: TERRAIN_SCATTER_TIP,
      onRebuild: () => {
        const head = document.getElementById('edScatterHead');
        if (head) head.title = TERRAIN_SCATTER_TIP;
      },
    });
    fillIndexedRows(TERRAIN_SCATTER_FIELDS, protos);
    // 原型が 1 つも無い（テレインが無い・サーバーが scatter を送らない）ときは
    // 見出しと配置の数も出さない（見出しだけ残さない）。
    const empty = protos.length === 0;
    const head = document.getElementById('edScatterHead');
    if (head) head.hidden = empty;
    const countEl = document.getElementById('edScatterCount');
    if (countEl) countEl.hidden = empty;
    if (countEl) {
      const count = scatter && Number.isFinite(scatter.count) ? scatter.count : 0;
      const max = scatter && Number.isFinite(scatter.max) ? scatter.max : 0;
      countEl.textContent = scatter ? '配置 ' + count + ' / ' + max + ' 個' : '';
      // 上限に達したら散布の筆はもう置けない（サーバーが止める）ので目立たせる。
      countEl.classList.toggle('warn', max > 0 && count >= max);
    }
  }
