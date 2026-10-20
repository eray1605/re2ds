// Host test for the door sequence: runs a door program with stub graphics and
// prints the part positions over time.
// Build: cc -Itests/stubs -Isource tests/door_host.c source/script.c -o build/door_host
// Run:   build/door_host 0   (door file number; needs nitrofiles/door, nitrofiles/sound)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "assets.h"

static int brightness;
void setBrightness(int screen, int level) { (void)screen; brightness = level; }
void setup_camera(const int32_t from[3], const int32_t to[3], int screen) { (void)from; (void)to; (void)screen; }
static int sounds[4];
void *sound_bank_load(const char *path) { (void)path; return (void *)1; }
void sound_bank_free(void *bank) { (void)bank; }
int sound_play(const void *bank, int note, int volume, int pan) { (void)bank; (void)volume; (void)pan; printf("  sound %d\n", note); sounds[note & 3]++; return 0; }

void *asset_load(const char *path, uint32_t *size_out)
{
    const char *p = strstr(path, "nitro:/");
    char real[256];
    snprintf(real, sizeof(real), "nitrofiles/%s", p ? p + 7 : path);
    FILE *f = fopen(real, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *d = malloc(n); fread(d, 1, n, f); fclose(f);
    if (size_out) *size_out = n;
    return d;
}
void asset_free(void *d) { free(d); }
int model_load(Model *m, const char *path)
{
    m->data = asset_load(path, NULL);
    if (!m->data) return 0;
    const uint8_t *b = m->data;
    m->header = (const ModelHeader *)b;
    m->bones = (const ModelBone *)(b + m->header->off_bones);
    m->lists = (const uint32_t *)(b + m->header->off_lists);
    return m->header->magic == MODEL_MAGIC;
}
#include "../source/door.c"
void game_aot_on(int n) { (void)n; }
int game_busy(void) { return 0; }
void game_show_message(const char *t) { (void)t; }
int game_message_open(void) { return 0; }
void game_cut_change(int c) { (void)c; }
void game_set_player_pos(int32_t x, int32_t y, int32_t z) { (void)x; (void)y; (void)z; }
void game_set_player_dir(int16_t d) { (void)d; }
void game_set_box_flag(int b, int f) { (void)b; (void)f; }
void game_play_sound(int bank, int sound) { (void)bank; (void)sound; }
void game_spawn_enemy(int s, int t, int v, int x, int y, int z, int d) { (void)s; (void)t; (void)v; (void)x; (void)y; (void)z; (void)d; }
const char *room_message(const Room *r, int n) { (void)r; (void)n; return NULL; }

int main(int argc, char **argv)
{
    int file = argc > 1 ? atoi(argv[1]) : 0, variant = argc > 2 ? atoi(argv[2]) : 0;
    if (!door_begin(file, variant)) { printf("door %d missing\n", file); return 1; }
    int n = 0;
    const char *dump = getenv("DOOR_DUMP");   // frames to dump as JSON, e.g. "30,120,240"
    while (door_frame()) {
        if (dump) {
            char key[16];
            snprintf(key, sizeof(key), ",%d,", n);
            char list[256];
            snprintf(list, sizeof(list), ",%s,", dump);
            if (strstr(list, key)) {
                printf("JSON {\"frame\": %d, \"parts\": [", n);
                for (int i = 0, first = 1; i < MAX_PARTS; i++)
                    if (parts[i].active) {
                        printf("%s{\"i\": %d, \"part\": %d, \"flags\": %d, \"pos\": [%d, %d, %d], \"rot\": [%d, %d, %d]}",
                               first ? "" : ", ", i, parts[i].part, parts[i].flags, parts[i].pos[0], parts[i].pos[1],
                               parts[i].pos[2], parts[i].rot[0], parts[i].rot[1], parts[i].rot[2]);
                        first = 0;
                    }
                printf("]}\n");
            }
        }
        if (n % 30 == 0) {
            printf("frame %3d bright %3d:", n, brightness);
            for (int i = 0; i < MAX_PARTS; i++)
                if (parts[i].active)
                    printf("  p%d[%d] pos %d,%d,%d rot %d,%d,%d", i, parts[i].part, parts[i].pos[0], parts[i].pos[1],
                           parts[i].pos[2], parts[i].rot[0], parts[i].rot[1], parts[i].rot[2]);
            printf("\n");
        }
        n++;
    }
    door_end();
    printf("door %d variant %d: %d DS frames (%.1f s), close sound %d\n", file, variant, n, n / 60.0, sounds[1]);
    return 0;
}
