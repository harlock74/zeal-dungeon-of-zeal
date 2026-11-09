#pragma once
#include <stdint.h>
#define U 1
#define D 2
#define L 4
#define R 8
#define ENTER 16
#define QUIT 32
#define FIRE 64
#define TITLE 0
#define PLAY 1
#define DEAD 2
#define TRANSPORT 3
#define ENDING 4
#define GAMEOVER 5
#define FRAME_US 16768UL
#define STEP_US 75849UL
#define LOOP_US 37924UL
#define IRQ_US 20980UL
#define FLOOR_COUNT 8
#define ROOMS_PER_FLOOR 9
#define MAX_ENEMIES 25
#define MAX_SHOTS 4
#define SPRITES 40
#define MAX_REPEAT 3
/* The renderer's assembly embeds these clocks and record counts. Fail the
   build if a future edit changes only one side of that contract. */
typedef char zdungeon_tick_contract[(FRAME_US == 16768UL && STEP_US == 75849UL
    && IRQ_US == 20980UL && FRAME_US * 2 < STEP_US && FRAME_US < IRQ_US) ? 1 : -1];
typedef char zdungeon_record_contract[(FLOOR_COUNT == 8 && ROOMS_PER_FLOOR == 9
    && MAX_ENEMIES == 25 && MAX_SHOTS == 4 && SPRITES == 40) ? 1 : -1];
/* New structures, not original record bytes. Room association survives exits. */
typedef struct {
    uint8_t active, room, x, y, type, anim, cursor, dir, visible;
} Enemy;
typedef struct {
    uint8_t active, x, y;
    int8_t dx, dy;
    uint8_t owner;
} Shot;
typedef struct {
    uint8_t active, x, y, timer;
} Effect;
typedef struct {
    uint8_t kind, x, y, type;
} RoomObject;
typedef struct {
    uint8_t count;
    RoomObject objects[16];
} FloorRoom;
typedef struct {
    uint8_t room, x, y;
} Site;
/* The turn-owned fields are explicitly saveable for future alternating play.
   This release is single-player and allocates only one live Game. */
typedef struct {
    uint32_t score, nextlife;
    uint16_t visited[8];
    uint8_t lives, floor, repeat, ammo, key, piles, capacity, charges, pile_mask, picture_mask,
        pictures[8];
} PlayerSave;
typedef struct {
    uint32_t frames, score, high, nextlife, phase, irq_phase, overruns;
    uint32_t loops, opportunities, irq_ticks, enemy_updates, anim_updates, shot_steps;
    uint16_t protection, reinforce, repopulate, visited[8], deaths, kills, shots_fired,
        hostile_shots, seed, chests;
    uint8_t mode, floor, room, x, y, face, aim, lives, ammo, repeat, paused, previous;
    uint8_t timer, cooldown, destination, quota, pending, scene_dirty, hud_dirty, item_dirty;
    uint8_t picked_types, up, down, endings, extras, sound_events, max_sprites;
    uint8_t key, piles, capacity, charges, picture, feedback, feedback_time, final_active, final_x,
        final_y, final_room;
    uint8_t pile_mask, picture_mask, pictures[8], chest_mask, previous_room, move_clock, anim_clock,
        final_clock;
    uint8_t key_taken, picture_taken, pile_taken, transport_frames, last_floor_choice, fallbacks,
        clamps;
    Site sites[4];
} Game;
extern Game g;
extern uint8_t room_handoff;
extern Enemy enemies[MAX_ENEMIES];
extern Shot shots[MAX_SHOTS];
extern Effect effects[5];
extern uint8_t solid[125], room_tiles[1200];
extern FloorRoom* current_room;
extern FloorRoom floor_rooms[9];
uint8_t game_object_active(const RoomObject* o);
void game_init(void);
void game_tick(uint8_t held);
void game_new(uint8_t replay);
void game_enter(uint8_t room, uint8_t x, uint8_t y);
void game_fixture(uint8_t stress);
uint8_t game_blocked(uint8_t x, uint8_t y);
