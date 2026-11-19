// Charon web UI: AI シーン生成のモーダル（index.html の #aiGen）。app.js の後ろに
// 読む古典スクリプトで、そのトップレベル（send / sceneData / isEditorCam /
// storeJson / loadStoredJson）をそのまま使う。ここの関数もグローバルのまま
// （index.html の onclick= が名前で呼ぶ）。
//
// やることは「依頼文を ai.generate で送る」と「/scene の ai を描く」だけ。
// LLM と Meshy を呼ぶのも、できた文書を検証して適用するのもサーバー
// （src/autogen/ + AutoGenComponent）。API キーはブラウザに来ない。

  let aiGenOpen = false;
  let aiLogSeq = -1;
  const AI_PROMPT_KEY = 'wiz.ai.prompt';
  const AI_STAGE_LABEL = {
    llm: 'LLM に問い合わせ中', meshes: '3D モデルを生成中', layout: '配置を整えています',
    review: '画面を撮って見た目を確認中',
    cancelling: 'キャンセル中', done: '完了', failed: '失敗'
  };

  function openAiGen() {
    aiGenOpen = true;
    document.getElementById('aiGen').hidden = false;
    const ta = document.getElementById('aiPrompt');
    if (!ta.value) {
      const saved = loadStoredJson(AI_PROMPT_KEY);
      if (typeof saved === 'string') ta.value = saved;
    }
    aiLogSeq = -1;  // 開くたびに描き直す
    renderAiGen();
    ta.focus();
  }
  function closeAiGen() {
    aiGenOpen = false;
    document.getElementById('aiGen').hidden = true;
  }

  function aiMode() {
    const el = document.querySelector('input[name="aiMode"]:checked');
    return el ? el.value : 'new';
  }

  function aiGenerate() {
    const prompt = document.getElementById('aiPrompt').value.trim();
    if (!prompt) { document.getElementById('aiPrompt').focus(); return; }
    const mode = aiMode();
    // @ の名前はサーバーも確かめるが（無い名前なら送らずに断る）、ここで先に止める。
    const m = aiResolveMentions(prompt, aiNameList().map((x) => x.name));
    if (mode === 'new' && (m.found.length || m.unknown.length)) {
      document.getElementById('aiStage').textContent =
        '「新規」では今のシーンの物を @ で指せません（「編集」か「追加」を選んでください）';
      return;
    }
    if (m.unknown.length) {
      document.getElementById('aiStage').textContent =
        'シーンに無い名前です: @' + m.unknown.join('、@');
      return;
    }
    // 置き換えるモードは確認する（📄 XML の適用と同じく今の配置が消える）。
    if (mode !== 'append' &&
        !confirm('生成したシーンで現在のシーンを置き換えます（未保存の変更は失われます）。よろしいですか？')) {
      return;
    }
    storeJson(AI_PROMPT_KEY, prompt);
    // テレインの未保存の筆を捨てる確認（XML の適用と同じ扱い）。
    if (mode !== 'append' && typeof discardTerrain === 'function') discardTerrain();
    send('ai.generate', {
      prompt: prompt,
      mode: mode,
      meshes: document.getElementById('aiMeshes').checked,
      // 選択中の物を「これ」として渡すか（欄が出ていて、チェックが付いているとき）。
      useSelection: !document.getElementById('aiSelLbl').hidden &&
                    document.getElementById('aiUseSel').checked
    });
    document.getElementById('aiStage').textContent = '送信しました...';
  }
  function aiCancel() { send('ai.cancel'); }

  function renderAiGen() {
    if (!aiGenOpen || !sceneData) return;
    const ai = sceneData.ai;
    const conf = document.getElementById('aiConf');
    const go = document.getElementById('aiGo');
    const stop = document.getElementById('aiStop');
    if (!ai) {
      conf.textContent = 'このサーバーは AI シーン生成に対応していません';
      go.disabled = true;
      stop.disabled = true;
      return;
    }
    const editable = sceneData.mode === 'editor' && isEditorCam();
    const parts = [];
    // 使えない理由はサーバーが言い分ける（llmIssue / meshyIssue）。古いサーバーは
    // 理由を返さないので、そのときは従来の「未設定」の文面に落とす。
    const why = (issue, host, unset) => {
      if (issue === 'https') return host + ' は https ですが、このビルドは OpenSSL 無し（https 不可）';
      if (issue === 'url') return '接続先の URL が読めません（config.json の ai.llm.url / ai.meshy.url）';
      return unset;
    };
    parts.push(ai.llm ? ('LLM: ' + ai.llmModel + ' @ ' + ai.llmHost)
                      : 'LLM: ' + why(ai.llmIssue, (ai.llmModel ? ai.llmModel + ' @ ' : '') + ai.llmHost,
                                      '未設定（config.json の ai.llm.model）'));
    parts.push(ai.meshy ? ('3D: ' + ai.meshyHost + (ai.meshyRefine ? '（テクスチャ付き）' : '（形のみ）'))
                        : '3D: ' + why(ai.meshyIssue, ai.meshyHost, '未設定（config.json の ai.meshy.apiKey）'));
    if (!ai.https && ai.llmIssue !== 'https' && ai.meshyIssue !== 'https') parts.push('https 不可のビルド');
    conf.textContent = parts.join(' ／ ');
    conf.classList.toggle('bad', !ai.llm);

    const meshes = document.getElementById('aiMeshes');
    meshes.disabled = !ai.meshy;
    if (!ai.meshy) meshes.checked = false;
    document.getElementById('aiMeshesLbl').title = ai.meshy
      ? ('1 回に新しく作るのは最大 ' + ai.maxMeshes + ' 個（同じ説明文はキャッシュ）')
      : 'Meshy 互換の API キーが設定されていません';

    go.disabled = ai.busy || !ai.llm || !editable;
    go.title = !editable ? 'エディタモードの Editor Camera のページで使えます' : '';
    stop.disabled = !ai.busy;
    document.getElementById('aiBarFill').style.width =
      Math.round((ai.busy || ai.stage === 'done' ? ai.progress : 0) * 100) + '%';
    document.getElementById('aiStage').textContent =
      ai.busy ? ((AI_STAGE_LABEL[ai.stage] || ai.stage) + ' ' +
                 Math.round(ai.progress * 100) + '%')
              : (ai.message || '');
    renderAiPointing();
    if (ai.seq !== aiLogSeq) {
      aiLogSeq = ai.seq;
      const log = document.getElementById('aiLog');
      log.textContent = (ai.log || []).join('\n');
      log.scrollTop = log.scrollHeight;
    }
  }

  // ---- 名指し（@名前）と選択中の物 --------------------------------------------
  // LLM は XML の数値だけでは「これ」「左の箱」がどれか分からないので、
  // ユーザーが名前で指せるようにする。サーバーは今のシーンの要約
  // （src/autogen/SceneDigest）に「選択中」「名指し」の印を付けて渡す。

  // @ で指せる名前（サーバーの mentionableNames と同じ顔ぶれ）。名前の無い物は、
  // サーバーが送る前に付ける "body<番号>" で出す（番号 = objects の並び =
  // 削除済みを詰めた番号。既にある名前と重なれば _2 … = nameUnnamedBodies と同じ）。
  function aiObjectNames() {
    const objs = (sceneData && sceneData.objects) || [];
    const taken = new Set(objs.filter((o) => o.name).map((o) => o.name));
    const byIndex = new Map();
    objs.forEach((o, k) => {
      let n = o.name;
      if (!n) {
        const base = 'body' + k;
        n = base;
        for (let i = 2; taken.has(n); ++i) n = base + '_' + i;
        taken.add(n);
      }
      byIndex.set(o.index, { name: n, unnamed: !o.name, shape: o.shape || 'body' });
    });
    return byIndex;
  }
  function aiNameList() {
    const out = [];
    const seen = new Set();
    const add = (name, kind) => {
      if (name && !seen.has(name)) { seen.add(name); out.push({ name: name, kind: kind }); }
    };
    if (!sceneData) return out;
    for (const v of aiObjectNames().values()) {
      add(v.name, v.shape + (v.unnamed ? '（名前なし）' : ''));
    }
    (sceneData.joints || []).forEach((j) => add(j.name, 'joint'));
    (sceneData.lights || []).forEach((l) => add(l.name, 'light'));
    (sceneData.cameras || []).forEach((c, i) => { if (c.active) add('cam' + i, 'camera'); });
    return out;
  }

  // 依頼文の @名前 を拾う（サーバーの resolveMentions と同じ規則: 名前のうち最も
  // 長く一致するもの。日本語の名前の後ろに助詞が続いても切れる。英数字の直後の
  // @ = メールアドレスは名指しではない）。
  const AI_IDENT = /[A-Za-z0-9_]/;
  const AI_PREV = /[A-Za-z0-9_.]/;
  const AI_BREAK = /[\s、。，．「」『』【】（）！？,;!?()[\]{}"'`<>\/\\\u3000]/;
  function aiResolveMentions(text, names) {
    const found = [], unknown = [];
    for (let i = 0; i < text.length; ++i) {
      if (text[i] !== '@') continue;
      if (i > 0 && AI_PREV.test(text[i - 1])) continue;
      const start = i + 1;
      let best = '';
      for (const n of names) {
        if (n.length <= best.length || !text.startsWith(n, start)) continue;
        const end = start + n.length;
        if (end < text.length && AI_IDENT.test(n[n.length - 1]) && AI_IDENT.test(text[end])) {
          continue;  // @box10 に対する box1 は一致と見なさない
        }
        best = n;
      }
      if (best) {
        if (!found.includes(best)) found.push(best);
        i = start + best.length - 1;
        continue;
      }
      let end = start;
      while (end < text.length && !AI_BREAK.test(text[end])) ++end;
      let tok = text.slice(start, end).replace(/[.:-]+$/, '');
      if (tok.length > 20) tok = tok.slice(0, 20) + '…';
      if (tok && !unknown.includes(tok)) unknown.push(tok);
      if (end > start) i = end - 1;
    }
    return { found: found, unknown: unknown };
  }

  // 選択中の物の欄と、名指しの確認の行。
  function renderAiPointing() {
    const mode = aiMode();
    const selLbl = document.getElementById('aiSelLbl');
    const sel = sceneData && sceneData.selected;
    const selName = sel ? (aiObjectNames().get(sel.index) || {}).name : '';
    selLbl.hidden = mode === 'new' || !selName;
    if (selName) document.getElementById('aiSelName').textContent = selName;

    const text = document.getElementById('aiPrompt').value;
    const m = aiResolveMentions(text, aiNameList().map((x) => x.name));
    const out = document.getElementById('aiMentions');
    const esc = (t) => String(t).replace(/[&<>"]/g, (c) =>
      ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
    if (!m.found.length && !m.unknown.length) {
      out.innerHTML = mode === 'new' ? '' : '@ でシーンの物を名指しできます（候補が出ます）';
      return;
    }
    if (mode === 'new') {
      out.innerHTML = '<span class="bad">「新規」では @ で今のシーンの物を指せません</span>';
      return;
    }
    out.innerHTML = m.found.map((n) => '<span class="ok">✓ @' + esc(n) + '</span>')
      .concat(m.unknown.map((n) => '<span class="bad">✗ @' + esc(n) + '（シーンに無い）</span>'))
      .join(' ');
  }

  // ---- @ の補完 --------------------------------------------------------------
  let aiAcItems = [];
  let aiAcPick = 0;
  let aiAcAt = -1;  // 補完中の @ の位置（-1 = 閉じている）

  // カーソルの直前が「@ + 打ちかけの名前」なら @ の位置を返す。
  function aiAcQueryAt(ta) {
    const text = ta.value;
    const caret = ta.selectionStart;
    if (caret !== ta.selectionEnd) return -1;
    for (let i = caret - 1; i >= 0 && caret - i <= 64; --i) {
      const ch = text[i];
      if (ch === '@') return (i > 0 && AI_PREV.test(text[i - 1])) ? -1 : i;
      if (AI_BREAK.test(ch)) return -1;
    }
    return -1;
  }
  function aiAcClose() {
    aiAcAt = -1;
    aiAcItems = [];
    document.getElementById('aiAc').hidden = true;
  }
  function aiAcUpdate() {
    const ta = document.getElementById('aiPrompt');
    const at = aiAcQueryAt(ta);
    if (at < 0 || !sceneData) { aiAcClose(); return; }
    const q = ta.value.slice(at + 1, ta.selectionStart).toLowerCase();
    const all = aiNameList();
    const head = all.filter((x) => x.name.toLowerCase().startsWith(q));
    const mid = all.filter((x) => !x.name.toLowerCase().startsWith(q) &&
                                  x.name.toLowerCase().includes(q));
    const items = head.concat(mid).slice(0, 40);
    if (!items.length) { aiAcClose(); return; }
    if (at !== aiAcAt) aiAcPick = 0;
    aiAcAt = at;
    aiAcItems = items;
    aiAcPick = Math.min(aiAcPick, items.length - 1);
    const box = document.getElementById('aiAc');
    box.innerHTML = '';
    items.forEach((x, i) => {
      const row = document.createElement('div');
      if (i === aiAcPick) row.className = 'on';
      const n = document.createElement('span');
      n.textContent = '@' + x.name;
      const k = document.createElement('span');
      k.className = 'aiAcKind';
      k.textContent = x.kind;
      row.append(n, k);
      // mousedown で入れる（click だと先に textarea の blur が来て一覧が消える）。
      row.addEventListener('mousedown', (e) => { e.preventDefault(); aiAcAccept(i); });
      box.appendChild(row);
    });
    box.hidden = false;
    const on = box.children[aiAcPick];
    if (on && on.scrollIntoView) on.scrollIntoView({ block: 'nearest' });
  }
  function aiAcAccept(i) {
    const ta = document.getElementById('aiPrompt');
    const x = aiAcItems[i];
    if (!x || aiAcAt < 0) return;
    const before = ta.value.slice(0, aiAcAt + 1);
    const after = ta.value.slice(ta.selectionStart);
    const sep = after.startsWith(' ') ? '' : ' ';
    ta.value = before + x.name + sep + after;
    const caret = before.length + x.name.length + sep.length;
    ta.setSelectionRange(caret, caret);
    aiAcClose();
    renderAiPointing();
    ta.focus();
  }
  // 依頼文の欄に @名前 を入れて開く（オブジェクトの右クリックから）。「新規」なら
  // 名指しが意味を持たないので「編集」に切り替える。
  function aiMentionObject(index) {
    const v = aiObjectNames().get(index);
    if (!v) return;
    if (!aiGenOpen) openAiGen();
    if (aiMode() === 'new') {
      const edit = document.querySelector('input[name="aiMode"][value="edit"]');
      if (edit) edit.checked = true;
    }
    const ta = document.getElementById('aiPrompt');
    const pos = ta.selectionStart === ta.selectionEnd && document.activeElement === ta
      ? ta.selectionStart : ta.value.length;
    const pre = ta.value.slice(0, pos);
    const lead = pre && !/\s$/.test(pre) ? ' ' : '';
    const text = lead + '@' + v.name + ' ';
    ta.value = pre + text + ta.value.slice(pos);
    ta.setSelectionRange(pos + text.length, pos + text.length);
    ta.focus();
    renderAiPointing();
  }

  (function wireAiPrompt() {
    const ta = document.getElementById('aiPrompt');
    if (!ta) return;
    ta.addEventListener('input', () => { aiAcUpdate(); renderAiPointing(); });
    ta.addEventListener('click', aiAcUpdate);
    ta.addEventListener('blur', aiAcClose);
    ta.addEventListener('keydown', (e) => {
      if (aiAcAt < 0 || !aiAcItems.length) return;
      if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
        e.preventDefault();
        const n = aiAcItems.length;
        aiAcPick = (aiAcPick + (e.key === 'ArrowDown' ? 1 : n - 1)) % n;
        aiAcUpdate();
      } else if ((e.key === 'Enter' && !e.ctrlKey && !e.metaKey) || e.key === 'Tab') {
        e.preventDefault();
        aiAcAccept(aiAcPick);
      } else if (e.key === 'Escape') {
        e.preventDefault();  // 下の document の Esc（モーダルを閉じる）より先に一覧だけ閉じる
        aiAcClose();
      }
    });
    // ←→ や Home / End でカーソルが @ の語から出たら閉じる（keyup で位置が決まる）。
    ta.addEventListener('keyup', (e) => {
      if (e.key.startsWith('Arrow') && e.key !== 'ArrowUp' && e.key !== 'ArrowDown') aiAcUpdate();
      if (e.key === 'Home' || e.key === 'End') aiAcUpdate();
    });
    document.querySelectorAll('input[name="aiMode"]').forEach((r) =>
      r.addEventListener('change', renderAiPointing));
  })();

  // app.js の pollScene（500ms）が sceneData を更新する。こちらは開いている
  // 間だけ同じ間隔で描き直す（pollScene に手を入れずに済む）。
  setInterval(renderAiGen, 500);

  // Esc で閉じる。app.js のキー処理は入力欄にフォーカスがあると何もしない
  // ので、依頼文を打っている最中でも閉じられるようにここで受ける。
  document.addEventListener('keydown', (e) => {
    if (e.defaultPrevented) return;  // @ の補完が受けたキー（Esc で一覧だけ閉じる等）
    if (e.key === 'Escape' && aiGenOpen) {
      e.preventDefault();
      closeAiGen();
    } else if (aiGenOpen && e.key === 'Enter' && (e.ctrlKey || e.metaKey) &&
               !document.getElementById('aiGo').disabled) {
      e.preventDefault();  // Ctrl+Enter で生成
      aiGenerate();
    }
  });
