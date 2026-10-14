// re2ds - interpreter for the RE2 room scripts (see script.h).
#include <stdlib.h>
#include <string.h>

#include "script.h"

#define NUM_THREADS 12
#define MAX_BLOCKS 16
#define MAX_CALLS 8
#define BUDGET 2000            // instructions per thread and tick (guards endless loops)

// Instruction sizes in bytes, opcode included (OpenBiohazard2 fileio/rdt_scd.go);
// 0 = unknown opcode.
const uint8_t scd_op_size[256] = {
    [0] = 1, [1] = 1, [2] = 1, [3] = 4, [4] = 4, [5] = 2, [6] = 4, [7] = 4, [8] = 1, [9] = 4,
    [10] = 3, [11] = 1, [12] = 1, [13] = 6, [14] = 2, [15] = 4, [16] = 2, [17] = 4, [18] = 2,
    [19] = 4, [20] = 6, [21] = 2, [22] = 2, [23] = 6, [24] = 2, [25] = 2, [26] = 2, [29] = 4,
    [32] = 1, [33] = 4, [34] = 4, [35] = 6, [36] = 4, [37] = 3, [38] = 6, [39] = 4, [40] = 1,
    [41] = 2, [42] = 1, [43] = 6, [44] = 20, [45] = 38, [46] = 3, [47] = 4, [48] = 1, [49] = 1,
    [50] = 8, [51] = 8, [52] = 4, [53] = 3, [54] = 12, [55] = 4, [57] = 8, [58] = 16, [59] = 32,
    [60] = 2, [61] = 3, [62] = 6, [63] = 4, [64] = 8, [65] = 10, [66] = 1, [67] = 4, [68] = 22,
    [70] = 10, [71] = 2, [72] = 16, [75] = 3, [76] = 5, [77] = 22, [78] = 22, [80] = 4,
    [81] = 6, [82] = 6, [83] = 6, [84] = 22, [87] = 8, [88] = 4, [89] = 4, [90] = 2, [91] = 2,
    [92] = 3, [93] = 2, [94] = 2, [95] = 2, [96] = 14, [97] = 4, [98] = 2, [99] = 1,
    [100] = 16, [101] = 2, [102] = 1, [103] = 28, [104] = 40, [105] = 30, [106] = 6,
    [107] = 4, [108] = 1, [109] = 4, [110] = 6, [111] = 2, [122] = 16, [123] = 16,
    // missing in OpenBiohazard2; inferred from the scripts of disc 1 (every
    // occurrence then decodes to evt_end with all jump targets on instruction
    // starts). Only sizes with clear evidence; others still stop the thread.
    [73] = 8, [114] = 7, [115] = 4, [116] = 4, [117] = 18, [128] = 2, [132] = 2,
    [134] = 10, [136] = 2,
};

enum {
    OP_NOP = 0, OP_EVT_END = 1, OP_EVT_NEXT = 2, OP_EVT_CHAIN = 3, OP_EVT_EXEC = 4, OP_EVT_KILL = 5,
    OP_IF = 6, OP_ELSE = 7, OP_ENDIF = 8, OP_SLEEP = 9, OP_WSLEEP = 11, OP_WSLEEPING = 12,
    OP_FOR = 13, OP_NEXT = 14, OP_WHILE = 15, OP_EWHILE = 16, OP_DO = 17, OP_EDWHILE = 18,
    OP_SWITCH = 19, OP_CASE = 20, OP_DEFAULT = 21, OP_ESWITCH = 22, OP_GOTO = 23, OP_GOSUB = 24,
    OP_RETURN = 25, OP_BREAK = 26, OP_CK = 33, OP_SET = 34, OP_CMP = 35, OP_SAVE = 36,
    OP_COPY = 37, OP_CALC = 38, OP_CALC2 = 39, OP_CUT_CHG = 41, OP_CUT_OLD = 42,
    OP_MESSAGE_ON = 43, OP_AOT_SET = 44, OP_WORK_SET = 46, OP_POS_SET = 50, OP_DIR_SET = 51,
    OP_SCA_ID_SET = 55, OP_DIR_CK = 57, OP_DOOR_AOT_SET = 59, OP_CUT_AUTO = 60, OP_MEMBER_CMP = 62,
    OP_AOT_RESET = 70, OP_AOT_ON = 71, OP_CUT_REPLACE = 75, OP_ITEM_AOT_SET = 78,
    OP_SCE_TRG_CK = 80, OP_KEEP_ITEM_CK = 94, OP_AOT_SET_4P = 103, OP_DOOR_AOT_SET_4P = 104,
    OP_ITEM_AOT_SET_4P = 105,
};

enum { B_IF, B_FOR, B_WHILE, B_DO, B_DOCOND, B_SWITCH };

typedef struct {
    uint8_t type;
    uint16_t start, end;       // block covers [start, end) of the code
    uint16_t loop;             // for: body start; docond: do body start
    uint16_t count;            // for: iterations left
} Block;

typedef struct {
    uint8_t active;
    const uint8_t *code;       // SCD block (function table + bytecode)
    uint16_t pc;
    uint16_t sleep;
    uint8_t wait_message;
    uint8_t work_type;         // work_set: 1 = player, 3 = enemy, 4 = object
    int nblocks, ncalls;
    Block blocks[MAX_BLOCKS];
    struct {
        uint16_t pc;
        uint8_t nblocks;
    } calls[MAX_CALLS];
} Thread;

Aot aots[NUM_AOTS];
int script_threads_running;
int script_unknown_ops;

static Thread threads[NUM_THREADS];
static const Room *cur_room;
static uint8_t flags[NUM_FLAG_ARRAYS][32];
static int16_t vars[NUM_VARS];
static uint8_t cam_map[32];
static uint8_t cam_auto = 1;
static uint8_t old_cam;
static struct {
    uint8_t item, count;
} inventory[16];

static inline int16_t rd16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static inline uint16_t ru16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static uint16_t function_start(const uint8_t *code, int n)
{
    uint16_t table_size = ru16(code);
    if (n < 0 || 2 * n >= table_size)
        return 0xFFFF;
    return ru16(code + 2 * n);
}

// ------------------------------------------------------------ flags, vars
int flag_get(int array, int bit)
{
    if (array < 0 || array >= NUM_FLAG_ARRAYS || bit < 0 || bit > 255)
        return 0;
    return (flags[array][bit >> 3] >> (bit & 7)) & 1;
}

void flag_set(int array, int bit, int value)
{
    if (array < 0 || array >= NUM_FLAG_ARRAYS || bit < 0 || bit > 255)
        return;
    if (value)
        flags[array][bit >> 3] |= 1 << (bit & 7);
    else
        flags[array][bit >> 3] &= ~(1 << (bit & 7));
}

int cut_replace(int cam) { return cam >= 0 && cam < 32 ? cam_map[cam] : cam; }
int cut_auto_enabled(void) { return cam_auto; }

int inventory_has(int item)
{
    for (int i = 0; i < 16; i++)
        if (inventory[i].count && inventory[i].item == item)
            return 1;
    return 0;
}

void inventory_add(int item, int amount)
{
    for (int i = 0; i < 16; i++)
        if (inventory[i].count && inventory[i].item == item) {
            inventory[i].count += amount;
            return;
        }
    for (int i = 0; i < 16; i++)
        if (!inventory[i].count) {
            inventory[i].item = item;
            inventory[i].count = amount ? amount : 1;
            return;
        }
}

// ------------------------------------------------------------ AOTs
static void aot_rect(Aot *a, const uint8_t *p)  // p: x, z, w, d
{
    int16_t x = rd16(p), z = rd16(p + 2), w = rd16(p + 4), d = rd16(p + 6);
    a->x[0] = x;     a->z[0] = z;
    a->x[1] = x + w; a->z[1] = z;
    a->x[2] = x + w; a->z[2] = z + d;
    a->x[3] = x;     a->z[3] = z + d;
}

static void aot_quad(Aot *a, const uint8_t *p)  // p: 4 x (x, z)
{
    for (int k = 0; k < 4; k++) {
        a->x[k] = rd16(p + 4 * k);
        a->z[k] = rd16(p + 4 * k + 2);
    }
}

// Common head of all AOT instructions: op, aot, id, sat, floor, super
static Aot *aot_head(const uint8_t *ins)
{
    if (ins[1] >= NUM_AOTS)
        return NULL;
    Aot *a = &aots[ins[1]];
    a->active = 1;
    a->type = ins[2];
    a->sat = ins[3];
    a->floor = ins[4];
    a->inside = 0;
    return a;
}

static void op_aot(const uint8_t *ins)
{
    Aot *a = aot_head(ins);
    if (!a)
        return;
    memset(a->data, 0, sizeof(a->data));
    switch (ins[0]) {
    case OP_AOT_SET:            // + x z w d, data[6]
        aot_rect(a, ins + 6);
        memcpy(a->data, ins + 14, 6);
        break;
    case OP_AOT_SET_4P:         // + 4 points, data[6]
        aot_quad(a, ins + 6);
        memcpy(a->data, ins + 22, 6);
        break;
    case OP_DOOR_AOT_SET:       // + x z w d, door data[18]
        aot_rect(a, ins + 6);
        memcpy(a->data, ins + 14, 18);
        break;
    case OP_DOOR_AOT_SET_4P:
        aot_quad(a, ins + 6);
        memcpy(a->data, ins + 22, 18);
        break;
    case OP_ITEM_AOT_SET:       // + x z w d, item data[8]
        aot_rect(a, ins + 6);
        memcpy(a->data, ins + 14, 8);
        break;
    case OP_ITEM_AOT_SET_4P:
        aot_quad(a, ins + 6);
        memcpy(a->data, ins + 22, 8);
        break;
    }
}

// aot_reset: op, aot, id, sat, data[6] (position stays)
static void op_aot_reset(const uint8_t *ins)
{
    if (ins[1] >= NUM_AOTS)
        return;
    Aot *a = &aots[ins[1]];
    a->type = ins[2];
    a->sat = ins[3];
    memcpy(a->data, ins + 4, 6);
}

// ------------------------------------------------------------ threads
static Thread *start_thread(int slot, const uint8_t *code, int function)
{
    uint16_t pc = function_start(code, function);
    if (pc == 0xFFFF)
        return NULL;
    if (slot < 0 || slot >= NUM_THREADS) {
        for (slot = 1; slot < NUM_THREADS && threads[slot].active; slot++)
            ;
        if (slot == NUM_THREADS)
            return NULL;
    }
    Thread *t = &threads[slot];
    memset(t, 0, sizeof(*t));
    t->active = 1;
    t->code = code;
    t->pc = pc;
    return t;
}

void script_start_event(int function)
{
    if (cur_room && cur_room->exec_script)
        start_thread(-1, cur_room->exec_script, function);
}

static int call_base(const Thread *t)
{
    return t->ncalls ? t->calls[t->ncalls - 1].nblocks : 0;
}

static Block *push_block(Thread *t, int type, int start, int end)
{
    if (t->nblocks == MAX_BLOCKS)
        return NULL;
    Block *b = &t->blocks[t->nblocks++];
    b->type = type;
    b->start = start;
    b->end = end;
    b->loop = 0;
    b->count = 0;
    return b;
}

// Leave every block whose range no longer contains pc. Returns 1 if a do
// loop's conditions passed and the thread jumped back (= end of an iteration).
static int unwind(Thread *t)
{
    int base = call_base(t);
    while (t->nblocks > base) {
        Block *b = &t->blocks[t->nblocks - 1];
        if (t->pc >= b->start && t->pc < b->end)
            return 0;
        t->nblocks--;
        if (b->type == B_DOCOND && t->pc == b->end) {
            t->pc = b->loop;
            return 1;
        }
    }
    return 0;
}

// A condition failed: leave the innermost if / while / do-condition.
static void condition_failed(Thread *t)
{
    for (int i = t->nblocks - 1; i >= call_base(t); i--) {
        Block *b = &t->blocks[i];
        if (b->type == B_IF) {
            t->pc = b->end;
            // into the else body: the if block now ends with it
            if (b->end >= 4 && t->code[b->end - 4] == OP_ELSE) {
                b->end = b->end - 4 + ru16(t->code + b->end - 2);
                t->nblocks = i + 1;
            } else {
                t->nblocks = i;
            }
            return;
        }
        if (b->type == B_WHILE || b->type == B_DOCOND) {
            t->pc = b->end;
            t->nblocks = i;
            return;
        }
    }
    // condition outside any if/loop: ignored
}

static int compare(int16_t a, int op, int16_t b)
{
    switch (op) {
    case 0: return a == b;
    case 1: return a > b;
    case 2: return a >= b;
    case 3: return a < b;
    case 4: return a <= b;
    case 5: return a != b;
    default: return (a & b) != 0;
    }
}

static int16_t calc(int16_t a, int op, int16_t b)
{
    switch (op) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return b ? a / b : 0;
    case 4: return b ? a % b : 0;
    case 5: return a | b;
    case 6: return a & b;
    case 7: return a ^ b;
    case 8: return ~a;
    case 9: return a << (b & 15);
    case 10: return (uint16_t)a >> (b & 15);
    default: return a >> (b & 15);
    }
}

static int16_t *var(int n) { return &vars[n < NUM_VARS ? n : 0]; }

// Runs one thread until it yields or ends.
static void run_thread(Thread *t)
{
    if (t->wait_message) {
        if (game_message_open())
            return;
        t->wait_message = 0;
    }
    if (t->sleep) {
        t->sleep--;
        return;
    }
    for (int budget = BUDGET; budget > 0 && t->active; budget--) {
        if (unwind(t))
            return; // do loop iteration done
        const uint8_t *ins = t->code + t->pc;
        int op = ins[0];
        int size = scd_op_size[op];
        if (size == 0) {
            script_unknown_ops++;
            t->active = 0;
            return;
        }
        int cond = -1; // condition result of this instruction, -1 = not a condition
        int next = t->pc + size;

        switch (op) {
        case OP_EVT_END:
        case OP_RETURN:
            if (t->ncalls) {
                t->ncalls--;
                next = t->calls[t->ncalls].pc;
                t->nblocks = t->calls[t->ncalls].nblocks;
            } else {
                t->active = 0;
                return;
            }
            break;
        case OP_EVT_NEXT:
            t->pc = next;
            return;
        case OP_EVT_CHAIN: { // replace this event with another function
            uint16_t pc = function_start(t->code, ins[3]);
            if (pc == 0xFFFF) {
                t->active = 0;
                return;
            }
            t->nblocks = t->ncalls = 0;
            next = pc;
            break;
        }
        case OP_EVT_EXEC:
            start_thread(ins[1] == 0xFF ? -1 : ins[1], t->code, ins[3]);
            break;
        case OP_EVT_KILL:
            if (ins[1] < NUM_THREADS && &threads[ins[1]] != t)
                threads[ins[1]].active = 0;
            break;
        case OP_IF:
            push_block(t, B_IF, t->pc, next + ru16(ins + 2));
            break;
        case OP_ELSE: // reached the end of the if body
            next = t->pc + ru16(ins + 2);
            break;
        case OP_SLEEP:
            t->sleep = ru16(ins + 2);
            t->pc = next;
            return;
        case OP_WSLEEP:
        case OP_WSLEEPING:
            t->pc = next;
            return;
        case OP_FOR:
            if (ru16(ins + 4) == 0) {
                next += ru16(ins + 2);
            } else {
                Block *b = push_block(t, B_FOR, t->pc, next + ru16(ins + 2));
                if (b) {
                    b->loop = next;
                    b->count = ru16(ins + 4);
                }
            }
            break;
        case OP_NEXT:
            if (t->nblocks > call_base(t)) {
                Block *b = &t->blocks[t->nblocks - 1];
                if (b->type == B_FOR && --b->count > 0)
                    next = b->loop;
            }
            break;
        case OP_WHILE:
            push_block(t, B_WHILE, t->pc, next + ru16(ins + 2));
            break;
        case OP_EWHILE:
            if (t->nblocks > call_base(t) && t->blocks[t->nblocks - 1].type == B_WHILE) {
                t->pc = t->blocks[--t->nblocks].start; // re-check the conditions next tick
                return;
            }
            break;
        case OP_DO:
            push_block(t, B_DO, t->pc, next + ru16(ins + 2));
            break;
        case OP_EDWHILE:
            if (t->nblocks > call_base(t) && t->blocks[t->nblocks - 1].type == B_DO) {
                Block *d = &t->blocks[t->nblocks - 1];
                Block *c = push_block(t, B_DOCOND, t->pc, d->end);
                if (c)
                    c->loop = d->start + scd_op_size[OP_DO];
            }
            break;
        case OP_SWITCH: {
            uint16_t end = next + ru16(ins + 2);
            int16_t value = *var(ins[1]);
            uint16_t target = end, p = next;
            while (p < end) {
                if (t->code[p] == OP_CASE) {
                    if (rd16(t->code + p + 4) == value) {
                        target = p + 6;
                        break;
                    }
                    p += 6 + ru16(t->code + p + 2);
                } else if (t->code[p] == OP_DEFAULT) {
                    target = p + 2;
                    break;
                } else {
                    break;
                }
            }
            push_block(t, B_SWITCH, t->pc, end);
            next = target;
            break;
        }
        case OP_GOTO:
            next = t->pc + rd16(ins + 4);
            break;
        case OP_GOSUB: {
            uint16_t pc = function_start(t->code, ins[1]);
            if (pc != 0xFFFF && t->ncalls < MAX_CALLS) {
                t->calls[t->ncalls].pc = next;
                t->calls[t->ncalls].nblocks = t->nblocks;
                t->ncalls++;
                next = pc;
            }
            break;
        }
        case OP_BREAK:
            for (int i = t->nblocks - 1; i >= call_base(t); i--) {
                int type = t->blocks[i].type;
                if (type == B_FOR || type == B_WHILE || type == B_DO || type == B_SWITCH) {
                    next = t->blocks[i].end;
                    t->nblocks = i;
                    break;
                }
            }
            break;

        case OP_CK:
            cond = flag_get(ins[1], ins[2]) == ins[3];
            break;
        case OP_SET:
            flag_set(ins[1], ins[2], ins[3] == 7 ? !flag_get(ins[1], ins[2]) : ins[3]);
            break;
        case OP_CMP:
            cond = compare(*var(ins[2]), ins[3], rd16(ins + 4));
            break;
        case OP_SAVE:
            *var(ins[1]) = rd16(ins + 2);
            break;
        case OP_COPY:
            *var(ins[1]) = *var(ins[2]);
            break;
        case OP_CALC:
            *var(ins[3]) = calc(*var(ins[3]), ins[2], rd16(ins + 4));
            break;
        case OP_CALC2:
            *var(ins[2]) = calc(*var(ins[2]), ins[1], *var(ins[3]));
            break;
        case OP_KEEP_ITEM_CK:
            cond = inventory_has(ins[1]);
            break;
        case OP_DIR_CK:
        case OP_MEMBER_CMP:
        case OP_SCE_TRG_CK:
            cond = 1; // needs object state the engine does not have yet
            break;

        case OP_CUT_CHG:
            game_cut_change(ins[1]);
            break;
        case OP_CUT_OLD:
            game_cut_change(old_cam);
            break;
        case OP_CUT_AUTO:
            cam_auto = ins[1];
            break;
        case OP_CUT_REPLACE:
            if (ins[1] < 32)
                cam_map[ins[1]] = ins[2];
            break;
        case OP_MESSAGE_ON: {
            const char *text = cur_room ? room_message(cur_room, ins[2]) : NULL;
            if (text && *text) {
                game_show_message(text);
                t->wait_message = 1;
                t->pc = next;
                return;
            }
            break;
        }
        case OP_AOT_SET:
        case OP_AOT_SET_4P:
        case OP_DOOR_AOT_SET:
        case OP_DOOR_AOT_SET_4P:
        case OP_ITEM_AOT_SET:
        case OP_ITEM_AOT_SET_4P:
            op_aot(ins);
            break;
        case OP_AOT_RESET:
            op_aot_reset(ins);
            break;
        case OP_WORK_SET:
            t->work_type = ins[1];
            break;
        case OP_SCA_ID_SET:
            game_set_box_flag(ins[1], ru16(ins + 2));
            break;
        case OP_POS_SET:
            if (t->work_type == 1)
                game_set_player_pos(rd16(ins + 2), rd16(ins + 4), rd16(ins + 6));
            break;
        case OP_DIR_SET:
            if (t->work_type == 1)
                game_set_player_dir(rd16(ins + 4));
            break;
        default:
            break; // not implemented yet: skipped
        }

        t->pc = next;
        if (cond == 0)
            condition_failed(t);
    }
}

void script_tick(void)
{
    script_threads_running = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        if (threads[i].active) {
            run_thread(&threads[i]);
            script_threads_running++;
        }
    }
}

void script_camera_changed(int previous, int cam)
{
    old_cam = previous;
    vars[VAR_CUT] = cam;
}

void script_room_enter(const Room *room)
{
    cur_room = room;
    memset(aots, 0, sizeof(aots));
    memset(threads, 0, sizeof(threads));
    for (int i = 0; i < 32; i++)
        cam_map[i] = i;
    cam_auto = 1;
    script_unknown_ops = 0;

    // init script: run function 0 to completion
    if (room->init_script) {
        Thread *t = start_thread(0, room->init_script, 0);
        for (int guard = 0; t && t->active && guard < 64; guard++) {
            t->sleep = 0;
            run_thread(t);
        }
        if (t)
            t->active = 0;
    }
    if (room->exec_script)
        start_thread(0, room->exec_script, 0);

    // Doors the converter found in the scripts but that were not set (the init
    // script stopped early at an unsupported opcode): add them so every room
    // stays reachable.
    for (int i = 0; i < room->header->num_doors; i++) {
        const RoomDoor *d = &room->doors[i];
        if (d->aot >= NUM_AOTS || aots[d->aot].active)
            continue;
        Aot *a = &aots[d->aot];
        a->active = 1;
        a->type = SCE_DOOR;
        a->sat = d->sat;
        a->floor = d->floor;
        memcpy(a->x, d->x, sizeof(a->x));
        memcpy(a->z, d->z, sizeof(a->z));
        uint8_t *p = a->data;
        int16_t dir = (int16_t)(d->next_dir / 8);
        memcpy(p, d->next, 6);
        memcpy(p + 6, &dir, 2);
        p[8] = d->next_stage - 1;
        p[9] = d->next_room;
        p[10] = d->next_camera;
        p[11] = d->next_floor;
        p[13] = d->door_type;
        p[15] = d->key_id;
        p[16] = d->key_type;
    }
}
