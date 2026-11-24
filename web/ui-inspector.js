// Charon web UI: Inspector の secUi 節（選択した UI の Canvas か要素の設計値と、その編集）。
// app.js と ui.js の後ろに読む古典スクリプト（app.js の ndEsc / send、ui.js の uiData /
// uiAddElement / uiRemove を使う）。表示名はサーバーが送る label をそのまま出す。
//
// 欄はサーバーの schema（性質の表・種類の表・Canvas の属性の表から作ったもの。
// scene/UiJson.h）を走って組む = 性質ごとの名前をここに書き並べない（性質・種類・
// 属性を足しても、この節の変更は 0）。欄の形は schema の type で決め、lo / hi は欄の
// 見た目（min / max）にだけ使う - 丸めはサーバー（scene/UiOps.h）が持つ。欄は離れた
// とき（change）に、その 1 つの値だけを送る（edit.ui.set / edit.ui.element.set の部分更新）。
// 表示だけにするのは、サーバーが editable = false と送った要素（読めなかった共用 UI の
// 中身など。規則はサーバーの uiops::elementsEditable）と、書き換えられない属性（schema の
// editable = false）。共用 UI の要素も編集できる（同じ UI を使う全 Canvas に効き、共用の
// ファイルへは保存で書く）。

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
  // uiColorToHex の逆（"#rrggbb" と不透明度 a → [r,g,b,a] のリニア値）。読めない色は null。
  function uiHexToColor(hex, a) {
    const m = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex || '');
    if (!m) return null;
    return [parseInt(m[1], 16) / 255, parseInt(m[2], 16) / 255, parseInt(m[3], 16) / 255,
            Number.isFinite(a) ? a : 1];
  }

  function uiFormatNumber(v) {
    return Number.isFinite(v) ? String(Math.round(v * 1000) / 1000) : '—';
  }
  // 数の欄に入れる字（有効数字 10 桁で丸めて、0.30000000000000004 のような計算の端数を
  // 出さない。表示だけの行の uiFormatNumber より細かい = 欄から送り返しても値がほぼ変わらない）。
  // 整数の欄（isInt）は丸めない（10 桁を超える整数の字が変わるため）。
  function uiFieldNumber(v, isInt) {
    return isInt ? String(v) : String(Number(v.toPrecision(10)));
  }
  // 打った数を schema の lo / hi に収めて欄に出す（見た目だけ。送る値の丸めはサーバー）。
  // 丸めた結果が送る前の値と同じでも、範囲の外の字が欄に残らない。
  function uiShowInRange(row, spec) {
    if (!(spec.hi > spec.lo)) return;
    for (const el of row.querySelectorAll('input[type="number"][data-ui-field]')) {
      const v = parseFloat(el.value);
      if (!Number.isFinite(v)) continue;
      const clamped = Math.min(spec.hi, Math.max(spec.lo, v));
      if (clamped !== v) el.value = uiFieldNumber(clamped, spec.type === 'int');
    }
  }
  // 値 → 表示の HTML（型は schema の type。表示だけの行に使う）。
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
  function uiReadOnlyRowHtml(spec, v, isDefault) {
    return '<div class="edRow uiRow"><span>' + ndEsc(spec.label) + '</span>' +
           '<span class="uiVal' + (isDefault ? ' uiDefault' : '') + '"' +
           (isDefault ? ' title="既定値（XML に書いていない）"' : '') + '>' +
           uiFormatValue(spec, v) + '</span></div>';
  }

  // ---- 編集できる欄 ---------------------------------------------------------------
  // 数値の成分の数と見出し（型 → 欄の並び）。
  const UI_COMPONENTS = {
    vec2: ['X', 'Y'], vec3: ['X', 'Y', 'Z'], euler: ['X', 'Y', 'Z'],
    rect: ['X0', 'Y0', 'X1', 'Y1'],
  };
  // 数の欄の min / max / step（見た目だけ。丸めはサーバー）。
  function uiNumberAttrs(spec) {
    const lim = (k, v) => (Number.isFinite(v) && Math.abs(v) < 1e5 ? ' ' + k + '="' + v + '"' : '');
    const ranged = spec.hi > spec.lo;
    return ' step="' + (spec.type === 'int' ? '1' : 'any') + '"' +
           (ranged ? lim('min', spec.lo) + lim('max', spec.hi) : '');
  }
  // 1 行の欄（name は性質 / 属性の名前。data-ui-field で送り先を見分ける）。
  function uiFieldHtml(spec) {
    const n = ' data-ui-field="' + ndEsc(spec.name) + '"';
    switch (spec.type) {
      case 'number': case 'int':
        return '<input class="num" type="number"' + uiNumberAttrs(spec) + n + '>';
      case 'vec2': case 'vec3': case 'euler': case 'rect': {
        const labels = UI_COMPONENTS[spec.type];
        return '<span class="uiVec" style="--n:' + labels.length + '">' +
          labels.map((l, k) => '<label class="trCell"><span class="axis">' + l + '</span>' +
            '<input class="num" type="number"' + uiNumberAttrs(spec) + n +
            ' data-ui-k="' + k + '"></label>').join('') + '</span>';
      }
      case 'color':
        return '<span class="uiColor"><input class="col" type="color"' + n + '>' +
          '<input class="num" type="number" min="0" max="1" step="0.05" title="不透明度"' +
          n + ' data-ui-k="3"></span>';
      case 'bool':
        return '<input type="checkbox"' + n + '>';
      case 'enum':
        return '<select class="txt"' + n + '>' +
          (spec.enum || []).map((e) => '<option value="' + ndEsc(e) + '">' + ndEsc(e) + '</option>')
            .join('') + '</select>';
      case 'text':
        return '<textarea class="txt" rows="2"' + n + '></textarea>';
      case 'indexList':
        return '<input class="txt" type="text" placeholder="空 = 全部" title="空白区切りの番号"' + n + '>';
      default:  // name / path
        return '<input class="txt" type="text"' + n + '>';
    }
  }
  // 行（見出し + 欄 + 既定値へ戻すボタン）。resettable = 要素の性質（書いていない状態がある）。
  function uiEditRowHtml(spec, resettable) {
    return '<div class="edRow uiRow" data-ui-row="' + ndEsc(spec.name) + '"><span>' +
      ndEsc(spec.label) + '</span><span class="uiEdit">' + uiFieldHtml(spec) +
      (resettable ? '<button class="uiReset" data-ui-reset="' + ndEsc(spec.name) +
                    '" title="既定値に戻す（XML に書かない）">↺</button>' : '') +
      '</span></div>';
  }

  // 欄へ値を入れる（フォーカス中の欄は触らない = 打っている途中の値を消さない。force は
  // 送った欄へサーバーの戻り（丸めた値・正規化した名前）を 1 回だけ入れ直すとき）。
  function uiSetFieldValue(el, spec, v, force) {
    if (el === document.activeElement && !force) return;
    // 値が無い（uiSelectedValue が null / undefined = 既定値が届かない種類）なら欄を空にする
    // （前の選択の値を残さない。色の欄は空にできないので黒）。
    if (v === undefined || v === null) {
      if (el.type === 'checkbox') el.checked = false;
      else el.value = '';
      el.classList.remove('uiBad');
      return;
    }
    const k = el.dataset.uiK !== undefined ? parseInt(el.dataset.uiK, 10) : -1;
    switch (spec.type) {
      case 'number': case 'int':
        el.value = Number.isFinite(v) ? uiFieldNumber(v, spec.type === 'int') : '';
        break;
      case 'vec2': case 'vec3': case 'euler': case 'rect':
        el.value = Array.isArray(v) && Number.isFinite(v[k]) ? uiFieldNumber(v[k], false) : '';
        break;
      case 'color':
        if (!Array.isArray(v)) break;
        el.value = k === 3 ? uiFieldNumber(v.length > 3 ? v[3] : 1, false) : uiColorToHex(v);
        break;
      case 'bool':
        el.checked = !!v;
        break;
      case 'indexList':
        // 正しい並びは番号の配列、正しくない並びは書かれた文字列のまま（そのまま
        // 送り返せる = サーバーは文字列も同じ規則で読む）。
        el.value = Array.isArray(v) ? v.join(' ') : String(v || '');
        el.classList.toggle('uiBad', !Array.isArray(v));
        break;
      default:
        el.value = v === undefined || v === null ? '' : String(v);
    }
  }
  // 欄 → 送る値（読めなければ undefined = 送らない。次のポーリングで元に戻る）。current は
  // いまの値（組の欄の一部が読めないとき、その成分に使う）。
  function uiFieldValue(row, spec, current) {
    const els = row.querySelectorAll('[data-ui-field]');
    const first = els[0];
    switch (spec.type) {
      case 'number': case 'int': {
        const v = parseFloat(first.value);
        return Number.isFinite(v) ? v : undefined;
      }
      case 'vec2': case 'vec3': case 'euler': case 'rect': {
        const v = Array.from(els).map((e) => parseFloat(e.value));
        return v.every(Number.isFinite) ? v : undefined;
      }
      case 'color': {
        // 不透明度の欄が空・不正なら今の不透明度のまま（1 にしない）。
        const typed = parseFloat(els[1].value);
        const now = Array.isArray(current) && current.length > 3 ? current[3] : undefined;
        const a = Number.isFinite(typed) ? typed : now;
        if (!Number.isFinite(a)) return undefined;
        return uiHexToColor(first.value, a) || undefined;
      }
      case 'bool':
        return first.checked;
      default:  // enum / text / name / path / indexList（並びは文字列のまま送る）
        return first.value;
    }
  }

  let uiInspectorKey = '';  // 前回組んだ欄の形（同じなら値だけを入れ直す）
  let uiInspectorSel = null;  // 欄を組んだときの選択（送り先）
  // 送った欄（サーバーの戻りを 1 回入れ直す）: {canvas, element, row, version, sent, until}。
  // version = 送ったときの設計値の版（進んだら戻りが届いた）、sent = 送ったときの欄の字
  // （その後に打ち直していたら入れ直さない）。
  let uiPendingField = null;
  const UI_PENDING_MS = 3000;  // 戻りを待つ長さ（版が進まない = 何も変わらなかった、の打ち切り）
  function uiFieldTexts(row) {
    return JSON.stringify(Array.from(row.querySelectorAll('[data-ui-field]'))
      .map((el) => (el.type === 'checkbox' ? el.checked : el.value)));
  }
  // 選択の今の値（唯一の定義）: Canvas なら属性、要素なら書いてあればその値・無ければ
  // 既定値（要素ごとに決まる既定 = サーバーの defaults、例: ボタンの hover は背景から作る色。
  // 無ければ schema の既定値）。名前の欄（'#name'）は名前。
  function uiSelectedValue(sel, spec) {
    if (spec.name === '#name') return sel.name || '';
    if (sel.element < 0) return (sel.attrs || {})[spec.name];
    const own = (obj) => Object.prototype.hasOwnProperty.call(obj || {}, spec.name);
    if (own(sel.props)) return sel.props[spec.name];
    return own(sel.defaults) ? sel.defaults[spec.name] : spec.def;
  }
  // この行に、自分が送った操作の戻りを入れ直すか（フォーカス中でも 1 回だけ）。入れ直すのは
  // 戻った値が送る前の値とも送った値とも違うとき = サーバーが丸めた・正規化したとき
  // だけ。送る前の値のままなら、まだ届いていない（別の変更で版が進んだだけ）ので待つ。
  // 送った値のままなら欄はもう正しいので覚えを捨てるだけ。待ち切った（丸めた結果が送る前の
  // 値と同じ = 上限の欄に上限超えを打った、など）ときも 1 回入れ直す（打った字を残さない）。
  function uiTakePendingRow(u, sel, row, now) {
    const p = uiPendingField;
    if (!p) return false;
    const sameSel = p.canvas === sel.canvas && p.element === sel.element;
    if (!sameSel) {
      if (Date.now() > p.until) uiPendingField = null;  // 別の物を選んだまま待ち切った
      return false;
    }
    if (p.row !== row.dataset.uiRow) return false;  // この行の番が来るまで残す
    if (Date.now() > p.until) {
      // 待ち切った: サーバーが当てていれば（版が進んだ）1 回入れ直す。まだ当てていなければ
      // 入れ直さない（古い値に戻さない。届けば次の版で欄は普段どおり入れ直される）。
      uiPendingField = null;
      return u.version !== p.version && uiFieldTexts(row) === p.sent;
    }
    if (u.version === p.version) return false;
    const got = JSON.stringify(now);
    if (got === p.before) return false;
    uiPendingField = null;
    if (got === p.sentValue) return false;
    return uiFieldTexts(row) === p.sent;  // 送った後に打ち直していなければ
  }
  // show = 選択が UI のとき（app.js の renderEditor が決める）。
  function renderUiInspector(show) {
    const sec = document.getElementById('secUi');
    const u = uiData();
    const sel = (show && u && u.selected) ? u.selected : null;
    sec.hidden = !sel;
    if (!sel) { uiInspectorKey = ''; uiInspectorSel = null; return; }
    const canvas = (u.canvases || []).find((c) => c.index === sel.canvas);
    const canvasName = canvas ? canvas.label : sel.canvasLabel;
    const schema = u.schema || { props: [], kinds: [], canvas: [] };
    const isCanvas = sel.element < 0;
    const readOnly = !isCanvas && !sel.editable;  // サーバーが編集させない要素は表示だけ
    const kind = isCanvas ? null : schema.kinds.find((k) => k.name === sel.kind);
    // Canvas は今の表示方法に効く属性だけ（効く表示方法はサーバーの表 = schema の modes）。
    const mode = isCanvas ? (sel.attrs || {}).mode : '';
    const specs = isCanvas
      ? schema.canvas.filter((s) => !s.modes || s.modes.indexOf(mode) >= 0)
      : (kind ? kind.props : []).map((n) => schema.props.find((p) => p.name === n)).filter((p) => p);

    document.getElementById('uiSelLabel').textContent =
      (isCanvas ? '🖼 ' + canvasName
                : canvasName + ' / ' + sel.label) +
      (sel.shared ? '（共用 UI）' : '');

    // 欄の形（どの選択の・どの欄か・共用 UI か・グローバルか = ボタン）が変わったときだけ
    // 組み直す。表示だけの行は値も形に入れる（中身ごと組み直す）。
    const key = [sel.canvas, sel.element, sel.kind || '', mode, readOnly, canvas ? canvas.ui : '',
                 canvas ? canvas.editable : '', uiSharedFileOf(canvas),
                 readOnly ? JSON.stringify(sel.props) : ''].join('|');
    const props = sel.props || {};
    const has = (name) => Object.prototype.hasOwnProperty.call(props, name);
    const valueOf = (spec) => uiSelectedValue(sel, spec);
    const root = document.getElementById('uiProps');
    if (key !== uiInspectorKey) {
      uiInspectorKey = key;
      let html = '';
      if (readOnly) {
        for (const spec of specs) html += uiReadOnlyRowHtml(spec, valueOf(spec), !has(spec.name));
      } else {
        html += uiEditRowHtml({ name: '#name', label: '名前', type: 'name' }, false);
        for (const spec of specs) {
          html += (isCanvas && !spec.editable)
            ? uiReadOnlyRowHtml(spec, valueOf(spec), false)
            : uiEditRowHtml(spec, !isCanvas);
        }
      }
      html += uiActionsHtml(schema, sel, canvas);
      root.innerHTML = html;
    }
    uiInspectorSel = sel;
    if (!readOnly) {
      for (const row of root.querySelectorAll('[data-ui-row]')) {
        const name = row.dataset.uiRow;
        const spec = name === '#name' ? { name: name, type: 'name' } : specs.find((s) => s.name === name);
        if (!spec) continue;
        const v = valueOf(spec);
        const force = uiTakePendingRow(u, sel, row, v);
        for (const el of row.querySelectorAll('[data-ui-field]')) uiSetFieldValue(el, spec, v, force);
        // 要素の性質は書いていなければ既定値（薄く出し、↺ を隠す）。
        const isDefault = !isCanvas && name !== '#name' && !has(name);
        row.classList.toggle('uiDefault', isDefault);
        const reset = row.querySelector('[data-ui-reset]');
        if (reset) reset.hidden = isDefault;
      }
    }
    document.getElementById('uiHint').textContent = readOnly
      ? 'この要素は編集できません（共用 UI のファイルが読めないか、UI の宣言がありません）。'
      : (isCanvas ? '値は欄を離れたときに送ります。'
                  : '薄い値は既定値（XML に書いていない値）です。↺ で既定値に戻します。') +
        (sel.shared && canvas ? ' 共用 UI（' + canvas.ui + '）の中身は、これを使う全部の Canvas に効き、' +
                      'グローバルならシーンの保存でファイルに書きます。' : '');
  }

  // Canvas が使う UI の file（空 = ローカルか、UI を使っていない）。共用にする / ローカルに
  // する で変わる = ボタンの組み直しの目印。
  function uiSharedFileOf(canvas) {
    const a = canvas && canvas.ui ? uiAsset(canvas.ui) : null;
    return (a && a.file) || '';
  }
  // 節の末尾のボタン（要素の追加・削除、Canvas なら中身の XML・共用）。追加は種類の表
  // （schema.kinds）から並べる。
  function uiActionsHtml(schema, sel, canvas) {
    const editable = !!(canvas && canvas.editable);
    let html = '';
    if (editable) {
      html += '<div class="edLabel">' + (sel.element < 0 ? '要素を追加' : '子の要素を追加') + '</div>' +
        '<div class="edRow btnRow">' + schema.kinds.map((k) =>
          '<button data-ui-add="' + ndEsc(k.name) + '">＋ ' + ndEsc(k.name) + '</button>').join('') +
        '</div>';
    }
    if (sel.element < 0 && canvas) {
      const share = uiShareMenuItem(canvas);
      html += '<div class="edRow btnRow">' +
        (editable ? '<button data-ui-xml="1" title="中身（要素とアニメ）を XML で直す">📄 XML で編集</button>' : '') +
        (share ? '<button data-ui-share="1" title="' + ndEsc(share.hint || '') + '">' +
                 ndEsc(share.label) + '</button>' : '') +
        '</div>';
    }
    if (sel.element < 0 || editable) {
      html += '<div class="edRow btnRow"><button class="danger" data-ui-remove="1">🗑 ' +
        (sel.element < 0 ? 'Canvas を削除' : '要素を削除') + '</button></div>';
    }
    return html;
  }

  // 欄の変更を送る（その 1 つの値だけ。部分更新）。
  function uiSendField(row) {
    const sel = uiInspectorSel;
    const u = uiData();
    if (!sel || !u) return;
    const name = row.dataset.uiRow;
    const isCanvas = sel.element < 0;
    const target = isCanvas ? { canvas: sel.canvas } : { canvas: sel.canvas, element: sel.element };
    const cmd = isCanvas ? 'edit.ui.set' : 'edit.ui.element.set';
    // before = 送る前の値、sentValue = 送った値（戻りの見分けに使う。uiTakePendingRow）。
    const remember = (before, value) => {
      uiPendingField = { canvas: sel.canvas, element: sel.element, row: name, version: u.version,
                         sent: uiFieldTexts(row), before: JSON.stringify(before),
                         sentValue: JSON.stringify(value), until: Date.now() + UI_PENDING_MS };
    };
    if (name === '#name') {
      const value = row.querySelector('[data-ui-field]').value;
      remember(uiSelectedValue(sel, { name: name }), value);
      send(cmd, Object.assign(target, { name: value }));
      return;
    }
    const specs = isCanvas ? u.schema.canvas : u.schema.props;
    const spec = specs.find((s) => s.name === name);
    if (!spec) return;
    const current = uiSelectedValue(sel, spec);
    const v = uiFieldValue(row, spec, current);
    if (v === undefined) return;
    const values = {};
    values[name] = v;
    uiShowInRange(row, spec);
    remember(current, v);
    send(cmd, Object.assign(target, isCanvas ? { attrs: values } : { props: values }));
  }

  (function wireUiInspector() {
    const root = document.getElementById('uiProps');
    if (!root) return;
    root.addEventListener('change', (e) => {
      const row = e.target.closest ? e.target.closest('[data-ui-row]') : null;
      if (row) uiSendField(row);
    });
    root.addEventListener('click', (e) => {
      const sel = uiInspectorSel;
      const b = e.target.closest ? e.target.closest('button') : null;
      if (!b || !sel) return;
      if (b.dataset.uiReset) {
        send('edit.ui.element.set',
             { canvas: sel.canvas, element: sel.element, reset: [b.dataset.uiReset] });
      } else if (b.dataset.uiAdd) {
        uiAddElement(sel.canvas, b.dataset.uiAdd, sel.element);
      } else if (b.dataset.uiRemove) {
        uiRemove(sel.canvas, sel.element);
      } else if (b.dataset.uiXml) {
        openUiXmlEditor(sel.canvas);
      } else if (b.dataset.uiShare) {
        const u = uiData();
        const c = u && (u.canvases || []).find((x) => x.index === sel.canvas);
        const share = c && uiShareMenuItem(c);
        if (share) share.onClick();
      }
    });
  })();
