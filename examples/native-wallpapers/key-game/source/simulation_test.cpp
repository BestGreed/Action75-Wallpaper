#include "game.hpp"
#include <cstdio>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #expr); return 1; } } while (false)
int main() {
    key_game::Game g;
    g.right = true;
    for (int i = 0; i < 60; ++i) g.step(1.f / 120);
    CHECK(g.x > 200 && g.score == 1);
    g.right = false;
    const float stop = g.x;
    g.step(0.1f);
    CHECK(g.x == stop);
    g.jump(true);
    g.step(.05f);
    CHECK(g.y < g.floor_y - g.radius);
    g.jump(false);
    CHECK(g.vy >= -120);
    g.pause(true);
    const float altitude = g.y;
    g.step(.1f);
    CHECK(g.y == altitude);
    g.pause(false);
    g.sleep(true);
    g.step(.1f);
    CHECK(g.y == altitude);
    g.sleep(false);
    for (int i = 0; i < 120; ++i) g.step(1.f / 120);
    g.right = true;
    for (int i = 0; i < 240; ++i) g.step(1.f / 120);
    CHECK(g.hits == 1 && g.x == 100 && !g.right);
    std::puts("key-game simulation passed");
}
