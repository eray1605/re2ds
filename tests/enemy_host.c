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
int cull_models = 0; // POLY_CULL_NONE in the stubs
void game_show_message(const char *t) { (void)t; }
int game_message_open(void) { return 0; }
void game_cut_change(int c) { (void)c; }
void game_set_player_pos(int32_t x, int32_t y, int32_t z) { (void)x; (void)y; (void)z; }
void game_set_player_dir(int16_t d) { (void)d; }
void game_set_box_flag(int b, int f) { (void)b; (void)f; }
void game_play_sound(int bank, int sound) { (void)bank; (void)sound; }
void game_spawn_enemy(int s, int t, int v, int voice, int x, int y, int z, int d)
{
    printf("  spawn slot %d type %02x flags %04x voice %d at %d %d %d dir %d\n", s, t, v, voice, x, y, z, d);
    enemy_spawn(s, t, v, voice, x, y, z, d);
}
// Leon for the grab test: stands still at PX, 0
static int host_hp = 200, host_held, host_dead;
static int32_t host_px, host_pz;
void game_enemy_sound(int sound, int32_t x, int32_t z) { (void)x; (void)z; if (getenv("TRACE")) printf("  enemy sound %d\n", sound); }
void game_player_voice(int sound) { if (getenv("TRACE")) printf("  voice %d\n", sound); }
int game_player_damage(int dmg) { if (dmg < 41) dmg += dmg >> 1; host_hp -= dmg; printf("  bite: hp %d\n", host_hp); return host_hp < 0 ? 2 : 0; }
void game_player_grabbed(int on) { host_held = on; printf("  grabbed %d\n", on); }
void game_player_pose(const Model *m, int a, int k, int32_t x, int32_t z, int16_t d) { (void)m; (void)d; host_px = x; host_pz = z; if (getenv("TRACE")) printf("  leon anim %d key %d at %d %d\n", a, k, (int)x, (int)z); }
void game_player_died(void) { host_dead = 1; printf("  died\n"); }
void game_blood(int k, int32_t x, int32_t y, int32_t z, int16_t d, int sp, int32_t f) { (void)x; (void)z; (void)d; (void)sp; (void)f; if (getenv("TRACE")) printf("  blood %d at y %d\n", k, (int)y); }
int game_enemy_member(int s, int m, int *v) { return enemy_member_get(s, m, v); }
void game_enemy_member_set(int s, int m, int v) { printf("  member_set enemy %d member %d = %04x\n", s, m, v & 0xFFFF); enemy_member_set(s, m, v); }
int game_sight_blocked(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return 0; }
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
        EnemyTarget p = { .x = host_held ? host_px : (getenv("PX") ? atoi(getenv("PX")) : 5899),
                          .z = host_held ? host_pz : 0, .can_grab = !host_held && !host_dead,
                          .pressed = getenv("MASH") && (t & 1), .dead = host_dead };
        enemy_tick(&p);
        if (enemy_spawn_calls != before) printf("tick %d: %d spawn calls total, %d active\n", t, enemy_spawn_calls, enemy_count());
        if (getenv("TRACE")) enemy_debug_print(t);
    }
    printf("enemies active: %d\n", enemy_count());
    return 0;
}
