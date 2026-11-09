#include <stdint.h>
#include <string.h>
#include <core.h>
#include <zos_sys.h>
#include <zos_vfs.h>
#include "game.h"
#include "input.h"
#include "sound.h"
#include "world.h"
#include "video.h"
uint8_t room_tiles[1200];
FloorRoom* current_room = floor_rooms;
static RoomObject title_objects[2];
#ifdef ZDUNGEON_TEST
extern uint16_t test_clock(void);
uint16_t room_load_max, floor_load_max;
#endif
static uint8_t header[8];
static char filename[] = "assets/rooms/f1_r0_0.ztr";
static uint8_t exact(zos_dev_t fd, void* target, uint16_t size)
{
    uint8_t* p = target;
    uint16_t n;
    while (size) {
        n = size;
        if (read(fd, p, &n) != ERR_SUCCESS || !n)
            return 1;
        p += n;
        size -= n;
    }
    return 0;
}
static uint8_t scene_read(zos_dev_t fd, uint8_t floor, uint8_t room)
{
    if (exact(fd, header, 8)
        || memcmp(header, "ZTR2", 4) || header[4] != 0
        || header[5] != floor || header[6] != room || header[7] != 0)
        return 1;
    return exact(fd, room_tiles, 1200);
}
uint8_t world_load(uint8_t floor, uint8_t room)
{
    zos_dev_t fd;
    uint8_t failed;
#ifdef ZDUNGEON_TEST
    uint16_t begin = test_clock(), elapsed;
#endif
    filename[14] = '1' + floor;
    filename[17] = '0' + room / 3;
    filename[19] = '0' + room % 3;
    fd = open(filename, O_RDONLY);
    if (fd < 0)
        return 1;
    failed = scene_read(fd, floor, room);
    close(fd);
    if (failed)
        return 2;
    current_room = floor_rooms + room;
#ifdef ZDUNGEON_TEST
    elapsed = test_clock() - begin;
    if (elapsed > room_load_max)
        room_load_max = elapsed;
#endif
    return 0;
}
/* Title reuses the tile buffer and retains its own two legacy value records.
   The two value records contain kind7/8, cell x/y and digit width. */
static zos_dev_t title_fd = -1;
uint8_t world_splash(void)
{
    title_fd = open("assets/splash.ztr", O_RDONLY);
    if (title_fd < 0)
        return 1;
    if (exact(title_fd, header, 8) || memcmp(header, "ZTR1", 4)
        || header[4] != 2 || header[5] != 255 || header[6] != 255 || header[7] != 2
        || exact(title_fd, room_tiles, 1200) || exact(title_fd, solid, 125)
        || exact(title_fd, title_objects, sizeof(title_objects))) {
        close(title_fd);
        title_fd = -1;
        return 2;
    }
    return 0;
}
uint8_t world_splash_text(void)
{
    uint8_t failed;
    if (title_fd < 0)
        return 1;
    failed = exact(title_fd, room_tiles, 1200);
    close(title_fd);
    title_fd = -1;
    return failed ? 2 : 0;
}
FloorRoom floor_rooms[9];
static uint8_t floor_error(uint8_t error)
{
    uint8_t i;
    for (i = 0; i < 9; ++i)
        floor_rooms[i].count = 0;
    return error;
}
uint8_t world_floor(uint8_t floor)
{
    static char path[] = "assets/rooms/f1.ztf";
    zos_dev_t fd;
    uint8_t i, failed;
#ifdef ZDUNGEON_TEST
    uint16_t begin = test_clock(), elapsed;
#endif
    path[14] = '1' + floor;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return floor_error(1);
    failed = exact(fd, header, 4);
    if (!failed && memcmp(header, "ZTF1", 4))
        failed = 1;
    if (!failed)
        failed = exact(fd, floor_rooms, sizeof(floor_rooms));
    close(fd);
    if (failed)
        return floor_error(2);
    for (i = 0; i < 9; ++i)
        if (floor_rooms[i].count > 16)
            return floor_error(3);
#ifdef ZDUNGEON_TEST
    elapsed = test_clock() - begin;
    if (elapsed > floor_load_max)
        floor_load_max = elapsed;
#endif
    return 0;
}
RoomObject* world_find(uint8_t room, uint8_t kind, uint8_t nth)
{
    FloorRoom* r = floor_rooms + room;
    RoomObject* object = r->objects;
    uint8_t count = r->count;
    for (; count; --count, ++object)
        if (object->kind == kind) {
            if (!nth)
                return object;
            --nth;
        }
    return 0;
}
