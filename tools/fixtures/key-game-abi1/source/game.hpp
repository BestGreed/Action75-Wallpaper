#pragma once

#include <algorithm>
#include <cmath>

namespace key_game {
struct Game {
    static constexpr float arena_width = 960, floor_y = 270, radius = 13;
    float x = 100, y = floor_y - radius, vy = 0, clock = 0;
    float coin_x = 196, hazard_x = 488;
    int score = 0, hits = 0;
    bool left = false, right = false, jump_held = false, paused = false, sleeping = false;
    float immunity = 0;

    void clear_input() { left = right = jump_held = false; }
    void pause(bool value) { paused = value; clear_input(); }
    void sleep(bool value) { sleeping = value; clear_input(); }

    void jump(bool down) {
        if (paused || sleeping) return;
        if (down && !jump_held && y >= floor_y - radius && vy == 0) vy = -325;
        if (!down && vy < -120) vy = -120;
        jump_held = down;
    }
    void step(float dt) {
        if (paused || sleeping || !std::isfinite(dt) || dt <= 0) return;
        dt = std::min(dt, 0.12f);
        while (dt > 0) {
            const float t = std::min(dt, 1.f / 120);
            dt -= t;
            clock += t;
            x = std::clamp(x + (int(right) - int(left)) * 208 * t, radius, arena_width - radius);
            vy += 730 * t;
            y += vy * t;
            if (y >= floor_y - radius) { y = floor_y - radius; vy = 0; }
            if (std::abs(x - coin_x) < radius + 10 && std::abs(y - (floor_y - 23)) < radius + 10) {
                ++score;
                coin_x = 96 + std::fmod(coin_x + 173, 770.f);
            }
            immunity = std::max(0.f, immunity - t);
            if (immunity == 0 && std::abs(x - hazard_x) < radius + 12 && y + radius > floor_y - 19) {
                ++hits;
                x = 100; y = floor_y - radius; vy = 0; immunity = 1;
                clear_input();
            }
        }
    }
};
} // namespace key_game
