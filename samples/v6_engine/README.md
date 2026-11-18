# V6 エンジン（`assets/scenes/v6_engine.xml`）の部品メッシュの生成

`samples/` は見本シーンの素材を作るためのもの（エンジン本体のビルドにも実行にも
使わない）。シーンごとにフォルダを分ける。

`assets/models/v6/*.glb` と `assets/env/engine_studio.hdr` はここのスクリプトで
作っている（手で描いたモデルではない）。寸法を変えたら作り直して、シーンの
ボディの位置（カム・バルブ）も同じ `geom.py` の値に合わせる。

```bash
pip install manifold3d trimesh numpy scipy networkx
cd samples/v6_engine
python3 meshes.py      # 動く部品: クランク・コンロッド・ピストン・バルブ・カム
python3 statics.py     # 動かない部品: ブロック・ヘッド・吸気・排気とターボ・チェーン・架台とダイナモ
python3 studio_hdr.py ../../assets/env/engine_studio.hdr   # 環境光
```

- `geom.py` … 寸法の唯一の定義（クランク半径・コンロッド長・ボア・バルブの傾きと
  位置・カム軸の位置・点火順）。シーンのボディとジョイントの位置もここから出す。
- `meshlib.py` … CSG（manifold3d）→ PBR 材質付きの .glb（trimesh）の道具と材質の表。
  CSG の断面（カットモデルの切り口）だけ別の材質（赤）にする `add_split` もここ。
- 各部品は**そのボディの座標系**で作る（プレハブの部品は pos 0・回転 0・倍率 1）。
  原点: クランク = クランク中心、コンロッド = 2 つの穴の中点、ピストン = ピン中心、
  バルブ = 傘の面の中心（バルブ軸が +Y）、カム = カム軸、ブロック = クランク中心。
- カムローブの形は `valvetrain` の式のリフト（余弦形、作用角 240°）を平らなバケットで
  受けたときの包絡線（半平面の交わり）なので、絵とバルブの動きが一致する。
