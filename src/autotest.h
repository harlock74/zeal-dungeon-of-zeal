#pragma once
#include "game.h"
extern volatile uint8_t test_mode, capture_request, test_held, test_stop, test_floor;
uint8_t test_input(void);
uint8_t test_done(void);
