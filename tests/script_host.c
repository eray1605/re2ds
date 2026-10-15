// Host test for the script interpreter: runs the scripts of every converted
// room for a while with stub engine hooks and reports problems.
// Build: cc -O1 -g -fsanitize=undefined -Isource tests/script_host.c -o build/script_host
// Run:   build/script_host nitrofiles/room/room*0.bin
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "script.h"

static int messages, cut_changes, pos_sets;

void game_show_message(const char *text) { messages++; (void)text; }
int game_message_open(void) { return 0; }
void game_cut_change(int cam) { cut_changes++; (void)cam; }
void game_set_player_pos(int32_t x, int32_t y, int32_t z) { pos_sets++; (void)x; (void)y; (void)z; }
void game_set_player_dir(int16_t dir) { (void)dir; }
void game_set_box_flag(int box, int flag) { (void)box; (void)flag; }
void game_play_sound(int bank, int sound) { (void)bank; (void)sound; }
int game_enemy_member(int s, int m, int *v) { (void)s; (void)m; (void)v; return 0; }
void game_enemy_member_set(int s, int m, int v) { (void)s; (void)m; (void)v; }
void game_spawn_enemy(int s, int t, int v, int voice, int x, int y, int z, int d) { (void)voice; if (getenv("SHOW_SPAWNS")) printf("  spawn slot %d type %02x variant %d at %d %d %d dir %d\n", s, t, v, x, y, z, d); }

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
        if (script_unknown_ops)
            bad++;
        free(data);
    }
    printf("%d rooms with problems\n", bad);
    return 0;
}
