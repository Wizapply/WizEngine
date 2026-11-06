#pragma once

// 速度の減衰: 設定と、積分の後の適用。物理スレッド専用。

namespace wizengine {
namespace physics {

class BodyRegistry;

class VelocityDamping {
public:
    explicit VelocityDamping(BodyRegistry& bodies) : bodies_(bodies) {}
    VelocityDamping(const VelocityDamping&) = delete;
    VelocityDamping& operator=(const VelocityDamping&) = delete;

    void set(double linearPerSecond, double angularPerSecond);
    void apply(double dt);

private:
    BodyRegistry& bodies_;
    double linearDamping_ = 0.0;
    double angularDamping_ = 0.0;
};

}  // namespace physics
}  // namespace wizengine
