#pragma once
#include "game.h"
uint8_t world_load(uint8_t floor, uint8_t room);
uint8_t world_floor(uint8_t floor);
RoomObject* world_find(uint8_t room, uint8_t kind, uint8_t nth);
extern uint16_t room_load_max, floor_load_max;

uint8_t world_splash(void);
uint8_t world_splash_text(void);
