// Host test: a room's scripts with Leon and the enemies as the scripts move
// them (plc.c), without graphics. Starts an event function and logs what
// happens: plc commands, messages, camera cuts, positions.
// Build: cc -Itests/stubs -Isource tests/cutscene_host.c source/script.c source/enemy.c source/plc.c -o build/cutscene_host
// Run:   ./build/cutscene_host nitro:/room/room1010.bin 0x16 [ticks]
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "assets.h"
#include "enemy.h"
#include "plc.h"
#include "script.h"
void setBrightness(int screen, int level) { (void)screen; (void)level; }
static FILE *host_fopen(const char *path, const char *mode)
{
    const char *p = strstr(path, "nitro:/");
    char real[256];
    snprintf(real, sizeof(real), "nitrofiles/%s", p ? p + 7 : path);
    return fopen(real, mode);
}
#define fopen host_fopen
#include "../source/assets.c"
#undef fopen

static int tick;
static Room room;
static Model leon, rbj, rbj_e[MAX_ENEMIES];
static struct {
    int32_t x, y, z;
    int16_t dir;
    const Model *set;
    int anim, key;
    Plc plc;
} pl;

#define PLW_FIRST 10
static PlcBody body(void)
{
    PlcBody b = { .x = pl.x, .z = pl.z, .dir = pl.dir, .set = pl.set, .anim = pl.anim, .key = pl.key,
                  .room_set = rbj.data ? &rbj : NULL, .own_set = &leon, .own_first = PLW_FIRST, .own_first2 = PLW_FIRST,
                  .walk_anim = PLW_FIRST, .idle_anim = PLW_FIRST + 2, .table = 16 };
    return b;
}
static void back(const PlcBody *b)
{
    pl.x = b->x; pl.z = b->z; pl.dir = b->dir; pl.set = b->set; pl.anim = b->anim; pl.key = b->key;
}

int game_blocked(int32_t a, int32_t b, int32_t c, int32_t d, int32_t y) { (void)a; (void)b; (void)c; (void)d; (void)y; return 0; }
int camera_sees(int32_t x, int32_t y, int32_t z, int32_t m) { (void)x; (void)y; (void)z; (void)m; return 1; }
int camera_occluded(int32_t x, int32_t y, int32_t z) { (void)x; (void)y; (void)z; return 0; }
int cull_models = 0;
void game_aot_on(int n) { (void)n; }
int game_busy(void) { return 0; }
void game_show_message(const char *t) { printf("t%d  MESSAGE: %.60s\n", tick, t); }
int game_message_open(void) { return 0; }
void game_cut_change(int c) { printf("t%d  cut %d\n", tick, c); }
void game_set_player_pos(int32_t x, int32_t y, int32_t z) { pl.x = x; pl.y = y; pl.z = z; printf("t%d  leon pos %d %d %d\n", tick, (int)x, (int)y, (int)z); }
void game_set_player_dir(int16_t d) { pl.dir = d * 8; }
void game_set_box_flag(int b, int f) { printf("t%d  box %d flag %04x\n", tick, b, f & 0xFFFF); }
void game_bgm_control(int i, int o, int t, int l, int r) { (void)i; (void)o; (void)t; (void)l; (void)r; }
void game_bgmtbl_set(int s, int r, int m, int u) { (void)s; (void)r; (void)m; (void)u; }
void game_item_get(int item, int count) { printf("  item get %02x x%d\n", item, count); }
void game_movie(int n) { (void)n; }
void game_play_sound(int bank, int sound) { printf("t%d  sound bank %d #%d\n", tick, bank, sound); }
void game_spawn_enemy(int s, int t, int v, int voice, int x, int y, int z, int d)
{
    printf("t%d  spawn slot %d type %02x flags %04x at %d %d\n", tick, s, t, v, x, z);
    enemy_spawn(s, t, v, voice, x, y, z, d);
}
void game_enemy_motion(int s, int m, int c) { enemy_set_motion(s, m, c); }
void game_enemy_skin(int s, int m) { enemy_set_skin(s, m); }
void game_enemy_sound(int sound, int32_t x, int32_t z) { (void)sound; (void)x; (void)z; }
void game_player_voice(int sound) { (void)sound; }
int game_player_damage(int dmg) { (void)dmg; return 0; }
void game_player_grabbed(int on) { (void)on; }
void game_player_pose(const Model *m, int a, int k, int32_t x, int32_t z, int16_t d) { (void)m; (void)a; (void)k; (void)x; (void)z; (void)d; }
void game_player_died(void) {}
void game_player_hurt(int kind) { (void)kind; }
void game_player_killed(void) {}
void game_blood(int k, int32_t x, int32_t y, int32_t z, int16_t d, int sp, int32_t f) { (void)k; (void)x; (void)y; (void)z; (void)d; (void)sp; (void)f; }
int game_sight_blocked(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return 0; }
int game_enemy_member(int s, int m, int *v) { return enemy_member_get(s, m, v); }
void game_enemy_member_set(int s, int m, int v) { enemy_member_set(s, m, v); }
int game_player_member(int m, int *v)
{
    switch (m) { case 0x0B: *v = pl.x; return 1; case 0x0D: *v = pl.z; return 1; case 0x0F: *v = pl.dir / 8; return 1; }
    return 0;
}
void game_player_member_set(int m, int v) { if (m == 0x0F) pl.dir = ((int16_t)v * 8) & 32767; }
static int voice_left;
void game_voice(int k, int n) { printf("t%d  VOICE %d.%d\n", tick, k, n); voice_left = 60; }
int game_voice_playing(void) { return voice_left > 0; }
void game_object_set(const uint8_t *ins) { (void)ins; }
void game_object_hide(int model) { (void)model; }
void game_espr_on(const uint8_t *ins) { (void)ins; }
void game_espr_kill(int id, int type) { (void)id; (void)type; }
void game_floor_sound_set(int n, int on) { (void)n; (void)on; }
static int16_t obj_pos[32][3], obj_dir[32];
int game_object_member(int n, int m, int *v)
{
    if (n >= 32) return 0;
    if (m >= 0x0B && m <= 0x0D) { *v = obj_pos[n][m - 0x0B]; return 1; }
    if (m == 0x0F) { *v = obj_dir[n]; return 1; }
    return 0;
}
void game_object_member_set(int n, int m, int v)
{
    if (n >= 32) return;
    if (m >= 0x0B && m <= 0x0D) obj_pos[n][m - 0x0B] = (int16_t)v;
    if (m == 0x0F) { obj_dir[n] = (int16_t)v; printf("t%d  object %d dir %d\n", tick, n, obj_dir[n]); }
}
int game_plc(int w, int i, const uint8_t *ins)
{
    printf("t%d  plc %02x work %d.%d  %02x %02x %02x\n", tick, ins[0], w, i, ins[1], ins[2], ins[3]);
    if (w == 3 || w == 2)
        return enemy_plc(w == 2 ? PARTNER_SLOT : i, ins);
    if (w != 1)
        return 0;
    PlcBody b = body();
    plc_command(&pl.plc, &b, ins);
    back(&b);
    return 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3 || !room_load(&room, argv[1])) { printf("usage / room load failed\n"); return 1; }
    model_load(&leon, "nitro:/model/leon_w02.bin");
    char path[64];
    strcpy(path, argv[1]);
    char *dot = strstr(path, ".bin");
    if (dot) {
        strcpy(dot, "_rbj.bin");
        model_load(&rbj, path);
        for (int i = 0; i < MAX_ENEMIES; i++) {
            if (i == PARTNER_SLOT)
                snprintf(dot, 20, "_rbj_p.bin");
            else
                snprintf(dot, 20, "_rbj_e%d.bin", i);
            if (model_load(&rbj_e[i], path))
                enemy_set_room_set(i, &rbj_e[i]);
        }
    }
    pl.set = &leon;
    pl.anim = PLW_FIRST + 2;
    enemy_room_reset();
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (rbj_e[i].data)
            enemy_set_room_set(i, &rbj_e[i]);
    script_room_enter(&room);
    int ticks = argc > 3 ? atoi(argv[3]) : 2000;
    for (tick = 0; tick < ticks; tick++) {
        if (tick == 20)
            script_start_event((int)strtol(argv[2], NULL, 0));
        if (getenv("FLAG") && tick == atoi(getenv("FLAG_TICK") ? getenv("FLAG_TICK") : "20")) { // e.g. FLAG=5:0
            int a, b;
            if (sscanf(getenv("FLAG"), "%d:%x", &a, &b) == 2)
                flag_set(a, b, 1);
        }
        if (voice_left) voice_left--;
        script_tick();
        PlcBody b = body();
        plc_tick(&pl.plc, &b);
        back(&b);
        EnemyTarget p = { .x = pl.x, .z = pl.z, .can_grab = 0 };
        enemy_tick(&p);
        if (getenv("SHOOT") && tick > 20 && tick % 15 == 0) { // Leon shoots the nearest enemy
            int32_t tx, tz;
            int target = enemy_aim_target(pl.x, 0, pl.z, 0, &tx, &tz);
            if (target >= 0) {
                int16_t dir = (int16_t)((int)(atan2f((float)-(tz - pl.z), (float)(tx - pl.x)) * 32768.0f / 6.2831853f) & 32767);
                static const uint16_t dmg[3] = { 16, 15, 14 };
                printf("t%d  shot -> %d\n", tick, enemy_hit(pl.x, 0, pl.z, dir, 0, 2, dmg, 1));
            }
        }
        if (tick % 50 == 0) {
            printf("t%d  leon %d %d dir %d %s anim %d key %d | threads %d\n", tick, (int)pl.x, (int)pl.z,
                   pl.dir & 32767, pl.set == &rbj ? "room" : "own", pl.anim, pl.key, script_threads_running);
            for (int i = 0; i < MAX_ENEMIES; i++) {
                int x, z, r, f = 0;
                enemy_member_get(i, 7, &f);
                if (enemy_member_get(i, 0x0B, &x) && enemy_member_get(i, 0x0D, &z) && enemy_member_get(i, 2, &r))
                    printf("        enemy %d at %d %d routine %d flags %04x\n", i, x, z, r, f & 0xFFFF);
            }
            if (getenv("TRACE")) // state, animation and its frame count
                enemy_debug_print(tick);
        }
        if (tick > 40 && !script_threads_running)
            break;
    }
    printf("end at tick %d, flags 5:1e %d 7:00 %d 5:20 %d 5:21 %d 4:1a %d 4:1b %d 6:1c %d 4:07 %d\n", tick, flag_get(5, 0x1e), flag_get(7, 0), flag_get(5, 0x20), flag_get(5, 0x21),
           flag_get(4, 0x1a), flag_get(4, 0x1b), flag_get(6, 0x1c), flag_get(4, 0x07));
    return 0;
}
