// re2ds - loading converted data from NitroFS
#include <stdio.h>
#include <stdlib.h>

#include <nds/arm9/decompress.h>

#include "assets.h"

void *asset_load(const char *path, uint32_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *data = aligned_alloc(32, (size + 31) & ~31);
    if (data && fread(data, 1, size, f) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (size_out)
        *size_out = (uint32_t)size;
    return data;
}

void asset_free(void *data)
{
    free(data);
}

int room_load(Room *room, const char *path)
{
    room->data = asset_load(path, NULL);
    if (!room->data)
        return 0;
    const uint8_t *p = room->data;
    room->header = (const RoomHeader *)p;
    if (room->header->magic != ROOM_MAGIC || room->header->version != ROOM_VERSION) {
        free(room->data);
        room->data = NULL;
        return 0;
    }
    p += sizeof(RoomHeader);
    room->cameras = (const RoomCamera *)p;
    p += sizeof(RoomCamera) * room->header->num_cameras;
    room->switches = (const RoomSwitch *)p;
    p += sizeof(RoomSwitch) * room->header->num_switches;
    room->boxes = (const RoomBox *)p;
    p += sizeof(RoomBox) * room->header->num_boxes;
    room->doors = (const RoomDoor *)p;
    const uint8_t *base = room->data;
    room->init_script = room->header->size_init ? base + room->header->off_init : NULL;
    room->exec_script = room->header->size_exec ? base + room->header->off_exec : NULL;
    return 1;
}

const char *room_message(const Room *room, int n)
{
    if (n < 0 || n >= room->header->num_msgs)
        return NULL;
    const uint8_t *msgs = (const uint8_t *)room->data + room->header->off_msgs;
    return (const char *)msgs + (msgs[2 * n] | msgs[2 * n + 1] << 8);
}

int model_load(Model *model, const char *path)
{
    model->data = asset_load(path, NULL);
    if (!model->data)
        return 0;
    const uint8_t *base = model->data;
    model->header = (const ModelHeader *)base;
    if (model->header->magic != MODEL_MAGIC)
        return 0;
    model->bones = (const ModelBone *)(base + model->header->off_bones);
    model->lists = (const uint32_t *)(base + model->header->off_lists);
    model->anims = (const ModelAnim *)(base + model->header->off_anims);
    model->anim_pool = (const uint16_t *)(model->anims + model->header->num_anims);
    model->frame_size = sizeof(ModelFrame) + 6 * model->header->num_bones;
    return 1;
}

int masks_load(Masks *masks, const char *path)
{
    uint8_t *packed = asset_load(path, NULL);
    if (!packed)
        return 0;
    // BIOS LZ77 header: type 0x10, unpacked size in the upper 24 bits
    uint32_t size = (packed[1] | packed[2] << 8 | packed[3] << 16);
    masks->data = aligned_alloc(32, (size + 31) & ~31);
    if (!masks->data) {
        free(packed);
        return 0;
    }
    decompress(packed, masks->data, LZ77);
    free(packed);
    const uint8_t *p = masks->data;
    masks->header = (const MaskHeader *)p;
    if (masks->header->magic != MASK_MAGIC) {
        free(masks->data);
        return 0;
    }
    p += sizeof(MaskHeader);
    masks->palette = (const uint16_t *)p;
    p += 512;
    masks->pixels = p;
    p += 256 * masks->header->tex_height;
    masks->quads = (const MaskQuad *)p;
    return 1;
}
