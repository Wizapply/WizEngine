#pragma once

// PhysicsWorld の実装ファイル（PhysicsWorld*.cpp）だけが使う私的ヘッダ。実装を
// 担当ごとに分けたので、2 つ以上のファイルが使う型と小さな関数をここに 1 か所で
// 持つ。公開 API は PhysicsWorld.h。他のファイルからは含めないこと。
//
//   PhysicsWorld.cpp             : 構築・1 ステップの順番・全体を起こす・機能の有無・小さな担当の中継（ソフトボディの問い合わせも）
//   PhysicsWorldSystem.cpp       : 系を組む順番と系の設定の中継（本体は PhysicsSystem）
//   PhysicsWorldRegistry.cpp     : ボディの追加の中継（本体は BodyFactory）と退場の順番（名簿は BodyRegistry）
//   PhysicsWorldMotion.cpp       : 運動の中継・ソフトボディの置き直しの分岐・固定（本体は BodyMotion / SoftBodies）
//   PhysicsWorldJoints.cpp       : ジョイント
//   PhysicsWorldJointsManual.cpp : Multicore 用の手計算のジョイント
//   PhysicsWorldCables.cpp       : FEA ケーブル
//   PhysicsWorldModal.cpp        : モーダル解析
//
// using namespace はここに書かない（各 .cpp に書く）。

#include "physics/PhysicsParts.h"
#include "physics/PhysicsWorld.h"
#include "physics/ChronoCompat.h"
#include "physics/ChronoMath.h"
#include "terrain/TerrainContact.h"

#include <chrono/core/ChMatrix33.h>
#include <chrono/physics/ChContactMaterial.h>
#include <chrono/physics/ChSystem.h>

#include <array>
#include <cmath>
#include <memory>
#include <vector>

// 窓口の実装（グローバルの名前空間）から担当の部品をそのまま呼ぶための別名。
using wizengine::physics::quatFromZAxis;

// ソフトボディの形の記録（terrain/ の型）を書くための別名。
namespace wterrain = wizengine::terrain;
