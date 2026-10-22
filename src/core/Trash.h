#pragma once

#include <filesystem>
#include <string>

namespace wizengine {

// ファイルを OS のごみ箱へ移す（元に戻せる削除）。
//   Windows: シェルのごみ箱（SHFileOperationW + FOF_ALLOWUNDO。確認・進捗・
//            エラーのダイアログは出さない）。**ごみ箱を使えないドライブでは
//            完全に消える**（ネットワークドライブ・ごみ箱を切っている設定・
//            ごみ箱の容量超過。シェルの仕様）。その警告ダイアログを出す
//            FOF_WANTNUKEWARNING は付けない: 呼び手（物理スレッド）が止まるため。
//   それ以外: 同じフォルダの .trash/ サブフォルダへ移す（名前がぶつかれば
//            _2, _3 … を付けて空いている名前へ。上書きはしない）。フォルダの
//            一覧（directory_iterator）は再帰しないので、移した物は一覧に出ない。
// 成功なら true。失敗なら false と英語の reason（ファイルはそのまま）。
bool moveToTrash(const std::filesystem::path& path, std::string& reason);

}  // namespace wizengine
