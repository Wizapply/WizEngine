// Charon web UI: テレイン欄（World 節の <terrain>）。app.js の後ろに読む
// 古典スクリプトで、app.js のトップレベル（send / num / setField / round2 /
// sceneData / owner など）をそのまま使う。ここの関数もグローバルのままにする
// （index.html の onchange= と app.js の renderEditor などが名前で呼ぶため）。

  // ---- テレイン（World 節）-----------------------------------------------
  // 地面と違い、変えたキーだけを送る（サーバーは部分更新）。形のキーは
  // 1 回でノイズ生成と全チャンクの作り直しが走るので、欄の change（確定時）
  // だけで送り、さらにスピンボタンの連打を 1 回にまとめる（短い間引き）。
  // 表面のキーと有効チェックは軽いのですぐ送る（溜まっている形のキーも一緒に）。
  const TERRAIN_SHAPE_KEYS = ['size', 'resolution', 'height', 'pos', 'seed',
                              'octaves', 'gain', 'lacunarity', 'scale'];
  const TERRAIN_SHAPE_DELAY_MS = 300;
  let terrainPatch = {};
  let terrainTimer = null;
  function flushTerrain() {
    if (terrainTimer) { clearTimeout(terrainTimer); terrainTimer = null; }
    if (Object.keys(terrainPatch).length === 0) return;
    const patch = terrainPatch;
    terrainPatch = {};
    send('edit.terrain', patch);
  }
  // 送信待ちの形のキーを捨てる。シーンの読込・XML 適用・全消しの前に呼ぶ:
  // 残すと、前のシーンで触った値が新しく読んだシーンのテレインに後から当たる。
  function discardTerrain() {
    if (terrainTimer) { clearTimeout(terrainTimer); terrainTimer = null; }
    terrainPatch = {};
  }
  // ページを離れるときは送信待ちを捨てずに送る（触った値が黙って消えない
  // ように）。通常の fetch は離脱で打ち切られうるので keepalive を付ける。
  window.addEventListener('pagehide', () => {
    if (terrainTimer) { clearTimeout(terrainTimer); terrainTimer = null; }
    if (!owner || Object.keys(terrainPatch).length === 0) return;
    const msg = Object.assign({ cmd: 'edit.terrain' }, terrainPatch);
    terrainPatch = {};
    fetch('input', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(msg),
      keepalive: true
    }).catch(() => {});
  });
  function applyTerrain(key) {
    const t = (sceneData && sceneData.terrain) || {};
    const pick = (id) => document.getElementById(id);
    // 欄が空・数でないときは今の値（無ければ送らない）。
    const n = (id, cur) => num(id, cur);
    let value;
    switch (key) {
      case 'enabled': value = pick('edTerrainOn').checked; break;
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
      case 'seed': value = Math.round(n('edTerrainSeed', t.seed)); break;
      case 'octaves': value = Math.round(n('edTerrainOct', t.octaves)); break;
      case 'gain': value = n('edTerrainGain', t.gain); break;
      case 'lacunarity': value = n('edTerrainLac', t.lacunarity); break;
      case 'scale': value = n('edTerrainScale', t.scale); break;
      case 'texture': value = pick('edTerrainTex').value.trim(); break;
      case 'tile': value = n('edTerrainTile', t.tile); break;
      case 'color': value = pick('edTerrainColor').value; break;
      case 'roughness': value = n('edTerrainRough', t.roughness); break;
      case 'metallic': value = n('edTerrainMetal', t.metallic); break;
      default: return;
    }
    if (typeof value === 'number' && !Number.isFinite(value)) return;
    if (value === undefined) return;
    terrainPatch[key] = value;
    if (key === 'enabled') {
      // チェックを入れた / 外した瞬間に欄の表示も切り替える（次のポーリング
      // を待たない）。
      document.getElementById('edTerrainProps').hidden = !value;
    }
    if (TERRAIN_SHAPE_KEYS.includes(key)) {
      if (terrainTimer) clearTimeout(terrainTimer);
      terrainTimer = setTimeout(flushTerrain, TERRAIN_SHAPE_DELAY_MS);
    } else {
      flushTerrain();
    }
  }
  function renderTerrainSection() {
    const props = document.getElementById('edTerrainProps');
    if (!props || !sceneData) return;
    // 古いサーバー（hasTerrain / terrain を出さない）では欄を出さない。
    const t = sceneData.terrain;
    const on = !!sceneData.hasTerrain && !!t;
    setField('edTerrainOn', on);
    // チェックにフォーカスがある間（押した直後でサーバーへの反映待ちかも
    // しれない間）は、チェックの見た目に合わせる（古い値で一瞬戻さない）。
    const onEl = document.getElementById('edTerrainOn');
    props.hidden = !(onEl === document.activeElement ? onEl.checked : on);
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
    set('seed', 'edTerrainSeed', t.seed);
    set('octaves', 'edTerrainOct', t.octaves);
    set('gain', 'edTerrainGain', round2(t.gain));
    set('lacunarity', 'edTerrainLac', round2(t.lacunarity));
    set('scale', 'edTerrainScale', round2(t.scale));
    set('texture', 'edTerrainTex', t.texture || '');
    set('tile', 'edTerrainTile', round2(t.tile));
    if (t.color) set('color', 'edTerrainColor', t.color);
    set('roughness', 'edTerrainRough', round2(t.roughness));
    set('metallic', 'edTerrainMetal', round2(t.metallic));
  }
