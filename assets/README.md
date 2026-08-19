# assets/

Files the scene loads at runtime, copied next to the executable by the build.

- `*.hdr` — equirectangular environment map. シーン XML の `<environment>` が
  assets/ からの相対パスで参照する:

  ```xml
  <worldbody>
    <environment hdr="studio.hdr" intensity="30000"/>
    ...
  ```

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
  書けば起動時に読み込む。形式の定義は `src/SceneDocument.h` の先頭と
  CLAUDE.md の「シーン文書（XML）」の章。`scenes/*.json` は旧形式で、同じ
  名前の `.xml` が無いときだけ読み込みに使う（保存は常に `.xml`）。
- `materials/*.mat` — material sources, compiled by matc at build time into
  `.filamat` next to the executable.
- `textures/ground.png` — the floor texture. シーン XML の
  `<ground texture="textures/ground.png"/>` が参照する（空 = 市松模様）。
- `*.hdr`, `*.glb` (this folder's root) — environment maps and models named
  by SceneConfig.h. Not committed to git (see .gitignore); fetch your own.

Everything under this folder is copied next to the executable at build time.
