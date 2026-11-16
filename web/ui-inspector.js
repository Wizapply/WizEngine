// Charon web UI: Inspector の secUi 節（選択した UI の Canvas か要素の設計値）。app.js と
// ui.js の後ろに読む古典スクリプト（app.js の ndEsc、ui.js の uiData / uiCanvasLabel /
// uiElementLabel を使う）。
//
// 欄はサーバーの schema（性質の表・種類の表・Canvas の属性の表から作ったもの。
// scene/UiJson.h）を走って組む = 性質ごとの名前をここに書き並べない（性質・種類・
// 属性を足しても、この節の変更は 0）。いまは読み取り専用（編集は後のタスク）。

  // UI の色 [r,g,b,a]（リニア値 0..1）→ <input type="color"> と同じ "#rrggbb"。規則は
  // ほかのブラウザ API の色と同じ（リニア値をそのまま 0-255 へ四捨五入して写す。
  // サーバーの colorToHex（document/EditorTypes.h）/ parseHexColor（core/HexColor.h））。
  // a は別に持つ（<input type="color"> は不透明度を持てない）。
  function uiColorToHex(c) {
    const ch = (v) => {
      const i = Math.round((Number.isFinite(v) ? v : 0) * 255);
      return Math.min(255, Math.max(0, i)).toString(16).padStart(2, '0');
    };
    return '#' + ch(c[0]) + ch(c[1]) + ch(c[2]);
  }

  function uiFormatNumber(v) {
    return Number.isFinite(v) ? String(Math.round(v * 1000) / 1000) : '—';
  }
  // 値 → 表示の HTML（型は schema の type）。
  function uiFormatValue(spec, v) {
    if (v === undefined || v === null) return '—';
    switch (spec.type) {
      case 'number':
      case 'int':
        return uiFormatNumber(v);
      case 'vec2': case 'vec3': case 'euler': case 'rect':
        return Array.isArray(v) ? v.map(uiFormatNumber).join(', ') : '—';
      case 'color': {
        if (!Array.isArray(v)) return '—';
        const hex = uiColorToHex(v);
        const a = v.length > 3 ? v[3] : 1;
        return '<span class="uiSwatch" style="background:' + hex + '"></span>' +
               hex + ' · α ' + uiFormatNumber(a);
      }
      case 'bool':
        return v ? 'オン' : 'オフ';
      case 'indexList':
        // 正しくない並びは書かれた文字列のまま届く（そのときはどれも選ばない）。
        if (!Array.isArray(v)) return '<span class="uiBad">不正: ' + ndEsc(v) + '</span>';
        return v.length ? v.join(' ') : '（指定なし）';
      default:  // enum / text / name / path
        return v === '' ? '（なし）' : ndEsc(v);
    }
  }
  function uiRowHtml(spec, v, isDefault) {
    return '<div class="edRow uiRow"><span>' + ndEsc(spec.label) + '</span>' +
           '<span class="uiVal' + (isDefault ? ' uiDefault' : '') + '"' +
           (isDefault ? ' title="既定値（XML に書いていない）"' : '') + '>' +
           uiFormatValue(spec, v) + '</span></div>';
  }

  let uiInspectorKey = '';  // 前回描いた中身（同じなら DOM を触らない）
  // show = 選択が UI のとき（app.js の renderEditor が決める）。
  function renderUiInspector(show) {
    const sec = document.getElementById('secUi');
    const u = uiData();
    const sel = (show && u && u.selected) ? u.selected : null;
    sec.hidden = !sel;
    if (!sel) { uiInspectorKey = ''; return; }
    const canvas = (u.canvases || []).find((c) => c.index === sel.canvas);
    const canvasName = canvas ? uiCanvasLabel(canvas) : ('Canvas ' + sel.canvas);
    // 要素を選んでいる間も見出しに Canvas の名前を出すので、それも比べる。
    const key = JSON.stringify(sel) + '@' + canvasName;
    if (key === uiInspectorKey) return;
    uiInspectorKey = key;

    const schema = u.schema || { props: [], kinds: [], canvas: [] };
    let label;
    let html = '';
    if (sel.element < 0) {
      label = '🖼 ' + canvasName;
      for (const spec of schema.canvas) html += uiRowHtml(spec, sel.attrs[spec.name], false);
    } else {
      label = (canvasName + ' / ' + uiElementLabel({ name: sel.name, kind: sel.kind, id: sel.element }));
      const kind = schema.kinds.find((k) => k.name === sel.kind);
      const props = sel.props || {};
      for (const name of (kind ? kind.props : [])) {
        const spec = schema.props.find((p) => p.name === name);
        if (!spec) continue;
        const set = Object.prototype.hasOwnProperty.call(props, name);
        html += uiRowHtml(spec, set ? props[name] : spec.def, !set);
      }
    }
    document.getElementById('uiSelLabel').textContent =
      label + (sel.shared ? '（共用 UI）' : '');
    document.getElementById('uiProps').innerHTML = html;
    // 既定値を薄く出すのは要素の性質だけ（Canvas の属性は全部の値をそのまま出す）。
    document.getElementById('uiHint').textContent =
      (sel.element < 0 ? '' : '薄い字の値は既定値（XML に書いていない値）です。') +
      'いまは表示だけで、値は 📄 XML で編集します。';
  }
