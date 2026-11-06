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
static uint8_t blocked, keyboard_owner, pad_active;
static uint16_t pad_suppressed = 0x0fff;
/* Small diagnostic history, read-only to the owned host test harness. */
volatile uint16_t input_trace_seq;
volatile uint8_t input_trace[128];
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
        if (read(DEV_STDIN, key_buffer, &count) != ERR_SUCCESS) {
            input_clear();
            release_prefix = 0;
            pad_active = 0;
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
            if (bit)
                keyboard_owner = 1;
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
    pad = controller_read();
    if (!(pad & 0xf000)) {
        pad_suppressed &= pad;
        pad &= ~pad_suppressed;
        /* Explicit Start selects a pad. Floating/mouse lines cannot add arrows
           to keyboard movement; a keyboard key selects keyboard ownership. */
        if (pad & BUTTON_START) {
            pad_active = 1;
            keyboard_owner = 0;
        }
        if (!pad_active || keyboard_owner)
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
