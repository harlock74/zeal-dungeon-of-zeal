#include <string.h>
#include <stddef.h>
#include "game.h"
#include "room_timing.h"
#include "input.h"
#include "sound.h"
#include "world.h"
#include "video.h"
Game g;
Enemy enemies[MAX_ENEMIES];
Shot shots[MAX_SHOTS];
Effect effects[5];
uint8_t solid[125];
uint8_t room_handoff;
static uint8_t doorway_enter;
static uint8_t chest_cache_room = 255, chest_cache_count;
static RoomObject* chest_cache[16];
static const uint8_t moves[8] = { 20, 22, 16, 22, 20, 20, 18, 16 };
static const uint8_t animations[8] = { 8, 16, 19, 17, 8, 8, 35, 24 };
/* create_enemy assembly supports counts3,6,or a power of two only. */
static const uint8_t frame_counts[8] = { 4, 2, 6, 3, 4, 3, 2, 2 };
static const uint8_t points[8] = { 10, 20, 20, 30, 40, 40, 50, 50 };
static const uint8_t shooting[8] = { 0, 1, 0, 2, 0, 0, 3, 0 };
enum { N, NE, E, SE, S, SW, W, NW, PX, PY, PURSUIT, HOLD };
/* Independently authored command sequences expressing Stage D's classes.
   No original script sequences or direction-byte encoding are reused. */
static const uint8_t patterns[8][12] = { { E, N, E, PURSUIT, N, E, PURSUIT, N, E, E, N, PURSUIT },
    { E, E, PY, W, W, PY, E, PURSUIT, W, W, PX, PURSUIT },
    { HOLD, HOLD, HOLD, HOLD, HOLD, HOLD, HOLD, HOLD, HOLD, HOLD, HOLD, HOLD },
    { N, N, SE, SW, SE, SW, N, PURSUIT, N, SE, SW, PURSUIT },
    { N, NE, E, SE, PURSUIT, S, SW, W, NW, PURSUIT, E, PURSUIT },
    { N, NW, W, SW, PURSUIT, S, SE, E, NE, PURSUIT, W, PURSUIT },
    { E, S, W, N, HOLD, HOLD, PURSUIT, E, S, W, N, PURSUIT },
    { E, E, N, PY, W, W, S, PX, E, N, W, PURSUIT } };
static const int8_t vx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 }, vy[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
static uint8_t random8(void)
{
    uint16_t x = g.seed;
    x ^= x << 7;
    x ^= x >> 9;
    x ^= x << 8;
    g.seed = x ? x : 0x6d2b;
    return g.seed;
}
static int8_t sign(uint8_t target, uint8_t value)
{
    return target > value ? 1 : target < value ? -1 : 0;
}
static void direction(uint8_t mask, int8_t* dx, int8_t* dy)
{
    *dx = (mask & (L | R)) == L ? -1 : (mask & (L | R)) == R ? 1 : 0;
    *dy = (mask & (U | D)) == U ? -1 : (mask & (U | D)) == D ? 1 : 0;
}
static uint8_t aim_at(uint8_t x, uint8_t y)
{
    return (g.x > x ? R : g.x < x ? L : 0) | (g.y + 1 > y ? D : g.y + 1 < y ? U : 0);
}
static void award(uint16_t n)
{
    if (g.score > 999999UL - n)
        g.score = 999999UL;
    else
        g.score += n;
    while (g.score >= g.nextlife && g.nextlife <= 999999UL) {
        if (g.lives < 9) {
            ++g.lives;
            ++g.extras;
            sound_event(7);
        }
        g.nextlife += 10000UL;
    }
    if (g.score > g.high)
        g.high = g.score;
    g.hud_dirty = 1;
}
static void feedback(uint8_t message)
{
    g.feedback = message;
    g.feedback_time = 120;
    g.hud_dirty = 1;
}
static void resupply(void)
{
    g.ammo = g.ammo > 205 ? 255 : g.ammo + 50;
    g.hud_dirty = 1;
}
static void burst(uint8_t x, uint8_t y)
{
    uint8_t i;
    for (i = 0; i < 5; ++i)
        if (!effects[i].active) {
            effects[i].active = 1;
            effects[i].x = x;
            effects[i].y = y;
            effects[i].timer = 12;
            return;
        }
}
uint8_t game_object_active(const RoomObject* o)
{
    uint8_t k;
    if (o->kind == 5)
        return g.floor == 7;
    if (o->kind < 1 || o->kind > 4)
        return 0;
    k = o->kind - 1;
    if (g.sites[k].room != g.room || g.sites[k].x != o->x || g.sites[k].y != o->y)
        return 0;
    return o->kind == 1 ? !g.key_taken
        : o->kind == 2  ? !g.pile_taken
        : o->kind == 3  ? !g.picture_taken
                        : 1;
}
static uint8_t structure(uint8_t x, uint8_t y)
{
    Site* p = g.sites + 3;
    if (g.room != p->room)
        return 0;
    if (x == p->x && (y + 1 == p->y || y + 2 == p->y))
        return 1;
    return (x + 1 == p->x || x == p->x + 1) && (y == p->y || y == p->y + 1);
}
static const uint8_t bit_mask[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };
static uint8_t wall(uint8_t x, uint8_t y)
{
    return solid[(uint16_t)y * 5 + (x >> 3)] & bit_mask[x & 7];
}
uint8_t game_blocked(uint8_t x, uint8_t y)
{
    return x >= 40 || y >= 25 || wall(x, y) || structure(x, y);
}
#ifdef __SDCC
typedef char enemy_query_layout[(sizeof(Enemy) == 9 && offsetof(Enemy, active) == 0
                                    && offsetof(Enemy, room) == 1 && offsetof(Enemy, x) == 2
                                    && offsetof(Enemy, y) == 3 && offsetof(Enemy, visible) == 8
                                    && offsetof(Game, room) == 88)
        ? 1
        : -1];
/* Profile: two full scans cost 10,010 cycles on the failed floor-1 frame.
   Same first-match query and record order; no state writes or random calls.
   A=x, L=y, return A=index/255. All record offsets are guarded above. */
static uint8_t enemy_at(uint8_t x, uint8_t y) __naked
{
    (void)x;
    (void)y;
    __asm__("ld d,l\n ld e,a\n ld hl,#_enemies+3\n ld b,#25\n ld c,#0\n"
            "00871$:\n ld a,(hl)\n cp d\n jr nz,00875$\n"
            "dec hl\n ld a,(hl)\n cp e\n jr nz,00874$\n"
            "dec hl\n ld a,(_g+88)\n cp (hl)\n jr nz,00873$\n"
            "dec hl\n ld a,(hl)\n or a\n jr z,00872$\n"
            "push hl\n ld a,l\n add a,#8\n ld l,a\n jr nc,00876$\n inc h\n"
            "00876$:\n ld a,(hl)\n pop hl\n or a\n jr z,00872$\n ld a,c\n ret\n"
            "00872$:\n inc hl\n"
            "00873$:\n inc hl\n"
            "00874$:\n inc hl\n"
            "00875$:\n ld a,l\n add a,#9\n ld l,a\n jr nc,00877$\n inc h\n"
            "00877$:\n inc c\n djnz 00871$\n ld a,#255\n ret\n");
}
#else
static uint8_t enemy_at(uint8_t x, uint8_t y)
{
    uint8_t i;
    Enemy* e = enemies;
    for (i = 0; i < MAX_ENEMIES; ++i, ++e)
        if (e->y == y && e->x == x && e->active && e->visible && e->room == g.room)
            return i;
    return 255;
}
#endif
static uint8_t duplicate(uint8_t room, uint8_t x, uint8_t y, uint8_t except)
{
    uint8_t i;
    for (i = 0; i < MAX_ENEMIES; ++i)
        if (i != except && enemies[i].active && enemies[i].room == room && enemies[i].x == x
            && enemies[i].y == y)
            return 1;
    return 0;
}
/* SDCC's packed Enemy and register ABI: A=slot,L=room,stack=record.
   Same two random8 calls and exact remainders; bounded byte arithmetic avoids
   signed division and repeated pointer spills. C remains the host reference. */
#ifdef __SDCC
static void create_enemy(uint8_t i, uint8_t room, const RoomObject* o) __naked
{
    __asm__(
        "push ix\n"
        "ld c,l\n"
        "ld hl,#4\n"
        "add hl,sp\n"
        "ld e,(hl)\n"
        "inc hl\n"
        "ld d,(hl)\n"
        "push de\n"
        "ld l,a\n"
        "ld h,#0\n"
        "ld e,a\n"
        "ld d,h\n"
        "add hl,hl\n"
        "add hl,hl\n"
        "add hl,hl\n"
        "add hl,de\n"
        "ld de,#_enemies\n"
        "add hl,de\n"
        "push hl\n"
        "pop ix\n"
        "pop hl\n"
        "ld 0(ix),#1\n"
        "ld 1(ix),c\n"
        "inc hl\n"
        "ld a,(hl)\n"
        "ld 2(ix),a\n"
        "inc hl\n"
        "ld a,(hl)\n"
        "ld 3(ix),a\n"
        "inc hl\n"
        "ld a,(hl)\n"
        "and #7\n"
        "ld 4(ix),a\n"
        "call _random8\n"
        "ld b,a\n"
        "ld e,4(ix)\n"
        "ld d,#0\n"
        "ld hl,#_frame_counts\n"
        "add hl,de\n"
        "ld c,(hl)\n"
        "ld a,c\n"
        "cp #3\n"
        "jr z,00781$\n"
        "cp #6\n"
        "jr z,00781$\n"
        "dec c\n"
        "ld a,b\n"
        "and c\n"
        "jr 00784$\n"
        "00781$:\n"
        "ld a,b\n"
        "and #15\n"
        "ld e,a\n"
        "ld a,b\n"
        "rrca\n"
        "rrca\n"
        "rrca\n"
        "rrca\n"
        "and #15\n"
        "add a,e\n"
        "00782$:\n"
        "sub #3\n"
        "jr nc,00782$\n"
        "add a,#3\n"
        "ld d,a\n"
        "ld a,c\n"
        "cp #6\n"
        "ld a,d\n"
        "jr nz,00784$\n"
        "xor b\n"
        "and #1\n"
        "ld a,d\n"
        "jr z,00784$\n"
        "add a,#3\n"
        "00784$:\n"
        "ld 5(ix),a\n"
        "call _random8\n"
        "ld b,a\n"
        "and #240\n"
        "rrca\n"
        "rrca\n"
        "ld c,a\n"
        "ld a,b\n"
        "and #15\n"
        "add a,c\n"
        "00785$:\n"
        "sub #12\n"
        "jr nc,00785$\n"
        "add a,#12\n"
        "ld 6(ix),a\n"
        "ld c,#0\n"
        "ld a,(_g+89)\n"
        "cp 2(ix)\n"
        "jr z,00788$\n"
        "jr c,00787$\n"
        "ld c,#8\n"
        "jr 00788$\n"
        "00787$:\n"
        "ld c,#4\n"
        "00788$:\n"
        "ld d,#0\n"
        "ld a,(_g+90)\n"
        "inc a\n"
        "cp 3(ix)\n"
        "jr z,00791$\n"
        "jr c,00790$\n"
        "ld d,#2\n"
        "jr 00791$\n"
        "00790$:\n"
        "ld d,#1\n"
        "00791$:\n"
        "ld a,c\n"
        "or d\n"
        "ld 7(ix),a\n"
        "ld 8(ix),#0\n"
        "pop ix\n"
        "pop hl\n"
        "pop bc\n"
        "jp (hl)\n"
    );
}
#else
static void create_enemy(uint8_t i, uint8_t room, const RoomObject* o)
{
    Enemy* e = enemies + i;
    e->active = 1;
    e->room = room;
    e->x = o->x;
    e->y = o->y;
    e->type = o->type & 7;
    e->anim = random8() % frame_counts[e->type];
    {
        uint8_t cursor = random8();
        while (cursor >= 12)
            cursor -= 12;
        e->cursor = cursor;
    }
    e->dir = aim_at(e->x, e->y);
    e->visible = 0;
}
#endif
/* Placement records do not change until setup_floor reloads floor_rooms.
   Cache their first four kind-0 records, preserving world_find order, and the
   aliases that share a cell. Occupancy is rebuilt from live enemies on every
   population pass; no RNG calls or placement attempts are added or removed. */
static RoomObject* population_sites[9][4];
static uint8_t population_aliases[9][4], population_used[9];
#ifdef __SDCC
typedef char population_table_contract[(sizeof(population_sites) == 72
    && sizeof(population_aliases) == 36 && sizeof(population_used) == 9) ? 1 : -1];
/* Same ordered first-four records and unordered coordinate pairs as C below.
   The measured C preparation spills cost6.35ms; bounded packed-field strides
   avoid that cost without changing RNG or placement order. IRQs remain on. */
typedef char assert_cache_layout[(sizeof(FloorRoom) == 65 && sizeof(RoomObject) == 4
    && offsetof(FloorRoom, objects) == 1 && offsetof(RoomObject, kind) == 0
    && offsetof(RoomObject, x) == 1 && offsetof(RoomObject, y) == 2
    && sizeof(RoomObject*) == 2) ? 1 : -1];
typedef char assert_population_game_layout[(offsetof(Game, room) == 88
    && offsetof(Game, x) == 89 && offsetof(Game, y) == 90
    && offsetof(Game, previous_room) == 135 && sizeof(Enemy) == 9
    && offsetof(Enemy, active) == 0 && offsetof(Enemy, room) == 1
    && offsetof(Enemy, x) == 2 && offsetof(Enemy, y) == 3
    && offsetof(Enemy, type) == 4 && offsetof(Enemy, anim) == 5
    && offsetof(Enemy, cursor) == 6 && offsetof(Enemy, dir) == 7
    && offsetof(Enemy, visible) == 8) ? 1 : -1];
static uint16_t cache_row_sites, cache_row_aliases;
static uint8_t cache_remaining, cache_bit, cache_earlier, cache_x, cache_y;
static void population_cache(void) __naked
{
    __asm__(
        "push ix\n"
        "push iy\n"
        "ld hl,#_population_sites\n"
        "ld de,#_population_sites+1\n"
        "ld bc,#71\n"
        "ld (hl),#0\n"
        "ldir\n"
        "ld hl,#_population_aliases\n"
        "ld de,#_population_aliases+1\n"
        "ld bc,#35\n"
        "ld (hl),#0\n"
        "ldir\n"
        "ld iy,#_floor_rooms\n"
        "ld ix,#_population_sites\n"
        "ld b,#9\n"
        "00701$:\n"
        "push bc\n"
        "ld c,0(iy)\n"
        "push iy\n"
        "pop hl\n"
        "inc hl\n"
        "ld b,#4\n"
        "00702$:\n"
        "ld a,c\n"
        "or a\n"
        "jr z,00704$\n"
        "ld a,(hl)\n"
        "or a\n"
        "jr nz,00703$\n"
        "ld 0(ix),l\n"
        "ld 1(ix),h\n"
        "inc ix\n"
        "inc ix\n"
        "dec b\n"
        "jr z,00704$\n"
        "00703$:\n"
        "ld de,#4\n"
        "add hl,de\n"
        "dec c\n"
        "jr 00702$\n"
        "00704$:\n"
        "ld a,b\n"
        "add a,a\n"
        "ld e,a\n"
        "ld d,#0\n"
        "add ix,de\n"
        "ld de,#65\n"
        "add iy,de\n"
        "pop bc\n"
        "djnz 00701$\n"
        "ld iy,#_population_sites\n"
        "ld ix,#_population_aliases\n"
        "ld b,#9\n"
        "00705$:\n"
        "push bc\n"
        "push iy\n"
        "pop hl\n"
        "ld (_cache_row_sites),hl\n"
        "push ix\n"
        "pop hl\n"
        "ld (_cache_row_aliases),hl\n"
        "ld a,#4\n"
        "ld (_cache_remaining),a\n"
        "ld a,#1\n"
        "ld (_cache_bit),a\n"
        "xor a\n"
        "ld (_cache_earlier),a\n"
        "00706$:\n"
        "ld 0(ix),#0\n"
        "ld l,0(iy)\n"
        "ld h,1(iy)\n"
        "ld a,h\n"
        "or l\n"
        "jr z,00709$\n"
        "inc hl\n"
        "ld a,(hl)\n"
        "ld (_cache_x),a\n"
        "inc hl\n"
        "ld a,(hl)\n"
        "ld (_cache_y),a\n"
        "ld a,(_cache_bit)\n"
        "ld 0(ix),a\n"
        "ld a,(_cache_earlier)\n"
        "or a\n"
        "jr z,00709$\n"
        "ld b,a\n"
        "ld c,#1\n"
        "ld hl,(_cache_row_sites)\n"
        "push iy\n"
        "ld iy,(_cache_row_aliases)\n"
        "00707$:\n"
        "ld e,(hl)\n"
        "inc hl\n"
        "ld d,(hl)\n"
        "inc hl\n"
        "push hl\n"
        "ex de,hl\n"
        "inc hl\n"
        "ld e,(hl)\n"
        "inc hl\n"
        "ld d,(hl)\n"
        "ld a,(_cache_x)\n"
        "cp e\n"
        "jr nz,00708$\n"
        "ld a,(_cache_y)\n"
        "cp d\n"
        "jr nz,00708$\n"
        "ld a,0(ix)\n"
        "or c\n"
        "ld 0(ix),a\n"
        "ld a,(_cache_bit)\n"
        "or 0(iy)\n"
        "ld 0(iy),a\n"
        "00708$:\n"
        "pop hl\n"
        "inc iy\n"
        "sla c\n"
        "djnz 00707$\n"
        "pop iy\n"
        "00709$:\n"
        "inc ix\n"
        "inc iy\n"
        "inc iy\n"
        "ld hl,#_cache_earlier\n"
        "inc (hl)\n"
        "ld hl,#_cache_bit\n"
        "sla (hl)\n"
        "ld hl,#_cache_remaining\n"
        "dec (hl)\n"
        "jp nz,00706$\n"
        "pop bc\n"
        "dec b\n"
        "jp nz,00705$\n"
        "pop iy\n"
        "pop ix\n"
        "ret\n"
    );
}
#else
static void population_cache(void)
{
    uint8_t room;
    FloorRoom* floor_room = floor_rooms;
    RoomObject** sites = population_sites[0];
    uint8_t* aliases = population_aliases[0];
    memset(population_sites, 0, sizeof(population_sites));
    memset(population_aliases, 0, sizeof(population_aliases));
    for (room = 0; room < 9; ++room, ++floor_room, sites += 4, aliases += 4) {
        RoomObject* object = floor_room->objects;
        RoomObject** next = sites;
        uint8_t* next_alias = aliases;
        uint8_t count = floor_room->count, slots = 4, bit = 1;
        for (; count && slots; --count, ++object)
            if (!object->kind) {
                RoomObject** earlier = sites;
                uint8_t* earlier_alias = aliases;
                uint8_t earlier_bit = 1, mask = bit;
                for (; earlier != next; ++earlier, ++earlier_alias, earlier_bit <<= 1)
                    if ((*earlier)->x == object->x && (*earlier)->y == object->y) {
                        *earlier_alias |= bit;
                        mask |= earlier_bit;
                    }
                *next++ = object;
                *next_alias++ = mask;
                bit <<= 1;
                --slots;
            }
    }
}
#endif
#ifdef __SDCC
/* These are scratch bytes, never gameplay state or trace fields. The packed
   layout offsets are the existing Game.room/x/y/previous_room fields. */
static uint8_t population_initial, population_offset, population_slot;
static uint8_t population_attempt, population_room, population_index;
static void population_occupancy(void) __naked
{
    __asm__(
        "push ix\n"
        "push iy\n"
        "ld hl,#_population_used\n"
        "ld de,#_population_used+1\n"
        "ld bc,#8\n"
        "ld (hl),#0\n"
        "ldir\n"
        "ld ix,#_enemies\n"
        "ld b,#25\n"
        "00751$:\n"
        "ld a,0(ix)\n"
        "or a\n"
        "jr z,00755$\n"
        "ld a,1(ix)\n"
        "add a,a\n"
        "add a,a\n"
        "add a,a\n"
        "ld l,a\n"
        "ld h,#0\n"
        "ld de,#_population_sites\n"
        "add hl,de\n"
        "push hl\n"
        "pop iy\n"
        "ld c,#4\n"
        "ld d,#1\n"
        "00752$:\n"
        "ld l,0(iy)\n"
        "ld h,1(iy)\n"
        "ld a,h\n"
        "or l\n"
        "jr z,00754$\n"
        "inc hl\n"
        "ld a,(hl)\n"
        "cp 2(ix)\n"
        "jr nz,00754$\n"
        "inc hl\n"
        "ld a,(hl)\n"
        "cp 3(ix)\n"
        "jr nz,00754$\n"
        "ld a,1(ix)\n"
        "add a,#<(_population_used)\n"
        "ld l,a\n"
        "ld a,#0\n"
        "adc a,#>(_population_used)\n"
        "ld h,a\n"
        "ld a,d\n"
        "or (hl)\n"
        "ld (hl),a\n"
        "00754$:\n"
        "inc iy\n"
        "inc iy\n"
        "sla d\n"
        "dec c\n"
        "jr nz,00752$\n"
        "00755$:\n"
        "ld de,#9\n"
        "add ix,de\n"
        "djnz 00751$\n"
        "pop iy\n"
        "pop ix\n"
        "ret\n"
    );
}
static void populate(uint8_t initial) __naked
{
    __asm__(
        "ld (_population_initial),a\n"
        "call _random8\n"
        "and #7\n"
        "ld (_population_offset),a\n"
        "ld a,(_population_initial)\n"
        "or a\n"
        "call nz,_population_cache\n"
        "call _population_occupancy\n"
        "xor a\n"
        "ld (_population_slot),a\n"
        "00761$:\n"
        "ld a,(_population_initial)\n"
        "or a\n"
        "jr nz,00762$\n"
        "ld a,(_population_slot)\n"
        "ld l,a\n"
        "ld h,#0\n"
        "ld e,a\n"
        "ld d,h\n"
        "add hl,hl\n"
        "add hl,hl\n"
        "add hl,hl\n"
        "add hl,de\n"
        "ld de,#_enemies\n"
        "add hl,de\n"
        "ld a,(hl)\n"
        "or a\n"
        "jp nz,00769$\n"
        "00762$:\n"
        "xor a\n"
        "ld (_population_attempt),a\n"
        "00763$:\n"
        "ld a,(_population_slot)\n"
        "ld b,a\n"
        "ld a,(_population_attempt)\n"
        "add a,b\n"
        "ld b,a\n"
        "ld a,(_population_offset)\n"
        "add a,b\n"
        "and #7\n"
        "cp #4\n"
        "jr c,00764$\n"
        "inc a\n"
        "00764$:\n"
        "ld (_population_room),a\n"
        "ld b,a\n"
        "ld a,(_population_initial)\n"
        "or a\n"
        "jr nz,00765$\n"
        "ld a,(_g+88)\n"
        "cp b\n"
        "jp z,00768$\n"
        "ld a,(_g+135)\n"
        "cp b\n"
        "jp z,00768$\n"
        "00765$:\n"
        "ld a,(_population_slot)\n"
        "srl a\n"
        "srl a\n"
        "srl a\n"
        "ld c,a\n"
        "ld a,(_population_attempt)\n"
        "srl a\n"
        "srl a\n"
        "srl a\n"
        "add a,c\n"
        "ld c,a\n"
        "ld a,(_population_offset)\n"
        "add a,c\n"
        "and #3\n"
        "ld c,a\n"
        "ld b,a\n"
        "ld a,#1\n"
        "inc b\n"
        "00766$:\n"
        "dec b\n"
        "jr z,00767$\n"
        "add a,a\n"
        "jr 00766$\n"
        "00767$:\n"
        "ld d,a\n"
        "ld a,(_population_room)\n"
        "ld e,a\n"
        "ld hl,#_population_used\n"
        "ld b,#0\n"
        "push bc\n"
        "ld c,a\n"
        "add hl,bc\n"
        "ld a,(hl)\n"
        "and d\n"
        "pop bc\n"
        "jp nz,00768$\n"
        "ld a,e\n"
        "add a,a\n"
        "add a,a\n"
        "add a,c\n"
        "ld (_population_index),a\n"
        "add a,a\n"
        "ld l,a\n"
        "ld h,#0\n"
        "ld de,#_population_sites\n"
        "add hl,de\n"
        "ld e,(hl)\n"
        "inc hl\n"
        "ld d,(hl)\n"
        "ld a,d\n"
        "or e\n"
        "jr z,00768$\n"
        "push de\n"
        "ld a,(_population_room)\n"
        "ld l,a\n"
        "ld a,(_population_slot)\n"
        "call _create_enemy\n"
        "ld a,(_population_index)\n"
        "ld l,a\n"
        "ld h,#0\n"
        "ld de,#_population_aliases\n"
        "add hl,de\n"
        "ld c,(hl)\n"
        "ld a,(_population_room)\n"
        "ld l,a\n"
        "ld h,#0\n"
        "ld de,#_population_used\n"
        "add hl,de\n"
        "ld a,(hl)\n"
        "or c\n"
        "ld (hl),a\n"
        "jr 00769$\n"
        "00768$:\n"
        "ld hl,#_population_attempt\n"
        "inc (hl)\n"
        "ld a,(hl)\n"
        "cp #32\n"
        "jp c,00763$\n"
        "00769$:\n"
        "ld hl,#_population_slot\n"
        "inc (hl)\n"
        "ld a,(hl)\n"
        "cp #25\n"
        "jp c,00761$\n"
        "ret\n"
    );
}
#else
static void populate(uint8_t initial)
{
    uint8_t i, attempt, offset = random8() & 7;
    Enemy* e = enemies;
    if (initial)
        population_cache();
    memset(population_used, 0, sizeof(population_used));
    for (i = 0; i < MAX_ENEMIES; ++i, ++e)
        if (e->active) {
            uint8_t n;
            for (n = 0; n < 4; ++n) {
                RoomObject* o = population_sites[e->room][n];
                if (o && o->x == e->x && o->y == e->y)
                    population_used[e->room] |= (uint8_t)1 << n;
            }
        }
    e = enemies;
    for (i = 0; i < MAX_ENEMIES; ++i, ++e)
        if (initial || !e->active) {
            for (attempt = 0; attempt < 32; ++attempt) {
                uint8_t r = (i + attempt + offset) & 7;
                uint8_t n = ((i >> 3) + (attempt >> 3) + offset) & 3;
                RoomObject* o;
                if (r >= 4)
                    ++r;
                if (!initial && (r == g.room || r == g.previous_room))
                    continue;
                o = population_sites[r][n];
                if (o && !(population_used[r] & ((uint8_t)1 << n))) {
                    create_enemy(i, r, o);
                    population_used[r] |= population_aliases[r][n];
                    break;
                }
            }
        }
}
#endif
static void select_sites(void)
{
    uint8_t k, r, j, used = 0;
    uint16_t mask = 0;
    for (k = 0; k < 4; ++k) {
        r = random8() % 9;
        for (j = 0; j < 9; ++j) {
            RoomObject* o = world_find(r, k + 1, 0);
            if (r != 4 && !(mask & ((uint16_t)1 << r)) && o) {
                g.sites[k].room = r;
                g.sites[k].x = o->x;
                g.sites[k].y = o->y;
                mask |= (uint16_t)1 << r;
                ++used;
                break;
            }
            r = (r + 1) % 9;
        }
    }
    if (used != 4)
        asset_error = 4;
    g.final_room = 255;
    if (g.floor == 7) {
        r = random8() % 9;
        for (j = 0; j < 9; ++j) {
            RoomObject* o = world_find(r, 6, 0);
            if (o) {
                g.final_room = r;
                g.final_x = o->x;
                g.final_y = o->y;
                break;
            }
            r = (r + 1) % 9;
        }
    }
}
static void activate(void)
{
    uint8_t i, count = 0;
    g.quota = 5 + g.repeat; /* 5 observed on floor 0; other bases unresolved. */
    for (i = 0; i < MAX_ENEMIES; ++i) {
        Enemy* e = enemies + i;
        e->visible = e->active && e->room == g.room && count < g.quota;
        if (e->visible)
            ++count;
    }
    g.pending = g.quota - count;
}
void game_enter(uint8_t room, uint8_t x, uint8_t y)
{
#ifdef EMBER_TEST
    test_room_begin(); /* A real reload also occurs on same-room respawn. */
#endif
    if (!doorway_enter) {
        input_clear();
        g.previous = 0;
    }
    g.previous_room = g.room;
    g.room = room;
    g.x = x;
    g.y = y;
    g.visited[g.floor] |= (uint16_t)1 << room;
    memset(shots, 0, sizeof(shots));
    memset(effects, 0, sizeof(effects));
    chest_cache_room = 255;
    asset_error = world_load(g.floor, room);
    if (asset_error) {
        g.mode = GAMEOVER;
        g.scene_dirty = 1;
        return;
    }
    if (g.repopulate >= 600)
        populate(0);
    activate();
    g.reinforce = g.floor == 2 ? 19 : 0;
    g.move_clock = moves[g.floor];
    g.anim_clock = animations[g.floor];
    g.cooldown = 0;
    g.final_active = g.floor == 7 && g.room == g.final_room && !g.piles;
    g.item_dirty = 0;
    g.scene_dirty = 1;
    g.hud_dirty = 1;
}
static uint8_t entry_occupied(uint8_t room, uint8_t x, uint8_t y)
{
    uint8_t i;
    for (i = 0; i < MAX_ENEMIES; ++i)
        if (enemies[i].active && enemies[i].room == room && enemies[i].x == x
            && (enemies[i].y == y || enemies[i].y == y + 1))
            return 1;
    return 0;
}
static void doorway(uint8_t room, uint8_t x, uint8_t y)
{
    uint8_t i, px = x, py = y;
    doorway_enter = 1;
    game_enter(room, x, y);
    doorway_enter = 0;
    if (asset_error)
        return;
    for (i = 0; i < 38; ++i) {
        if (!game_blocked(px, py) && !game_blocked(px, py + 1) && !entry_occupied(room, px, py)) {
            g.x = px;
            g.y = py;
            room_handoff = 1;
            sound_step();
            return;
        }
        if (x == 1)
            ++px;
        else if (x == 37)
            --px;
        else if (y == 1)
            ++py;
        else
            --py;
        if (px >= 39 || py >= 24)
            break;
    }
    /* An exceptional blocked lane uses a free interior cell. */
    for (py = 3; py < 22; ++py)
        for (px = 4; px < 36; ++px)
            if (!game_blocked(px, py) && !game_blocked(px, py + 1)
                && !entry_occupied(room, px, py)) {
                g.x = px;
                g.y = py;
                room_handoff = 1;
                sound_step();
                return;
            }
}
static void setup_floor(uint8_t floor)
{
#ifdef EMBER_TEST
    test_room_begin();
#endif
    g.floor = floor;
    g.key = 0;
    g.key_taken = 0;
    g.pile_taken = !!(g.pile_mask & (1 << floor));
    g.picture_taken = !!(g.picture_mask & (1 << floor));
    video_collection(VIDEO_COLLECTION_FLOOR);
    g.picture = g.pictures[floor];
    g.repopulate = 0;
    g.final_active = 0;
    g.final_clock = 0;
    asset_error = world_floor(floor);
    if (asset_error) {
        g.mode = GAMEOVER;
        g.scene_dirty = 1;
        return;
    }
    select_sites();
    if (asset_error) {
        g.mode = GAMEOVER;
        g.scene_dirty = 1;
        return;
    }
    memset(enemies, 0, sizeof(enemies));
    populate(1);
}
void game_init(void)
{
    chest_cache_room = 255;
    memset(&g, 0, sizeof(g));
    g.mode = TITLE;
    g.lives = 5;
    g.capacity = 1;
    g.seed = 0x6d2b;
    g.nextlife = 10000;
    g.scene_dirty = 1;
}
void game_new(uint8_t replay)
{
    uint8_t i;
    uint8_t repeat = g.repeat, charges = g.charges;
    if (replay && repeat < MAX_REPEAT)
        ++repeat;
    g.mode = PLAY;
    g.paused = 0;
    g.key = g.piles = 0;
    g.ammo = 75;
    g.capacity = 1;
    g.repeat = replay ? repeat : 0;
    g.charges = replay ? (charges < 9 ? charges + 1 : 9) : 2;
    g.aim = 0;
    g.face = 1;
    g.phase = g.irq_phase = 0;
    g.cooldown = 0;
    g.protection = 0;
    g.pile_mask = g.picture_mask = 0;
    g.feedback = g.feedback_time = 0;
    if (!replay) {
        g.score = 0;
        g.lives = 5;
        g.nextlife = 10000;
        memset(g.visited, 0, sizeof(g.visited));
        g.seed = (uint16_t)g.frames ^ 0x6d2b;
    }
    for (i = 0; i < 8; ++i)
        g.pictures[i] = random8() & 3;
    setup_floor(0);
    if (!asset_error)
        game_enter(4, 18, 13);
}
static void die(void)
{
    if (g.mode != PLAY || g.protection)
        return;
    input_clear();
    g.previous = 0;
    ++g.deaths;
    --g.lives;
    g.mode = DEAD;
    g.timer = 72;
    g.hud_dirty = 1;
    g.final_active = 0;
    memset(shots, 0, sizeof(shots));
    sound_event(4);
}
static void kill_enemy(uint8_t i)
{
    Enemy* e = enemies + i;
    e->active = e->visible = 0;
    ++g.kills;
    award(points[g.floor]);
    burst(e->x, e->y);
    sound_event(2);
}
static uint8_t shot_contact(Shot* s)
{
    uint8_t i;
    RoomObject* o = room_objects;
    if (game_blocked(s->x, s->y))
        return 1;
    if (s->owner == 1) {
        i = enemy_at(s->x, s->y);
        if (i != 255) {
            kill_enemy(i);
            return 1;
        }
    } else if (s->x == g.x && (s->y == g.y || s->y == g.y + 1)) {
        die();
        return 1;
    }
    for (i = 0; i < room_object_count; ++i, ++o) {
        if (s->y == o->y && o->kind >= 1 && o->kind <= 5 && o->kind != 4
            && (s->x == o->x || (o->kind == 5 && s->x == o->x + 1)) && game_object_active(o))
            return 1;
    }
    return 0;
}
static uint8_t spawn_shot(uint8_t friendly, uint8_t x, uint8_t y, int8_t dx, int8_t dy)
{
    uint8_t i, first = friendly ? 0 : 2, end = friendly ? g.capacity : 4;
    if (friendly) {
        if (!g.ammo)
            return 0;
        if (g.ammo < 64) {
            if (g.capacity != 1) {
                g.capacity = 1;
                g.hud_dirty = 1;
            }
            end = 1;
        }
        if (g.capacity == 2 && g.cooldown)
            return 0;
    }
    for (i = first; i < end; ++i)
        if (!shots[i].active) {
            Shot* s = shots + i;
            if (friendly && g.capacity == 2)
                g.cooldown = 6;
            if ((!dx && !dy) || x == 0 || x >= 39 || y == 0 || y >= 24 || game_blocked(x, y))
                return 0;
            s->x = x;
            s->y = y;
            s->dx = dx;
            s->dy = dy;
            s->owner = friendly ? 1 : 2;
            s->active = 1;
            if (friendly) {
                if (i == 0)
                    --g.ammo;
                ++g.shots_fired;
                g.hud_dirty = 1;
                sound_event(1);
            } else
                ++g.hostile_shots;
            if (shot_contact(s))
                s->active = 0;
            return 1;
        }
    return 0;
}
static void update_shots(void)
{
    uint8_t i;
    Shot* s = shots;
    for (i = 0; i < 4; ++i, ++s)
        if (s->active) {
            s->x += s->dx;
            s->y += s->dy;
            ++g.shot_steps;
            if (s->x == 0 || s->x >= 39 || s->y == 0 || s->y >= 24 || shot_contact(s))
                s->active = 0;
        }
}
static uint8_t chest_at(uint8_t x, uint8_t y, uint8_t interact)
{
    uint8_t i;
    RoomObject* o;
    if (g.floor != 7)
        return 0;
    /* Room object records are immutable while loaded. Retain their order and
       cache only the chest subset; repeated enemy queries avoid unrelated sites. */
    if (chest_cache_room != g.room) {
        chest_cache_count = 0;
        o = room_objects;
        for (i = 0; i < room_object_count; ++i, ++o)
            if (o->kind == 5)
                chest_cache[chest_cache_count++] = o;
        chest_cache_room = g.room;
    }
    for (i = 0; i < chest_cache_count; ++i) {
        o = chest_cache[i];
        if (y == o->y && (x == o->x || x == o->x + 1)) {
            if (interact && g.key && g.piles) {
                award((uint16_t)g.piles * 8);
                g.piles = 0;
                resupply();
                ++g.chests;
                feedback(6);
                sound_event(3);
            }
            return 1;
        }
    }
    return 0;
}
static void pickups(void)
{
    uint8_t i;
    for (i = 0; i < room_object_count; ++i) {
        RoomObject* o = room_objects + i;
        uint8_t outcome;
        if (o->kind < 1 || o->kind > 4 || g.x != o->x || (g.y != o->y && g.y + 1 != o->y))
            continue;
        if (!game_object_active(o))
            continue;
        if (o->kind == 4) {
            if (g.y == o->y) {
                input_clear();
                g.previous = 0;
                video_collection(VIDEO_COLLECTION_CLEAR);
                g.mode = TRANSPORT;
                g.destination = g.floor < 7 ? g.floor + 1 : g.floor;
                g.transport_frames = 0;
                g.hud_dirty = 1;
                sound_event(8);
            }
            continue;
        }
        if (o->kind == 1) {
            g.key = 1;
            g.key_taken = 1;
            award(100);
            feedback(1);
        } else if (o->kind == 2) {
            g.pile_taken = 1;
            g.pile_mask |= 1 << g.floor;
            if (g.piles < 99)
                ++g.piles;
            award(100);
            resupply();
            feedback(2);
        } else {
            g.picture_taken = 1;
            g.picture_mask |= 1 << g.floor;
            resupply();
            outcome = random8() % 3;
            if (outcome == 0) {
                award(100);
                feedback(3);
            } else if (outcome == 1) {
                g.protection = 240;
                feedback(4);
            } else {
                g.capacity = 2;
                feedback(5);
            }
        }
        video_collection(o->kind);
        g.picked_types |= 1 << (o->kind - 1);
        g.item_dirty |= 1 << (o->kind - 1);
        g.hud_dirty = 1;
        burst(o->x, o->y);
        sound_event(3);
    }
}
static void move_player(uint8_t held)
{
    int8_t dx, dy;
    uint8_t nx, ny, i;
    direction(held, &dx, &dy);
    if (!dx && !dy)
        return;
    nx = g.x + dx;
    ny = g.y + dy;
    if (nx == 0 && g.room % 3 && ny >= 10 && ny <= 13) {
        doorway(g.room - 1, 37, ny);
        return;
    }
    if (nx == 39 && g.room % 3 < 2 && ny >= 10 && ny <= 13) {
        doorway(g.room + 1, 1, ny);
        return;
    }
    if (ny == 0 && g.room >= 3 && nx >= 18 && nx <= 22) {
        doorway(g.room - 3, nx, 22);
        return;
    }
    if (ny == 23 && g.room < 6 && nx >= 18 && nx <= 22) {
        doorway(g.room + 3, nx, 1);
        return;
    }
    if (chest_at(nx, ny, 1) || chest_at(nx, ny + 1, 1))
        return;
    if (game_blocked(nx, ny) || game_blocked(nx, ny + 1))
        return;
    i = enemy_at(nx, ny);
    if (i == 255)
        i = enemy_at(nx, ny + 1);
    if (i != 255) {
        die();
        return;
    }
    g.x = nx;
    g.y = ny;
    sound_step();
    pickups();
}
static void reinforce(void)
{
    uint8_t dir, start, i, j, slot = 255, x = 0, y = 0, valid = 0;
    if (!g.pending)
        return;
    for (i = 0; i < MAX_ENEMIES; ++i)
        if (enemies[i].room != g.room || !enemies[i].active)
            slot = i;
    if (slot == 255)
        return;
    start = random8() & 3;
    for (i = 0; i < 4; ++i) {
        dir = (start + i) & 3;
        if (dir == 0 && g.room >= 3 && !wall(20, 0)) {
            x = 20;
            y = 1;
            valid = 1;
        }
        if (dir == 1 && g.room % 3 < 2 && !wall(39, 12)) {
            x = 38;
            y = 12;
            valid = 1;
        }
        if (dir == 2 && g.room < 6 && !wall(20, 24)) {
            x = 20;
            y = 23;
            valid = 1;
        }
        if (dir == 3 && g.room % 3 && !wall(0, 12)) {
            x = 1;
            y = 12;
            valid = 1;
        }
        if (valid && !game_blocked(x, y) && !duplicate(g.room, x, y, slot)
            && !(x == g.x && (y == g.y || y == g.y + 1))) {
            for (j = 0; j < 4; ++j)
                if (shots[j].active && shots[j].x == x && shots[j].y == y)
                    valid = 0;
            if (valid)
                break;
        }
        valid = 0;
    }
    if (valid) {
        RoomObject o;
        o.x = x;
        o.y = y;
        o.type = g.floor;
        create_enemy(slot, g.room, &o);
        enemies[slot].cursor = 128;
        enemies[slot].visible = 1;
        --g.pending;
    }
}
static void update_enemies(void)
{
    /* Main-loop only: fixed scratch avoids repeated Z80 stack spills. IRQs
       never call gameplay. Keep record order and service semantics unchanged. */
    static uint8_t i, command, fresh, shot, mask, nx, ny;
    static int8_t dx, dy;
    static Enemy work;
    static Enemy* record;
    for (i = 0, record = enemies; i < MAX_ENEMIES; ++i, ++record) {
        if (!record->active || !record->visible || record->room != g.room)
            continue;
        memcpy(&work, record, sizeof(work));
        dx = 0;
        dy = 0;
        fresh = work.cursor >= 128;
        if (g.floor == 2) {
            if (!(random8() & 31) || !work.dir)
                work.dir = aim_at(work.x, work.y);
            direction(work.dir, &dx, &dy);
        } else {
            command = fresh ? PURSUIT : patterns[g.floor][work.cursor];
            if (command < 8) {
                dx = vx[command];
                dy = vy[command];
            } else {
                if (command == PX || command == PURSUIT)
                    dx = sign(g.x, work.x);
                if (command == PY || command == PURSUIT)
                    dy = sign(g.y + 1, work.y);
            }
        }
        work.cursor = fresh || work.cursor == 11 ? 0 : work.cursor + 1;
        nx = work.x + dx;
        ny = work.y + dy;
        if ((nx == g.x && (ny == g.y || ny == g.y + 1))) {
            die();
        } else if (!game_blocked(nx, ny) && !chest_at(nx, ny, 0)) {
            uint8_t j, hit = 0;
            for (j = 0; j < 2; ++j)
                if (shots[j].active && shots[j].x == nx && shots[j].y == ny) {
                    shots[j].active = 0;
                    kill_enemy(i);
                    work.active = work.visible = 0;
                    hit = 1;
                    break;
                }
            if (!hit) {
                work.x = nx;
                work.y = ny;
                if (nx == 0 && work.room % 3) {
                    --work.room;
                    work.x = 38;
                    work.visible = 0;
                } else if (nx == 39 && work.room % 3 < 2) {
                    ++work.room;
                    work.x = 1;
                    work.visible = 0;
                } else if (ny == 0 && work.room >= 3) {
                    work.room -= 3;
                    work.y = 23;
                    work.visible = 0;
                } else if (ny == 24 && work.room < 6) {
                    work.room += 3;
                    work.y = 1;
                    work.visible = 0;
                }
            }
        } else if (g.floor == 2) {
            /* Correct both independent direction fields; do not randomize X for Y. */
            if (work.dir & (L | R))
                work.dir ^= L | R;
            if (work.dir & (U | D))
                work.dir ^= U | D;
        }
        shot = shooting[g.floor];
        if (work.active && work.visible && shot && !fresh && !(random8() & 3)) {
            /* Preserve the random draw even when both hostile slots are busy. A
               failed spawn has no side effects; skip its unused aiming calculation. */
            if (!shots[2].active || !shots[3].active) {
                mask = aim_at(work.x, work.y);
                direction(mask, &dx, &dy);
                if (shot == 1)
                    dy = 0;
                else if (shot == 2)
                    dx = 0;
                else if (dx)
                    dy = 0;
                spawn_shot(0, work.x + dx, work.y + dy, dx, dy);
            }
        }
        memcpy(record, &work, sizeof(work));
    }
    if (g.reinforce >= 32) {
        g.reinforce = 0;
        reinforce();
    }
    ++g.enemy_updates;
}
static void final_service(void)
{
    int8_t dx, dy;
    uint8_t nx, ny;
    g.final_active = g.floor == 7 && g.room == g.final_room && !g.piles;
    if (!g.final_active)
        return;
    if (++g.final_clock >= 4) {
        g.final_clock = 0;
        dx = sign(g.x, g.final_x);
        dy = sign(g.y, g.final_y);
        nx = g.final_x + dx;
        ny = g.final_y + dy;
        if (!game_blocked(nx, ny) && !game_blocked(nx, ny + 1)) {
            g.final_x = nx;
            g.final_y = ny;
        }
    }
    if (g.final_x == g.x && (g.final_y == g.y || g.final_y + 1 == g.y || g.final_y == g.y + 1)) {
        award(2000 + (uint16_t)(g.repeat < 4 ? g.repeat : 4) * 100);
        ++g.endings;
        video_collection(VIDEO_COLLECTION_CLEAR);
        g.mode = ENDING;
        g.timer = 180;
        g.final_active = 0;
        input_clear();
        g.scene_dirty = 1;
        sound_event(6);
    }
}
static void transport(uint8_t pressed)
{
    uint8_t dest, fallback = 0;
    Site* p = g.sites + 3;
    if (g.transport_frames < 255)
        ++g.transport_frames;
    if (g.key && g.floor < 7) {
        if ((pressed & (U | D)) == D) {
            g.destination = g.floor ? g.floor - 1 : 0;
            if (!g.floor)
                ++g.clamps;
        } else if (pressed & U)
            g.destination = g.floor + 1;
        if (pressed & FIRE) {
            g.mode = PLAY;
            g.y = p->y + 2;
            input_clear();
            g.hud_dirty = 1;
            return;
        }
        if (!(pressed & ENTER))
            return;
        dest = g.destination;
    } else {
        /* Eleven original service phases include unknown waits: a visible bounded
           45-display-frame transition is the documented duration approximation. */
        if (g.transport_frames < 45 && !(pressed & ENTER))
            return;
        if (!g.charges) {
            g.mode = PLAY;
            g.y = p->y + 1;
            feedback(9);
            input_clear();
            return;
        }
        --g.charges;
        dest = random8() & 3;
        ++g.fallbacks;
        fallback = 1;
    }
    if (dest == g.floor && !fallback) {
        g.mode = PLAY;
        g.x = p->x;
        g.y = p->y + 1;
        feedback(8);
        input_clear();
        return;
    }
    if (dest > g.floor)
        ++g.up;
    else if (dest < g.floor)
        ++g.down;
    g.last_floor_choice = dest;
    {
        uint8_t same = dest == g.floor;
        g.mode = PLAY;
        setup_floor(dest);
        feedback(same ? 8 : 10);
    }
    if (!asset_error) {
        p = g.sites + 3;
        game_enter(p->room, p->x, p->y + 1);
        sound_event(5);
    }
}
void game_tick(uint8_t held)
{
    uint8_t pressed = held & ~g.previous, i;
    int8_t dx, dy;
    g.previous = held;
    ++g.frames;
    if (g.mode == TITLE) {
        if (pressed & ENTER)
            game_new(0);
        return;
    }
    if (g.mode == ENDING) {
        if ((pressed & ENTER) || !--g.timer)
            game_new(1);
        return;
    }
    if (g.mode == GAMEOVER) {
        if (pressed & ENTER) {
            g.mode = TITLE;
            g.scene_dirty = 1;
        }
        return;
    }
    if (g.mode == DEAD) {
        if (!--g.timer) {
            if (!g.lives) {
                g.mode = GAMEOVER;
                g.scene_dirty = 1;
            } else {
                g.mode = PLAY;
                game_enter(4, 18, 13);
            }
        }
        return;
    }
    if (g.mode == TRANSPORT) {
        transport(pressed);
        g.hud_dirty = 1;
        return;
    }
    if (pressed & ENTER) {
        input_clear();
        g.previous = 0;
        g.paused = !g.paused;
        g.hud_dirty = 1;
        if (g.paused)
            sound_mute();
    }
    if (g.paused)
        return;
    if (g.feedback_time && !--g.feedback_time) {
        g.feedback = 0;
        g.hud_dirty = 1;
    }
    for (i = 0; i < 5; ++i)
        if (effects[i].active && !--effects[i].timer)
            effects[i].active = 0;
    g.irq_phase += FRAME_US;
    if (g.irq_phase >= IRQ_US) {
        g.irq_phase -= IRQ_US;
        ++g.irq_ticks;
        if (g.cooldown)
            --g.cooldown;
        if (g.move_clock)
            --g.move_clock;
        if (g.anim_clock)
            --g.anim_clock;
    }
    g.phase += FRAME_US * 2;
    if (g.phase < STEP_US)
        return;
    g.phase -= STEP_US;
    ++g.loops;
    update_shots();
    if (g.mode != PLAY)
        return;
    if (!(g.loops & 1)) {
        ++g.opportunities;
        if (g.protection) {
            --g.protection;
            if (!g.protection)
                g.hud_dirty = 1;
        }
        if (g.reinforce < 32)
            ++g.reinforce;
        if (g.repopulate < 600)
            ++g.repopulate;
        direction(held, &dx, &dy);
        if (dx || dy) {
            g.aim = (dx < 0 ? L : dx > 0 ? R : 0) | (dy < 0 ? U : dy > 0 ? D : 0);
            g.face = dx < 0 ? 3 : dx > 0 ? 1 : dy < 0 ? 0 : 2;
        }
        if (held & FIRE) {
            direction(g.aim, &dx, &dy);
            spawn_shot(1, g.x + dx, g.y + 1 + dy * (g.aim == U ? 2 : 1), dx, dy);
        } else
            move_player(held);
        if (g.mode != PLAY)
            return;
        final_service();
        if (g.mode != PLAY)
            return;
    }
    if (!g.move_clock) {
        update_enemies();
        g.move_clock = moves[g.floor];
        if (g.mode != PLAY)
            return;
    }
    if (!g.anim_clock) {
        Enemy* e = enemies;
        uint8_t count = frame_counts[g.floor];
        for (i = 0; i < MAX_ENEMIES; ++i, ++e)
            if (e->active && e->visible) {
                if (++e->anim >= count)
                    e->anim = 0;
            }
        g.anim_clock = animations[g.floor];
        ++g.anim_updates;
    }
}
void game_fixture(uint8_t stress)
{
#ifdef ZDUNGEON_TEST
    uint8_t i;
    static uint8_t fixture_ready;
    extern volatile uint8_t test_floor;
    if (stress == 2) {
        extern volatile uint8_t test_room, test_x, test_y, test_key, test_charges, test_site;
        extern volatile uint16_t test_seed;
        g.seed = test_seed; /* Fixture construction must not inherit title-frame RNG. */
        setup_floor(test_floor);
        if (test_site) {
            Site* p = g.sites + 3;
            game_enter(p->room, p->x + (test_site == 2 ? -1 : test_site == 3 ? 1 : 0), p->y + 2);
        } else
            game_enter(test_room, test_x, test_y);
        memset(enemies, 0, sizeof(enemies));
        g.pending = 0;
        g.reinforce = 0;
        g.protection = 0;
        g.key = test_key;
        g.charges = test_charges;
        g.seed = test_seed;
        return;
    }
    if (stress && !fixture_ready) {
        g.repeat = MAX_REPEAT;
        setup_floor(test_floor);
        game_enter(0, 18, 13);
        g.quota = 5 + MAX_REPEAT;
        g.pending = 0;
        for (i = 0; i < MAX_ENEMIES; ++i) {
            enemies[i].active = 1;
            enemies[i].visible = i < g.quota;
            enemies[i].room = i < g.quota ? g.room : 1;
            enemies[i].x = 5 + i;
            enemies[i].y = 8;
            enemies[i].type = g.floor;
        }
        fixture_ready = 1;
    }
    g.mode = PLAY;
    g.x = 18;
    g.y = 13;
    g.aim = R;
    g.face = 1;
    if (!stress) {
        memset(enemies, 0, sizeof(enemies));
        g.pending = 0;
        g.reinforce = 0;
    }
    if (stress) {
        g.capacity = 2;
        g.protection = 60000;
        g.ammo = 100 + (g.frames & 1);
        g.hud_dirty = 1;
        if (g.move_clock <= 1) {
            Enemy* e = enemies;
            for (i = 0; i < g.quota; ++i, ++e) {
                if (!e->active || !e->visible || e->room != g.room) {
                    e->active = e->visible = 1;
                    e->room = g.room;
                    e->x = 5 + i;
                    e->y = 8;
                    e->type = g.floor;
                }
            }
        }
        /* Exact original maximum-load records, copied in bulk. The fixture must
           neither kill its eight actors nor charge normal play for C reset loops. */
        {
            static const Shot full_shots[4] = { { 1, 10, 20, 1, 0, 1 }, { 1, 11, 20, 1, 0, 1 },
                { 1, 12, 20, 1, 0, 2 }, { 1, 13, 20, 1, 0, 2 } };
            static const Effect full_effects[5] = { { 1, 20, 15, 12 }, { 1, 21, 15, 12 },
                { 1, 22, 15, 12 }, { 1, 23, 15, 12 }, { 1, 24, 15, 12 } };
            memcpy(shots, full_shots, sizeof(shots));
            memcpy(effects, full_effects, sizeof(effects));
        }
    }
#else
    (void)stress;
#endif
}
