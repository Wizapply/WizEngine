// Charon web UI: UI（Canvas と要素。XML の <canvas>）の階層一覧の行と選択。app.js の
// 後ろに読む古典スクリプトで、app.js のトップレベル（send / myEditorSel / sceneItemRow /
// ndEsc / sceneData など）をそのまま使う。ここの関数もグローバルのままにする（app.js の
// renderHierarchy / pickHierarchyRow / 右クリックが名前で呼ぶ）。
// サーバーの /scene の ui = { canvases, assets, selected, schema }（scene/UiJson.h）。
// 宣言した UI（assets）はアセットパネルのタイル、共用（グローバル）の操作は app.js の
// edit.lib.*（kind = ui）。要素を編集してよいかはサーバーが送る editable だけで決める。選択は
// editorSel の kind "canvas"（index = Canvas の番号、sub = 要素の id。-1 = Canvas そのもの）。
// Inspector の欄は ui-inspector.js。編集は edit.ui.*（サーバーの scene/UiOps.h）。

  // 種類ごとの一覧のアイコン（知らない種類は •）。
  const UI_KIND_ICON = { text: 'T', image: '▣', button: '◉' };

  function uiData() {
    return (sceneData && sceneData.ui) ? sceneData.ui : null;
  }
  // 表示名はサーバーが label として送る（名前の無い物の呼び方はサーバーの 1 か所）。

  // この行（Canvas の番号、要素の id。-1 = Canvas）が今の選択か。
  function uiRowSelected(canvas, element) {
    const s = myEditorSel();
    const sub = (s.sub === undefined) ? -1 : s.sub;
    return s.kind === 'canvas' && s.index === canvas && sub === element;
  }

  // 階層一覧の UI の行: Canvas の行と、その下に要素を親子の段で字下げして並べる
  // （要素は文書順 = 親は自分より前にある）。canPick / color は地面・テレインの行と同じ。
  function uiHierarchyRows(canPick, color) {
    const u = uiData();
    if (!u || !u.canvases) return '';
    let html = '';
    for (const c of u.canvases) {
      const attrs = (element) =>
        ' data-ui-canvas="' + c.index + '" data-ui-element="' + element + '"';
      html += sceneItemRow({
        kind: 'canvas', icon: '🖼&#xFE0E;',
        label: ndEsc(c.label) + (c.ui ? ' 🔗' : ''),
        // cameras に正しくない番号があると、どのカメラにも出ない（サーバーの規則）。
        sub: c.mode + (c.camerasValid ? '' : ' · カメラの指定が不正'),
        title: 'クリックで選択（Canvas の設定を見る）' + (c.ui ? '。共用 UI: ' + ndEsc(c.ui) : ''),
        selected: uiRowSelected(c.index, -1), canPick: canPick, color: color,
        attrs: attrs(-1), depth: 0 });
      const depthOf = {};  // 要素の id → 段
      for (const e of (c.elements || [])) {
        const d = (e.parent >= 0 && depthOf[e.parent] !== undefined) ? depthOf[e.parent] + 1 : 1;
        depthOf[e.id] = d;
        html += sceneItemRow({
          kind: 'canvas', icon: UI_KIND_ICON[e.kind] || '•', label: ndEsc(e.label),
          sub: ndEsc(e.kind), title: 'クリックで選択（要素の性質を見る）',
          selected: uiRowSelected(c.index, e.id), canPick: canPick, color: color,
          attrs: attrs(e.id), depth: d });
      }
    }
    return html;
  }

  // 選ぶ（canvas < 0 で解除）。判定はサーバー（エディタモード × エディタカメラのページ、
  // 消えた物は選ばない）。
  function selectUi(canvas, element) {
    send('select.ui', { canvas: canvas, element: element });
  }
  // 一覧の行のクリック（app.js の pickHierarchyRow から）。選んでいた行をもう一度押すと
  // 解除。戻り値 = 選んだか（app.js が先に行を光らせる）。
  function pickUiRow(row) {
    const canvas = parseInt(row.dataset.uiCanvas, 10);
    const element = parseInt(row.dataset.uiElement, 10);
    const on = !uiRowSelected(canvas, element);
    selectUi(on ? canvas : -1, element);
    return on;
  }
  // 一覧の行の右クリック（app.js の objList の contextmenu から）: 選択・要素の追加
  // （種類は schema の種類の表から。編集してよくない中身には足さない）・XML・共用・削除。
  function uiRowMenuItems(row) {
    const canvas = parseInt(row.dataset.uiCanvas, 10);
    const element = parseInt(row.dataset.uiElement, 10);
    const label = row.querySelector('span:nth-child(2)');
    const selected = uiRowSelected(canvas, element);
    const u = uiData();
    const c = u && (u.canvases || []).find((x) => x.index === canvas);
    const editable = !!(c && c.editable);
    const items = [
      { head: label ? label.textContent : 'UI' },
      { label: selected ? '選択を解除' : '選択',
        onClick: () => selectUi(selected ? -1 : canvas, element) },
    ];
    if (editable) {
      for (const k of ((u && u.schema && u.schema.kinds) || [])) {
        items.push({ label: '＋ ' + k.name + (element < 0 ? ' を追加' : ' を子に追加'),
                     onClick: () => uiAddElement(canvas, k.name, element) });
      }
    }
    if (element < 0 && c) {
      // XML のモーダルは選んでいる Canvas の中身を開くので、先に選ぶ。
      if (editable) {
        items.push({ label: '📄 中身を XML で編集', hint: '要素とアニメ',
                     onClick: () => { selectUi(canvas, -1); openUiXmlEditor(canvas); } });
      }
      const share = uiShareMenuItem(c);
      if (share) items.push(share);
    }
    if (element < 0 || editable) {
      items.push({ label: '🗑 削除', danger: true, onClick: () => uiRemove(canvas, element) });
    }
    return items;
  }

  // ---- 編集（サーバーが受けるのはエディタモード × エディタカメラのページだけ）--------
  // アセットパネルの Canvas タイル: overlay の Canvas を足す（サーバーが選択にする）。
  function uiAddCanvas() {
    send('edit.ui.add', {});
  }
  // 要素を足す（parent = 親の要素の id。-1 = Canvas の直下。サーバーが選択にする）。
  function uiAddElement(canvas, kind, parent) {
    send('edit.ui.element.add', { canvas: canvas, kind: kind, parent: parent });
  }
  // Canvas（element = -1）か要素を消す。中身ごと消えるもの（Canvas・子のある要素）は確かめる。
  function uiRemove(canvas, element) {
    const u = uiData();
    const c = u && (u.canvases || []).find((x) => x.index === canvas);
    if (!c) return;
    if (element < 0) {
      const n = c.ui ? 0 : (c.elements || []).length;
      if (n > 0 && !confirm(c.label + ' を中の要素 ' + n + ' 個ごと削除しますか？')) return;
      send('edit.ui.remove', { canvas: canvas });
      return;
    }
    const hasChild = (c.elements || []).some((e) => e.parent === element);
    if (c.ui && !confirm('共用 UI（' + c.ui + '）の中の要素です。これを使う全部の Canvas から' +
                         '消えます。削除しますか？')) return;
    if (hasChild && !confirm('この要素を中の要素ごと削除しますか？')) return;
    send('edit.ui.element.remove', { canvas: canvas, element: element });
  }
  // Delete / Backspace キー（app.js の deleteSelection から）: 選んでいる UI を消す。
  function removeUiSelection() {
    const s = myEditorSel();
    if (s.kind !== 'canvas') return;
    uiRemove(s.index, (s.sub === undefined) ? -1 : s.sub);
  }

  // ---- 宣言した UI と共用（グローバル）--------------------------------------------
  // サーバーは edit.lib.share / unshare / import / delete（kind = ui。scene/UiOps.h の
  // applyLibrary）。名前の正規化と重なりの判定もサーバー。
  function uiAsset(name) {
    const u = uiData();
    return ((u && u.assets) || []).find((a) => a.name === name) || null;
  }
  // Canvas を共用にする / やめる 1 項目（showContextMenu の形。出せなければ null）:
  //   自分の中身の Canvas … 中身を新しい共用 UI にする（名前を聞く。Canvas はその UI を使う）
  //   ローカルの UI を使う … その UI をグローバルにする
  //   グローバルの UI を使う … その UI をこのシーン専用にする
  function uiShareMenuItem(c) {
    if (!c.ui) {
      return { label: '🌐 共用 UI にする（全シーン共通）', hint: 'assets/' + LIB_DIR.ui + '/<名前>.xml',
               onClick: () => uiShareCanvas(c) };
    }
    const a = uiAsset(c.ui);
    if (!a) return null;  // 宣言が無い
    return a.file
      ? { label: '📍 UI ' + c.ui + ' をローカルにする', hint: 'グローバルのファイルは残ります',
          onClick: () => unshareLibraryAsset('ui', c.ui) }
      : { label: '🌐 UI ' + c.ui + ' をグローバルにする', hint: 'assets/' + LIB_DIR.ui + '/' + c.ui + '.xml',
          onClick: () => shareLibraryAsset('ui', c.ui) };
  }
  // 自分の中身の Canvas を共用 UI にする（名前の既定は Canvas の名前）。名前はサーバーと
  // 同じ規則で正規化してから、上書きの確認（shareLibraryAsset）に渡す。
  function uiShareCanvas(c) {
    const name = prompt('共用 UI の名前（英数字と _ -。assets/' + LIB_DIR.ui +
                        '/<名前>.xml に書きます）', c.name || 'ui');
    if (name === null) return;
    const clean = cleanAssetName(name);
    if (!clean) return;
    shareLibraryAsset('ui', clean, { canvas: c.index });
  }
  // 宣言した UI を表示する Canvas を足す（アセットパネルのタイルのクリック）。
  function uiAddCanvasWith(name) {
    send('edit.ui.add', { ui: name, name: name });
  }
  // アセットパネルの UI のタイル（app.js の renderAssets から。tile / badge は向こうの
  // 作り方）。戻り値 = { local, global, key }（key = 組み直しの目印）。
  function uiAssetTiles(tile, badge) {
    const u = uiData();
    const assets = (u && u.assets) || [];
    const lib = (sceneData && sceneData.library && sceneData.library.ui) || [];
    const out = { local: '', global: '', key: '' };
    out.key = assets.map((a) => a.name + ':' + a.elements + ':' + (a.file || '') +
                               (a.unreadable ? '!' : '')).join('|') + '~' + lib.join('|');
    const one = (a) => tile('data-uiasset="' + ndEsc(a.name) + '" onclick="uiAddCanvasWith(this.dataset.uiasset)"',
      'クリック: この UI を表示する Canvas を足す（' + a.elements + ' 要素）' +
      (a.file ? '\nグローバル（' + a.file + '。編集はシーンの保存でファイルに書きます）' : '') +
      (a.unreadable ? '\nファイルが読めません（中身は空・編集できません）' : '') +
      ' ／ 右クリック: メニュー',
      '🖼&#xFE0E;' + (a.file ? badge : ''), ndEsc(a.name), 'ui', a.unreadable ? 'lib' : '');
    const used = new Set(assets.map((a) => a.name));
    for (const a of assets) {
      if (a.file) out.global += one(a); else out.local += one(a);
    }
    for (const n of lib) {
      if (used.has(n)) continue;
      out.global += tile('data-ulib="' + ndEsc(n) + '" onclick="importLibraryAsset(\'ui\', this.dataset.ulib)"',
        'グローバル（assets/' + LIB_DIR.ui + '/' + n + '.xml）。このシーンではまだ使っていません\n' +
        'クリック: このシーンで使う ／ 右クリック: メニュー',
        '🖼&#xFE0E;' + badge, ndEsc(n), 'ui', 'lib');
    }
    return out;
  }
  // アセットパネルの UI のタイルの右クリック。
  function uiAssetMenuItems(name) {
    const a = uiAsset(name);
    const file = (a && a.file) || '';
    return [
      { head: (file ? '🌐 ' : '🖼 ') + name + (file ? '（グローバル）' : '') },
      { label: '＋ この UI を表示する Canvas を足す', onClick: () => uiAddCanvasWith(name) },
      { sep: true },
      ...libraryMenuItems('ui', name, file),
    ];
  }
