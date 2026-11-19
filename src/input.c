#include <stdint.h>
#include <core.h>
#include <zos_sys.h>
#include <zos_vfs.h>
#include <zos_keyboard.h>
#include <zgdk/input.h>
#include "game.h"
#include "input.h"
#include "sound.h"
#include "world.h"
#include "video.h"
static uint8_t key_backing[127], *key_buffer, keys, release_prefix;
static uint8_t blocked, pad_active;
static uint16_t pad_suppressed = 0x0fff;
/* Small diagnostic history, read-only to the owned host test harness. */
volatile uint16_t input_trace_seq;
volatile uint8_t input_trace[128];
#ifdef ZDUNGEON_TEST
volatile uint8_t input_test_override, input_test_count, input_test_bytes[8], input_test_result;
volatile uint16_t input_test_pad;
const uint8_t input_test_codes[10] = { KB_UP_ARROW, KB_DOWN_ARROW, KB_LEFT_ARROW,
    KB_RIGHT_ARROW, KB_KEY_SPACE, KB_KEY_Z, KB_KEY_ENTER, KB_ESC, KB_RELEASED, KB_KEY_A };
const uint16_t input_test_buttons[12] = { BUTTON_UP, BUTTON_DOWN, BUTTON_LEFT,
    BUTTON_RIGHT, BUTTON_B, BUTTON_START, BUTTON_SELECT, BUTTON_A, BUTTON_X,
    BUTTON_Y, BUTTON_L, BUTTON_R };
#endif
void input_begin(void)
{
    key_buffer = (uint8_t*)(((uintptr_t)key_backing + 63) & ~(uintptr_t)63);
    if (input_init(1) != ERR_SUCCESS)
        exit(1);
}
void input_clear(void)
{
    /* Consume releases normally; typematic makes cannot revive cleared keys. */
    blocked |= keys;
    keys = 0;
    pad_suppressed = 0x0fff;
}
void input_end(void)
{
    input_flush();
    ioctl(DEV_STDIN, KB_CMD_SET_MODE, (void*)(KB_MODE_COOKED | KB_READ_BLOCK));
}
uint8_t input_poll(void)
{
    uint8_t i, bit;
    uint16_t pad;
    for (;;) {
        uint16_t count = 64;
#ifdef ZDUNGEON_TEST
        if (input_test_override) {
            count = input_test_count < 8 ? input_test_count : 8;
            for (i = 0; i < count; ++i)
                key_buffer[i] = input_test_bytes[i];
            input_test_count = 0;
        } else
#endif
        if (read(DEV_STDIN, key_buffer, &count) != ERR_SUCCESS) {
            input_clear();
            release_prefix = 0;
            break;
        }
        for (i = 0; i < count; ++i) {
            uint8_t code = key_buffer[i];
            input_trace[input_trace_seq & 127] = code;
            ++input_trace_seq;
            bit = 0;
            if (code == KB_RELEASED) {
                release_prefix = 1;
                continue;
            }
            switch (code) {
            case KB_UP_ARROW:
                bit = U;
                break;
            case KB_DOWN_ARROW:
                bit = D;
                break;
            case KB_LEFT_ARROW:
                bit = L;
                break;
            case KB_RIGHT_ARROW:
                bit = R;
                break;
            case KB_KEY_SPACE:
                bit = FIRE;
                break;
            case KB_KEY_Z:
                bit = 128;
                break;
            case KB_KEY_ENTER:
                bit = ENTER;
                break;
            case KB_ESC:
                bit = QUIT;
                break;
            }
            if (release_prefix) {
                keys &= ~bit;
                blocked &= ~bit;
            } else if (!(blocked & bit))
                keys |= bit;
            release_prefix = 0;
        }
        if (!count)
            break;
    }
    bit = keys & 127;
    if (keys & 128)
        bit |= FIRE;
#ifdef ZDUNGEON_TEST
    pad = input_test_override ? input_test_pad : controller_read();
#else
    pad = controller_read();
#endif
    if (!(pad & 0xf000)) {
        pad_suppressed &= pad;
        pad &= ~pad_suppressed;
        /* Ignore floating direction lines until a real mapped button is seen.
           Once selected, keep the pad active alongside keyboard input. */
        if (pad & (BUTTON_B | BUTTON_START | BUTTON_SELECT))
            pad_active = 1;
        if (!pad_active)
            return bit;
        if (pad & BUTTON_UP)
            bit |= U;
        if (pad & BUTTON_DOWN)
            bit |= D;
        if (pad & BUTTON_LEFT)
            bit |= L;
        if (pad & BUTTON_RIGHT)
            bit |= R;
        if (pad & BUTTON_B)
            bit |= FIRE;
        if (pad & BUTTON_START)
            bit |= ENTER;
        if (pad & BUTTON_SELECT)
            bit |= QUIT;
    } else
        pad_suppressed = 0x0fff;
    return bit;
}

/* Missing-assets path only; normal play never calls this helper. */
void input_error_acknowledge(void)
{
    uint16_t count;
    uint8_t i, code;
    /* About250ms at the target10MHz: two48,076-iteration26-T-state loops.
       Leave IRQs enabled and make no calls to the now-disabled video output. */
    __asm__("ld d,#2\n00981$:\n ld bc,#48076\n00982$:\n dec bc\n ld a,b\n or c\n jr nz,00982$\n dec d\n jr nz,00981$");
    ioctl(DEV_STDIN, KB_CMD_SET_MODE, (void*)(KB_MODE_RAW | KB_READ_NON_BLOCK));
    /* The launch Return's break has now arrived; discard all typeahead. */
    do {
        count = 64;
        if (read(DEV_STDIN, key_buffer, &count) != ERR_SUCCESS)
            count = 0;
    } while (count);
    for (;;) {
        count = 64;
        if (read(DEV_STDIN, key_buffer, &count) != ERR_SUCCESS)
            continue;
        for (i = 0; i < count; ++i) {
            code = key_buffer[i];
            input_trace[input_trace_seq & 127] = code;
            ++input_trace_seq;
            /* One physical Return may produce FE0A FE0A. A stale OS decoder prefix
               can turn its make into a release. After the delayed drain,
               accept Enter regardless of a preceding KB_RELEASED byte. */
            if (code == KB_KEY_ENTER) {
                key_buffer[0] = '\n';
                count = 1;
                write(DEV_STDOUT, key_buffer, &count);
                return;
            }
        }
    }
}
