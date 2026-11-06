#ifndef ROOM_TIMING_H
#define ROOM_TIMING_H
#ifdef EMBER_TEST
#include <stdint.h>
extern uint16_t late_room_entry, late_other, max_room_entry_us;
extern uint8_t max_late_per_entry, room_iteration_flags;
void test_room_begin(void);
#endif
#endif
