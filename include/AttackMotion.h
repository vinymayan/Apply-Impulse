#pragma once
#include <algorithm>
#include <cmath>
#include <numbers>

namespace AttackMotion {
    inline float SteeringStep(float error, float strength, float speed, float dt) {
        return std::clamp(error * std::clamp(strength * dt, 0.0f, 1.0f), -speed * dt, speed * dt);
    }

    inline float VisualYaw(float steering, float spin) {
        const float turn = 2.0f * std::numbers::pi_v<float>;
        float yaw = std::fmod(steering + spin, turn);
        return yaw < 0.0f ? yaw + turn : yaw;
    }

    // Full-Force convention: right*x - forward*y. Vertical velocity is separate.
    inline void HorizontalVelocity(float x, float y, float yaw, float& worldX, float& worldY) {
        const float s = std::sin(yaw);
        const float c = std::cos(yaw);
        worldX = -c * x - s * y;
        worldY =  s * x - c * y;
    }

    inline void BehindPosition(float targetX, float targetY, float targetYaw, float distance,
                               float& outX, float& outY) {
        outX = targetX - std::sin(targetYaw) * distance;
        outY = targetY - std::cos(targetYaw) * distance;
    }
}
