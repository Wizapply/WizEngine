// Charon web UI: テレイン欄（World 節の <terrain>）。app.js の後ろに読む
// 古典スクリプトで、app.js のトップレベル（send / num / setField / round2 /
// sceneData / owner など）をそのまま使う。ここの関数もグローバルのままにする
// （index.html の onchange= と app.js の renderEditor などが名前で呼ぶため）。

  // ---- テレイン（World 節）-----------------------------------------------
  // 地面と違い、変えたキーだけを送る（サーバーは部分更新）。形のキーは
  // 1 回で格子と全チャンクの作り直しが走るので、欄の change（確定時）
  // だけで送り、さらにスピンボタンの連打を 1 回にまとめる（短い間引き）。
  // 表面のキーは軽いのですぐ送る（溜まっている形のキーも一緒に）。
  // ノイズの値はここに混ぜない（「ノイズで生成」のボタンが generate で送る）。
  const TERRAIN_SHAPE_KEYS = ['size', 'resolution', 'height', 'pos'];
  const TERRAIN_SHAPE_DELAY_MS = 300;
  let terrainPatch = {};
  let terrainTimer = null;
  // 送信待ちを送る。extra は同じ 1 通に足すキー（ノイズで生成の generate =
  // サーバーが部分更新を先に当てる。2 通に分けると届く順番が入れ替わりうる）。
  function flushTerrain(extra, keepalive) {
    if (terrainTimer) { clearTimeout(terrainTimer); terrainTimer = null; }
    const patch = Object.assign(terrainPatch, extra || {});
    terrainPatch = {};
    if (Object.keys(patch).length === 0) return;
    send('edit.terrain', patch, keepalive);
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
  function applyTerrain(key) {
    const t = (sceneData && sceneData.terrain) || {};
    const pick = (id) => document.getElementById(id);
    // 欄が空・数でないときは今の値（無ければ送らない）。
    const n = (id, cur) => num(id, cur);
    let value;
    switch (key) {
      case 'enabled':
        value = pick('edTerrainOn').checked;
        // 入れたら新規（名前を聞く）。hasTerrain が古い値（外した直後）でも
        // new は有無によらず作るので、必ず意味のある操作になる。
        if (value) { newTerrain(); return; }
        break;
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
      terrainTimer = setTimeout(() => flushTerrain(), TERRAIN_SHAPE_DELAY_MS);
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
    set('texture', 'edTerrainTex', t.texture || '');
    set('tile', 'edTerrainTile', round2(t.tile));
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
  function currentTerrain() {
    const t = sceneData && sceneData.hasTerrain ? sceneData.terrain : null;
    return { name: (t && t.data) || '', dirty: !!(t && sceneData.terrainDirty) };
  }
  // 取り替えると失う変更があるか（サーバーの未保存 + ここで確定待ちの欄）。
  // テレインが無ければ失うものは無い（確定待ちの欄は黙って捨ててよい -
  // 新規は既定の大きさで作る仕様）。
  function terrainUnsaved() {
    const cur = currentTerrain();
    return !!cur.name && (cur.dirty || Object.keys(terrainPatch).length > 0);
  }
  // 名前に使える文字が 1 つも無かったときの知らせ（cleanAssetName の結果が空）。
  const TERRAIN_NAME_RULE = '名前に使えるのは英数字と _ - だけです';

  // ---- ノイズで生成と取り消し（World 節のテレイン欄）------------------------
  // 値は送るときだけ使い、テレインには覚えない（サーバーは送られなかった
  // キーを既定値にし、整数の丸めもする）。次に開いたときのために、この
  // ブラウザの localStorage に覚えておく。欄が空ならそのキーは送らない。
  const TERRAIN_NOISE_FIELDS = {
    seed: 'edTerrainSeed', octaves: 'edTerrainOct', gain: 'edTerrainGain',
    lacunarity: 'edTerrainLac', scale: 'edTerrainScale'
  };
  const TERRAIN_NOISE_STORE = 'terrain:noise';
  function generateTerrainNoise() {
    const g = {};
    for (const key in TERRAIN_NOISE_FIELDS) {
      const v = num(TERRAIN_NOISE_FIELDS[key], NaN);
      if (Number.isFinite(v)) g[key] = v;
    }
    try { localStorage.setItem(TERRAIN_NOISE_STORE, JSON.stringify(g)); }
    catch (e) { /* プライベートモード等: 覚えないだけ */ }
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
  try {
    const saved = JSON.parse(localStorage.getItem(TERRAIN_NOISE_STORE) || '{}');
    for (const key in TERRAIN_NOISE_FIELDS) {
      if (saved && Number.isFinite(saved[key])) {
        setField(TERRAIN_NOISE_FIELDS[key], saved[key]);
      }
    }
  } catch (e) { /* 読めなければ空 = 既定値のまま */ }

  // ---- テレインのタイル（アセットパネル）------------------------------------
  // 一覧は /scene の terrainFiles（assets/terrains/*.terrain の見出し）。
  // 今のテレインは .sel、保存していない変更があれば名前に *。まだファイルの
  // 無い新規（保存前）は一覧に無いので、今のテレインのぶんを先頭に足す。
  // 単クリックは何もしない（開くのはダブルクリック、右クリックはメニュー）。
  function terrainTileEntries() {
    const files = (sceneData && sceneData.terrainFiles) || [];
    const cur = currentTerrain();
    const list = files.slice();
    if (cur.name && !files.some((f) => sameTerrainName(f.name, cur.name))) {
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
      '@' + e.cur.name + (e.cur.dirty ? '*' : '');
  }
  function renderTerrainTiles() {
    const e = terrainTileEntries();
    let html = '';
    for (const f of e.list) {
      const cur = !!e.cur.name && sameTerrainName(f.name, e.cur.name);
      const dirty = cur && e.cur.dirty;
      const title = '解像度 ' + f.resolution + ' ／ 全幅 ' + round2(f.size) +
        ' m ／ 高さ ' + round2(f.height) + ' m' + (dirty ? '（未保存）' : '') +
        '\nダブルクリック: 開く ／ 右クリック: メニュー（名前の変更）';
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
    const isCur = sameTerrainName(name, currentTerrain().name);
    const tileNow = () => Array.from(
      document.querySelectorAll('#assetList [data-terrain]'))
      .find((el) => el.dataset.terrain === name);
    return [
      { head: '⛰ ' + name },
      { label: '✎ 名前の変更', onClick: () => startTerrainRename(tileNow()) },
      { label: '開く', hint: isCur ? '開いています' : 'ダブルクリック',
        disabled: isCur, onClick: () => openTerrain(name) },
    ];
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
    const onEl = document.getElementById('edTerrainOn');
    const name = prompt('新しいテレインの名前（英数字と _ -）',
                        (sceneData && sceneData.sceneFile) || 'terrain');
    const clean = name === null ? '' : cleanAssetName(name);
    if (name !== null && !clean) showAssetStatus(TERRAIN_NAME_RULE);
    const extra = clean
      ? confirmReplaceTerrain('新しいテレイン「' + clean + '」を作ります') : null;
    if (!extra) {
      // 取り消したらチェックも今の状態へ戻す。
      if (onEl) onEl.checked = !!currentTerrain().name;
      return;
    }
    discardTerrain();
    send('edit.terrain', Object.assign({ new: clean }, extra));
  }
  // 開く（確認は confirmReplaceTerrain、最後の守りはサーバー）。
  function openTerrain(name) {
    if (!name || terrainRenameActive()) return;
    if (sameTerrainName(name, currentTerrain().name)) return;  // 開いている
    const extra = confirmReplaceTerrain('「' + name + '」を開きます');
    if (!extra) return;
    discardTerrain();
    send('edit.terrain', Object.assign({ open: name }, extra));
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
  }
  window.addEventListener('focus', refreshTerrainFiles);
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible') refreshTerrainFiles();
  });
