// Host test: room scripts + enemy.c with the real model files.
// Build: cc -Itests/stubs -Isource tests/enemy_host.c source/script.c source/enemy.c -o build/enemy_host
#include <math.h>
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
// walls as in main.c (blocked / penetration), with the room's boxes
static Room host_room;
static int32_t pen(const RoomBox *b, int32_t x, int32_t z)
{
    const int32_t r = 400;
    int32_t ix = x - (b->x - r), ox = b->x + b->w + r - x, iz = z - (b->z - r), oz = b->z + b->d + r - z;
    if (ix <= 0 || ox <= 0 || iz <= 0 || oz <= 0) return 0;
    int32_t d = ix < ox ? ix : ox;
    if (iz < d) d = iz;
    if (oz < d) d = oz;
    return d;
}
int game_blocked(int32_t fx, int32_t fz, int32_t x, int32_t z, int32_t y)
{
    (void)y;
    if (!host_room.data || !getenv("WALLS")) return 0;
    for (int i = 0; i < host_room.header->num_boxes; i++) {
        const RoomBox *b = &host_room.boxes[i];
        if (!(b->floors & 1) || b->shape == 11 || b->shape == 12 || !(b->flag & 0x0400)) continue;
        int32_t d = pen(b, x, z);
        if (d > 0 && d > pen(b, fx, fz)) return 1;
    }
    return 0;
}
const RoomBox *game_room_boxes(int *n) { *n = host_room.data ? host_room.header->num_boxes : 0; return host_room.boxes; }
int camera_sees(int32_t x, int32_t y, int32_t z, int32_t m) { (void)x; (void)y; (void)z; (void)m; return 1; }
int camera_occluded(int32_t x, int32_t y, int32_t z) { (void)x; (void)y; (void)z; return 0; }
int camera_view_area(int32_t x, int32_t z) { (void)x; (void)z; return 1; }
int cull_models = 0; // POLY_CULL_NONE in the stubs
void game_aot_on(int n) { (void)n; }
int game_busy(void) { return 0; }
void game_show_message(const char *t) { (void)t; }
int game_message_open(void) { return 0; }
void game_cut_change(int c) { if (getenv("TRACE_CUTS")) printf("  cut %d\n", c); }
void game_set_player_pos(int32_t x, int32_t y, int32_t z) { (void)x; (void)y; (void)z; }
void game_set_player_dir(int16_t d) { (void)d; }
void game_set_box_flag(int b, int f) { if (host_room.data && b >= 0 && b < host_room.header->num_boxes) ((RoomBox *)host_room.boxes)[b].flag = f; printf("  box %d flag %04x\n", b, f & 0xFFFF); }
void game_bgm_control(int i, int o, int t, int l, int r) { (void)t; (void)l; (void)r; printf("  bgm control %d op %d\n", i, o); }
void game_bgmtbl_set(int s, int r, int m, int u) { (void)s; (void)r; (void)m; (void)u; }
void game_item_get(int item, int count) { printf("  item get %02x x%d\n", item, count); }
void game_movie(int n) { (void)n; }
void game_play_sound(int bank, int sound) { if (getenv("TRACE_SOUNDS")) printf("  sound bank %d %#x\n", bank, sound); }
void game_spawn_enemy(int s, int t, int v, int voice, int x, int y, int z, int d)
{
    printf("  spawn slot %d type %02x flags %04x voice %d at %d %d %d dir %d\n", s, t, v, voice, x, y, z, d);
    enemy_spawn(s, t, v, voice, x, y, z, d);
}
void game_enemy_motion(int s, int m, int c) { enemy_set_motion(s, m, c); }
void game_enemy_skin(int s, int m) { enemy_set_skin(s, m); }
void game_cut_be_set(int c, int n, int on) { (void)c; (void)n; (void)on; }
void game_enemy_shadow(int s, int r, int g, int b, int w, int h, int ox, int oz) { enemy_set_shadow(s, r, g, b, w, h, ox, oz); }
// Leon for the grab test: stands still at PX, 0
static int host_hp = 200, host_held, host_dead;
static int32_t host_px, host_pz;
void game_enemy_sound(int sound, int32_t x, int32_t z) { (void)x; (void)z; if (getenv("TRACE")) printf("  enemy sound %d\n", sound); }
void game_player_voice(int sound) { if (getenv("TRACE")) printf("  voice %d\n", sound); }
int game_player_damage(int dmg) { if (dmg < 41) dmg += dmg >> 1; host_hp -= dmg; printf("  bite: hp %d\n", host_hp); return host_hp < 0 ? 2 : 0; }
void game_player_grabbed(int on) { host_held = on; printf("  grabbed %d\n", on); }
void game_player_pose(const Model *m, int a, int k, int32_t x, int32_t z, int16_t d) { (void)m; (void)d; host_px = x; host_pz = z; if (getenv("TRACE")) printf("  leon anim %d key %d at %d %d\n", a, k, (int)x, (int)z); }
void game_player_died(void) { host_dead = 1; printf("  died\n"); }
void game_player_hurt(int kind) { printf("  hurt %d\n", kind); }
void game_player_killed(void) { host_dead = 1; printf("  killed\n"); }
void game_blood(int k, int32_t x, int32_t y, int32_t z, int16_t d, int sp, int32_t f) { (void)x; (void)z; (void)d; (void)sp; (void)f; if (getenv("TRACE")) printf("  blood %d at y %d\n", k, (int)y); }
void game_partner_shot(int32_t x, int32_t y, int32_t z) { if (getenv("TRACE")) printf("  partner shot at %d %d %d\n", (int)x, (int)y, (int)z); }
int game_enemy_member(int s, int m, int *v) { return enemy_member_get(s, m, v); }
int game_player_member(int m, int *v) { (void)m; (void)v; return 0; }
void game_player_member_set(int m, int v) { (void)m; (void)v; }
void game_voice(int k, int n) { (void)k; (void)n; }
int game_voice_playing(void) { return 0; }
// room objects: position and facing for the scripts (member 0x0B-0x0D, 0x0F)
static int16_t host_obj[32][4];
void game_object_set(const uint8_t *ins)
{
    if (ins[1] >= 32)
        return;
    for (int k = 0; k < 3; k++)
        host_obj[ins[1]][k] = (int16_t)(ins[14 + 2 * k] | ins[15 + 2 * k] << 8);
    host_obj[ins[1]][3] = (int16_t)(ins[22] | ins[23] << 8);
}
void game_object_hide(int model) { (void)model; }
void game_espr_on(const uint8_t *ins) { (void)ins; }
void game_espr3d_on(const uint8_t *ins) { (void)ins; }
void game_espr_kill(int id, int type) { (void)id; (void)type; }
void game_floor_sound_set(int n, int on) { (void)n; (void)on; }
int game_object_member(int n, int m, int *v)
{
    if (n >= 32 || !((m >= 0x0B && m <= 0x0D) || m == 0x0F))
        return 0;
    *v = host_obj[n][m == 0x0F ? 3 : m - 0x0B];
    return 1;
}
void game_object_member_set(int n, int m, int v)
{
    if (n < 32 && ((m >= 0x0B && m <= 0x0D) || m == 0x0F))
        host_obj[n][m == 0x0F ? 3 : m - 0x0B] = (int16_t)v;
}
int game_plc(int w, int i, const uint8_t *ins) { return w == 3 ? enemy_plc(i, ins) : 1; }
void game_enemy_member_set(int s, int m, int v) { printf("  member_set enemy %d member %d = %04x\n", s, m, v & 0xFFFF); enemy_member_set(s, m, v); }
int game_sight_blocked(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return 0; }
int main(int argc, char **argv)
{
    Room room;
    if (!room_load(&room, argc > 1 ? argv[1] : "nitro:/room/room1000.bin")) { printf("room load failed\n"); return 1; }
    host_room = room;
    enemy_room_reset();
    if (getenv("ZONES")) // e.g. nitrofiles/room/room1020_blk.bin
        enemy_set_zones(asset_load(getenv("ZONES"), NULL));
    script_room_enter(&room);
    if (getenv("FLAGS")) { // array:bit,... e.g. 1:3 (the partner follows)
        char buf[256], *save = NULL;
        snprintf(buf, sizeof(buf), "%s", getenv("FLAGS"));
        for (char *it = strtok_r(buf, ",", &save); it; it = strtok_r(NULL, ",", &save)) {
            int array, bit;
            if (sscanf(it, "%i:%i", &array, &bit) == 2)
                flag_set(array, bit, 1);
        }
    }
    if (getenv("SPAWN")) { // slot:type:flags:x:y:z:dir,... (dir in RE2 units), e.g. 0:0x22:0x0101:-27000:-3000:-12400:1024
        char buf[256], *save = NULL;
        snprintf(buf, sizeof(buf), "%s", getenv("SPAWN"));
        for (char *it = strtok_r(buf, ",", &save); it; it = strtok_r(NULL, ",", &save)) {
            int slot, type, flags, x, y, z, dir;
            if (sscanf(it, "%i:%i:%i:%i:%i:%i:%i", &slot, &type, &flags, &x, &y, &z, &dir) == 7)
                enemy_spawn(slot, type, flags, 0, x, y, z, (int16_t)dir);
        }
    }
    int ticks = getenv("TICKS") ? atoi(getenv("TICKS")) : 100;
    int noise_at = getenv("NOISE_AT") ? atoi(getenv("NOISE_AT")) : 0; // NOISE only from this tick on
    // PMOVE=from:to:vx:vz - Leon walks vx / vz a tick in those ticks (facing that way)
    int move_from = 0, move_to = 0, move_vx = 0, move_vz = 0, moved_x = 0, moved_z = 0;
    if (getenv("PMOVE"))
        sscanf(getenv("PMOVE"), "%i:%i:%i:%i", &move_from, &move_to, &move_vx, &move_vz);
    for (int t = 0; t < ticks; t++) {
        if (t >= move_from && t < move_to) {
            moved_x += move_vx;
            moved_z += move_vz;
        }
        int before = enemy_spawn_calls;
        if (getenv("MEMBERS")) { // tick:slot:member:value,... e.g. 5:0:0x17:1
            char buf[256], *save = NULL;
            snprintf(buf, sizeof(buf), "%s", getenv("MEMBERS"));
            for (char *it = strtok_r(buf, ",", &save); it; it = strtok_r(NULL, ",", &save)) {
                int at, slot, member, value;
                if (sscanf(it, "%i:%i:%i:%i", &at, &slot, &member, &value) == 4 && at == t)
                    enemy_member_set(slot, member, value);
            }
        }
        if (getenv("EVENT") && t == 5) // e.g. EVENT=3: the room's event function 3
            script_start_event((int)strtol(getenv("EVENT"), NULL, 0));
        if (getenv("USE")) { // tick:item[:aot] - the item USEd (variable 2), at that AOT (variable 0)
            int at = -1, item = 0, aot = -1;
            if (sscanf(getenv("USE"), "%i:%i:%i", &at, &item, &aot) >= 2 && at == t) {
                script_set_var(VAR_ITEM_USED, item);
                if (aot >= 0)
                    script_set_var(VAR_AOT_FRONT, aot);
                printf("t%d used item %#x\n", t, item);
            }
        }
        script_tick();
        EnemyTarget p = { .x = host_held ? host_px : (getenv("PX") ? atoi(getenv("PX")) : 5899) + moved_x,
                          .z = host_held ? host_pz : (getenv("PZ") ? atoi(getenv("PZ")) : 0) + moved_z,
                          .dir = move_vx || move_vz ? (int16_t)((int)(atan2f((float)-move_vz, (float)move_vx) * 32768.0f / 6.2831853f) & 32767)
                                                    : (int16_t)(getenv("PDIR") ? atoi(getenv("PDIR")) : 0), .y = getenv("PY") ? atoi(getenv("PY")) : 0, .hp = 200, .noise = getenv("NOISE") && t >= noise_at ? atoi(getenv("NOISE")) : 0, .can_grab = !host_held && !host_dead,
                          .pressed = getenv("MASH") && (t & 1), .dead = host_dead };
        enemy_tick(&p);
        // Leon shoots the nearest zombie; MULTI: the shotgun's later checks 2 and 4 ticks on
        if (getenv("SHOOT") && (t % 15 == 0 || (getenv("MULTI") && (t % 15 == 2 || t % 15 == 4))) && !host_held) {
            int32_t tx, tz;
            int target = enemy_aim_target(p.x, 0, p.z, 0, &tx, &tz);
            if (target >= 0) {
                int16_t dir = (int16_t)((int)(atan2f((float)-(tz - p.z), (float)(tx - p.x)) * 32768.0f / 6.2831853f) & 32767);
                // WEAPON: 2 the VP70, 7 / 8 the shotguns; AIM: 0 straight, 1 up, 2 down (default)
                int weapon = getenv("WEAPON") ? atoi(getenv("WEAPON")) : 2;
                static const uint16_t vp70[3] = { 16, 15, 14 }, shotgun[3] = { 200, 60, 40 }, custom[3] = { 300, 80, 60 };
                const uint16_t *dmg = weapon == 7 ? shotgun : weapon == 8 ? custom : vp70;
                int hit = enemy_hit(p.x, 0, p.z, dir, 0, weapon, dmg, getenv("AIM") ? atoi(getenv("AIM")) : AIM_DOWN);
                printf("t%d shot -> %d\n", t, hit);
            }
        }
        if (getenv("LOOK") && t % 45 == 0) { // what Leon's head turns to (facing LOOK, DS units)
            int32_t hx, hy, hz;
            int look = enemy_look_target(p.x, p.y - 2507, p.z, (int16_t)atoi(getenv("LOOK")), 7000, 0x5DC);
            if (look >= 0 && enemy_look_point(look, &hx, &hy, &hz))
                printf("t%d look -> %d at %d %d %d\n", t, look, (int)hx, (int)hy, (int)hz);
            else
                printf("t%d look -> none\n", t);
        }
        if (enemy_spawn_calls != before) printf("tick %d: %d spawn calls total, %d active\n", t, enemy_spawn_calls, enemy_count());
        if (getenv("TRACE")) enemy_debug_print(t);
    }
    printf("enemies active: %d, automatic cuts %s\n", enemy_count(), cut_auto_enabled() ? "on" : "off");
    return 0;
}
