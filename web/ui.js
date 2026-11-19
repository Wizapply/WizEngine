// Charon web UI: UI（Canvas と要素。XML の <canvas>）の階層一覧の行と選択。app.js の
// 後ろに読む古典スクリプトで、app.js のトップレベル（send / myEditorSel / sceneItemRow /
// ndEsc / sceneData など）をそのまま使う。ここの関数もグローバルのままにする（app.js の
// renderHierarchy / pickHierarchyRow / 右クリックが名前で呼ぶ）。
// サーバーの /scene の ui = { canvases, assets, selected, schema }（scene/UiJson.h）。
// 宣言した UI（assets）はアセットパネルのタイル、共用（グローバル）の操作は app.js の
// edit.lib.*（kind = ui）。要素を編集してよいかはサーバーが送る editable だけで決める。選択は
// editorSel の kind "canvas"（index = Canvas の番号、sub = 要素の id。-1 = Canvas そのもの）。
// Inspector の欄は ui-inspector.js。編集は edit.ui.*（サーバーの scene/UiOps.h）。

  function uiData() {
    return (sceneData && sceneData.ui) ? sceneData.ui : null;
  }
  // 種類ごとの一覧のアイコン（サーバーの種類の表 = schema.kinds の icon。知らない種類は •）。
  function uiKindIcon(kind) {
    const u = uiData();
    const k = ((u && u.schema && u.schema.kinds) || []).find((x) => x.name === kind);
    return (k && k.icon) ? ndEsc(k.icon) : '•';
  }
  // 表示名はサーバーが label として送る（名前の無い物の呼び方はサーバーの 1 か所）。

  // この行（Canvas の番号、要素の id。-1 = Canvas）が今の選択か。
  function uiRowSelected(canvas, element) {
    const s = myEditorSel();
    const sub = (s.sub === undefined) ? -1 : s.sub;
    return s.kind === 'canvas' && s.index === canvas && sub === element;
  }

  // 階層一覧の UI の行: Canvas の行と、その下に要素を文書順に、親子の段で字下げして並べる。
  // 段（depth）と親を引けなかった印（parentMissing）はサーバーの scene/UiJson が配置の規則
  // （ui/UiLayout の parentIndices）から作る。canPick / color は地面・テレインの行と同じ。
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
      for (const e of (c.elements || [])) {
        // 段と「親を引けなかった」の印はサーバーが配置と同じ規則で作って送る（ここは出すだけ）。
        const note = e.parentMissing ? ' · 親 #' + e.parent + ' を引けない（直下に置く）' : '';
        html += sceneItemRow({
          kind: 'canvas', icon: uiKindIcon(e.kind), label: ndEsc(e.label),
          sub: ndEsc(e.kind) + note, title: 'クリックで選択（要素の性質を見る）',
          selected: uiRowSelected(c.index, e.id), canPick: canPick, color: color,
          attrs: attrs(e.id), depth: e.depth || 1 });
      }
    }
    return html;
  }

  // このページから UI を選べないときの理由（選べるなら空）。選べるかはサーバーの判定
  // （editsUiFrom）を /scene の editsUi でそのまま受け、ここは理由の文を出し分けるだけ。
  function uiSelectRefusal() {
    if (sceneData && sceneData.editsUi) return '';
    if (prefabEdit()) return 'プレハブの編集中は UI を選べません（◀ 戻る で抜けてから）';
    return 'UI はエディタモードの Editor Camera のページでだけ選べます';
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
    const items = [{ head: label ? label.textContent : 'UI' }];
    // このページから UI を選べない・編集できない（サーバーの editsUiFrom = /scene の editsUi）
    // ときは、押しても何も起きない項目を出さず、理由だけを出す。
    const refusal = uiSelectRefusal();
    if (refusal) {
      items.push({ label: refusal, disabled: true });
      return items;
    }
    items.push({ label: selected ? '選択を解除' : '選択',
                 onClick: () => selectUi(selected ? -1 : canvas, element) });
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
  // mode = 'world' なら空間に立てる板（置き場所はサーバーがエディタカメラの前に決める）。
  // 省略なら映像の上に重ねる Canvas。
  function uiAddCanvas(mode) {
    send('edit.ui.add', mode ? { attrs: { mode: mode } } : {});
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
  // サーバーは edit.lib.share / unshare / import / delete（kind = ui。判断の流れは
  // scene/LibraryFlow、UI の部品は scene/UiLibraryOps）。名前の正規化と重なりの判定もサーバー。
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
    // グローバルにする / ローカルにするの項目はタイルの右クリックと同じ（app.js の
    // libraryMenuItems）。Canvas の行からは、どの UI のことかを見出しに足す。
    const item = libraryMenuItems('ui', c.ui, a.file)[0];
    item.label += '（UI ' + c.ui + '）';
    return item;
  }
  // 自分の中身の Canvas を共用 UI にする（名前の既定は Canvas の名前）。名前はサーバーと
  // 同じ規則で正規化してから、上書きの確認（shareLibraryAsset）に渡す。正規化で全部落ちた
  // 名前は打った字のまま送る（断る理由の文はサーバーの 1 か所 = libstatus::unusableName）。
  function uiShareCanvas(c) {
    const name = prompt('共用 UI の名前（英数字と _ -。assets/' + LIB_DIR.ui +
                        '/<名前>.xml に書きます）', c.name || 'ui');
    if (name === null) return;
    const clean = cleanAssetName(name);
    shareLibraryAsset('ui', clean || name, { canvas: c.index });
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

  // ---- UI のノード（イベントのノードエディタ。app.js の ndNodeHtml が呼ぶ）--------------
  // onUiClick（ボタンが押されたら）と setUiProp（要素の性質を書き換える）の本文。対象は Canvas の
  // 番号（target）、要素は名前（element）。setUiProp の値は性質の型で決まる欄（schema の
  // nodeField = サーバーの ui::nodeField: value / vec / color / text）に置く。候補（Canvas・
  // 要素の名前・性質）は /scene の ui から引く（性質の表はブラウザに持たない）。
  function uiNodeCanvasItems() {
    const u = uiData();
    return ((u && u.canvases) || []).map((c) => ({ v: c.index, t: c.label }));
  }
  // そのノードが指す Canvas の要素（target = -1 なら全 Canvas の要素）。
  // clickOnly = 押下を受け止める種類だけ（schema の kinds の click = サーバーの
  // ui::kindCatchesPointer）。「押されたら」の候補に使う。
  function uiNodeElements(n, clickOnly) {
    const u = uiData();
    const kinds = (u && u.schema && u.schema.kinds) || [];
    const clickable = (e) => kinds.some((k) => k.name === e.kind && k.click);
    const list = [];
    for (const c of ((u && u.canvases) || [])) {
      if (n.target >= 0 && c.index !== n.target) continue;
      for (const e of (c.elements || [])) {
        if (!clickOnly || clickable(e)) list.push(e);
      }
    }
    return list;
  }
  // SetUiProp が動かない理由（判定はサーバー = /scene の events のノードの unset。規則は
  // editor::uiPropNodeUnset で、実行と同じ）の表示の文。設定済み・知らない理由なら空。
  const UI_NODE_UNSET_TEXT = {
    prop: '性質を選んでください',
    canvas: 'Canvas を選ぶか「ボタンが押されたら」から繋いでください',
    element: '要素の名前を入れるか「ボタンが押されたら」から繋いでください',
  };
  function uiNodeUnsetReason(n) {
    return (n.unset && UI_NODE_UNSET_TEXT[n.unset]) || '';
  }
  function uiSchemaProp(name) {
    const u = uiData();
    return ((u && u.schema && u.schema.props) || []).find((p) => p.name === name) || null;
  }
  // 性質の候補: 名前で指した要素の種類が持つもの（引けなければ全部）。
  function uiNodePropNames(n) {
    const u = uiData();
    const schema = (u && u.schema) || {};
    const e = n.element ? uiNodeElements(n).find((x) => x.name === n.element) : null;
    const kind = e && (schema.kinds || []).find((k) => k.name === e.kind);
    if (kind) return kind.props || [];
    return (schema.props || []).map((p) => p.name);
  }
  // 組み直しの目印（Canvas と要素の名前が変わったらノードエディタを組み直す）。
  function uiNodeOptKey() {
    const u = uiData();
    return ((u && u.canvases) || []).map((c) => c.index + ':' + c.label + ':' +
      (c.elements || []).map((e) => e.name + '/' + e.kind).join(',')).join(';');
  }
  // 性質を選び直したら、その性質の型の欄に既定値を入れて送る（前の性質の値を持ち越さない）。
  function uiNodePropChange(id, name) {
    // 欄の既定値はサーバーが欄の形で送る（schema の nodeDef = ui::nodeFieldsFromValue）。
    const p = uiSchemaProp(name);
    ndPatch(id, Object.assign({ prop: name }, (p && p.nodeDef) || {}));
  }
  // 値の欄（性質の型で決まる 1 つ）。
  function uiNodeValueHtml(n, p) {
    const id = n.id;
    if (!p) return '';  // 性質を選んでいない（未設定の印は uiNodeBody）
    if (p.nodeField === 'value' && p.type === 'bool') {
      return ndRow('値', '<input type="checkbox"' + (n.value ? ' checked' : '') +
                   ' onchange="ndPatch(' + id + ',{value:this.checked?1:0})">');
    }
    if (p.nodeField === 'value') {
      return ndRow('値', '<input class="num" type="number" step="any" value="' + (n.value || 0) +
                   '" onchange="ndPatch(' + id + ',{value:parseFloat(this.value)||0})">');
    }
    if (p.nodeField === 'vec') {
      // 欄の並びは app.js の ndVecChange が読む形（x y z。2 成分の型の z は隠した 0）。
      const count = p.components || 3;
      let h = '<span class="ndVec">';
      for (const ax of ['x', 'y', 'z']) {
        const shown = ax !== 'z' || count === 3;
        h += '<input class="num" type="' + (shown ? 'number' : 'hidden') + '" step="any" value="' +
             (shown ? n.vec[ax] : 0) + '" title="' + ax.toUpperCase() + '" data-vec="' + ax +
             '" onchange="ndVecChange(' + id + ',this)">';
      }
      return ndRow('値', h + '</span>');
    }
    if (p.nodeField === 'color') {
      return ndRow('色', '<input class="col" type="color" value="' + n.color +
                   '" onchange="ndPatch(' + id + ',{color:this.value})">');
    }
    if (p.type === 'enum' && Array.isArray(p.enum)) {
      let h = '<select class="strSel" onchange="ndPatch(' + id + ',{text:this.value})">';
      for (const name of p.enum) {
        h += '<option value="' + ndEsc(name) + '"' + (n.text === name ? ' selected' : '') + '>' +
             ndEsc(name) + '</option>';
      }
      return ndRow('値', h + '</select>');
    }
    return ndRow('値', '<input type="text" value="' + ndEsc(n.text || '') +
                 '" title="' + ndEsc(p.label || p.name) + '" onchange="ndPatch(' + id +
                 ',{text:this.value})">');
  }
  // ノードの本文（ndNodeHtml の def.target === 'canvas'）。
  function uiNodeBody(n) {
    const click = n.kind === 'onUiClick';
    let h = ndRow('Canvas', ndSelHtml(n.id, 'target', uiNodeCanvasItems(), n.target,
                                      [[-1, click ? '(どれでも)' : '(押された Canvas)']]));
    const listId = 'uiNodeEl' + n.id;
    let options = '';
    for (const e of uiNodeElements(n, click)) {
      if (e.name) options += '<option value="' + ndEsc(e.name) + '">';
    }
    h += ndRow('要素', '<input type="text" list="' + listId + '" value="' + ndEsc(n.element || '') +
               '" placeholder="' + (click ? '(どれでも)' : '(押された要素)') +
               '" onchange="ndPatch(' + n.id + ',{element:this.value})">' +
               '<datalist id="' + listId + '">' + options + '</datalist>');
    if (click) return h;
    let sel = '<select class="strSel" onchange="uiNodePropChange(' + n.id + ',this.value)">';
    const names = uiNodePropNames(n);
    if (!names.includes(n.prop)) {
      sel += '<option value="' + ndEsc(n.prop || '') + '" selected>' +
             (n.prop ? '? ' + ndEsc(n.prop) : '(選んでください)') + '</option>';
    }
    for (const name of names) {
      const p = uiSchemaProp(name);
      sel += '<option value="' + ndEsc(name) + '"' + (n.prop === name ? ' selected' : '') + '>' +
             ndEsc(p && p.label ? p.label + ' (' + name + ')' : name) + '</option>';
    }
    h += ndRow('性質', sel + '</select>');
    h += uiNodeValueHtml(n, uiSchemaProp(n.prop));
    const unset = uiNodeUnsetReason(n);
    if (unset) h += ndRow('', '<span class="ndUnset" title="' + ndEsc(unset) + '">⚠ 未設定: ' +
                          ndEsc(unset) + '</span>');
    return h;
  }
