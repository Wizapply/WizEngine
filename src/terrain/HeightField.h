#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include <Eigen/Core>

// テレインの「高さ格子」: 生成・問い合わせ・メッシュ化の純粋な計算。
//
// src/vehicle/ と同じ位置付けで、Chrono も Filament もシーン文書
// （EditorTypes.h）も知らない。だから単体で回してテストできる
// （tests/terrain/）し、どのスレッドでもコピーできる。境界（Scene）が
// 文書の値（editor::TerrainDesc）を TerrainParams に詰め替え、出てきた
// 配列を Renderer（TerrainChunkMesh）へ詰め替えて渡す。当たり判定は
// 格子そのものを使う（TerrainContact = PhysicsWorld::setTerrain の自前の接触）。
//
// 座標の約束:
//   * 格子は X-Z 平面に並ぶ正方形。頂点 (i, j) の i が +X、j が +Z の向き。
//     頂点数は 1 辺 resolution + 1、セル（クアッド）数は 1 辺 resolution。
//   * ワールド座標 = (minX + i * cell, baseY + h(i, j), minZ + j * cell)。
//     minX / minZ は「中心 - size / 2」。高さは baseY からの相対値で持つ。
//   * セル (i, j) の 4 頂点を v00 = (i, j), v10 = (i+1, j), v01 = (i, j+1),
//     v11 = (i+1, j+1) と呼ぶ。セルは対角線 v00-v11 で 2 枚の三角形に割り、
//     どの出力（描画・衝突・高さ・法線・レイ）も同じ割り方を使う
//     = 見えている面・当たる面・問い合わせの答えが一致する:
//       三角形 A = (v00, v01, v11)  … セル内で w >= u の側（v01 を含む）
//       三角形 B = (v00, v11, v10)  … セル内で u >  w の側（v10 を含む）
//     u, w はセル内の局所座標（0〜1、u が +X、w が +Z）。巻き方は上（+Y）から
//     見て反時計回り（Renderer::TerrainChunkMesh の約束と同じ）。
namespace wizengine {
namespace terrain {

// 3 次元ベクトル（double）は Eigen（ヘッダのみ、third_parties/eigen）。境界で
// 各エンジンの型に詰め替える。Eigen は既定コンストラクタで値を初期化しない
// ので、零が要る所は Vec3::Zero() で作る。
using Vec3 = Eigen::Vector3d;

// 1 辺のセル数の上限。HeightField のコンストラクタと generateHeightField が
// 共通で使う（(4096 + 1)^2 頂点 = float で約 67 MB。実用の上限は文書側の
// clampTerrain がもっと小さく決める。ここは落ちないための安全弁）。
constexpr int kMaxResolution = 4096;
// チャンク 1 辺のクアッド数の上限。Renderer::addTerrain の上限
// （Renderer::kMaxTerrainQuads）と同じ値に保つこと（両方を見る Scene が
// static_assert で照合する）。
constexpr int kMaxChunkQuads = 256;

// 生成パラメータ。シーン文書の <terrain> の属性に 1 対 1 で対応する素の値
// （文書側の型 editor::TerrainDesc はこれを知らない。Scene が詰め替える）。
// 範囲の丸めは文書側（clampTerrain）が本番で、generateHeightField は
// 落ちない程度の安全弁だけを掛ける。
struct TerrainParams {
    float size = 200.0f;      // 1 辺の長さ (m)。正方形の全幅（半寸法ではない）
    int resolution = 256;     // 1 辺のセル（クアッド）数。頂点は +1
    float height = 20.0f;     // 起伏の高さ (m)。最低点が baseY、最高点が baseY + height
    float posX = 0.0f;        // 中心の X (m)
    float posY = 0.0f;        // 最低点の Y (m) = baseY
    float posZ = 0.0f;        // 中心の Z (m)
    uint32_t seed = 0;        // 乱数の種。同じ種なら同じ地形
    int octaves = 5;          // fbm の重ね数。0 = 平ら（全部 baseY）
    float gain = 0.5f;        // オクターブごとの振幅の倍率
    float lacunarity = 2.0f;  // オクターブごとの周波数の倍率
    float scale = 80.0f;      // 最も大きなうねりの波長 (m)
};

// Renderer::TerrainChunkMesh と同じ並びの素の配列（Renderer のヘッダを
// include しないため別の型にしてある。境界でそのまま move して詰め替える）。
//   positions … 3 個で 1 頂点（ワールド座標 m）
//   normals   … 3 個で 1 頂点（単位ベクトル、滑らかな頂点法線）
//   uvs       … 2 個で 1 頂点（メートル単位 = ワールドの x, z）
//   indices   … 3 個で 1 三角形（上から見て反時計回り）
// 頂点はチャンク内で行優先: 番号 = r * (quads + 1) + c（c が +X、r が +Z）。
// 頂点数 (quads + 1)^2、インデックス数 quads^2 * 6 で固定。
struct ChunkMesh {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<uint32_t> indices;
};

// 格子の添字の矩形。半開区間 [i0, i1) x [j0, j1)（i が +X、j が +Z）。
// 単位（頂点 = サンプルか、セルか）はこの型では決めず、使う側が決めて
// clamped に渡す数で揃える: サンプルなら verticesPerSide()、セルなら
// resolution()。編集の記録（Undo）と描画の作り直し（dirty チャンク）が使う。
struct GridRect {
    int i0 = 0;
    int j0 = 0;
    int i1 = 0;
    int j1 = 0;

    bool empty() const { return i0 >= i1 || j0 >= j1; }
    // 両方を含む最小の矩形。空の側は無視する（両方空なら空）。
    GridRect united(const GridRect& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        return {std::min(i0, o.i0), std::min(j0, o.j0), std::max(i1, o.i1),
                std::max(j1, o.j1)};
    }
    // 四方へ n 広げる（負なら縮める）。空はそのまま。
    GridRect expanded(int n) const {
        if (empty()) return *this;
        return {i0 - n, j0 - n, i1 + n, j1 + n};
    }
    // [0, count) x [0, count) に切り詰める。結果が空なら {0, 0, 0, 0}
    // （裏返った矩形を返さない = (i1 - i0) * (j1 - j0) が負にならない）。
    GridRect clamped(int count) const {
        const GridRect r{std::max(i0, 0), std::max(j0, 0), std::min(i1, count),
                         std::min(j1, count)};
        return r.empty() ? GridRect{} : r;
    }
};

class HeightField {
public:
    // 空（resolution 0）。valid() が false で、問い合わせは全部「無い」扱い。
    HeightField() = default;
    // 平らな格子（高さ全部 0）。resolution は 1〜kMaxResolution、size は
    // 正、座標は有限であること（満たさなければ空のまま）。
    HeightField(int resolution, float size, float centerX, float baseY,
                float centerZ);

    bool valid() const { return resolution_ > 0; }
    int resolution() const { return resolution_; }      // 1 辺のセル数
    int verticesPerSide() const { return resolution_ + 1; }
    float size() const { return size_; }
    float cellSize() const { return cell_; }
    float minX() const { return minX_; }
    float minZ() const { return minZ_; }
    // 最後の頂点（i または j = resolution）の座標。頂点の位置と同じ式
    // （min + resolution * cellSize を double で）で出すので、size と float の
    // 丸めの分だけ違うことがあっても、範囲の判定と頂点が食い違わない。
    double maxX() const { return double(minX_) + double(resolution_) * double(cell_); }
    double maxZ() const { return double(minZ_) + double(resolution_) * double(cell_); }
    float baseY() const { return baseY_; }

    // 格子値（baseY からの相対高さ）。範囲外の添字は端へ丸める。
    float sample(int i, int j) const;
    void setSample(int i, int j, float h);
    // 全格子値（行優先: 番号 = j * verticesPerSide() + i）。
    const std::vector<float>& samples() const { return heights_; }
    std::vector<float>& samples() { return heights_; }
    // 格子値の最小・最大（相対高さ）。空なら 0。
    float minSample() const;
    float maxSample() const;
    // 高さ（格子値 = baseY からの相対値）を全部 k 倍する。高さの上限を変えた
    // ときの比例伸縮に使う。k が正の有限値でなければ何もしない（0 や負で
    // 潰したり裏返したりしない）。
    // 共有中の格子（shared_ptr<const>）には呼べない = コピーに対して呼ぶ。
    // HeightFieldOps.cpp。
    void scaleHeights(float k);

    // 穴（セル単位）。穴のセルは描画で面積 0、接触を作らず、レイが抜ける
    // （どれも isSolidCell を通る）。範囲外の添字は setHole が無視し、
    // holeAt が false。
    void setHole(int i, int j, bool hole);
    bool holeAt(int i, int j) const;
    bool anyHoles() const;   // 穴が 1 つでもあるか（全セルを走査する）
    void clearHoles();       // 穴を全部埋める

    // 格子値（高さ・穴）を保ったまま、大きさと位置だけを変えた新しい格子。
    // 引数の条件はコンストラクタと同じ（満たさなければ空）。HeightFieldOps.cpp。
    HeightField withGeometry(float size, float centerX, float baseY, float centerZ) const;
    // 大きさと位置を保ったまま、1 辺のセル数を resolution にした新しい格子。
    // 高さは新しい格子点で heightAt（地表と同じ 2 三角形の補間。格子点が重なる
    // 所では元の値そのもの）。縮小は格子点で拾うので、細い起伏は失われうる。
    // 穴は、縮小なら新しいセルと重なる元のセルが 1 つでも穴なら穴（OR = 穴が
    // 消えない）、拡大なら新しいセルの中心を含む元のセル（最近傍）。
    // resolution が 1〜kMaxResolution の外・空の格子なら空。HeightFieldOps.cpp。
    HeightField resampled(int resolution) const;

    // (x, z) が格子の水平な範囲（境界を含む）に入っているか。
    bool contains(double x, double z) const;
    // (x, z) の真下に地面の中身があるか（= contains かつ、そのセルが穴でない）。
    // セルは heightAt / normalAt と同じ選び方（floor、最後の頂点の上は端の
    // セル）。範囲外・空の格子・有限でない座標は false。テレインの接触検出
    // （TerrainContact）が点ごとに見る。穴は isSolidCell が受け持つ。
    bool solidAt(double x, double z) const;

    // 地表のワールド Y。描画・衝突と同じ 2 三角形の平面で補間する（セル内で
    // 区分線形 = 格子点では格子値そのもの）。**範囲外は端へ丸めた点の高さ**
    // （格子の外へ縁の高さが続いているとみなす）。外かどうかを区別したい
    // ときは contains を先に見る。空の格子は 0、x / z が有限でなければ baseY。
    double heightAt(double x, double z) const;
    // 地表の法線（ワールド、単位ベクトル、+Y 側）。(x, z) を含む三角形の
    // 面法線 = heightAt の勾配。セルは floor で選び、対角線ちょうど（u == w）は
    // 三角形 A の側。範囲外は heightAt と同じく端へ丸める。空の格子と、x / z が
    // 有限でないときは (0, 1, 0)。
    Vec3 normalAt(double x, double z) const;

    // 頂点 (i, j) の滑らかな法線（隣の格子値の中央差分。端は片側差分）。
    // 描画の頂点法線に使う。チャンクの継ぎ目でも同じ値になる。
    Vec3 vertexNormal(int i, int j) const;

    // 1 辺 quadsPerChunk クアッドのチャンクに割ったときの 1 辺のチャンク数
    // （切り上げ）。resolution が割り切れないときは、最後の列 / 行のチャンクが
    // 一部だけ埋まる（buildChunkMesh 参照）。
    int chunksPerSide(int quadsPerChunk) const;
    // セルの矩形 cells（TerrainEdit の rect と同じセル単位）を書き換えたとき、
    // 作り直しが要るチャンクの矩形（チャンク単位、0〜chunksPerSide に切り詰め
    // 済み）。頂点法線は隣のサンプルを見るので、書き換えた角のサンプルの ±1 を
    // 含むチャンク（継ぎ目の頂点は両側のチャンクが持つ）を全部入れる。cells が
    // 空・空の格子・quadsPerChunk < 1 なら空。HeightFieldOps.cpp。
    GridRect chunksTouched(const GridRect& cells, int quadsPerChunk) const;

    // チャンク (cx, cz) のメッシュを out に書く（配列は作り直す）。
    // チャンクが格子の外にはみ出す部分（resolution が quadsPerChunk で
    // 割り切れないとき）は、頂点を格子の端へ丸め、そのクアッドは面積 0 の
    // 三角形（同じ頂点 3 回）にする = 頂点数・インデックス数は常に固定。
    // 穴のセルも同じ形で入る（isSolidCell）。引数が不正（チャンクの
    // 番号が範囲外・quadsPerChunk が 1〜kMaxChunkQuads の外）なら false。
    bool buildChunkMesh(int cx, int cz, int quadsPerChunk, ChunkMesh& out) const;

    // レイと地表の交差。origin から dir（正規化しなくてよい。内部で正規化
    // する）の向きへ maxDist (m) まで、格子を DDA で辿ってセルの 2 三角形と
    // 当てる。当たれば最初の交点までの距離 (m) と面法線を書いて true。
    //   * 面は片面: 上（法線の側）から入るレイだけが当たる（地中から上へ
    //     向けたレイは当たらない）。
    //   * 格子の水平な範囲の外では当たらない（縁の外に壁は無い）。
    //   * dir が零ベクトル・空の格子・maxDist <= 0 は当たらない。origin / dir
    //     に有限でない成分がある・maxDist が NaN も当たらない（maxDist = +Inf は可）。
    //   * 1 つのセルの中で 2 三角形の両方に当たったときは、距離が違えば
    //     近い方（斜めのレイが尾根をかすめる場合）、同じ距離（対角線の
    //     ごく近く）なら交点の (u, w) で選ぶ（対角線ちょうどは A）= normalAt と
    //     同じ規則・同じ式。片方だけ当たればその三角形。だから真下向きのレイは normalAt と
    //     同じ三角形を選ぶ（法線の値は計算式が違うので丸め誤差の範囲で
    //     一致）。斜めのレイがセルの辺・格子点ちょうどに当たったときは、
    //     DDA が先に辿ったセル（= floor のセルとは限らない）の三角形の法線に
    //     なりうる。距離は丸め誤差の範囲で heightAt と一致する（高さは連続な
    //     ので、どちらのセルで当てても同じ点）。
    // 当たらなければ tOut / normalOut は触らない。
    bool raycast(const Vec3& origin, const Vec3& dir, double maxDist, double& tOut,
                 Vec3& normalOut) const;

private:
    // セル (i, j) が中身を持つか（範囲内かつ穴でない）。描画（縮退三角形）と
    // 接触（solidAt = 接触を作らない）とレイ（hitCell = 当たらない）が
    // ここを通るので、穴に同時に従う。
    bool isSolidCell(int i, int j) const;
    // セル (i, j) の 2 三角形とレイの交差（片面）。t の範囲 [tMin, tMax]。
    bool hitCell(int i, int j, const Vec3& o, const Vec3& d, double tMin,
                 double tMax, double& tOut, Vec3& normalOut) const;
    // (x, z) を含むセル (i, j)（floor、範囲外は端へ丸める）とセル内の (u, w)。
    // x / z は有限であること。
    void locate(double x, double z, int& i, int& j, double& u, double& w) const;
    // 指定のセル (i, j) から見た (x, z) の (u, w)。locate と同じ式なので、
    // 同じ点なら同じ値になる（対角線の判定 w >= u が食い違わない）。
    void cellLocal(double x, double z, int i, int j, double& u, double& w) const;
    // 頂点 (i, j) のワールド座標（添字は範囲内であること）。
    Vec3 vertexWorld(int i, int j) const;

    int resolution_ = 0;
    float size_ = 0.0f;
    float cell_ = 0.0f;
    float minX_ = 0.0f;
    float minZ_ = 0.0f;
    float baseY_ = 0.0f;
    std::vector<float> heights_;
    // 穴のマスク（セルごと、行優先: 番号 = j * resolution + i）。1 = 穴、
    // 0 = 中身。**空なら穴は無い**（ただし穴を全部埋めたあとは、全部 0 の
    // 配列のこともある）: 最初の穴を開けるまで確保しない（穴の無いテレインが
    // 大半で、1024 なら 1 MB を持たずに済む）。保存形式（.terrain の HOLE は
    // 0 = 穴）とは別。
    std::vector<uint8_t> holes_;
};

// パラメータから高さ格子を作る（TerrainNoise.cpp、stb_perlin の fbm）。
// fbm の値を格子全体の最小〜最大で 0〜height に伸ばす（= height が実際の
// 起伏の高さになる）。地形の形は中心からの相対座標で決まるので、pos を
// 動かしても形は変わらない。
// 同じパラメータなら必ず同じ格子（決定的。スレッドにも依存しない）。
// 範囲外のパラメータは安全側へ丸める（resolution 1〜kMaxResolution、size > 0、
// octaves 0〜16、scale > 0）。
HeightField generateHeightField(const TerrainParams& params);

}  // namespace terrain
}  // namespace wizengine
