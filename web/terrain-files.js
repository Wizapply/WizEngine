// Charon web UI: テレインのファイル（アセットパネルの ⛰ タイル・名前の変更・
// 右クリックのメニュー・新規 / 開く / 複製 / 削除 / シーンから外す・一覧の取り直し）。
// app.js と terrain.js の後ろに読む古典スクリプトで、app.js（send / round2 / ndEsc /
// cleanAssetName / ASSET_NAME_MAX / showAssetStatus / isEditorCam / selectTerrain /
// sceneData）と terrain.js（currentTerrain / isOpenTerrain / sameTerrainName /
// terrainUnsaved / discardTerrain）のトップレベルをそのまま使う。ここの関数も
// グローバルのままにする（index.html の onclick= と app.js の renderAssets
// などが名前で呼ぶため）。

  // 名前に使える文字が 1 つも無かったときの知らせ（cleanAssetName の結果が空）。
  const TERRAIN_NAME_RULE = '名前に使えるのは英数字と _ - だけです';

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
