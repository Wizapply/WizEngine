// Charon web UI: オーディオ（Unity の AudioSource / AudioListener）。app.js の後ろに読む
// 古典スクリプトで、そのトップレベル（send / sceneData / owner / isEditorCam /
// mySelectedDesc / myEditorSel / setField）をそのまま使う。
//
// **音はサーバーが作る**（audio/AudioMixer。耳 = このページのカメラ、2D / 3D・距離の
// 減衰・パン・ドップラーまで）。映像と同じ WebRTC の回線に Opus の音声トラックとして
// 乗り、映像と同じ MediaStream に入るので、ブラウザが RTCP の送信者報告で揃えて出す
// （絵と音が同期する）。鳴らすのは映像の要素（#video）で、このファイルがするのは:
//   ・🔊 ボタン（#vhAudio）= 映像の要素のミュートの切り替え。ブラウザは操作の前に
//     音の出る再生をさせない（自動再生の制限）ので、要素は muted で始め、最初の
//     クリック / キーで覚えている設定（既定 = 音を出す）に戻す
//   ・Inspector の ▶ 試聴（エディタで 1 回だけ 2D で鳴らす。これだけはこのページの
//     Web Audio = 映像とは無関係）
//   ・Inspector / World タブの欄（下の「編集の UI」）

  const audioMutedKey = 'charon.audioMuted';
  let audioMuted = false;  // 使う人の設定（覚えておく）
  try { audioMuted = localStorage.getItem(audioMutedKey) === '1'; } catch (e) { /* 無くてよい */ }
  let audioUnlocked = false;  // 操作があった = 音の出る再生が許される
  // ?popup=1 のプレビュー窓は鳴らさない（元のページと二重に鳴るため）。
  const audioEnabledPage = !document.body.classList.contains('popup');

  function audioVideoEl() { return document.getElementById('video'); }
  function audioApplyMute() {
    const v = audioVideoEl();
    if (!v) return;
    const want = !audioEnabledPage || audioMuted || !audioUnlocked;
    if (v.muted !== want) {
      v.muted = want;
      // ミュートを外すと止まることがある（自動再生の制限）。操作の中なので再生し直せる。
      if (!want) v.play().catch(() => {});
    }
    renderAudioButton();
  }
  function audioUnlock() {
    if (audioUnlocked) return;
    audioUnlocked = true;
    audioApplyMute();
  }
  document.addEventListener('pointerdown', audioUnlock, true);
  document.addEventListener('keydown', audioUnlock, true);

  function audioToggleMute() {
    audioUnlocked = true;
    audioMuted = !audioMuted;
    try { localStorage.setItem(audioMutedKey, audioMuted ? '1' : '0'); } catch (e) { /* 無くてよい */ }
    audioApplyMute();
  }
  function renderAudioButton() {
    const b = document.getElementById('vhAudio');
    if (!b) return;
    const v = audioVideoEl();
    const hasTrack = !!(v && v.srcObject && v.srcObject.getAudioTracks &&
                        v.srcObject.getAudioTracks().length);
    const silent = !v || v.muted;
    b.textContent = silent ? '🔇' : '🔊';
    b.classList.toggle('on', !silent);
    b.title = !hasTrack ? '音声のトラックがありません（サーバーに opusenc が無いか、接続前）'
            : audioMuted ? '音を出す（いまはミュート）'
            : !audioUnlocked ? 'クリックで音を出す（ブラウザは操作の前に音を出させません）'
            : 'ミュート（シミュレート中のオーディオソースの音。サーバーで作り映像と同期）';
    b.hidden = !audioEnabledPage;
  }
  // 接続し直したときの要素の状態に合わせて、ボタンの表示を時々取り直す。
  setInterval(renderAudioButton, 1000);

  // ---- エディタでの試聴（Inspector の ▶）。2D で 1 回だけ、このページの Web Audio。
  let audioCtx = null;
  let audioPreviewVoice = null;
  const audioClips = new Map();  // clip → Promise<AudioBuffer | null>
  function audioEnsureCtx() {
    if (audioCtx) return audioCtx;
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return null;
    try { audioCtx = new AC(); } catch (e) { return null; }
    return audioCtx;
  }
  function audioLoadClip(clip) {
    if (audioClips.has(clip)) return audioClips.get(clip);
    const p = fetch('clip?file=' + encodeURIComponent(clip))
      .then((r) => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.arrayBuffer(); })
      .then((data) => new Promise((resolve, reject) => {
        const ctx = audioEnsureCtx();
        if (!ctx) { reject(new Error('Web Audio が使えません')); return; }
        const q = ctx.decodeAudioData(data, resolve, reject);
        if (q && q.then) q.then(resolve, reject);
      }))
      .catch((e) => { console.warn('audio preview: ' + clip + ': ' + e); audioClips.delete(clip); return null; });
    audioClips.set(clip, p);
    return p;
  }
  function audioPreview(clip, volume, pitch) {
    audioUnlock();
    const ctx = audioEnsureCtx();
    if (!ctx || !clip) return;
    if (ctx.state === 'suspended') ctx.resume().catch(() => {});
    audioPreviewStop();
    audioLoadClip(clip).then((buf) => {
      if (!buf) {
        alert('クリップを読めませんでした: ' + clip +
              '\n（試聴はブラウザが復号します。シミュレート中の音はサーバーが復号します）');
        return;
      }
      const g = ctx.createGain();
      g.gain.value = Number.isFinite(volume) ? volume : 1;
      g.connect(ctx.destination);
      const v = ctx.createBufferSource();
      v.buffer = buf;
      v.playbackRate.value = Number.isFinite(pitch) ? pitch : 1;
      v.connect(g);
      v.onended = () => { try { g.disconnect(); } catch (e) { /* 済み */ } };
      v.start();
      audioPreviewVoice = v;
    });
  }
  function audioPreviewStop() {
    if (audioPreviewVoice) {
      try { audioPreviewVoice.stop(); } catch (e) { /* 済み */ }
      audioPreviewVoice = null;
    }
  }

  // ---- 編集の UI（Inspector の secAudioSrc 節・World タブの secAudio 節）-------------------
  // /scene の audio = {sources:[{index, name, body, pos, clip, ...}], hasListener, listener, clips}。
  // 欄は「確定したら送る」（edit.audio.set {index, キー}）。ポーリングで組み直すのは中身が
  // 変わったときだけで、欄にフォーカスがある間は組み直さない（打っている途中を消さない）。
  const audioUiKeys = { obj: '', world: '' };
  function audioData() { return (sceneData && sceneData.audio) || { sources: [], clips: [] }; }
  const aEsc = (s) => String(s == null ? '' : s).replace(/[&<>"']/g, (ch) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[ch]));
  const aNum = (v) => (Number.isFinite(v) ? Math.round(v * 1000) / 1000 : 0);

  function audioSet(index, patch) { send('edit.audio.set', Object.assign({ index: index }, patch)); }
  function audioSetNum(index, key, el) {
    const v = parseFloat(el.value);
    if (Number.isFinite(v)) audioSet(index, { [key]: v });
  }
  function audioSetPos(index, el) {
    const box = el.closest('.auPos');
    const q = (ax) => parseFloat(box.querySelector('[data-ax="' + ax + '"]').value) || 0;
    audioSet(index, { pos: { x: q('x'), y: q('y'), z: q('z') } });
  }
  function audioAddSource(body) {
    const clips = audioData().clips || [];
    send('edit.audio.add', { body: body, clip: clips[0] || '', name: body >= 0 ? 'sound' : 'bgm' });
  }
  function audioRemoveSource(index, name) {
    if (!confirm('オーディオソース「' + (name || index) + '」を削除しますか？')) return;
    send('edit.audio.remove', { index: index });
  }
  function audioPreviewSource(index) {
    const s = (audioData().sources || []).find((a) => a.index === index);
    if (s) audioPreview(s.clip, s.volume, s.pitch);
  }

  // 1 つの音源のカード。
  function audioCardHtml(s, clips) {
    const i = s.index;
    let opts = '<option value=""' + (!s.clip ? ' selected' : '') + '>(なし)</option>';
    const list = clips.slice();
    if (s.clip && !list.includes(s.clip)) list.unshift(s.clip);
    for (const c of list) {
      opts += '<option value="' + aEsc(c) + '"' + (c === s.clip ? ' selected' : '') + '>' +
              aEsc(c) + '</option>';
    }
    const num = (key, step, min, max, title) =>
      '<input class="num" type="number" step="' + step + '"' +
      (min !== null ? ' min="' + min + '"' : '') + (max !== null ? ' max="' + max + '"' : '') +
      ' value="' + aNum(s[key]) + '" title="' + aEsc(title) + '" onchange="audioSetNum(' + i +
      ',\'' + key + '\',this)">';
    const chk = (key, title) =>
      '<input type="checkbox"' + (s[key] ? ' checked' : '') + ' title="' + aEsc(title) +
      '" onchange="audioSet(' + i + ',{' + key + ':this.checked})">';
    const p = s.pos || { x: 0, y: 0, z: 0 };
    const axis = (ax, v) =>
      '<label class="trCell"><span class="axis ax' + ax.toUpperCase() + '">' + ax.toUpperCase() +
      '</span><input class="num" type="number" step="0.1" data-ax="' + ax + '" value="' + aNum(v) +
      '" onchange="audioSetPos(' + i + ',this)"></label>';
    const blend = Math.round((s.spatialBlend || 0) * 100);
    let h = '<div class="auCard" data-index="' + i + '">';
    h += '<div class="edRow"><span>名前</span><input class="txt" type="text" value="' + aEsc(s.name) +
         '" title="制御器の audio:<名前> とイベントの「音を鳴らす」が引く名前（英数字と _ -）" ' +
         'onchange="audioSet(' + i + ',{name:this.value})"></div>';
    h += '<div class="edRow"><span>クリップ</span><span class="nowrap">' +
         '<select class="strSel auClip" title="assets/ の音のファイル（.wav .ogg .mp3 …）" ' +
         'onchange="audioSet(' + i + ',{clip:this.value})">' + opts + '</select>' +
         '<button title="試聴（2D で 1 回）" onclick="audioPreviewSource(' + i + ')">▶</button>' +
         '<button title="試聴を止める" onclick="audioPreviewStop()">■</button></span></div>';
    h += '<div class="edRow"><span>音量 / ピッチ</span><span>' +
         num('volume', 0.05, 0, 4, '音量（0〜1。1 を超えると持ち上げる）') +
         num('pitch', 0.05, 0.05, 4, 'ピッチ = 再生速度の倍率（2 = 1 オクターブ上）') + '</span></div>';
    h += '<div class="edRow"><span>ループ / 開始時に再生 / ミュート</span><span class="nowrap">' +
         chk('loop', 'ループ（エンジン音・BGM など）') +
         chk('playOnAwake', 'Play On Awake: シミュレート開始で鳴らす') +
         chk('mute', 'ミュート') + '</span></div>';
    h += '<div class="edRow"><span>2D ⇄ 3D</span><span class="nowrap">' +
         '<input type="range" min="0" max="100" step="5" value="' + blend + '" ' +
         'title="Spatial Blend: 0 = 2D（距離・向きに関係なく鳴る）、100 = 3D（距離で減衰・左右に振れる）" ' +
         'onchange="audioSet(' + i + ',{spatialBlend:this.value/100})" ' +
         'oninput="this.nextElementSibling.textContent=this.value>=100?\'3D\':this.value<=0?\'2D\':this.value+\'%\'">' +
         '<span class="auBlend">' + (blend >= 100 ? '3D' : blend <= 0 ? '2D' : blend + '%') +
         '</span></span></div>';
    if (blend < 100) {
      h += '<div class="edRow"><span>ステレオパン</span>' +
           num('panStereo', 0.1, -1, 1, '-1 = 左 … 1 = 右（2D の分にだけ効く）') + '</div>';
    }
    if (blend > 0) {
      h += '<div class="edRow"><span>距離 min / max</span><span>' +
           num('minDistance', 0.5, 0.01, null, 'ここまでは減衰しない (m)') +
           num('maxDistance', 5, 0.02, null, 'ここで減衰が止まる（linear は 0 になる）(m)') +
           '</span></div>';
      h += '<div class="edRow"><span>減衰 / ドップラー</span><span class="nowrap">' +
           '<select class="strSel" onchange="audioSet(' + i + ',{rolloff:this.value})">' +
           '<option value="logarithmic"' + (s.rolloff !== 'linear' ? ' selected' : '') +
           '>Logarithmic</option><option value="linear"' + (s.rolloff === 'linear' ? ' selected' : '') +
           '>Linear</option></select>' +
           num('dopplerLevel', 0.1, 0, 5, 'ドップラー効果の強さ（0 = 無し）') + '</span></div>';
    }
    h += '<div class="trRow auPos" title="付け先のローカル (m)"><span class="trName">位置</span>' +
         axis('x', p.x) + axis('y', p.y) + axis('z', p.z) + '</div>';
    h += '<div class="edRow btnRow"><button class="danger" onclick="audioRemoveSource(' + i +
         ',\'' + aEsc(s.name).replace(/\\/g, '') + '\')">🗑 このオーディオソースを削除</button></div>';
    h += '</div>';
    return h;
  }

  function audioRenderList(el, list, clips, keyName, extra) {
    const key = JSON.stringify(list) + '@' + clips.join('|') + '@' + extra;
    if (key === audioUiKeys[keyName]) return;
    if (el.contains(document.activeElement)) return;  // 打っている途中は触らない
    audioUiKeys[keyName] = key;
    el.innerHTML = list.length
      ? list.map((s) => audioCardHtml(s, clips)).join('')
      : '<div class="edHint">まだありません。</div>';
  }

  function renderAudioSections() {
    if (!sceneData) return;
    const a = audioData();
    const clips = a.clips || [];
    const sources = a.sources || [];
    const editing = sceneData.mode === 'editor';
    const editorHere = typeof isEditorCam === 'function' && isEditorCam();

    // ---- Inspector: 選んでいるオブジェクトの音源 ----
    const secObj = document.getElementById('secAudioSrc');
    if (secObj) {
      const sel = typeof mySelectedDesc === 'function' ? mySelectedDesc() : null;
      const eKind = (typeof myEditorSel === 'function' && editing) ? myEditorSel().kind : 'none';
      const show = !!sel && editorHere && eKind === 'none';
      secObj.hidden = !show;
      if (show) {
        const mine = sources.filter((s) => s.body === sel.index);
        audioRenderList(document.getElementById('auObjList'), mine, clips, 'obj', sel.index);
        const hint = document.getElementById('auObjHint');
        if (hint) {
          hint.textContent = clips.length
            ? '音はサーバーで作り、映像と一緒に届きます（シミュレート中だけ。🔊 でミュート解除）。'
            : 'assets/ に音のファイル（.wav .ogg .mp3）がありません。assets/audio/ に置くと選べます。';
        }
      }
    }

    // ---- World タブ: 耳と 2D の音（ワールドに付いた音源）----
    const secW = document.getElementById('secAudio');
    if (secW) {
      secW.hidden = !editorHere;
      if (editorHere) {
        const L = a.listener || {};
        const has = !!a.hasListener;
        const body = has ? L.body : -1;
        // 耳の付け先の select（カメラ = 既定 + 名前の付いたオブジェクト）。
        const selEl = document.getElementById('auLisBody');
        if (selEl && selEl !== document.activeElement) {
          const objs = (sceneData.objects || []).filter((o) => o.alive !== false);
          const key = objs.map((o) => o.index + ':' + (o.name || '')).join(',');
          if (selEl.dataset.key !== key) {
            selEl.dataset.key = key;
            selEl.innerHTML = '<option value="-1">カメラ（各ページ）</option>' +
              objs.map((o) => '<option value="' + o.index + '">' +
                       aEsc(o.name || ('#' + o.index)) + '</option>').join('');
          }
          selEl.value = String(body);
        }
        setField('auLisVol', aNum(has ? L.volume : 1));
        setField('auLisDoppler', aNum(has ? L.doppler : 1));
        setField('auLisSpeed', aNum(has ? L.soundSpeed : 343));
        const lp = (has && L.pos) || { x: 0, y: 0, z: 0 };
        const lr = (has && L.rot) || { x: 0, y: 0, z: 0 };
        setField('auLisPX', aNum(lp.x)); setField('auLisPY', aNum(lp.y)); setField('auLisPZ', aNum(lp.z));
        setField('auLisRX', aNum(lr.x)); setField('auLisRY', aNum(lr.y)); setField('auLisRZ', aNum(lr.z));
        const pose = document.getElementById('auLisPose');
        if (pose) pose.hidden = body < 0;
        const world = sources.filter((s) => s.body < 0);
        audioRenderList(document.getElementById('auWorldList'), world, clips, 'world', -1);
        // ほかの物に付いている音源の数（一覧はその物を選んで Inspector で）。
        const others = sources.length - world.length;
        const note = document.getElementById('auWorldNote');
        if (note) {
          note.textContent = others > 0
            ? 'オブジェクトに付いている音源が ' + others + ' 個あります（その物を選ぶと Inspector に出ます）。'
            : '';
        }
      }
    }
    renderAudioButton();
  }

  // World タブの耳の欄を送る（部分更新。カメラへ戻すときは節ごと外す）。
  function applyAudioListener() {
    const body = parseInt(document.getElementById('auLisBody').value, 10);
    const n = (id, d) => { const v = parseFloat(document.getElementById(id).value); return Number.isFinite(v) ? v : d; };
    const msg = {
      body: Number.isFinite(body) ? body : -1,
      volume: n('auLisVol', 1),
      doppler: n('auLisDoppler', 1),
      soundSpeed: n('auLisSpeed', 343),
      pos: { x: n('auLisPX', 0), y: n('auLisPY', 0), z: n('auLisPZ', 0) },
      rot: { x: n('auLisRX', 0), y: n('auLisRY', 0), z: n('auLisRZ', 0) },
    };
    const defaults = msg.body < 0 && msg.volume === 1 && msg.doppler === 1 && msg.soundSpeed === 343;
    send('edit.audiolistener', defaults ? { present: false } : msg);
  }

  renderAudioButton();
