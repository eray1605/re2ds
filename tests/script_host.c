// Host test for the script interpreter: runs the scripts of every converted
// room for a while with stub engine hooks and reports problems.
// Build: cc -O1 -g -fsanitize=undefined -Isource tests/script_host.c -o build/script_host
// Run:   build/script_host nitrofiles/room/room*0.bin
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "script.h"

static int messages, cut_changes, pos_sets;

void game_aot_on(int n) { (void)n; }
int game_busy(void) { return 0; }
void game_show_message(const char *text) { messages++; (void)text; }
int game_message_open(void) { return 0; }
void game_cut_change(int cam) { cut_changes++; (void)cam; }
void game_set_player_pos(int32_t x, int32_t y, int32_t z) { pos_sets++; (void)x; (void)y; (void)z; }
void game_set_player_dir(int16_t dir) { (void)dir; }
void game_set_box_flag(int box, int flag) { (void)box; (void)flag; }
void game_bgm_control(int i, int o, int t, int l, int r) { (void)i; (void)o; (void)t; (void)l; (void)r; }
void game_bgmtbl_set(int s, int r, int m, int u) { (void)s; (void)r; (void)m; (void)u; }
void game_object_super(int n, int p, int pt, const int16_t pos[3], const int16_t dir[3]) { (void)n; (void)p; (void)pt; (void)pos; (void)dir; }
void game_object_super_reset(int n, const int16_t dir[3]) { (void)n; (void)dir; }
void game_shake(int x, int y) { (void)x; (void)y; }
int game_player_poisoned(void) { return 0; }
void game_player_cure(void) {}
void game_espr_on2(const uint8_t *ins, int d) { (void)ins; (void)d; }
void game_espr_kill2(int h) { (void)h; }
int game_sight_blocked(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return 0; }
void game_enemy_part_set(int s, int p, int f, int v) { (void)s; (void)p; (void)f; (void)v; }
int game_keys(int pressed) { (void)pressed; return 0; }
void game_fade_set(int n, int k, int s) { (void)n; (void)k; (void)s; }
void game_fade_adjust(int n, int l) { (void)n; (void)l; }
void game_player_heal(int i, int a) { (void)i; (void)a; }
void game_item_get(int item, int count) { printf("  item get %02x x%d\n", item, count); }
void game_movie(int n) { (void)n; }
void game_play_sound(int bank, int sound) { (void)bank; (void)sound; }
int game_enemy_member(int s, int m, int *v) { (void)s; (void)m; (void)v; return 0; }
void game_enemy_member_set(int s, int m, int v) { (void)s; (void)m; (void)v; }
int game_player_member(int m, int *v) { (void)m; (void)v; return 0; }
void game_player_member_set(int m, int v) { (void)m; (void)v; }
void game_voice(int k, int n) { (void)k; (void)n; }
int game_voice_playing(void) { return 0; }
void game_object_set(const uint8_t *ins) { (void)ins; }
void game_object_hide(int model) { (void)model; }
void game_espr_on(const uint8_t *ins) { (void)ins; }
void game_espr3d_on(const uint8_t *ins) { (void)ins; }
void game_espr_kill(int id, int type) { (void)id; (void)type; }
void game_floor_sound_set(int n, int on) { (void)n; (void)on; }
int game_object_member(int n, int m, int *v) { (void)n; (void)m; (void)v; return 0; }
void game_object_member_set(int n, int m, int v) { (void)n; (void)m; (void)v; }
int game_plc(int w, int i, const uint8_t *ins) { (void)w; (void)i; (void)ins; return 1; }
void game_spawn_enemy(int s, int t, int v, int voice, int x, int y, int z, int d) { (void)voice; if (getenv("SHOW_SPAWNS")) printf("  spawn slot %d type %02x variant %d at %d %d %d dir %d\n", s, t, v, x, y, z, d); }
void game_enemy_motion(int s, int m, int c) { (void)s; (void)m; (void)c; }
void game_enemy_skin(int s, int m) { (void)s; (void)m; }
void game_cut_be_set(int c, int n, int on) { (void)c; (void)n; (void)on; }
void game_enemy_shadow(int s, int r, int g, int b, int w, int h, int ox, int oz)
{
    printf("  shadow enemy %d colour %02x %02x %02x size %d x %d offset %d %d\n", s, r, g, b, w, h, ox, oz);
}

const char *room_message(const Room *room, int n)
{
    if (n < 0 || n >= room->header->num_msgs)
        return NULL;
    const uint8_t *msgs = (const uint8_t *)room->data + room->header->off_msgs;
    return (const char *)msgs + (msgs[2 * n] | msgs[2 * n + 1] << 8);
}

#include "../source/script.c"

int main(int argc, char **argv)
{
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *data = malloc(n);
        fread(data, 1, n, f);
        fclose(f);
        Room room = { .data = data, .header = (const RoomHeader *)data };
        if (room.header->magic != ROOM_MAGIC || room.header->version != ROOM_VERSION) {
            printf("%s: bad header\n", argv[i]);
            bad++;
            continue;
        }
        const RoomHeader *h = room.header;
        room.doors = (const RoomDoor *)(data + sizeof(RoomHeader) + h->num_cameras * sizeof(RoomCamera) +
                                        h->num_switches * sizeof(RoomSwitch) + h->num_boxes * sizeof(RoomBox));
        room.init_script = room.header->size_init ? data + room.header->off_init : NULL;
        room.exec_script = room.header->size_exec ? data + room.header->off_exec : NULL;
        messages = cut_changes = pos_sets = 0;
        if (getenv("SYS19")) // the PS1's boot-time system flag (main.c, system_flags)
            flag_set(0, 0x19, 1);
        script_room_enter(&room);
        int unknown_after_init = script_unknown_ops;
        int aot_count = 0, doors = 0;
        for (int k = 0; k < NUM_AOTS; k++)
            if (aots[k].active) {
                aot_count++;
                doors += aots[k].type == SCE_DOOR;
            }
        int max_threads = 0;
        for (int tick = 0; tick < 25 * 30; tick++) {
            script_tick();
            if (script_threads_running > max_threads)
                max_threads = script_threads_running;
        }
        // threads that used their whole budget in one tick are probably stuck
        printf("%-28s aots %2d (doors %d)  threads max %2d now %2d  unknown %d/%d  msgs %d cut %d pos %d\n",
               strrchr(argv[i], '/') + 1, aot_count, doors, max_threads, script_threads_running,
               unknown_after_init, script_unknown_ops, messages, cut_changes, pos_sets);
        if (getenv("ITEMS")) // item AOTs after the scripts ran: item, amount, picked flag, model
            for (int k = 0; k < NUM_AOTS; k++)
                if (aots[k].active && aots[k].type == SCE_ITEM)
                    printf("    item %02x x%d picked %02x model %d\n", aots[k].data[0], aots[k].data[2],
                           aots[k].data[4], aots[k].data[6]);
        if (getenv("AOTS")) // every AOT after the scripts ran: type, data bytes
            for (int k = 0; k < NUM_AOTS; k++)
                if (aots[k].active)
                    printf("    aot %d type %d data %02x %02x %02x %02x %02x %02x %02x %02x\n", k, aots[k].type,
                           aots[k].data[0], aots[k].data[1], aots[k].data[2], aots[k].data[3], aots[k].data[4],
                           aots[k].data[5], aots[k].data[6], aots[k].data[7]);
        extern int script_unknown_at;
        if (script_unknown_ops)
            printf("    unknown op %d at %d\n", script_unknown_at >> 16, script_unknown_at & 0xFFFF);
        if (script_unknown_ops)
            bad++;
        free(data);
    }
    printf("%d rooms with problems\n", bad);
    return 0;
}
