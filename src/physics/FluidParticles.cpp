#include "physics/FluidParticles.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "core/Log.h"
#include "physics/PhysicsSystem.h"

#ifdef WIZ_USE_MULTICORE
#include <chrono_multicore/physics/Ch3DOFContainer.h>
#include <chrono_multicore/physics/ChSystemMulticore.h>
#endif

namespace wizengine {
namespace physics {

bool FluidParticles::add(const FluidSpec& spec, std::string& why) {
#ifdef WIZ_USE_MULTICORE
    if (system_.backend() != PhysicsBackend::Multicore) {
        why = "fluid particles need the Multicore backend (the system is core)";
        return false;
    }
    if (system_.contact() != ContactMethod::NSC) {
        why = "fluid particles need contact=\"nsc\" (Chrono::Multicore has no SMC fluid)";
        return false;
    }
    auto* mc = dynamic_cast<chrono::ChSystemMulticoreNSC*>(system_.sys().get());
    if (!mc) {
        why = "the system is not ChSystemMulticoreNSC";
        return false;
    }
    if (container_) {
        why = "this system already has fluid particles (recreate the system first)";
        return false;
    }
    const std::size_t n = std::min(spec.positions.size(), spec.velocities.size()) / 3;
    if (n == 0) {
        why = "no particles (the <fluid> blocks are empty)";
        return false;
    }

    // 値の決め方は Chrono の demo_MCORE_fluidNSC と同じ（粒子の間隔 = カーネル半径
    // × 0.9、緩和時間 = 4 ステップ、人工圧力の届く距離 = カーネル半径の 0.2 倍）。
    // ソルバを 1 段（SLIDING）にする（理由は PhysicsSystem::useSingleSlidingPhase。
    // 段を分ける既定のままだと最初のステップで例外になる - 本物の Chrono 9.0 で確認）。
    system_.useSingleSlidingPhase();
    auto fc = chrono_types::make_shared<chrono::ChFluidContainer>();
    // 先に系へ登録する（AddBodies はデータマネージャの配列へ直接足すので、
    // データマネージャを結んでからでないと使えない）。
    mc->Add3DOFContainer(fc);
    fc->kernel_radius = spec.kernelRadius;
    fc->collision_envelope = 0;
    fc->contact_recovery_speed = spec.recovery;
    fc->contact_cohesion = spec.cohesion;
    fc->contact_mu = spec.friction;
    fc->max_velocity = spec.maxVelocity;
    fc->rho = spec.density;
    fc->mass = spec.particleMass;
    fc->tau = spec.tau;
    fc->epsilon = spec.epsilon;
    fc->viscosity = spec.viscosity;
    fc->enable_viscosity = spec.viscosity > 0.0;
    fc->artificial_pressure = spec.artificialPressure > 0.0;
    fc->artificial_pressure_k = spec.artificialPressure;
    fc->artificial_pressure_dq = 0.2 * spec.kernelRadius;
    fc->artificial_pressure_n = 4;

    std::vector<chrono::real3> pos(n), vel(n);
    for (std::size_t i = 0; i < n; ++i) {
        pos[i] = chrono::real3(spec.positions[i * 3], spec.positions[i * 3 + 1],
                               spec.positions[i * 3 + 2]);
        vel[i] = chrono::real3(spec.velocities[i * 3], spec.velocities[i * 3 + 1],
                               spec.velocities[i * 3 + 2]);
    }
    fc->AddBodies(pos, vel);
    container_ = fc;
    count_ = n;
    initialPos_.assign(spec.positions.begin(), spec.positions.begin() + std::ptrdiff_t(n * 3));
    initialVel_.assign(spec.velocities.begin(), spec.velocities.begin() + std::ptrdiff_t(n * 3));
    // 変数（自由度）の数が変わったので、ソルバの配置を組み直す。
    system_.setup();
    LOGI("physics", "fluid: %zu particles, kernel %.4f m, mass %.5f kg, tau %.4f s", n,
         spec.kernelRadius, spec.particleMass, spec.tau);
    return true;
#else
    (void)spec;
    why = "this build has no Chrono::Multicore (configure with -DWIZ_USE_MULTICORE=ON)";
    return false;
#endif
}

void FluidParticles::positions(std::vector<float>& out) const {
    out.clear();
#ifdef WIZ_USE_MULTICORE
    if (!container_) return;
    out.resize(count_ * 3);
    for (std::size_t i = 0; i < count_; ++i) {
        const chrono::real3 p = container_->GetPos(int(i));
        out[i * 3] = float(p.x);
        out[i * 3 + 1] = float(p.y);
        out[i * 3 + 2] = float(p.z);
    }
#endif
}

double FluidParticles::maxSpeed() const {
    double best = 0.0;
#ifdef WIZ_USE_MULTICORE
    if (!container_) return 0.0;
    for (std::size_t i = 0; i < count_; ++i) {
        const chrono::real3 v = container_->GetPosDt(int(i));
        best = std::max(best, double(std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z)));
    }
#endif
    return best;
}

bool FluidParticles::resetToInitial() {
#ifdef WIZ_USE_MULTICORE
    if (!container_ || initialPos_.size() < count_ * 3 || initialVel_.size() < count_ * 3) {
        return false;
    }
    for (std::size_t i = 0; i < count_; ++i) {
        container_->SetPos(int(i), chrono::real3(initialPos_[i * 3], initialPos_[i * 3 + 1],
                                                 initialPos_[i * 3 + 2]));
        container_->SetPosDt(int(i), chrono::real3(initialVel_[i * 3], initialVel_[i * 3 + 1],
                                                   initialVel_[i * 3 + 2]));
    }
    return true;
#else
    return false;
#endif
}

void FluidParticles::clear() {
    container_.reset();
    count_ = 0;
    initialPos_.clear();
    initialVel_.clear();
}

}  // namespace physics
}  // namespace wizengine
