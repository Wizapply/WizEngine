#pragma once

#include <memory>
#include <string>

#include "vehicle/Formula.h"

// LuaJIT でノード式を実行する側。ユーザーは Lua を見ない: FormulaProgram が
// 生成したソース（luaSource）をここでコンパイルし、車輪ごとの実体を作る。
//
// 設計の要点:
//   * Lua ステートは 1 つ（物理スレッド専用。VehicleComponent が持つ）。
//     他のスレッドから触らない - Chrono と同じ約束。
//   * 入出力と状態は Lua 側で確保した FFI の double 配列。C++ はその
//     アドレスを覚えて直接読み書きするので、ティックごとのコピーも
//     テーブル生成も無い（GC を起こさない）。配列はレジストリに繋いで
//     おくので回収されない（LuaJIT の GC はオブジェクトを動かさない）。
//   * 1 グラフ = 1 関数。ノード境界のコストはトレース JIT が消す。
//   * LuaJIT の無いビルド（WIZ_HAVE_LUAJIT 未定義）では available() が false
//     で、FormulaProgram::interpret()（C++ インタプリタ）に落ちる。
struct lua_State;

namespace wizengine {
namespace vehicle {

class LuaRuntime {
public:
    static bool available();

    LuaRuntime();
    ~LuaRuntime();
    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    bool ok() const { return L_ != nullptr; }
    // "LuaJIT 2.1.x" のような版名（起動ログ用）。
    std::string version() const;
    // JIT が有効か（無効ならインタプリタ実行で遅い - 起動時に警告する）。
    bool jitEnabled() const;

    // プログラムをコンパイルして実体を作る。失敗（Lua の構文エラー = 生成器の
    // バグ、または LuaJIT 無し）なら nullptr で error に理由。
    std::unique_ptr<FormulaInstance> instantiate(
        const std::shared_ptr<const FormulaProgram>& program, std::string& error);

    // テスト用: Lua ソースを直接評価して数値を返す。
    bool evalNumber(const std::string& chunk, double& out, std::string& error);

    lua_State* state() { return L_; }

private:
    lua_State* L_ = nullptr;
    int newBuffersRef_ = -1;  // FFI 配列を作るヘルパ関数
};

}  // namespace vehicle
}  // namespace wizengine
