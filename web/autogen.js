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
      meshes: document.getElementById('aiMeshes').checked
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
    parts.push(ai.llm ? ('LLM: ' + ai.llmModel + ' @ ' + ai.llmHost)
                      : 'LLM: 未設定（config.json の ai.llm.model）');
    parts.push(ai.meshy ? ('3D: ' + ai.meshyHost + (ai.meshyRefine ? '（テクスチャ付き）' : '（形のみ）'))
                        : '3D: 未設定（config.json の ai.meshy.apiKey）');
    if (!ai.https) parts.push('https 不可のビルド');
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
    if (ai.seq !== aiLogSeq) {
      aiLogSeq = ai.seq;
      const log = document.getElementById('aiLog');
      log.textContent = (ai.log || []).join('\n');
      log.scrollTop = log.scrollHeight;
    }
  }

  // app.js の pollScene（500ms）が sceneData を更新する。こちらは開いている
  // 間だけ同じ間隔で描き直す（pollScene に手を入れずに済む）。
  setInterval(renderAiGen, 500);

  // Esc で閉じる。app.js のキー処理は入力欄にフォーカスがあると何もしない
  // ので、依頼文を打っている最中でも閉じられるようにここで受ける。
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && aiGenOpen) {
      e.preventDefault();
      closeAiGen();
    } else if (aiGenOpen && e.key === 'Enter' && (e.ctrlKey || e.metaKey) &&
               !document.getElementById('aiGo').disabled) {
      e.preventDefault();  // Ctrl+Enter で生成
      aiGenerate();
    }
  });
