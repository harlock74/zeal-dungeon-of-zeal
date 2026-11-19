#pragma once
#include "game.h"
void input_begin(void);
uint8_t input_poll(void);
void input_end(void);
void input_clear(void);
void input_error_acknowledge(void);

#ifdef ZDUNGEON_TEST
/* Test-build-only raw device inputs; the normal executable has no overrides. */
extern volatile uint8_t input_test_override, input_test_count, input_test_bytes[8], input_test_result;
extern volatile uint16_t input_test_pad;
extern const uint8_t input_test_codes[10];
extern const uint16_t input_test_buttons[12];
#endif
