#include <stdint.h>
#include <string.h>
#include <core.h>
#include <zos_sys.h>
#include <zos_vfs.h>
#include <zvb_gfx.h>
#include "game.h"
#include "input.h"
#include "sound.h"
#include "world.h"
#include "video.h"
#include "main.h"
#include "autotest.h"
#include "room_timing.h"
volatile uint8_t telemetry[128];
volatile uint8_t window_state[12];
static void window_export(void)
{
    window_state[0] = g.frames;
    window_state[1] = g.frames >> 8;
    window_state[2] = g.frames >> 16;
    window_state[3] = g.frames >> 24;
    window_state[4] = g.mode;
    window_state[5] = g.floor;
    window_state[6] = g.room;
    window_state[7] = g.x;
    window_state[8] = g.y;
    window_state[9] = g.lives;
    window_state[10] = g.paused;
    window_state[11] = g.previous;
}
static uint16_t stack_top, stack_low;
void start_checkpoint(void)
{
    __asm__("nop");
}
void active_checkpoint(void)
{
    __asm__("nop");
}
void event_checkpoint(void)
{
    __asm__("nop");
}
void progress_checkpoint(void)
{
    __asm__("nop");
}
void finish_checkpoint(void)
{
    __asm__("nop");
}
void stack_checkpoint(void)
{
    __asm__("nop");
}
void exit_checkpoint(void)
{
    __asm__("nop");
}
#ifdef ZDUNGEON_TEST
/* Test-only 100-us stopwatch in the existing emulator timer bank. */
static __sfr __at(0xa0) clock_ctrl;
static __sfr __at(0xa1) clock_div_l;
static __sfr __at(0xa2) clock_div_h;
static __sfr __at(0xa3) clock_reload_l;
static __sfr __at(0xa4) clock_reload_h;
static __sfr __at(0xa5) clock_counter_l;
static __sfr __at(0xa6) clock_counter_h;
static uint8_t clock_available, late_iteration;
static uint16_t clock_previous, clock_max, scene_started, scene_max;
#ifdef EMBER_TEST
uint16_t late_room_entry, late_other, max_room_entry_us;
uint8_t max_late_per_entry, room_iteration_flags;
static uint8_t room_entry_active, room_entry_lates, room_entry_started, room_screen_transition;
static uint16_t room_iteration_started;
void test_room_begin(void)
{
    if (!room_entry_started) {
        room_entry_started = 1;
        room_entry_active = 1;
        room_entry_lates = 0;
    }
}
#endif
static uint16_t clock_read(void)
{
    uint8_t bank = zvb_config_dev_idx, lo, hi;
    __asm__("di");
    zvb_map_peripheral(6);
    lo = clock_counter_l;
    hi = clock_counter_h;
    zvb_config_dev_idx = bank;
    __asm__("ei");
    return ((uint16_t)hi << 8) | lo;
}
static void clock_begin(void)
{
    uint8_t bank = zvb_config_dev_idx;
    __asm__("di");
    zvb_map_peripheral(6);
    clock_ctrl = 0;
    clock_div_l = 0x87;
    clock_div_h = 0x13;
    clock_reload_l = 1;
    clock_reload_h = 0;
    clock_counter_l = 0;
    clock_counter_h = 0;
    clock_ctrl = 0xc0;
    clock_available = clock_ctrl == 0xc0;
    zvb_config_dev_idx = bank;
    __asm__("ei");
    clock_previous = clock_read();
}
static void clock_check(void)
{
    uint16_t now = clock_read(), dt = now - clock_previous;
    clock_previous = now;
    if (dt > clock_max)
        clock_max = dt;
    if (dt > 190 && clock_available) {
        ++g.overruns;
#ifdef EMBER_TEST
        room_iteration_flags |= 4;
#endif
        if (late_iteration < 255)
            ++late_iteration;
    }
}
#ifdef EMBER_TEST
static void room_iteration_end(void)
{
    uint16_t ticks, elapsed;
    ticks = clock_read() - room_iteration_started;
    elapsed = ticks > 655 ? 65535 : ticks * 100;
    if (room_entry_active) {
        if (elapsed > max_room_entry_us)
            max_room_entry_us = elapsed;
        /* Scene drawing resets the legacy stopwatch baseline. Include the
           full measured iteration deadline so that reset cannot hide it. */
        if (room_iteration_flags || ticks > 190) {
            ++late_room_entry;
            if (room_entry_lates < 255)
                ++room_entry_lates;
            if (room_entry_lates > max_late_per_entry)
                max_late_per_entry = room_entry_lates;
        } else {
            /* The first on-budget iteration ends this entry's carry-over. */
            room_entry_active = 0;
        }
    } else if (!room_screen_transition && (room_iteration_flags || ticks > 190)) {
        ++late_other;
    }
}
#endif
uint16_t test_clock(void)
{
    return clock_available ? clock_read() : 0;
}
static void clock_end(void)
{
    uint8_t bank = zvb_config_dev_idx;
    __asm__("di");
    zvb_map_peripheral(6);
    clock_ctrl = 0;
    zvb_config_dev_idx = bank;
    __asm__("ei");
}
static __sfr __at(0xf0) capture_page;
static const __sfr __banked __at(0x00f0) capture_page_read;
void vram_checkpoint(void)
{
    __asm__("nop");
}
void tiles_checkpoint(void)
{
    __asm__("nop");
}
void tiles1_checkpoint(void)
{
    __asm__("nop");
}
void tiles2_checkpoint(void)
{
    __asm__("nop");
}
void tiles3_checkpoint(void)
{
    __asm__("nop");
}
static void capture(void)
{
    uint8_t saved, kind = capture_request;
    if (!kind)
        return;
    capture_request = 0;
    saved = capture_page_read;
    __asm__("di");
    capture_page = 64;
    vram_checkpoint();
    if (kind == 2) {
        capture_page = saved;
        __asm__("ei");
        return;
    }
    capture_page = 68;
    tiles_checkpoint();
    capture_page = 69;
    tiles1_checkpoint();
    capture_page = 70;
    tiles2_checkpoint();
    capture_page = 71;
    tiles3_checkpoint();
    capture_page = saved;
    __asm__("ei");
}
/* No calls or pushes while filling: preserve every live caller/return byte. */
static void paint_stack(void) __naked
{
    __asm__(
        "ld hl,#0\n add hl,sp\n ld (_stack_top),hl\n ex de,hl\n ld hl,#0xd000\n00001$:\n ld "
        "(hl),#0xa5\n inc hl\n ld a,h\n cp d\n jr nz,00001$\n ld a,l\n cp e\n jr nz,00001$\n ret");
}
static void measure_stack(void)
{
    volatile uint8_t* p = (volatile uint8_t*)0xd000;
    while ((uint16_t)p < stack_top && *p == 0xa5)
        ++p;
    stack_low = (uint16_t)p;
}
#endif
static void put16(uint16_t n, uint16_t v)
{
    telemetry[n] = v;
    telemetry[n + 1] = v >> 8;
}
static void put32(uint16_t n, uint32_t v)
{
    put16(n, v);
    put16(n + 2, v >> 16);
}
static void export(void)
{
    uint8_t i;
    telemetry[0] = 3;
    telemetry[1] = 7 /* telemetry format compatibility */;
    telemetry[2] = g.mode;
    telemetry[3] = g.floor;
    telemetry[4] = g.room;
    telemetry[5] = g.x;
    telemetry[6] = g.y;
    telemetry[7] = g.lives;
    telemetry[8] = g.ammo;
    telemetry[9] = g.key;
    telemetry[10] = g.repeat;
    put16(11, g.protection);
    telemetry[13] = g.paused;
#ifdef ZDUNGEON_TEST
    telemetry[14] = test_mode;
    telemetry[64] = clock_available;
    put16(66, clock_max);
#endif
    telemetry[15] = asset_error;
    put32(16, g.frames);
    put32(20, g.score);
    put16(24, g.deaths);
    put16(26, g.kills);
    put16(28, g.shots_fired);
    put16(30, g.hostile_shots);
    telemetry[32] = g.picked_types;
    telemetry[33] = g.up;
    telemetry[34] = g.down;
    telemetry[35] = g.endings;
    telemetry[36] = g.extras;
    telemetry[37] = 1;
    for (i = 0; i < 32; ++i)
        if (*((volatile uint8_t*)0xd000 + i) != 0xa5)
            telemetry[37] = 0;
    telemetry[38] = g.max_sprites;
    telemetry[39] = zvb_ctrl_status;
    put32(40, g.overruns);
    for (i = 0; i < 4; ++i)
        put16(44 + i * 2, g.visited[i]);
    put16(52, stack_low);
    put16(54, stack_low ? 0x10000UL - stack_low : 0);
    telemetry[60] = g.sound_events;
    telemetry[61] = g.destination;
#ifdef ZDUNGEON_TEST
    put16(68, scene_max);
#endif
}
/* All asset failures use the accepted acknowledgement and OS cleanup path.
   Successful play never enters here; no render loop retries failed I/O. */
static void asset_failure(uint8_t sound_ready)
{
    static const char message[] = "DUNGEON OF ZEAL: GAME FILES NOT FOUND.\nSEE README.\nPRESS "
                                  "RETURN TO RETURN TO ZEALOS.\n";
    uint16_t n = sizeof(message) - 1;
    telemetry[15] = asset_error;
    export();
    finish_checkpoint();
    if (sound_ready) {
#ifdef ZDUNGEON_TEST
        if (clock_available)
            clock_end();
#endif
        sound_mute();
    }
    video_exit();
    write(DEV_STDOUT, message, &n);
    input_error_acknowledge();
    input_end();
    exit(8);
}
int main(void)
{
    uint8_t held, old_destination, old_mode, old_floor, old_pause, old_lives;
    uint16_t progress = 0;
#ifdef ZDUNGEON_TEST
    paint_stack();
#endif
    input_begin();
    game_init();
    asset_error = video_init();
    if (asset_error)
        asset_failure(0);
    sound_init();
    video_scene();
    if (asset_error)
        asset_failure(1);
    gfx_wait_vblank(0);
    gfx_wait_end_vblank(0);
#ifdef ZDUNGEON_TEST
    clock_begin();
#endif
    window_export();
    export();
    start_checkpoint();
#ifdef ZDUNGEON_TEST
    capture();
#endif
    while (1) {
#ifdef EMBER_TEST
        room_iteration_started = clock_read();
        room_iteration_flags = room_entry_started = room_screen_transition = 0;
#endif
        old_destination = g.destination;
        old_mode = g.mode;
        old_floor = g.floor;
        old_pause = g.paused;
        old_lives = g.lives;
#ifdef ZDUNGEON_TEST
        held = test_input();
#else
        held = input_poll();
#endif
        if (held & QUIT)
            break;
        game_tick(held);
        if (asset_error)
            asset_failure(1);
        if (old_mode != g.mode || old_floor != g.floor || old_pause != g.paused
            || (old_lives != g.lives && !(room_handoff && g.lives > old_lives))) {
            input_clear();
            g.previous = 0;
        }
        sound_tick();
        window_export();
        if (g.scene_dirty) {
#ifdef EMBER_TEST
            room_screen_transition = g.mode == TITLE || g.mode == ENDING || g.mode == GAMEOVER;
#endif
#ifdef ZDUNGEON_TEST
            scene_started = clock_read();
#endif
            video_scene();
            if (asset_error)
                asset_failure(1);
#ifdef ZDUNGEON_TEST
            {
                uint16_t elapsed = clock_read() - scene_started;
                if (elapsed > scene_max)
                    scene_max = elapsed;
            }
#endif
            if (!room_handoff)
                export();
            event_checkpoint();
#ifdef ZDUNGEON_TEST
            capture();
#endif
            /* video_frame owns the single presentation wait pair on every
               path, including same-floor reloads and fixture construction. */
#ifdef ZDUNGEON_TEST
            if (g.frames == 1)
                active_checkpoint();
            if (!room_handoff)
                clock_previous = clock_read();
#endif
        }
        room_handoff = 0;
        video_frame();
#ifdef ZDUNGEON_TEST
        clock_check();
#ifdef EMBER_TEST
        room_iteration_end();
#endif
        if (g.mode == TRANSPORT && g.destination != old_destination) {
            export();
            event_checkpoint();
            capture();
        }
        if (++progress == 3600 || test_mode == 2 || test_mode == 6 || test_mode == 8
            || test_mode == 9 || (test_mode == 7 && g.frames < 600)) {
            progress = 0;
            progress_checkpoint();
            capture();
        }
        if (test_done()) {
            export();
            finish_checkpoint();
            measure_stack();
            export();
            stack_checkpoint();
            capture();
            break;
        }
#endif
    }
#ifdef ZDUNGEON_TEST
    clock_end();
#endif
    sound_mute();
    input_end();
    video_exit();
    telemetry[63] = 1;
    exit_checkpoint();
#ifdef ZDUNGEON_AUTOSTART_RESET
    __asm__("jp 0");
#endif
    exit(0);
    return 0;
}
