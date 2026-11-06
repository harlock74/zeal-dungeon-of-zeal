#pragma once
#include "game.h"
uint8_t video_init(void);
void video_scene(void);
void video_frame(void);
void video_exit(void);
extern uint8_t asset_error;
uint16_t video_upload_stack_used(void);
uint8_t video_upload_guard(void);

/* Renderer-owned collection state: 0 clears, 1–3 mark a pickup, 4 restores
   saved floor marks. These notifications never write gameplay state. */
#define VIDEO_COLLECTION_CLEAR 0
#define VIDEO_COLLECTION_FLOOR 4
void video_collection(uint8_t command);
