# Filament でフォトリアルな映像は作れるか

課題への回答（2026-09）。結論・根拠・限界・今回入れた実装・使い方・次の一手の順。

---

## 結論

**作れる。ただし「Filament を使えば写実になる」のではない。**

Filament は Google の *Physically Based Rendering in Filament* の実装そのもので、
実時間ラスタライザとしては最上位クラスの PBR を持っている。写実を決めるのは
エンジンではなく、そこへ渡す 3 つと、最後の 1 つ:

| 決めるもの | 中身 | WizEngine での置き場 |
| --- | --- | --- |
| 材質 | 表面が光をどう返すか（粗さ・金属・クリアコート） | `<geom>` / `<part>` の属性 |
| 光 | 実単位の直接光 + 環境マップ（IBL） | `<light>` / `<environment>` |
| 影と遮蔽 | 影の柔らかさ、隅と接地部の陰り | `<visual><quality>` / `<postprocess ssao>` |
| フィルム | 露出とトーンマップ（HDR → 画面） | `<visual><exposure>` / `<grading>` |

改修前の WizEngine は、この 4 つが**全部固定値**だった。だから「CG っぽい」のは
Filament のせいではなく、渡していたデータのせいだった:

1. 材質が 1 種類（roughness 0.75 の誘電体。金属も塗装も発光も無い）
2. 背景は無地の紺色。HDR は光としてだけ使い、背景にも映り込みにも出ない
3. 後処理ゼロ（AA は FXAA のみ、AO / ブルーム / 反射なし、影は 1024 の PCF）
4. 露出とトーンマップは Filament の既定のまま（変える口が無い）

今回、この 4 つをすべて**シーン文書の値**にした（下記「入れたもの」）。

---

## Filament にあるもの / ないもの

### ある（＝写実に効く道具は一通り揃っている）

- **PBR マテリアル**: metallic / roughness / reflectance、クリアコート（車の塗装）、
  異方性、サブサーフェス、布。エネルギー保存の BRDF。
- **IBL（環境マップ）**: 等距円筒の HDR を GPU でプリフィルタし、拡散と鏡面反射の
  両方に使う。スカイボックスとして背景にも出せる。
- **実単位の光**: 平行光は lux、点・スポットは lumen。カメラは F 値・シャッター・
  ISO の**物理露出**。つまり「現実の照明値を入れて、現実のカメラ設定で撮る」が
  そのまま成立する。
- **影**: PCF / DPCF / **PCSS**（距離で柔らかくなる本物らしい影）/ VSM、カスケード、
  スクリーン空間の接地影。
- **後処理**: SSAO（SAO / GTAO、ベント法線）、**スクリーン空間反射**、ブルーム
  （レンズフレア付き）、被写界深度、ビネット、フォグ、TAA / MSAA / FXAA、
  ACES・Filmic・AgX・PBR Neutral などのトーンマップ、カラーグレーディング（LUT）。
- **ガラス**: スクリーン空間の屈折・透過・吸収（Filament にはある。WizEngine 側は未実装）。

### ない（＝ここから先はオフラインの仕事）

- **本物の大域照明**。間接光は IBL + SSAO + SSR の近似まで。赤い壁の照り返しで
  白い箱がほんのり赤くなる、は出ない（Filament にリアルタイム GI は無い）。
- **画面の外は映らない**。SSR はスクリーン空間なので、カメラに映っていない物は
  反射に出ない（環境マップで代用する）。
- **面光源（エリアライト）**。光源は点・スポット・平行光・IBL のみ。蛍光灯の
  細長いハイライトは作れない（発光する板を置いても、その光は他を照らさない）。
- **コースティクス・屈折の多重反射**。
- 総じて **「写真と見分けがつかない」領域はパストレーサの仕事**。Filament は
  1 フレーム数 ms のラスタライザで、そこは狙っていない。

WizEngine の用途（サーバーで物理を回して映像を配る）では、**60 fps で回ること**が
前提なので、この線引きはむしろ都合が良い。写真そのものが要るカット（資料・
広報用の静止画）は、同じシーン文書をオフラインのレンダラへ渡すのが筋。

---

## 今回入れたもの

### 1. `<visual>` — 「どう撮るか」をシーンの一部に

```xml
<visual>
  <quality shadowMap="2048" cascades="3" shadow="pcss" contactShadows="true"
           msaa="4" taa="false" fxaa="true"/>
  <postprocess enabled="true" ssao="true" ssaoIntensity="1.2" bloom="0.08"
               ssr="true" dof="0" dofBlur="1" vignette="0.25"/>
  <exposure aperture="16" shutter="125" sensitivity="100"/>
  <grading tonemap="aces" contrast="1.05" saturation="1.02" temperature="0" tint="0"/>
</visual>
```

- 節を書かない文書は**既定値 = 従来の絵**。古いシーンの見た目は変わらない。
- ブラウザの Inspector に「描画」節を追加（**Draft / Standard / Photo** の
  プリセット + 全項目の数値）。プリセットの中身は C++（`editor::renderPreset`）が
  唯一の定義で、UI はボタンが名前を送るだけ。
- 全カメラに同時に効く。あとから作られるビュー（カメラの遅延生成）にも当たる。

### 2. 材質 — `<geom>` / `<part>` の属性

```xml
<geom type="sphere" size="0.22" mass="3" rgba="0.95 0.96 0.97 1"
      roughness="0.05" metallic="1"/>                        <!-- 鏡面の金属 -->
<geom type="box" size="0.3 0.16 0.22" mass="6" rgba="0.55 0.06 0.10 1"
      roughness="0.35" clearcoat="1" clearcoatRoughness="0.03"/>  <!-- 車の塗装 -->
<geom type="box" size="0.12 0.2 0.12" mass="1" rgba="0.25 0.85 0.95 1"
      emissive="6"/>                                          <!-- 発光（ブルームの光源）-->
```

`roughness` / `metallic` / `reflectance` / `clearcoat` / `clearcoatRoughness` /
`emissive`。**既定値と同じ値は保存時に書かない**ので、材質を触っていない
シーンのファイルは 1 バイトも太らない。

### 3. 背景と地面

- `<environment hdr="studio.hdr" intensity="25000" skybox="true"/>`
  — 環境マップを**背景にも**出す。映り込みと背景が同じキューブマップになるので、
  金属や光沢のある物が「そこにある」ように見える。**写真らしさが最も安く上がる
  スイッチ**（追加コストはほぼゼロ）。
- `<ground roughness="0.25"/>` — 磨かれた床。SSR と組み合わせると物が映り込む。

### 4. 見本シーン

`assets/scenes/photoreal.xml`（金属の粗さ違い 3 つ・誘電体の粗さ違い 3 つ・
クリアコートの塗装・ニス塗り・自己発光 + Photo プリセット）。

---

## 3 分で違いを見る

1. `assets/` に HDR を 1 つ置く（<https://polyhaven.com/hdris> の 2k で十分。
   ファイル名を `studio.hdr` にする）。**これが一番効く**。
2. 起動 → アセットパネルのタイル **photoreal** をダブルクリックで読み込み。
3. 比較するときは Inspector →「描画」の **Draft ⇄ Photo** を往復する。

HDR が無くても起動はする（警告を出して一様アンビエントになる）が、金属は
映すものが無いので黒くなる。

---

## 効果と費用

体感で効く順（＝先に触るべき順）:

| 施策 | 見た目への効き | 費用 |
| --- | --- | --- |
| 環境マップ + `skybox="true"` | ★★★ | ほぼ無料 |
| 材質（金属・粗さ・クリアコート） | ★★★ | 無料 |
| トーンマップ（ACES）と露出 | ★★ | 無料 |
| SSAO | ★★ | 小〜中 |
| 影を PCSS + 2048 + カスケード | ★★ | 中 |
| MSAA 4x | ★★ | 大（解像度に比例） |
| SSR | ★ | 中（床が磨かれている時だけ効く） |
| ブルーム | ★ | 小 |
| TAA | ★ | 小。ただし**速い剛体に残像** |

WizEngine は**視聴者がいない間は完全に休む**（`kIdleWhenUnwatched`）ので、
品質を上げても「誰も見ていない時間」の費用は増えない。実測はブラウザ下部の
オーバーレイ（`/stats` の fps と renderFrame の ms）で見る。

### 配信との兼ね合い（この構成に特有の注意）

サーバーで描いて H.264 / VP9 で配る以上、**最終的な見た目はビットレートでも
決まる**。細かいディザ・フィルムグレイン・強いビネットは圧縮が苦手な情報で、
ビットレートを食ってブロックノイズに化ける。フォトリアルを狙うなら、後処理を
盛るより先に `kVideoBitrate` と解像度を上げる方が効くことが多い。

---

## 次にやるなら（投資対効果順）

1. **材質テクスチャ**（baseColor / 法線 / ラフネス / AO マップ）。いまは 1 物体
   1 色で、表面の凹凸と汚れが無い。**写実にいちばん足りていないのはここ**
   （法線マップ 1 枚で、単色の箱が「素材」になる）。
2. **HDR の質**。実測の環境マップ（撮影地の光）にすると、それだけで整合する。
3. **ガラス**（Filament のスクリーン空間屈折）。透明な部品が要るなら。
4. **GTAO + ベント法線**（Filament にあるが、いまは SAO の既定を使っている）。
5. 面光源が要るなら Filament では代替（スポット + 発光する板）で近似するしかない。

---

## 関連

- 章の説明: `CLAUDE.md` の「フォトリアル描画（`<visual>` と材質）」
- 型の定義: `src/document/EditorTypes.h`（`RenderDesc` / `MaterialDesc` / `renderPreset`）
- 文書の読み書き: `src/document/SceneDocument.cpp`（`visualElement` / `visualFromXml`）
- 変換: `src/scene/RenderBridge.h`
- 適用: `src/render/Renderer.cpp`（`setRenderSettings` / `rebuildColorGrading` /
  `applyViewSettings` / `refreshSkybox`）
- マテリアル: `assets/materials/shaded.mat` / `ground_lit.mat`
