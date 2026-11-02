// Charon web UI: テレイン欄の「番号付きの欄」の共通部品（塗り分けの層 = terrain-layers.js、
// 散布物の原型 = terrain-scatter.js）。欄の表から行を組み立て、サーバーの値を入れ、
// 変えた欄を 1 つだけ送り、送った値をポーリングで戻さない守りを持つ。app.js と
// terrain.js の後ろ・terrain-layers.js の前に読む古典スクリプトで、app.js（send / num /
// setField / round2）と terrain.js（terrainIdentity / flushTerrain）のトップレベルを
// そのまま使う。ここの関数もグローバルのままにする（後ろのファイルが名前で使うため）。
//
// 欄の表（ROWS）は行の配列で、行ごとに見出し（label。null = その番号の表示名）と、
// その行に左から並ぶ欄（fields）を持つ。欄のキー = サーバーの JSON のキー
// （送るときも同じキー）。欄の spec: id は欄の id の頭（後ろに番号）、kind は値の
// 扱い: text = パス（前後の空白を落として送る）、num = 数（空 = 送らない）、
// numOrNull = 数か null（空 = null）、bool = チェック（text はチェックの横の文字）。
// cls = 欄の class、step / placeholder は入力欄の属性、tip はツールチップの本文。

  // 送った値（キーは欄の id、値は {value, until}）。サーバーの値が送った値に
  // なるまで（届かなければ TERRAIN_SENT_HOLD_MS まで）、ポーリングで欄を上書き
  // しない（適用は物理スレッドなので、送った直後の /scene はまだ古い）。
  // 丸められた・断られたときは期限で手放し、サーバーの値に戻る。
  const TERRAIN_SENT_HOLD_MS = 3000;
  // 守りはテレインごと: 別のテレインを開いた・新規にした（data か guid が
  // 変わった）ら捨てる（前のテレインへ送った値で新しいテレインの欄を止めない）。
  let terrainFieldSent = {};
  let terrainFieldSentFor = '';
  // 守りを今のテレインのものにする（見分けが変わっていたら空にする）。読む前・
  // 書く前に必ず通す（判定はここ 1 か所）。
  function syncTerrainFieldSent() {
    const who = terrainIdentity();
    if (who !== terrainFieldSentFor) {
      terrainFieldSent = {};
      terrainFieldSentFor = who;
    }
  }
  // 番号付きの欄 id にサーバーの値 v を入れる（送信待ちの間は触らない）。
  function setIndexedField(id, v) {
    syncTerrainFieldSent();
    const sent = terrainFieldSent[id];
    if (sent) {
      if (sent.value !== v && Date.now() < sent.until) return;
      delete terrainFieldSent[id];
    }
    setField(id, v);
  }
  // 欄が数か（num / numOrNull）。
  function isNumberField(spec) {
    return spec.kind === 'num' || spec.kind === 'numOrNull';
  }
  // キーで欄を引くための一覧（ROWS から作る写し。欄を足すのは ROWS だけ）。
  function indexedFields(rows) {
    return Object.assign({}, ...rows.map((r) => r.fields));
  }

  // 番号付きの行を box（id = boxId）の中に作る。行の数 count はサーバーの配列の
  // 長さ（HTML には持たない）で、番号ごとに 1 つの箱（.layerGroup）に ROWS の行
  // （.edRow.layerRow）を並べる。作るのは初回と数が変わったときだけで、欄に
  // フォーカスがある間は作り直さない（打っている途中の値を消さない）。Missing の
  // 無効化は外側の fieldset が新しい行にも効く。labelFn(i) = 番号 i の表示名、
  // key = 送る JSON のキー（layer / scatter）。opts:
  //   tip      見出しと欄のツールチップの後ろに付ける文
  //   fieldTip (field) => 欄のツールチップの後ろの文（無ければ tip）
  //   onRebuild() 作り直す直前に呼ぶ（見出しのツールチップ・行の控えの初期化）
  //   onRow(i, r, row) 行を 1 つ作るたびに呼ぶ（r = ROWS の要素、row = 行の要素）
  function buildIndexedRows(boxId, count, rows, labelFn, key, opts) {
    const box = document.getElementById(boxId);
    if (!box || box.childElementCount === count) return;
    if (box.contains(document.activeElement)) return;
    const o = opts || {};
    if (o.onRebuild) o.onRebuild();
    box.textContent = '';
    for (let i = 0; i < count; i++) {
      const name = labelFn(i);
      const group = document.createElement('div');
      group.className = 'layerGroup';
      for (const r of rows) {
        const row = document.createElement('div');
        row.className = 'edRow layerRow';
        const label = document.createElement('span');
        label.textContent = r.label || name;
        label.title = name + '。' + o.tip;
        row.appendChild(label);
        for (const field in r.fields) {
          const spec = r.fields[field];
          const input = document.createElement('input');
          input.id = spec.id + i;
          input.title = name + ' の' + spec.tip + '。' + (o.fieldTip ? o.fieldTip(field) : o.tip);
          // 送るときは行の fields で欄を引く（キーは ROWS 全体で一意）。
          input.onchange = () => sendIndexedField(key, r.fields, i, field);
          if (spec.kind === 'bool') {
            // チェックは横の文字ごと 1 つの label にする（文字を押しても切り替わる）。
            input.type = 'checkbox';
            const wrap = document.createElement('label');
            wrap.className = spec.cls;
            wrap.title = input.title;
            wrap.appendChild(input);
            wrap.appendChild(document.createTextNode(' ' + (spec.text || '')));
            row.appendChild(wrap);
            continue;
          }
          input.className = (isNumberField(spec) ? 'num ' : 'txt ') + spec.cls;
          input.type = isNumberField(spec) ? 'number' : 'text';
          if (spec.step) input.step = spec.step;
          if (spec.placeholder) input.placeholder = spec.placeholder;
          row.appendChild(input);
        }
        if (o.onRow) o.onRow(i, r, row);
        group.appendChild(row);
      }
      box.appendChild(group);
    }
  }
  // 番号付きの欄を 1 つ送る。key = 送る JSON のキー（layer / scatter）、fields =
  // 欄を引く一覧、index = 番号（0 から）、field = 変えた欄のキーで、その 1 つだけを
  // <key>{index, <field>} の 1 通ですぐ送る（触っていない欄の値は送らない =
  // 丸めた表示の値や古い値で上書きしない）。
  function sendIndexedField(key, fields, index, field) {
    const spec = fields[field];
    if (!spec) return;
    const id = spec.id + index;
    const el = document.getElementById(id);
    if (!el) return;
    // 数の欄に打ちかけの文字（「1e」「-」など）があると、ブラウザは value を ''
    // で返す。空と区別できないので送らない（numOrNull で null へ戻さない。
    // ポーリングが戻す）。
    if (isNumberField(spec) && el.validity && el.validity.badInput) return;
    const item = { index };
    let shown;
    if (spec.kind === 'bool') {
      item[field] = shown = !!el.checked;
    } else if (isNumberField(spec)) {
      const v = num(id, NaN);
      if (Number.isFinite(v)) {
        item[field] = v;
        shown = round2(v);
      } else if (spec.kind === 'numOrNull' && el.value.trim() === '') {
        item[field] = null;  // 空 = null（層ではテレイン全体の値に従う）
        shown = '';
      } else {
        return;  // 空・数でない = 送らない（ポーリングが戻す）
      }
    } else {
      item[field] = shown = el.value.trim();
    }
    syncTerrainFieldSent();
    terrainFieldSent[id] = { value: shown, until: Date.now() + TERRAIN_SENT_HOLD_MS };
    // 番号付きの欄は部分更新のキーに混ぜない（サーバーは layer / scatter の 1 通を
    // その欄だけの操作として扱う）。溜まっている形のキーは先に別の 1 通で送る
    // （番号付きの欄とは独立）。
    flushTerrain();
    send('edit.terrain', { [key]: item });
  }
  // 番号付きの欄にサーバーの値を入れる（items = サーバーの配列。順 = 番号）。
  // パスは文字列、数は丸めて、null・無い値は空の欄、チェックは真偽。
  function fillIndexedRows(fields, items) {
    items.forEach((item, i) => {
      for (const field in fields) {
        const spec = fields[field];
        const v = item[field];
        let shown;
        if (spec.kind === 'bool') shown = !!v;
        else if (isNumberField(spec)) shown = (v === null || v === undefined) ? '' : round2(v);
        else shown = v || '';
        setIndexedField(spec.id + i, shown);
      }
    });
  }
