#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "terrain/TerrainFile.h"

// assets/terrains/ の窓口（ファサード）: 一覧と見出しのキャッシュ・読込時の
// 解決（名前と背番号 → 使うファイル）・名前の変更・空いている名前の採番。
//
// 1 ファイルの中身の読み書きは TerrainFile の担当で、ここは「どのファイルか」
// だけを決める。シーン文書も Scene も知らない（フォルダのパスは呼び手が
// assetPath() で解決して渡す）。
//
// 名前 = 拡張子 .terrain を除いたファイル名で、core/SafeName.h の規則
// （英数字と _ -、64 文字まで）を満たすものだけを扱う。名前の一致は大文字・
// 小文字を区別しない（Windows のファイル名と同じ = "Terrain" と "terrain" を
// 別のテレインとして作らない）。
namespace wizengine {
namespace terrain {

// 一覧の 1 件（見出しだけを読んだ値）。
struct TerrainEntry {
    std::string name;
    TerrainGuid guid;
    int resolution = 0;
    float size = 0.0f;
    float height = 0.0f;
};

// 読込時の解決の結果（resolveTerrain）。
struct TerrainResolution {
    std::string file;   // 使うファイルの名前。空 = ファイル無し（平らで作る）
    TerrainGuid guid;   // 採用する背番号
    bool dataChanged = false;  // XML の data を file に直すべき（file != data 名）
    bool dirty = false;        // ファイルが無い = 次の保存で書くべき
    std::vector<std::string> warnings;  // 英語。ログ（LOGW）とステータスの件数へ
    std::vector<std::string> notes;     // 英語。ログ（LOGI）とステータスへ
};

// 読込時の解決規則（純粋関数。「名前順の最初」は entries の並びに依らず
// 名前の大小で選ぶ）。
//   A. 名前のファイルがあり背番号も一致 → それ
//   B. 名前はあるが背番号が違う → 背番号で走査。1 個 → それ（note）/
//      複数 → 名前順の最初（warning）/ 0 個 → 名前のファイルでその背番号を採る（warning）
//   C. 名前のファイルが無い → 背番号で走査。1 個 → それ（note）/ 複数 → 最初
//      （warning）/ 0 個 → ファイル無し + dirty、背番号は保つ（warning）
//   D. 背番号が無い → 名前のファイルでその背番号を採る。名前のファイルも
//      無ければ新しく採番（ファイル無し + dirty。名前があって見つからなければ warning）
// data 名が空で背番号だけがあるときは C と同じ（0 個なら file も空）。
TerrainResolution resolveTerrain(const std::vector<TerrainEntry>& entries,
                                 const std::string& dataName, const TerrainGuid& guid);

// 名前の変更の結果。
enum class TerrainRenameStatus {
    Ok,
    InvalidName,    // 正規化すると空
    SameName,       // from と同じ
    AlreadyExists,  // to の名前のファイルが既にある
    NotFound,       // from のファイルが無い（to は正しく空いている = 参照だけ変えてよい）
    IoError,        // ファイルの改名に失敗
};

struct TerrainRenameResult {
    TerrainRenameStatus status = TerrainRenameStatus::InvalidName;
    std::string name;    // 正規化した新しい名前（InvalidName 以外で入る）
    std::string reason;  // 英語。Ok なら空
    bool ok() const { return status == TerrainRenameStatus::Ok; }
};

// 所有者は Scene（refresh / rename は物理スレッド）。entries() だけは他の
// スレッド（HTTP）からも呼べる = キャッシュは mutex で守り、コピーを返す。
class TerrainLibrary {
public:
    explicit TerrainLibrary(std::filesystem::path dir);

    const std::filesystem::path& dir() const { return dir_; }

    // フォルダを走査して一覧を作り直す（*.terrain の見出しだけを読む）。
    // 読めないファイル・名前に使えない文字を含むファイルは一覧から外し、
    // 理由（英語）を返す。フォルダが無いのは空の一覧（警告なし）。
    std::vector<std::string> refresh();

    // 一覧のコピー（名前順）。
    std::vector<TerrainEntry> entries() const;

    // 今の一覧で resolveTerrain を引く。
    TerrainResolution resolve(const std::string& dataName, const TerrainGuid& guid) const;

    // ファイル from.terrain を to.terrain へ改名する（背番号は変えない）。
    // to は正規化する。失敗したら何も変えない。判定の順は InvalidName →
    // SameName → AlreadyExists → NotFound（だから NotFound なら to は有効で
    // 空いている）。
    TerrainRenameResult rename(const std::string& from, const std::string& to);

    // base（正規化する。空なら "terrain"）が空いていればそれ、ふさがって
    // いれば base_2, base_3 …。64 文字に収まるよう base 側を削る。
    std::string uniqueName(const std::string& base) const;

    // 名前のファイルのパス。name が正規化済みの名前でなければ空のパス。
    std::filesystem::path pathFor(const std::string& name) const;

private:
    // 名前がふさがっているか（キャッシュとディスクの両方を見る）。except と
    // 同じ名前（大文字・小文字を問わず）のキャッシュは数えない。
    bool taken(const std::string& name, const std::string& except = {}) const;

    std::filesystem::path dir_;
    mutable std::mutex mutex_;  // entries_ だけを守る（ファイル操作の間は持たない）
    std::vector<TerrainEntry> entries_;  // 名前順
};

}  // namespace terrain
}  // namespace wizengine
