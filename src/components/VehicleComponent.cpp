#include "components/VehicleComponent.h"

#include <cmath>
#include <memory>
#include <string>
#include <utility>

#include "document/EditorTypes.h"
#include "core/Log.h"
#include "scene/Scene.h"
#include "scene/SceneMath.h"
#include "terrain/HeightField.h"

namespace veh = wizengine::vehicle;

namespace {

veh::Vec3 toVec(const chrono::ChVector3d& v) {
    return veh::Vec3{v.x(), v.y(), v.z()};
}
chrono::ChVector3d toChrono(const veh::Vec3& v) {
    return chrono::ChVector3d(v.x, v.y, v.z);
}

}  // namespace

VehicleComponent::VehicleComponent() {
    if (veh::LuaRuntime::available()) {
        lua_ = std::make_unique<veh::LuaRuntime>();
        if (lua_->ok()) {
            LOGI("vehicle", "node formulas: %s (jit %s)", lua_->version().c_str(),
                 lua_->jitEnabled() ? "on" : "OFF - formulas will run slowly");
        } else {
            LOGW("vehicle", "node formulas: LuaJIT state could not be created - "
                            "using the C++ interpreter");
            lua_.reset();
        }
    } else {
        LOGI("vehicle", "node formulas: built without LuaJIT - using the C++ interpreter");
    }
}

bool VehicleComponent::onCommand(Scene& scene, std::size_t camIndex,
                                 const nlohmann::json& msg) {
    (void)scene;
    (void)camIndex;
    const std::string cmd = msg.value("cmd", "");
    if (cmd != "drive") return false;
    // /input は誰でも叩けるので型を信じない（jsonNumber は型違いで既定値）。
    throttle_.store(wizengine::editor::jsonNumber(msg, "throttle", 0.0));
    brake_.store(wizengine::editor::jsonNumber(msg, "brake", 0.0));
    steer_.store(wizengine::editor::jsonNumber(msg, "steer", 0.0));
    handbrake_.store(wizengine::editor::jsonNumber(msg, "handbrake", 0.0));
    return true;
}

void VehicleComponent::onEditorStep(Scene& scene, double dt) {
    (void)dt;
    if (!models_.empty()) {
        models_.clear();
        LOGI("vehicle", "runtime cleared (editor mode)");
    }
    count_.store(0);
    // 設計値から「止まっている車」の車輪位置。毎パス作り直しても数本の
    // ベクタなので安い（置き直し・XML の適用にそのまま追従する）。
    std::map<std::size_t, std::vector<veh::WheelLocalPose>> vis;
    for (std::size_t i = 0; i < scene.objectCount(); ++i) {
        const GameObject& obj = scene.object(i);
        if (!obj.alive || !obj.desc.hasVehicle) continue;
        vis[i] = veh::VehicleModel::designWheelPoses(obj.desc.vehicle, obj.desc.mass);
    }
    std::lock_guard<std::mutex> lk(visMutex_);
    visuals_.swap(vis);
}

void VehicleComponent::onPhysicsStep(Scene& scene, double dt) {
    PhysicsWorld& physics = scene.physics();
    // 車輪のレイを当てるプレハブ（collide 部品）の一覧。変わったときだけコピー。
    if (scene.editor().prefabVersion() != prefabVersion_) {
        prefabVersion_ = scene.editor().prefabVersion();
        prefabs_ = scene.editor().prefabAssets();
    }
    veh::VehicleInput input;
    input.throttle = throttle_.load();
    input.brake = brake_.load();
    input.steer = steer_.load();
    input.handbrake = handbrake_.load();

    // 計算式（またはタイヤの参照）が変わっていたら作り直す。式のコンパイルは
    // 生成時なので、これがノードエディタの編集を実行へ届ける唯一の経路。
    const std::uint64_t fv = scene.editor().formulaVersion();
    if (fv != formulaVersion_) {
        if (!models_.empty()) LOGI("vehicle", "formulas changed - rebuilding vehicles");
        models_.clear();
        reportedFailures_.clear();
        formulaVersion_ = fv;
    }
    std::vector<veh::FormulaGraphDesc> library;  // 生成するときだけ取る
    bool libraryLoaded = false;

    // テレインの高さ格子。共有ポインタをステップに 1 回だけ取り（末端の
    // terrainFieldMutex_ を 1 回）、車輪のレイはロックの外でこれを辿る
    // （レイごとに Scene::terrainRaycast を呼ぶと 扇 × 車輪 × 車両 回ロックを
    // 取る）。差し替えられても手元の格子は生きている。
    // 当たり判定（自前の接触のゴースト）が無いとき（作れなかった・Multicore）は
    // 使わない: 車輪だけが乗って車体と他の物は抜ける、という
    // 「レイと接触の食い違い」を作らないため（プレハブの collide 部品と同じ考え）。
    std::shared_ptr<const wizengine::terrain::HeightField> terrain;
    if (scene.hasTerrainCollision()) {
        terrain = scene.terrainField();
        if (terrain && !terrain->valid()) terrain.reset();
    }

    int count = 0;
    bool first = true;
    std::map<std::size_t, std::vector<veh::WheelLocalPose>> vis;
    // 物理スレッドはオブジェクト一覧の唯一の書き手なのでロックは要らない。
    for (std::size_t i = 0; i < scene.objectCount(); ++i) {
        GameObject& obj = scene.object(i);
        if (!obj.alive || !obj.desc.hasVehicle ||
            obj.physId == GameObject::kInvalidId) {
            continue;
        }
        if (obj.desc.fixed) {
            // 固定した車体は走らせないが、車輪は設計位置に出しておく。
            vis[i] = veh::VehicleModel::designWheelPoses(obj.desc.vehicle,
                                                         obj.desc.mass);
            continue;
        }
        auto it = models_.find(i);
        if (it == models_.end()) {
            if (!libraryLoaded) {
                library = scene.editor().formulaAssets();
                libraryLoaded = true;
            }
            it = models_.emplace(i, std::make_unique<veh::VehicleModel>(
                                        obj.desc.vehicle, lua_.get(), &library))
                     .first;
            LOGI("vehicle", "object %zu '%s': vehicle created (%zu wheels, %d ticks)",
                 i, obj.desc.name.c_str(), it->second->wheelCount(),
                 it->second->desc().ticks);
            for (const std::string& d : it->second->diagnostics()) {
                LOGI("vehicle", "  %s", d.c_str());
            }
            reportedFailures_[i] = 0;
        }
        veh::VehicleModel& model = *it->second;
        if (const int fails = model.formulaFailures(); fails > reportedFailures_[i]) {
            // 式が NaN や実行時エラーを出したら 1 回だけ知らせる（毎ステップ
            // 出すとログが埋まる）。そのティックは組み込みで代用している。
            if (reportedFailures_[i] == 0) {
                LOGW("vehicle", "object %zu '%s': tire formula failed (%d ticks so far) - "
                                "falling back to the built-in tire model for those ticks",
                     i, obj.desc.name.c_str(), fails);
            }
            reportedFailures_[i] = fails;
        }
        model.setInput(input);

        const BodyTransform t = physics.bodyTransform(obj.physId);
        veh::ChassisState chassis;
        chassis.position = veh::Vec3{t.px, t.py, t.pz};
        chassis.rotation = veh::Quat{t.qw, t.qx, t.qy, t.qz};
        chassis.velocity = toVec(physics.bodyVelocity(obj.physId));
        chassis.angularVelocity = toVec(physics.bodyAngularVelocity(obj.physId));
        chassis.mass = physics.bodyMass(obj.physId);

        // 車輪のレイは床（y = 0）だけでなく、シーンの剛体（箱・球。ソフト
        // ボディと自分自身は除く）にも当てる = 階段や坂の上を走れる。
        // レイキャスト式なので乗った物に力は返さない（固定の段差向き）。
        // 物理スレッドはオブジェクト一覧の唯一の書き手なのでロックは要らない。
        const std::size_t self = i;
        const wizengine::terrain::HeightField* const field = terrain.get();
        const veh::GroundQuery query = [this, &scene, &physics, self, field](
                                           const veh::Vec3& origin, const veh::Vec3& dir,
                                           double maxDist) {
            // テレイン。始点（車体の取り付け点）が地中にあるときは、片面の
            // レイが何にも当たらず車輪が抜けて車体が落ちる（斜面の谷で前輪の
            // 取り付け点が登り坂に潜った場合など）。そのときは「距離 0 で
            // 当たった」= サスが縮みきった扱いにして押し上げる（力は
            // WheelController の上限 = 静止荷重の 8 倍で頭打ち）。地中かは
            // 取り付け点の真上 reach の高さから真下へのレイで見る（レイと
            // 同じ面・同じ片面の規則 = 後の段階の穴にもそのまま従う）。
            // reach より深く潜っていたら救わない（地中に置いた車を毎ステップ
            // 押し続けない）。深さの上限はレイの長さを鉛直に直したもの: 扇の
            // 斜めのレイは長い（reach / cosθ）ので、そのままだと扇の中で上限が
            // ばらつく。鉛直に直すと上限が揃うのは車体が水平のときだけで、
            // 傾いた車体では扇の中で少しずつ違う（どのレイも救済の可否が
            // 変わるだけで、採るのは一番高い円 = 結果は距離 0 の当たり）。
            // 返す法線は地表の面法線だが、扇（rays > 1）では WheelController が
            // 「当たり点 → 円の中心」= ストラットの上向きに置き換えるので、
            // 使われるのは rays = 1 のときだけ。
            // **下を向いたレイだけ**救う（dir.y < -1e-9、planeGroundQuery の
            // 「下を向いていない」と同じしきい値）。上向き・水平のレイは平面も
            // テレインの片面のレイも当たらない約束で、救済だけがそれを破ると、
            // 横転しかけ・裏返りの車で取り付け点がわずかに埋まったとき、
            // 下や横を向いたストラットの上向きに沿って車体を地面へ押し込む。
            if (field && dir.y < -1e-9) {
                // maxDist は dir の倍数（planeGroundQuery と同じ単位）なので、
                // 鉛直の長さは maxDist * (-dir.y)。
                const double reach = std::isfinite(maxDist) ? maxDist * (-dir.y) : 0.0;
                if (reach > 0.0) {
                    const wizengine::terrain::Vec3 top{origin.x, origin.y + reach, origin.z};
                    const wizengine::terrain::Vec3 down{0.0, -1.0, 0.0};
                    double above = 0.0;
                    wizengine::terrain::Vec3 an = wizengine::terrain::Vec3::Zero();
                    // 戻り値は一度変数に受けてから使う（出力引数と同じ式に並べない規約）。
                    const bool covered = field->raycast(top, down, reach, above, an);
                    if (covered && above < reach) {
                        veh::GroundHit buried;
                        buried.hit = true;
                        buried.distance = 0.0;
                        buried.point = origin;
                        buried.normal = veh::Vec3{an.x(), an.y(), an.z()};
                        return buried;  // 距離 0 より近い当たりは無い
                    }
                }
            }
            veh::GroundHit best = veh::planeGroundQuery(origin, dir, maxDist);
            // 当たり判定の結果（dist / normal は出力引数）を採用するかは、必ず
            // 判定を呼び終えてから決める。consider(rayHits...(.., dist, normal),
            // dist, normal) の一行書きは引数の評価順が未規定で、MSVC は右から
            // 左に評価するため dist が判定前の古い値でコピーされ、前の部品の
            // 距離が使われて階段で車が跳ねた（GCC のテストでは再現しない）。
            double dist = 0.0;
            veh::Vec3 normal;
            auto consider = [&](bool hit) {
                if (hit && (!best.hit || dist < best.distance)) {
                    best.hit = true;
                    best.distance = dist;
                    best.point = origin + dir * dist;
                    best.normal = normal;
                }
            };
            // テレインの地表。y = 0 の平面とは「近い方」で決まる: 丘（平面より
            // 上）はテレインが先に当たり、平面より低い谷や格子の外は平面
            // （= 物理の床。テレインの下にも床は残っている）が当たる。法線は
            // 当たった三角形の面法線（HeightField::normalAt と同じ割り方）。
            // 扇のレイでは WheelController が「当たり点 → 円の中心」に置き換える。
            if (field) {
                const wizengine::terrain::Vec3 to{origin.x, origin.y, origin.z};
                const wizengine::terrain::Vec3 td{dir.x, dir.y, dir.z};
                // raycast の距離は dir を正規化した m 単位、ここの maxDist / dist は
                // dir の倍数（planeGroundQuery と同じ）。WheelController の dir は
                // 正規化済みで両者は一致するが、単位は dir の長さで揃えておく。
                const double len = dir.length();
                double tt = 0.0;
                wizengine::terrain::Vec3 tn = wizengine::terrain::Vec3::Zero();
                const bool hit = len > 1e-12 && field->raycast(to, td, maxDist * len, tt, tn);
                if (hit) {
                    dist = tt / len;
                    normal = veh::Vec3{tn.x(), tn.y(), tn.z()};
                }
                consider(hit);
            }
            for (std::size_t j = 0; j < scene.objectCount(); ++j) {
                if (j == self) continue;
                const GameObject& o = scene.object(j);
                if (!o.alive || o.physId == GameObject::kInvalidId || o.lattice) continue;
                const BodyTransform t = physics.bodyTransform(o.physId);
                const veh::Vec3 center{t.px, t.py, t.pz};
                const veh::Quat rot{t.qw, t.qx, t.qy, t.qz};
                if (o.desc.collision == wizengine::editor::ShapeKind::Box) {
                    const bool hit = veh::rayHitsOrientedBox(
                        origin, dir, center, rot,
                        veh::Vec3{o.desc.size.x * 0.5, o.desc.size.y * 0.5,
                                  o.desc.size.z * 0.5},
                        maxDist, dist, normal);
                    consider(hit);
                } else if (o.desc.collision != wizengine::editor::ShapeKind::None) {
                    const bool hit = veh::rayHitsSphereSurface(
                        origin, dir, center, o.desc.radius(), maxDist, dist, normal);
                    consider(hit);
                }
                // プレハブの collide 部品（階段の段など）。物理でも複合形状として
                // 本体に付いているので、レイも同じ形に当てる。部品はボディの
                // ローカル座標（clampPart により socket 無しの箱 / 球だけ）。
                if (o.desc.prefab.empty()) continue;
                for (const wizengine::editor::PrefabDesc& p : prefabs_) {
                    if (p.name != o.desc.prefab) continue;
                    for (const wizengine::editor::PartDesc& part : p.parts) {
                        if (!part.collide) continue;
                        const veh::Vec3 pc = center + rot.rotate(veh::Vec3{
                                                 part.position.x, part.position.y,
                                                 part.position.z});
                        if (part.kind == wizengine::editor::PartKind::Sphere) {
                            const bool hit = veh::rayHitsSphereSurface(
                                origin, dir, pc, part.size.x * 0.5, maxDist, dist, normal);
                            consider(hit);
                            continue;
                        }
                        const auto pq = scenemath::quatFromEulerDegrees(
                            part.rotation.x, part.rotation.y, part.rotation.z);
                        const veh::Quat prot = rot * veh::Quat{pq.w(), pq.x(), pq.y(), pq.z()};
                        const bool hit = veh::rayHitsOrientedBox(
                            origin, dir, pc, prot,
                            veh::Vec3{part.size.x * 0.5, part.size.y * 0.5, part.size.z * 0.5},
                            maxDist, dist, normal);
                        consider(hit);
                    }
                    break;
                }
            }
            return best;
        };
        const auto& forces = model.step(chassis, dt, query);
        for (const veh::ForceAtPoint& f : forces) {
            physics.applyForceAtPoint(obj.physId, toChrono(f.force),
                                      toChrono(f.point), dt);
        }
        // 診断: 車体（箱）が何かに触れていたら、相手の名前を 1 秒に 1 回。
        // 車輪はレイなので接触しない = 出たら車体が段や壁に当たっている。
        if (first && (contactCheck_++ % 60) == 0) {
            for (const auto& pr : physics.activeContactPairs()) {
                if (pr.first != obj.physId && pr.second != obj.physId) continue;
                const std::size_t other = pr.first == obj.physId ? pr.second : pr.first;
                std::string name = "body " + std::to_string(other);
                // テレインも出す（丘は車輪のレイが受けるので、車体が触れるのは
                // 段や壁と同じく「車輪で支えきれていない」印）。
                if (scene.isTerrainBody(other)) name = "terrain";
                for (std::size_t j = 0; j < scene.objectCount(); ++j) {
                    const GameObject& o = scene.object(j);
                    if (!o.alive || o.physId != other) continue;
                    name = "object " + std::to_string(j) + " '" + o.desc.name + "'";
                    break;
                }
                LOGI("vehicle", "object %zu '%s': chassis is in contact with %s", i,
                     obj.desc.name.c_str(), name.c_str());
            }
        }
        vis[i] = model.wheelLocalPoses();
        ++count;
        if (first) {
            const veh::VehicleTelemetry& tm = model.telemetry();
            rpm_.store(tm.rpm);
            gear_.store(tm.gear);
            speed_.store(tm.speed);
            clutch_.store(tm.clutch);
            first = false;
        }
    }
    // 消えたオブジェクトの実行状態を片付ける。
    for (auto it = models_.begin(); it != models_.end();) {
        const std::size_t i = it->first;
        if (!scene.objectAlive(i) || !scene.object(i).desc.hasVehicle) {
            reportedFailures_.erase(i);
            it = models_.erase(it);
        } else {
            ++it;
        }
    }
    count_.store(count);
    std::lock_guard<std::mutex> lk(visMutex_);
    visuals_.swap(vis);
}

void VehicleComponent::onPhysicsPassEnd(Scene& scene) {
    (void)scene;
    std::lock_guard<std::mutex> lk(visMutex_);
    std::swap(visPrev_, visCur_);
    visCur_ = visuals_;
}

bool VehicleComponent::wheelLocalPoses(std::size_t objectIndex,
                                       std::vector<veh::WheelLocalPose>& out,
                                       double alpha) {
    std::lock_guard<std::mutex> lk(visMutex_);
    if (alpha < 0.0) {
        const auto it = visuals_.find(objectIndex);
        if (it == visuals_.end()) return false;
        out = it->second;
        return true;
    }
    // 補間: 今回のパスの値を土台に、前回にも同じ車輪があればその間を取る
    // （車輪の数が変わった直後などは今回のまま）。
    const auto cur = visCur_.find(objectIndex);
    if (cur == visCur_.end()) return false;
    out = cur->second;
    const auto prev = visPrev_.find(objectIndex);
    if (prev == visPrev_.end() || prev->second.size() != out.size()) return true;
    auto mix = [alpha](double a, double b) { return a + (b - a) * alpha; };
    for (std::size_t w = 0; w < out.size(); ++w) {
        const veh::WheelLocalPose& a = prev->second[w];
        veh::WheelLocalPose& o = out[w];
        o.attach = veh::Vec3{mix(a.attach.x, o.attach.x), mix(a.attach.y, o.attach.y),
                             mix(a.attach.z, o.attach.z)};
        o.drop = mix(a.drop, o.drop);
        o.steerRad = mix(a.steerRad, o.steerRad);
        o.spinAngle = mix(a.spinAngle, o.spinAngle);  // 累積の角度なので折り返し無し
        o.radius = mix(a.radius, o.radius);
        o.width = mix(a.width, o.width);
        o.deflection = mix(a.deflection, o.deflection);
        // ソフトタイヤの粒子はワールド座標。点ごとに補間する（数が違えば今回）。
        if (a.softMesh && o.softMesh && a.softMesh->size() == o.softMesh->size()) {
            auto mesh = std::make_shared<std::vector<float>>(*o.softMesh);
            const float ft = float(alpha);
            const std::vector<float>& pa = *a.softMesh;
            for (std::size_t k = 0; k < mesh->size(); ++k) {
                (*mesh)[k] = pa[k] + ((*mesh)[k] - pa[k]) * ft;
            }
            o.softMesh = std::move(mesh);
        }
    }
    return true;
}

VehicleComponent::Snapshot VehicleComponent::snapshot() const {
    Snapshot s;
    s.count = count_.load();
    s.present = s.count > 0;
    s.rpm = rpm_.load();
    s.gear = gear_.load();
    s.speed = speed_.load();
    s.clutch = clutch_.load();
    return s;
}
