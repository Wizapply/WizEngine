# meshgen - 手元で .glb を作る（ローカルの Text to 3D）

Meshy（クラウドの Text to 3D）を使わずに、**手元で 3D モデル（.glb）を作る**ための
道具。`samples/v6_engine/` と同じ流儀（CSG = manifold3d → PBR 材質付きの .glb =
trimesh）で、違いは**スクリプトを LLM が書く**こと。

```
依頼文 ──► LLM（OpenAI 互換。config.json の ai.llm と同じ接続先）
         └─ cad.py の API で build(m) を書いた Python スクリプト
スクリプト ──► 別プロセスで制限付きに実行 ──► .glb + 確認用 .png
         └─ 例外・規則違反・浮いた部品があれば、内容を LLM に返して直させる（--repairs 回）
```

AI の画像生成系（Meshy / TRELLIS など）と違い、出来るのは**形を組み合わせた
CG らしい物**（家具・道具・機械・建物の部品・簡単な動物）。その代わり GPU が要らず、
寸法が正確で、ポリゴンが少なく、**スクリプトを読んで手で直せる**。さらに V6 エンジンの
見本と同じく、**動く所ごとに別の部品（.glb）に分けて、ジョイントでつないだシーン文書まで
出せる**（下の「動く部品のある物（組み立て）」）。

## 準備

```bash
pip install manifold3d trimesh numpy scipy networkx matplotlib
```

matplotlib は確認用の PNG にだけ使う（無ければ PNG を出さない）。

LLM の接続先は `config.json`（`build/config.json` があればそちら）の `ai.llm`
（`url` / `model` / `apiKey`）を読む。API キーが空なら環境変数 `WIZ_LLM_API_KEY` →
`OPENAI_API_KEY`。`--url` / `--model` で上書きできる。

## 使い方

```bash
cd samples/meshgen
python3 meshgen.py "木のダイニングチェア、クッション付き" --height 0.9
python3 meshgen.py "a red fire hydrant" --height 0.8 --name hydrant
python3 meshgen.py "引き出し 2 段と扉のある木の棚" --parts --scene      # 動く部品に分ける
# Ollama（キー不要）
python3 meshgen.py "a wooden barrel" --height 0.9 --url http://127.0.0.1:11434/v1 --model qwen2.5-coder:14b
```

出力は既定で `assets/generated/local/`（.gitignore 済み）。動かない物は次の 4 つ
（組み立ては下の章）:

| ファイル | 中身 |
|---|---|
| `<name>.glb` | モデル（材質ごとに 1 メッシュ、法線付き） |
| `<name>.py` | LLM が書いたスクリプト。**手で直して作り直せる** |
| `<name>.png` | 斜め前と横からの確認用の絵 |
| `<name>.chat.txt` | LLM との往復の全文（指示文・返答・検査の結果） |

スクリプトから作り直す（LLM を使わない）:

```bash
python3 meshgen.py --script ../../assets/generated/local/chair.py --height 0.9
python3 meshgen.py --script examples/mug.py -o ../../assets/generated/local/mug.glb
python3 meshgen.py --script examples/cabinet.py -o ../../assets/generated/local/cabinet.glb --scene
```

主なオプション: `--height`（全体の高さ (m) に合わせて等倍で縮尺）、`--origin
center|bottom`（原点 = 外接箱の中心〔既定。エンジンの body の pos と同じ意味〕か底の
中心）、`--repairs`（直させる回数、既定 3）、`--timeout`（スクリプトの実行時間の上限、
既定 120 s）、`--no-png`、`--print-prompt`（LLM に渡す指示文を表示）、`--parts`（LLM に
動く部品へ分けるよう頼む）、`--scene`（組み立てのシーン文書を `assets/scenes/` にも置く）、
`--layer`（組み立ての部品の衝突レイヤ、既定 7）。

## シーンで使う

終わりに表示される XML をそのまま使える（`file` は assets/ からの相対）:

```xml
<asset>
  <mesh name="chair" file="generated/local/chair.glb" scale="1"/>
</asset>
<worldbody>
  <body name="chair1" pos="0 0.5 0">
    <geom type="mesh" mesh="chair" size="0.3" mass="6"/>
  </body>
</worldbody>
```

当たり判定はエンジンの規則どおりモデルの凸包（椅子の下に物は入らない）。

## 動く部品のある物（組み立て）

扉・引き出し・蓋・車輪・羽根のような**動く所のある物は、動く単位ごとに部品へ分ける**。
`samples/v6_engine/`（クランク・コンロッド・ピストン…を別の .glb にしてジョイントで
つないだ）と同じ形を、スクリプト 1 本から作る:

```python
def build(m):
    base = m.part("base", fixed=True, collision="trimesh")   # 動かない本体（中が空洞のまま当たる）
    base.add(box(0.6, 0.9, 0.45, at=(0, 0.45, 0)) - box(0.56, 0.86, 0.43, at=(0, 0.45, 0.02)), "wood")
    door = m.part("door")                                    # 動く部品
    door.add(box(0.56, 0.66, 0.02, at=(0, 0.36, 0.235)), "wood")
    m.joint("door", "base", "hinge", anchor=(-0.285, 0.36, 0.235), axis=(0, -1, 0), range=(0, 110))
```

- 部品は全部**組み上がった姿勢のまま、同じ座標で**作る。`m.joint(子, 親, 種類, anchor, axis)` で
  親につなぐ（親が `"world"` なら地面）。種類は `hinge`（回転）/ `slide`（直動）/ `ball` /
  `weld`。`range=(lo, hi)`（hinge は度、slide は m、作った姿勢が 0）、`motor="speed"|"position"`
  と `target`、`stiffness` / `damping`。hinge の角度は **axis まわりの右ねじが正**（Chrono と
  同じ）なので、「開く向きが正」になるよう axis の向きを選ぶ。
- 出力:

  | ファイル | 中身 |
  |---|---|
  | `<name>/<部品>.glb` | 部品ごとのモデル。**原点はその部品の凸包の体積重心**（エンジンは凸包で当たる剛体の原点を体積重心へ寄せるので、こうしておくと文書の pos がそのまま置き場所になる） |
  | `<name>.xml` | シーン文書: `<mesh>` の宣言・部品ごとの `<body>`（質量 = 体積 × 材質の密度）・`<equality>` のジョイント・全体が入る cam0。ライトと地面は書かない（エンジンの既定で開く） |
  | `<name>.png` | 左 = 組み上がった姿勢と関節の軸（赤）、右 = **関節を range の 70% まで動かした姿勢**（扉が外へ開くか・引き出しが前へ出るかをここで確かめる） |
  | `<name>.py` / `<name>.chat.txt` | スクリプトと LLM との往復 |

- 使い方: `--scene` を付けると `assets/scenes/<name>.xml` にも置く（アセットパネルのシーンの
  タイルから開ける。meshgen が作った物以外は上書きしない）。今のシーンに足すなら `.xml` の
  中身をブラウザの 📄 XML に貼って適用する。
- **組み立ての部品どうしは当たらない**（全部品に `layer="7" nocollide="7"`）。引き出しが
  本体の凸包の中に入っていても押し合わない（CLAUDE.md の「ジョイントでつないだ物どうしも
  接触する」への対処）。床や他の物（レイヤ 0）とは当たる。代わりに、同じレイヤの別の
  組み立てどうしも当たらない（`--layer` で変えられる）。
- LLM に返す検査: 部品の中の浮いた形、存在しない部品名、関節の位置が部品から離れている
  （全体の 3% か 1 cm を超える）、子に関節が 2 つ、関節の輪、関節の無い部品が 2 つ以上、
  range と motor の併用、fixed の部品に関節。
- 見本: `examples/cabinet.py`（固定の本体 + 蝶番の扉 + 引き出し）、`examples/cart.py`
  （固定しない車体 + 自由に回る 4 輪 = 掴んで押すと転がる）。

## スクリプトの書き方（cad.py）

```python
def build(m):
    wood = material("wood")
    m.add(box(0.45, 0.04, 0.45, at=(0, 0.45, 0)), wood)          # 座面
    leg = box(0.04, 0.455, 0.04, at=(0.195, 0.2275, 0.195))
    m.add(leg.symmetric("x").symmetric("z"), wood)                # 4 本の脚
```

- 単位はメートル、Y が上、角度は度。**プリミティブは全部 `at` = 中心**で置く。
  床に立つ物は最低点を y = 0、前は +Z。
- 形: `box` `rounded_box` `cylinder` `cone` `sphere` `ellipsoid` `capsule` `torus`
  `extrude`（2D の多角形を押し出す）`revolve`（(半径, 高さ) の折れ線を Y 軸で回す）
  `tube`（3D の折れ線に沿った管）`hull`（凸包。球を包むと有機的な塊）。
- 組み合わせ: `a + b`（和）、`a - b`（差）、`a & b`（積）、`union(...)`。
  変換: `.move()` `.rotate()`（R = Rz Ry Rx、シーン文書の euler と同じ）
  `.rotate_about()` `.scale()` `.mirror()` `.symmetric()`（自分 + 鏡映）。
- 材質: `material("steel")` のようなプリセット名（`cad.py` の `PRESETS`）か、
  `material("#a0522d", metal=0, rough=0.6, emissive=0)`。色は **sRGB（見たままの色）**で
  書き、glTF へはリニアに直して入れる。
- 材質の `density`（kg/m³、プリセットは木 600・鋼 7850・樹脂 1100 など）が組み立ての
  部品の質量になる。`m.part(..., mass=kg)` で直接決めてもよい。
- **1 つの部品（動かない物ならその全体）の形どうしは 2〜5 mm 重ねる**。和が 2 つ以上に
  分かれたら「浮いた形」として LLM に返す（ぴったり接しているだけの面は manifold が
  つなぐので 1 つになるが、確実にするため重ねる）。
- 見本は `examples/`（`chair.py` と `cabinet.py` は LLM への指示文にも載る）。

## 安全について

LLM が書いたコードを動かすので、次の 3 重で守っている:

1. 構文木の検査（import・`_` で始まる名前と属性・`open` / `eval` / `getattr` など・
   while / try / lambda / class を弾く）
2. 組み込み関数を絞った名前空間で実行（`range` `len` `min` … だけ）
3. 別プロセスで実行（時間切れ `--timeout`、Linux / macOS はメモリ 4 GB の上限）

ただし Python の中での隔離は原理的に完全ではない。信用できない LLM の出力を回すときは
コンテナや VM の中で使うこと。

## 未（次の段）

- **エンジンの生成経路へつなぐ**: いまは手で回す道具。エンジンの `<mesh generate>`
  からこれを呼ぶには、Meshy 互換の API（タスクの作成 → 状態の取得 → .glb の URL）を
  話す小さなローカルサーバーにして `ai.meshy.url` をそこへ向けるのが、エンジン側を
  変えずに済む道。
- 見た目の確認を LLM に返す（PNG を画像入力のモデルへ渡して直させる）。
- テクスチャ（いまは形ごとに 1 色）。
- **組み立てはエンジンで未確認**: 文書はエンジンの文書層（`fromXml`）で警告 0 件・.glb は
  `GlbBounds` で読めることまで確認した。Chrono で扉が range どおりに開く・台車が転がる、は
  実機で見る（hinge の正の向きは Chrono の約束どおりのはずだが未確認）。
- 部品どうしの関節を 2 つ以上（閉じたリンク機構）、ソケット付きのプレハブ部品としての書き出し
  （車両の車輪など）、ジョイントのばね以外の Chrono の機能（gear・screw など）。
