// re2ds - loading converted data from NitroFS (see formats.h)
#pragma once

#include "formats.h"

// Loads a whole file into a new 32-byte aligned buffer; returns NULL on error.
void *asset_load(const char *path, uint32_t *size_out);

typedef struct {
    void *data;
    const RoomHeader *header;
    const RoomCamera *cameras;
    const RoomSwitch *switches;
    const RoomBox *boxes;
} Room;

typedef struct {
    void *data;
    const ModelHeader *header;
    const ModelBone *bones;
    const uint32_t *lists;       // file offsets per (bone, page), 0 = none
    const uint16_t *anim_pool;   // frame indices
    const ModelAnim *anims;
    uint32_t frame_size;         // bytes per ModelFrame incl. rotations
} Model;

typedef struct {
    void *data;
    const MaskHeader *header;
    const uint16_t *palette;
    const uint8_t *pixels;
    const MaskQuad *quads;
} Masks;

int room_load(Room *room, const char *path);
int model_load(Model *model, const char *path);
int masks_load(Masks *masks, const char *path);   // 0 if the file is missing
void asset_free(void *data);

static inline const ModelFrame *model_frame(const Model *m, int index)
{
    return (const ModelFrame *)((const uint8_t *)m->data + m->header->off_frames + index * m->frame_size);
}

static inline const int16_t *model_frame_rot(const Model *m, int index)
{
    return (const int16_t *)((const uint8_t *)model_frame(m, index) + sizeof(ModelFrame));
}
