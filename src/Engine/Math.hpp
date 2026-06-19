#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulse {

constexpr float Pi = 3.14159265358979323846f;
constexpr float TwoPi = Pi * 2.0f;

inline float degToRad(float degrees) {
    return degrees * Pi / 180.0f;
}

inline float radToDeg(float radians) {
    return radians * 180.0f / Pi;
}

inline float clamp(float value, float lo, float hi) {
    return std::max(lo, std::min(value, hi));
}

inline float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

inline float approach(float current, float target, float maxDelta) {
    if (current < target) {
        return std::min(current + maxDelta, target);
    }
    return std::max(current - maxDelta, target);
}

inline float wrapAngle(float radians) {
    while (radians > Pi) {
        radians -= TwoPi;
    }
    while (radians < -Pi) {
        radians += TwoPi;
    }
    return radians;
}

inline float angleDiff(float a, float b) {
    return wrapAngle(a - b);
}

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    Vec2() = default;
    Vec2(float inX, float inY) : x(inX), y(inY) {}

    Vec2 operator+(Vec2 rhs) const { return {x + rhs.x, y + rhs.y}; }
    Vec2 operator-(Vec2 rhs) const { return {x - rhs.x, y - rhs.y}; }
    Vec2 operator*(float scalar) const { return {x * scalar, y * scalar}; }
    Vec2 operator/(float scalar) const { return {x / scalar, y / scalar}; }

    Vec2& operator+=(Vec2 rhs) {
        x += rhs.x;
        y += rhs.y;
        return *this;
    }

    Vec2& operator-=(Vec2 rhs) {
        x -= rhs.x;
        y -= rhs.y;
        return *this;
    }

    Vec2& operator*=(float scalar) {
        x *= scalar;
        y *= scalar;
        return *this;
    }
};

inline float dot(Vec2 a, Vec2 b) {
    return a.x * b.x + a.y * b.y;
}

inline float lengthSq(Vec2 v) {
    return dot(v, v);
}

inline float length(Vec2 v) {
    return std::sqrt(lengthSq(v));
}

inline Vec2 normalize(Vec2 v) {
    const float len = length(v);
    if (len <= 0.00001f) {
        return {};
    }
    return v / len;
}

inline Vec2 fromAngle(float radians) {
    return {std::cos(radians), std::sin(radians)};
}

inline Vec2 rightFromForward(Vec2 forward) {
    return {-forward.y, forward.x};
}

inline Vec2 approach(Vec2 current, Vec2 target, float maxDelta) {
    Vec2 delta = target - current;
    const float dist = length(delta);
    if (dist <= maxDelta || dist <= 0.00001f) {
        return target;
    }
    return current + delta * (maxDelta / dist);
}

struct Rng {
    uint32_t state = 0x12345678u;

    explicit Rng(uint32_t seed = 0x12345678u) : state(seed) {}

    uint32_t nextU32() {
        uint32_t x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
        return x;
    }

    float unit() {
        return static_cast<float>(nextU32() & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
    }

    float range(float lo, float hi) {
        return lerp(lo, hi, unit());
    }

    int rangeInt(int lo, int hiInclusive) {
        const uint32_t span = static_cast<uint32_t>(hiInclusive - lo + 1);
        return lo + static_cast<int>(nextU32() % span);
    }
};

} // namespace pulse
