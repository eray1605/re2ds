// re2ds - subtitles (see subs.h). The file is kept in memory as read; lines
// are cut in place:
//   @<key>
//   <start ms> <end ms> <text>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "subs.h"

#define MAX_CLIPS 400
#define MAX_LINES 2000

typedef struct {
    uint32_t start, end;
    const char *text;
} SubLine;

typedef struct {
    const char *key;
    uint16_t first, count;
} SubClip;

static char *data;
static SubClip clips[MAX_CLIPS];
static SubLine lines[MAX_LINES];
static int num_clips, num_lines;

void subs_init(void)
{
    uint32_t size;
    char *raw = asset_load("nitro:/subs/subs.txt", &size);
    if (!raw)
        return;
    data = malloc(size + 1);
    if (!data) {
        asset_free(raw);
        return;
    }
    memcpy(data, raw, size);
    data[size] = 0;
    asset_free(raw);
    for (char *p = data; *p;) {
        char *end = strchr(p, '\n');
        if (end)
            *end = 0;
        if (*p == '@' && num_clips < MAX_CLIPS) {
            clips[num_clips].key = p + 1;
            clips[num_clips].first = num_lines;
            clips[num_clips].count = 0;
            num_clips++;
        } else if (*p && num_clips && num_lines < MAX_LINES) {
            char *t;
            SubLine *l = &lines[num_lines];
            l->start = strtoul(p, &t, 10);
            l->end = strtoul(t, &t, 10);
            while (*t == ' ')
                t++;
            l->text = t;
            num_lines++;
            clips[num_clips - 1].count++;
        }
        if (!end)
            break;
        p = end + 1;
    }
}

static const SubClip *find(const char *key)
{
    // asked every frame for the same clip: remember the last answer
    static char last_key[16];
    static const SubClip *last;
    if (!key)
        return NULL;
    if (!strcmp(key, last_key))
        return last;
    snprintf(last_key, sizeof(last_key), "%s", key);
    last = NULL;
    for (int i = 0; i < num_clips; i++)
        if (!strcmp(clips[i].key, key)) {
            last = &clips[i];
            break;
        }
    return last;
}

int subs_have(const char *key) { return find(key) != NULL; }

const char *subs_at(const char *key, uint32_t ms)
{
    const SubClip *c = find(key);
    if (!c)
        return NULL;
    for (int i = c->first; i < c->first + c->count; i++)
        if (ms >= lines[i].start && ms < lines[i].end + 500) // a little longer to read
            if (i + 1 >= c->first + c->count || ms < lines[i + 1].start)
                return lines[i].text;
    return NULL;
}
