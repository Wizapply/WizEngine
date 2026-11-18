#pragma once

#include <cstddef>
#include <string>

#include "document/EditorTypes.h"     // AppMode
#include "physics/PhysicsBackend.h"

// Scene が使う**実行環境の設定**（起動設定 config.json の "scene" / "physics"）。
//
// 以前は SceneConfig.h の constexpr だったもののうち、「どのマシンで・どう
// 動かすか」で変わる値だけをここへ移した（カメラのスロット数・エディタ
// カメラ・起動時のシーンとモード・視聴者がいない間の休止・物理バックエンドの
// 選び方）。物理の手触り（摩擦・減衰・反復回数など）やライトの初期構成は
// エンジンの既定値なので SceneConfig.h に残る。
//
// **既定値の定義はこのヘッダが唯一**。同梱の config.json は同じ値を書いて
// あるだけで、キーを省けばここの値になる（app/AppConfig が読む）。
struct SceneSystemConfig {
    // ---- カメラ ----------------------------------------------------------
    // カメラのスロット数（ページ /cam0/ .. /camN-1/。1〜16）。ページと WebRTC の
    // 受け口は起動時にこの数だけ作られる固定プール（どちらも軽い）。動画
    // ビュー（スワップチェーン + 読み戻しバッファ）は予備スロットぶんを
    // 作り置きせず、「カメラを追加」かページ初訪問の時点で描画スレッドが作る。
    std::size_t maxCameras = 5;
    // エディタ操作（配置・ギズモ・保存 / 読込）を受け付けるカメラ番号。範囲外は 0。
    std::size_t editorCamera = 0;

    // ---- 起動 ------------------------------------------------------------
    // 起動時に読むシーン文書（assets/scenes の名前。拡張子なし）。読めなければ
    // 警告して空のシーン（地面のみ）で起動する。空文字列 = 常に空で起動。
    std::string startupScene = "default";
    // 起動時のモード。Simulate = 置いた物がいきなり落ちてくる従来どおり、
    // Editor = 物理を止めて配置から入る。ブラウザからいつでも切り替えられる。
    wizengine::editor::AppMode startMode = wizengine::editor::AppMode::Simulate;
    // web モードで、視聴者がいない間は物理・描画・エンコードを全部止める。
    // false = 見ていなくても回し続ける。
    bool idleWhenUnwatched = true;

    // ---- 物理バックエンドの選び方（Scene::requiredBackend）----------------
    // シーンが何も要求しないときの系。Core を要する機能（FEA ケーブル・ChLoad・
    // 直接法・HHT・モーダル・テレイン）があれば Core、ソフトボディか多数の
    // 剛体があれば Multicore（ビルドにあれば）、どちらでもなければこれ。
    PhysicsBackend backend = PhysicsBackend::Core;
    // ソフトボディが 1 個でもあれば Multicore へ（1 個で最大 512 粒子 = 剛体）。
    bool multicoreForSoftBodies = true;
    // 剛体（プレハブの collide 部品も 1 個と数える）がこの数以上で Multicore。
    // 0 で無効。
    int multicoreMinBodies = 200;
    // テレインのあるシーンは Core にする（テレインの自前の接触は Multicore で
    // 張らない = false で Multicore が選ばれると、テレインは見えるが当たらない）。
    bool terrainForcesCore = true;
};
