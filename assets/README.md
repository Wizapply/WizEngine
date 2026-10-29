# assets/

Files the scene loads at runtime, copied next to the executable by the build.

- `*.hdr` — equirectangular environment map. シーン XML の `<environment>` が
  assets/ からの相対パスで参照する:

  ```xml
  <worldbody>
    <environment hdr="studio.hdr" intensity="30000" skybox="true"/>
    ...
  ```

  `skybox="true"` にすると、光としてだけでなく**背景にも**出る（映り込みと
  背景が一致する ＝ 写真らしさがいちばん安く上がる設定。`docs/photorealism.md`）。

  It is converted to a cubemap and prefiltered **on the GPU at load time**, so
  changing the file needs no rebuild — only a restart. Free HDRIs:
  <https://polyhaven.com/hdris> (2k is plenty).

  Why bother: glTF materials default to `metallicFactor = 1.0`, and metal has
  no diffuse colour of its own — it is visible only through what it reflects.
  With no environment, those surfaces go black wherever the direct lights do
  not reach.

- `*.glb` / `*.gltf` — モデル。シーン XML の `<asset><mesh name file scale/>`
  が assets/ からの相対パスで参照する（例: `file="models/crate.glb"`）。
  どのモデルをどの剛体で使うかはシーン文書が決める。

## Layout

- `scenes/*.xml` — シーン文書（MuJoCo 風の XML）。エディタの 💾 保存が書き、
  タイルのダブルクリックが読む。`SceneConfig.h` の `kStartupScene` に名前を
  書けば起動時に読み込む。形式の定義は `src/document/SceneDocument.h` の先頭と
  CLAUDE.md の「シーン文書（XML）」の章。`scenes/*.json` は旧形式で、同じ
  名前の `.xml` が無いときだけ読み込みに使う（保存は常に `.xml`）。
- `materials/*.mat` — material sources, compiled by matc at build time into
  `.filamat` next to the executable.
- `textures/ground.png` — the floor texture. シーン XML の
  `<ground texture="textures/ground.png"/>` が参照する（空 = 市松模様）。
- `terrains/*.terrain` — テレインの高さ・穴・塗り分けの重み（SPLT）と、塗り分けの
  層の定義（LAYR = 層ごとのテクスチャ・タイル・法線マップ・マスク・粗さ・金属・
  法線の強さ）。シーン XML の
  `<terrain data="名前">` が名前で参照する。層は .terrain が持つので、同じ
  .terrain をどのシーンで開いても同じ見た目になる。
- `textures/` の塗り分けの層のテクスチャ — 各自で用意し、コミットしない
  （`.gitignore` には入っていないので、`assets/textures/` に置いたときは
  `git add` に混ぜないよう注意）。置き方:
  1. <https://polyhaven.com/textures> などから、地面のテクスチャ（草・岩・土・
     雪など）の **Diffuse（色の画像）を 1K か 2K の PNG か JPG** で落とす。
     読み込み時に 1024×1024 へ揃える（2K・4K などの大きな画像は半分ずつ平均して
     縮めるので、細かい模様もちらつかない。読む時間とメモリは大きいほどかかる）。
     正方形でない画像は引き伸ばされる。
  2. `assets/textures/` に置く（次のビルドで実行フォルダへ写る）。試すだけなら
     実行フォルダの `build/assets/textures/` に直接置けば再ビルドも再起動も
     要らない。ただし層に入れたパスはそのままで画像だけを差し替えたときは
     読み直さない（シーンを開き直しても読み直さない）ので、パスを一度空にして
     入れ直す。
  3. Inspector のテレインの「塗り分けの層」に `textures/grass.png` のように
     assets/ からの相対パスで入れる。**層 1 は下地**（何も塗っていない所は全部
     層 1）で、層 2〜4 は 🎨 の自動塗りか 🖌 の筆で塗った所にだけ出る。🧽 は
     塗った所を下地へ戻す。
  4. 配信の画質のため、タイルは 4 m 以上・コントラストは控えめの画像が向く
     （細かく強い模様は映像の圧縮でつぶれやすい）。
  5. 凹凸とつやも付けるなら、同じ素材の次の 2 枚も落として層の「法線」「マスク」
     の欄に入れる（3 枚は同じタイルで貼られる）:
     - **法線マップ**: **「Normal (GL)」**（ファイル名は `*_nor_gl`）を **PNG** で。
       「Normal (DX)」（`*_nor_dx`。日本語の画面では「ノーマル(診断)」と表示される
       ことがある）は緑の向きが逆で、凹凸が反転して見える。強さの欄は 1 が原寸、
       0 で平ら。
     - **マスク**: **「AO/Rough/Metal」**（`*_arm`、日本語の画面では「AO/ラフ/メタル」）
       を **PNG** で。R = 遮蔽（すき間の暗さ）・G = 粗さ・B = 金属の 1 枚。マスクの
       ある層では粗さ・金属の数値は使わない（マスクが勝つ）。素材に ARM が無ければ、
       画像編集ソフトで R に AO（無ければ白）・G に rough・B に metal（無ければ黒）を
       入れた PNG を作る。
     - 読める形式は PNG / JPEG / TGA / BMP。**EXR は読めない**（ダウンロードの
       既定が EXR のことがあるので、形式を選び直す）。ブラウザで押すと画像が
       別のタブで開くだけのときは、その画像を右クリックして保存する。
     - 法線とマスクは色ではないデータなので、画像編集ソフトで書き出すときは
       色の補正（sRGB 変換・カラープロファイル）をかけない。
     - 粗さ・金属の欄を空にすると、Inspector 下のテレイン全体の値に従う。
     - 層の画像はパスが変わったときだけ読む。**画像を置いてからパスを入れる**
       （先にパスを入れて読めなかったときは、パスを一度空にして入れ直す）。
- `*.hdr`, `*.glb` (this folder's root) — environment maps and models named
  by the scene documents (`<environment hdr>`, `<asset><mesh file>`). Not
  committed to git (see .gitignore); fetch your own.

Everything under this folder is copied next to the executable at build time.
