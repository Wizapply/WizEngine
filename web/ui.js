// Charon web UI: UI（Canvas と要素。XML の <canvas>）の階層一覧の行と選択。app.js の
// 後ろに読む古典スクリプトで、app.js のトップレベル（send / myEditorSel / sceneItemRow /
// ndEsc / sceneData など）をそのまま使う。ここの関数もグローバルのままにする（app.js の
// renderHierarchy / pickHierarchyRow / 右クリックが名前で呼ぶ）。
// サーバーの /scene の ui = { canvases, selected, schema }（scene/UiJson.h）。選択は
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
  // （種類は schema の種類の表から。共用 UI の中身には足さない）・削除。
  function uiRowMenuItems(row) {
    const canvas = parseInt(row.dataset.uiCanvas, 10);
    const element = parseInt(row.dataset.uiElement, 10);
    const label = row.querySelector('span:nth-child(2)');
    const selected = uiRowSelected(canvas, element);
    const u = uiData();
    const c = u && (u.canvases || []).find((x) => x.index === canvas);
    const shared = !!(c && c.ui);
    const items = [
      { head: label ? label.textContent : 'UI' },
      { label: selected ? '選択を解除' : '選択',
        onClick: () => selectUi(selected ? -1 : canvas, element) },
    ];
    if (!shared) {
      for (const k of ((u && u.schema && u.schema.kinds) || [])) {
        items.push({ label: '＋ ' + k.name + (element < 0 ? ' を追加' : ' を子に追加'),
                     onClick: () => uiAddElement(canvas, k.name, element) });
      }
    }
    if (element < 0 || !shared) {
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
    if (hasChild && !confirm('この要素を中の要素ごと削除しますか？')) return;
    send('edit.ui.element.remove', { canvas: canvas, element: element });
  }
  // Delete / Backspace キー（app.js の deleteSelection から）: 選んでいる UI を消す。
  function removeUiSelection() {
    const s = myEditorSel();
    if (s.kind !== 'canvas') return;
    uiRemove(s.index, (s.sub === undefined) ? -1 : s.sub);
  }
