#include "game.h"
#include "input.h"
#include "sound.h"
#include "world.h"
#include "video.h"
#include "autotest.h"
volatile uint8_t test_mode = 5, capture_request, test_held, test_stop, test_floor = 7;
volatile uint8_t test_room, test_x = 18, test_y = 13, test_key, test_charges = 2, test_site;
volatile uint16_t test_seed = 1;
static uint8_t quiet_ready;
uint8_t test_input(void)
{
    if (test_mode == 2)
        return test_held;
    if (test_mode == 6)
        return input_poll();
    if (test_mode == 8 || test_mode == 9) {
        if (g.mode != TITLE && !quiet_ready) {
            game_fixture(2);
            quiet_ready = 1;
        }
        if (g.mode == TITLE && test_mode == 9)
            return ENTER;
        return test_mode == 9 ? test_held : input_poll();
    }
    if (g.mode == TITLE) {
        if (test_mode == 5 && g.frames < 120)
            return 0;
        return ENTER;
    }
    if (test_mode == 3) {
        game_fixture(1);
        /* Extra audio workload: keep shot and independent footstep synthesis active
           in every maximum-load floor. Fixture only; no gameplay-state mutation. */
        if (!(g.frames & 3)) {
            sound_event(1);
            sound_step();
        }
        return 0;
    }
    if (test_mode == 7) {
        if (!quiet_ready) {
            game_fixture(0);
            quiet_ready = 1;
        }
        return 0;
    }
    if (test_mode == 5) {
        /* UI-only presentation fixture. Natural completion is tested separately
           by mode 2 with input writes only; this must never count as a win. */
        if (g.frames == 320) {
            g.mode = ENDING;
            g.timer = 180;
            g.scene_dirty = 1;
        }
        return g.frames < 400 ? R : QUIT;
    }
    if (test_mode == 4) {
        if (g.mode == GAMEOVER || g.mode == ENDING)
            return g.previous & ENTER ? 0 : ENTER;
        if (g.mode == DEAD)
            return 0;
        if (g.mode == TRANSPORT)
            return g.key && g.floor < 7 ? (g.previous & ENTER ? 0 : ENTER | U) : 0;
        switch ((g.frames / 120) % 8) {
        case 0:
            return R;
        case 1:
            return R | FIRE;
        case 2:
            return D;
        case 3:
            return D | FIRE;
        case 4:
            return L;
        case 5:
            return L | FIRE;
        case 6:
            return U;
        default:
            return U | FIRE;
        }
    }
    return 0;
}
uint8_t test_done(void)
{
    return test_stop
        || (test_mode == 3       ? g.frames >= 2000
                : test_mode == 4 ? g.frames >= 107400UL
                : test_mode == 5 ? g.frames >= 400
                : test_mode == 7 ? g.frames >= 10001
                                 : 0);
}
