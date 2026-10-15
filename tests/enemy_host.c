// Host test: room scripts + enemy.c with the real model files.
// Build: cc -Itests/stubs -Isource tests/enemy_host.c source/script.c source/enemy.c -o build/enemy_host
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "assets.h"
#include "enemy.h"
#include "script.h"
void setBrightness(int screen, int level) { (void)screen; (void)level; }
static FILE *host_fopen(const char *path, const char *mode)
{
    const char *p = strstr(path, "nitro:/");
    char real[256];
    snprintf(real, sizeof(real), "nitrofiles/%s", p ? p + 7 : path);
    FILE *f = fopen(real, mode);
    if (!f)
        printf("  asset missing: %s\n", real);
    return f;
}
#define fopen host_fopen
#include "../source/assets.c"
#undef fopen
int game_blocked(int32_t a, int32_t b, int32_t c, int32_t d, int32_t y) { (void)a; (void)b; (void)c; (void)d; (void)y; return 0; }
int camera_sees(int32_t x, int32_t y, int32_t z, int32_t m) { (void)x; (void)y; (void)z; (void)m; return 1; }
void game_show_message(const char *t) { (void)t; }
int game_message_open(void) { return 0; }
void game_cut_change(int c) { (void)c; }
void game_set_player_pos(int32_t x, int32_t y, int32_t z) { (void)x; (void)y; (void)z; }
void game_set_player_dir(int16_t d) { (void)d; }
void game_set_box_flag(int b, int f) { (void)b; (void)f; }
void game_play_sound(int bank, int sound) { (void)bank; (void)sound; }
void game_spawn_enemy(int s, int t, int v, int x, int y, int z, int d)
{
    printf("  spawn slot %d type %02x\n", s, t);
    enemy_spawn(s, t, v, x, y, z, d);
}
int main(int argc, char **argv)
{
    Room room;
    if (!room_load(&room, argc > 1 ? argv[1] : "nitro:/room/room1000.bin")) { printf("room load failed\n"); return 1; }
    enemy_room_reset();
    script_room_enter(&room);
    int ticks = getenv("TICKS") ? atoi(getenv("TICKS")) : 100;
    for (int t = 0; t < ticks; t++) {
        int before = enemy_spawn_calls;
        script_tick();
        enemy_tick(5899, 0);
        if (enemy_spawn_calls != before) printf("tick %d: %d spawn calls total, %d active\n", t, enemy_spawn_calls, enemy_count());
        if (getenv("TRACE")) enemy_debug_print(t);
    }
    printf("enemies active: %d\n", enemy_count());
    return 0;
}
