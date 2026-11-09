#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <core.h>
#include <zos_sys.h>
#include <zos_vfs.h>
#include <zos_video.h>
#include <zvb_gfx.h>
#include <zvb_sprite.h>
#include "game.h"
#include "room_timing.h"
#include "input.h"
#include "sound.h"
#include "world.h"
#include "video.h"
#include "art_ids.h" /* maintained tile and animation names */
static gfx_context video;
static uint8_t video_ready;
static void prepare_sprites(void);
/* Full PLAY scenes already upload all actors while the screen is blank. */
static uint8_t scene_sprites_ready, blank_row[40], overlay_skip_header;
/* Editable title start line: row15, columns10–29,30 display frames per phase. */
static uint8_t title_start_line[20], title_flash_clock, title_start_visible;
static void clear_overlay(void) __naked
{
    /* Same IRQ-safe mapping/stack; unrolled zero stores reduce copy cost.
       A valid PLAY header and footer are kept; rows2–28 still clear. */
    __asm__(
        "ld (_upload_sp),sp\n"
        "ld sp,#_upload_stack+256\n"
        "ld bc,#0xc0f0\n"
        "in a,(c)\n"
        "ld (_upload_bank),a\n"
        "ld a,#64\n"
        "out (0xf3),a\n"
        "ld hl,#0xd000\n"
        "ld b,#30\n"
        "ld a,(_overlay_skip_header)\n"
        "or a\n"
        "jr z,00852$\n"
        "ld hl,#0xd0a0\n"
        "ld b,#27\n"
        "00852$:\n"
        "xor a\n"
        "00851$:\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld (hl),a\n"
        "inc hl\n"
        "ld de,#40\n"
        "add hl,de\n"
        "djnz 00851$\n"
        "ld a,(_upload_bank)\n"
        "out (0xf3),a\n"
        "ld sp,(_upload_sp)\n"
        "ret\n"
    );
}
gfx_sprite sprites[SPRITES];
static uint8_t backing[1023], *buffer;
uint8_t asset_error;
/* Two header rows, a transient pause span, and the bottom floor label. */
static uint8_t ui[4][40], ui_build, ui_dirty, ui_valid, ui_last[6];
static uint8_t ui_uploaded[4][40];
static uint32_t ui_score, ui_high;
uint8_t video_collection_marks;
static uint8_t marks_dirty;
void video_collection(uint8_t command)
{
    uint8_t previous = video_collection_marks;
    if (command == VIDEO_COLLECTION_FLOOR)
        video_collection_marks = (g.pile_taken ? 2 : 0) | (g.picture_taken ? 4 : 0);
    else if (command == VIDEO_COLLECTION_CLEAR)
        video_collection_marks = 0;
    else if (command <= 3)
        video_collection_marks |= 1 << (command - 1);
    if (video_collection_marks != previous)
        marks_dirty = 1;
}
static uint8_t retained_digits[12];
static char number_out[7];
static uint8_t ui_start[4], ui_end[4], sprite_span, previous_span, sprite_count, erase_tile[5];
static uint16_t erase_offset[5];
/* Page 3 aperture: leave the OS interrupt vectors mapped throughout uploads.
   IRQ handlers temporarily borrow page 3 and restore it. Their stack must
   remain in page 2 while page 3 contains VRAM. All sources are static. */
uint8_t upload_stack[256];
static uint8_t upload_bank;
static uint16_t upload_sp, upload_dst, upload_count;
static const void* upload_src;
static void upload_copy(void) __naked
{
    __asm__("ld (_upload_sp),sp\n ld sp,#_upload_stack+256\n ld bc,#0xc0f0\n in a,(c)\n ld "
            "(_upload_bank),a\n ld a,#64\n out (0xf3),a\n ld hl,(_upload_src)\n ld "
            "de,(_upload_dst)\n ld bc,(_upload_count)\n ldir\n ld a,(_upload_bank)\n out "
            "(0xf3),a\n ld sp,(_upload_sp)\n ret");
}
static void upload(uint16_t offset, const void* source, uint16_t count)
{
    upload_dst = 0xc000 + offset;
    upload_src = source;
    upload_count = count;
    upload_copy();
}
/* One bounded 30-row layer copy for doorway redraws. The accepted upload_copy
   and atlas_copy remain unchanged; this uses the same page-3 IRQ-safe stack. */
static void room_copy(void) __naked
{
    __asm__("ld (_upload_sp),sp\n ld sp,#_upload_stack+256\n ld bc,#0xc0f0\n in a,(c)\n ld "
            "(_upload_bank),a\n ld a,#64\n out (0xf3),a\n ld hl,#_room_tiles\n ld de,#0xc000\n ld "
            "b,#30\n00992$:\n push bc\n ld bc,#40\n ldir\n push hl\n ld hl,#40\n add hl,de\n ex "
            "de,hl\n pop hl\n pop bc\n djnz 00992$\n ld a,(_upload_bank)\n out (0xf3),a\n ld "
            "sp,(_upload_sp)\n ret");
}
static uint8_t tile_byte;
static void place(uint8_t tile, uint8_t layer, uint8_t x, uint8_t y)
{
    tile_byte = tile;
    upload((layer ? 0x1000 : 0) + (uint16_t)y * 80 + x, &tile_byte, 1);
}

static const uint8_t ui_rows[4] = { 0, 1, 14, 29 };
#ifdef ZDUNGEON_TEST
extern volatile uint8_t test_mode;
uint8_t late_prepare, late_upload;
#endif
static uint8_t ct(char ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return ch - 'A' + 49;
    if (ch >= '0' && ch <= '9')
        return ch - '0' + 75;
    if (ch == '-')
        return 85;
    if (ch == ':')
        return 86;
    return 48;
}
static void text(uint8_t x, uint8_t y, const char* s)
{
    uint8_t row = y < 2 ? y : y == 14 ? 2 : y == 29 ? 3 : 255;
    if (ui_build && row < 4 && ui_start[row] == 0 && ui_end[row] == 40) {
        uint8_t* cell = ui[row] + x;
        while (*s && x < 40) {
            *cell++ = ct(*s++);
            ++x;
        }
        return;
    }
    if (ui_build && row < 4) {
        uint8_t first = 40, last = 0;
        uint8_t* cell = ui[row] + x;
        while (*s && x < 40) {
            uint8_t tile = ct(*s++);
            if (*cell != tile) {
                *cell = tile;
                if (first == 40)
                    first = x;
                last = x + 1;
            }
            ++cell;
            ++x;
        }
        if (first < 40) {
            ui_dirty |= 1 << row;
            if (first < ui_start[row])
                ui_start[row] = first;
            if (last > ui_end[row])
                ui_end[row] = last;
        }
    } else
        while (*s && x < 40)
            place(ct(*s++), 1, x++, y);
}
static uint8_t* upload_shadow;
static void hud_upload_changed(void) __naked
{
    __asm__("ld (_upload_sp),sp\n ld sp,#_upload_stack+256\n push ix\n ld bc,#0xc0f0\n in a,(c)\n ld (_upload_bank),a\n ld a,#64\n out (0xf3),a\n ld hl,(_upload_src)\n ld de,(_upload_dst)\n ld ix,(_upload_shadow)\n ld bc,(_upload_count)\n ld b,c\n00993$:\n ld a,(hl)\n cp 0(ix)\n jr z,00994$\n ld (de),a\n ld 0(ix),a\n00994$:\n inc hl\n inc de\n inc ix\n djnz 00993$\n ld a,(_upload_bank)\n out (0xf3),a\n pop ix\n ld sp,(_upload_sp)\n ret");
}
static void hud_commit(void)
{
    uint8_t row;
    for (row = 0; row < 4; ++row) {
        if ((ui_dirty & (1 << row)) && ui_end[row] > ui_start[row]) {
            upload_dst = 0xd000 + (uint16_t)ui_rows[row] * 80 + ui_start[row];
            upload_src = ui[row] + ui_start[row];
            upload_shadow = ui_uploaded[row] + ui_start[row];
            upload_count = ui_end[row] - ui_start[row];
            hud_upload_changed();
        }
        ui_start[row] = 40;
        ui_end[row] = 0;
    }
    ui_dirty = 0;
}
static void number(uint8_t x, uint8_t y, uint32_t v, uint8_t digits)
{
    static const uint32_t power[6] = { 100000UL, 10000UL, 1000UL, 100UL, 10UL, 1UL };
    uint8_t i = 0, k = 6 - digits;
    while (k < 6) {
        uint8_t digit = 0;
        while (v >= power[k]) {
            v -= power[k];
            ++digit;
        }
        number_out[i++] = '0' + digit;
        ++k;
    }
    number_out[i] = 0;
    text(x, y, number_out);
}
static void byte_number(uint8_t x, uint8_t y, uint8_t v, uint8_t digits)
{
    char out[4];
    uint8_t i = 0, d = 0;
    if (digits == 3) {
        while (v >= 100) {
            v -= 100;
            ++d;
        }
        out[i++] = '0' + d;
    }
    d = 0;
    if (digits >= 2) {
        while (v >= 10) {
            v -= 10;
            ++d;
        }
        out[i++] = '0' + d;
    }
    out[i++] = '0' + v;
    out[i] = 0;
    text(x, y, out);
}
/* Startup atlas upload uses the same page-3/stack discipline as runtime.
   Keep the accepted runtime upload_copy unchanged. Each expanded chunk
   fits inside one tile-RAM bank; keyboard IRQs remain available. */
static uint8_t atlas_bank;
static void atlas_copy(void) __naked
{
    __asm__("ld (_upload_sp),sp\n ld sp,#_upload_stack+256\n ld bc,#0xc0f0\n in a,(c)\n ld "
            "(_upload_bank),a\n ld a,(_atlas_bank)\n out (0xf3),a\n ld hl,(_upload_src)\n ld "
            "de,(_upload_dst)\n ld bc,(_upload_count)\n ldir\n ld a,(_upload_bank)\n out "
            "(0xf3),a\n ld sp,(_upload_sp)\n ret");
}
static uint8_t load_file(const char* path, uint8_t palette)
{
    zos_dev_t fd = open(path, O_RDONLY);
    uint32_t offset = 0;
    uint16_t n, i;
    uint8_t packed;
    if (fd < 0)
        return 1;
    while (1) {
        n = 256;
        if (read(fd, buffer, &n) != ERR_SUCCESS) {
            close(fd);
            return 2;
        }
        if (!n)
            break;
        if (n > 256) {
            close(fd);
            return 3;
        }
        if (palette) {
            if (n != 32 || offset) {
                close(fd);
                return 3;
            }
            upload(0x0e00, buffer, n);
            offset = n;
        } else {
            /* At most 256 expanded tiles. A 32-bit end offset also represents
               exactly 64 KiB without wrapping to an empty atlas. */
            if ((uint32_t)n * 2 > 65536UL - offset) {
                close(fd);
                return 3;
            }
            i = n;
            while (i) {
                packed = buffer[--i];
                buffer[i * 2] = packed >> 4;
                buffer[i * 2 + 1] = packed & 15;
            }
            i = 0;
            while (i < n * 2) {
                uint16_t within_bank = (uint16_t)offset & 0x3fff;
                uint16_t remaining = n * 2 - i;
                upload_count = 16384U - within_bank;
                if (upload_count > remaining)
                    upload_count = remaining;
                atlas_bank = 68 + (uint8_t)(offset >> 14);
                upload_dst = 0xc000U + within_bank;
                upload_src = buffer + i;
                atlas_copy();
                offset += upload_count;
                i += upload_count;
            }
        }
    }
    close(fd);
    if (!palette && (offset & 255))
        return 3; /* A truncated tile is not a usable atlas. */
    return !offset;
}
uint8_t video_init(void)
{
    uint8_t i;
    gfx_sprite hidden = { 0, 0, 0, 0, 0 };
    memset(upload_stack, 0xa5, sizeof(upload_stack));
    buffer = (uint8_t*)(((uintptr_t)backing + 511) & ~(uintptr_t)511);
    gfx_enable_screen(0);
    if (gfx_initialize(ZVB_CTRL_VID_MODE_GFX_640_8BIT, &video) != GFX_SUCCESS)
        return 1;
    video_ready = 1;
    /* Installed games run from their package directory. Autostart has no
       executable pathname in ZealOS, so use HostFS root only when the local
       package is absent. After this selection every open is relative. */
    {
        zos_dev_t probe = open("assets/zdungeon.ztp", O_RDONLY);
        if (probe >= 0)
            close(probe);
        else if (chdir("H:/") != ERR_SUCCESS)
            return 2;
    }
    asset_error = load_file("assets/zdungeon.ztp", 1);
    if (asset_error)
        return 2;
    asset_error = load_file("assets/zdungeon.zts", 0);
    if (asset_error)
        return 3;
    for (i = 0; i < 128; ++i)
        gfx_sprite_render(&video, i, &hidden);
    zvb_ctrl_l0_scr_x_low = zvb_ctrl_l0_scr_x_high = zvb_ctrl_l0_scr_y_low = zvb_ctrl_l0_scr_y_high
        = 0;
    zvb_ctrl_l1_scr_x_low = zvb_ctrl_l1_scr_x_high = zvb_ctrl_l1_scr_y_low = zvb_ctrl_l1_scr_y_high
        = 0;
    if (world_splash())
        return 4;
    return world_splash_text() ? 4 : 0;
}
/* FLOOR NAMES: edit this one list; uppercase letters/spaces, at most14 characters. */
static const char* const floor_names[8] = {
    "THE CELLARS", "THE CRYPT", "BONE HALLS", "THE PITS",
    "SPIDER DEN", "FLOODED VAULT", "GHOST GALLERY", "THE DARK HEART"
};
/* Keep the prepared glyphs, not text that needs decoding on every scene. */
static uint8_t floor_caption[24];
static const uint8_t floor_prefix[10] = { 54, 60, 63, 63, 66, 48, 76, 48, 85, 48 };
static uint8_t floor_caption_floor = 255, floor_caption_x, floor_caption_end;
static void floor_footer(void)
{
    uint8_t first = floor_caption_x, last = floor_caption_end;
    if (floor_caption_floor != g.floor) {
        const char* name = floor_names[g.floor];
        uint8_t length = 10;
        memcpy(floor_caption, floor_prefix, 10);
        floor_caption[6] = 76 + g.floor;
        while (*name) {
            floor_caption[length++] = *name == ' ' ? 48 : *name - 'A' + 49;
            ++name;
        }
        floor_caption_x = (40 - length) / 2;
        floor_caption_end = floor_caption_x + length;
        if (floor_caption_floor == 255) {
            first = floor_caption_x;
            last = floor_caption_end;
        } else {
            if (floor_caption_x < first)
                first = floor_caption_x;
            if (floor_caption_end > last)
                last = floor_caption_end;
        }
        floor_caption_floor = g.floor;
    }
    /* Clear the old/new union, then mark only that bounded upload span. */
    memset(ui[3] + first, 0, last - first);
    memcpy(ui[3] + floor_caption_x, floor_caption, floor_caption_end - floor_caption_x);
    ui_start[3] = first;
    ui_end[3] = last;
    ui_dirty |= 8;
}
static const uint8_t hud_template[2][40] = {
    { 20, 48, 88, 75, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 56, 57, 55, 56, 48, 75, 75, 75, 75, 75, 75, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 20 },
    { 48, 74, 53, 49, 60, 48, 48, 48, 48, 48, 75, 75, 75, 75, 75, 75, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48 }
};
static void hud(void)
{
    uint8_t reuse_score = 0, reuse_high = 0;
    static const uint8_t corner[8] = { 20, 22, 24, 26, 103, 107, 110, 112 };
    static const uint8_t pictures[4] = { 13, 127, 128, 129 };
    marks_dirty = 0;
    if (ui_valid && ui_score == g.score && ui_high == g.high
        && ui_last[0] == g.floor && ui_last[2] == g.lives && ui_last[3] == g.paused
        && ui_last[4] == video_collection_marks && ui_last[5] == g.piles)
        return;
    ui_build = 1;
    if (!ui_valid) {
        reuse_score = g.score && ui_score == g.score;
        reuse_high = g.high && ui_high == g.high;
        if (reuse_score)
            memcpy(retained_digits, ui[1] + 10, 6);
        if (reuse_high)
            memcpy(retained_digits + 6, ui[0] + 19, 6);
        memcpy(ui, hud_template, sizeof(hud_template));
        if (reuse_score)
            memcpy(ui[1] + 10, retained_digits, 6);
        if (reuse_high)
            memcpy(ui[0] + 19, retained_digits + 6, 6);
        memset(ui[2], 0, 80);
        memset(ui_uploaded, 0, sizeof(ui_uploaded));
        memset(ui_start, 0, 4);
        memset(ui_end, 40, 4);
        ui_start[3] = 40;
        ui_end[3] = 0; /* floor_footer sets its own span; ENDING has no footer. */
        ui_dirty = 15;
    }
    if (!ui_valid || ui_last[0] != g.floor) {
        ui[0][0] = ui[0][39] = corner[g.floor];
        ui_start[0] = 0;
        ui_end[0] = 40;
        ui_dirty |= 1;
        if (g.mode != ENDING) {
            floor_footer();
        }
    }
    if (!ui_valid || ui_last[4] != video_collection_marks || ui_last[5] != g.piles
        || ui_last[0] != g.floor) {
        ui[0][4] = g.piles ? 12 : 48;
        ui[0][34] = video_collection_marks & 1 ? 14 : 48;
        ui[0][36] = video_collection_marks & 2 ? 12 : 48;
        ui[0][38] = video_collection_marks & 4 ? pictures[g.picture & 3] : 48;
        if (ui_start[0] > 4)
            ui_start[0] = 4;
        if (ui_end[0] < 39)
            ui_end[0] = 39;
        ui_dirty |= 1;
    }
    if (!ui_valid || ui_last[2] != g.lives)
        byte_number(3, 0, g.lives, 1);
    if (!ui_valid ? g.score != 0 && !reuse_score : ui_score != g.score)
        number(10, 1, g.score, 6);
    if (!ui_valid ? g.high != 0 && !reuse_high : ui_high != g.high)
        number(19, 0, g.high, 6);
    if (!ui_valid || ui_last[3] != g.paused) {
        /* Restore the exact transparent tile bytes beneath the six letters.
           Sprites and layer0 remain untouched, including while paused. */
        memset(ui[2], 0, 40);
        ui_start[2] = 17;
        ui_end[2] = 23;
        ui_dirty |= 4;
        if (g.paused)
            text(17, 14, "PAUSED");
    }
    ui_build = 0;
    ui_valid = 1;
    ui_score = g.score;
    ui_high = g.high;
    ui_last[0] = g.floor;
    ui_last[2] = g.lives;
    ui_last[3] = g.paused;
    ui_last[4] = video_collection_marks;
    ui_last[5] = g.piles;
}

static void reset_motion(void);
void video_scene(void)
{
    uint8_t x, y, i;
    scene_sprites_ready = 0;
    overlay_skip_header = g.mode == PLAY && ui_valid;
    reset_motion();
    if (!room_handoff) {
        if (!overlay_skip_header)
            ui_valid = 0;
        else {
            /* clear_overlay retains row29, so its buffer/shadow stay valid. */
            memset(ui_uploaded[2], 0, 40);
            ui_last[3] = 255; /* Rebuild any cleared pause overlay. */
        }
        gfx_enable_screen(0);
    }
    if (!room_handoff) {
        /* A PLAY room_copy overwrites all 40x30 visible layer-0 cells.
           Partial scenes still need their background cleared. */
        if (g.mode == ENDING || g.mode == GAMEOVER)
            for (y = 0; y < 30; ++y)
                upload((uint16_t)y * 80, blank_row, 40);
        clear_overlay();
    }
    if (g.mode == TITLE) {
        video_collection(VIDEO_COLLECTION_CLEAR);
        if (world_splash()) {
            asset_error = 1;
            return;
        }
        room_copy();
        if (world_splash_text()) {
            asset_error = 1;
            return;
        }
        for (y = 0; y < 30; ++y)
            upload(0x1000 + (uint16_t)y * 80, room_tiles + (uint16_t)y * 40, 40);
        memcpy(title_start_line, room_tiles + 15 * 40 + 10, sizeof(title_start_line));
        title_flash_clock = 0;
        title_start_visible = 1;
        /* Title lettering is entirely authored in splash.tmx. The legacy
           value records remain in its container for format compatibility. */
    } else if (g.mode == ENDING || g.mode == GAMEOVER) {
        for (y = 3; y < 27; ++y)
            for (x = 2; x < 38; ++x)
                if (x < 4 || x > 35 || y < 5 || y > 24)
                    place(135 + (x + y) % 2, 0, x, y);
        hud();
        hud_commit();
        if (g.mode == GAMEOVER)
            text(15, 14, "GAME OVER");

    } else {
        static const uint8_t pictures[4] = { 13, 127, 128, 129 };
        if (g.room == g.sites[3].room) {
            uint8_t row;
            uint8_t* cell = room_tiles + (uint16_t)g.sites[3].y * 40 + g.sites[3].x - 1;
            const uint8_t* tile = ART_SPECIALS_TRANSPORT;
            for (row = 0; row < 4; ++row) {
                *cell++ = *tile++;
                *cell++ = *tile++;
                *cell++ = *tile++;
                cell += 37;
            }
        }
        room_copy();
        for (i = 0; i < current_room->count; ++i) {
            RoomObject* o = current_room->objects + i;
            uint8_t tile = 0;
            if (o->kind == 4 || !game_object_active(o))
                continue;
            if (o->kind == 1)
                tile = 14;
            else if (o->kind == 2)
                tile = 12;
            else if (o->kind == 3)
                tile = pictures[g.picture & 3];
            else if (o->kind == 5) {
                tile = 15;
                place(125, 0, o->x + 1, o->y + 2);
            }
            if (tile)
                place(tile, 0, o->x, o->y + 2);
        }
        hud();
        if (!room_handoff)
            hud_commit();
        g.hud_dirty = 0;
    }
    if (!room_handoff) {
        scene_sprites_ready = g.mode == PLAY;
        prepare_sprites();
        upload(0x2800, sprites, sizeof(sprites));
        previous_span = sprite_span;
    }
    g.scene_dirty = 0;
    gfx_enable_screen(1);
}
static void sprite(uint8_t id, uint8_t x, uint8_t y, uint8_t tile, uint8_t flip)
{
    ++sprite_count;
    sprite_span = id + 1;
    sprites[id].x = (uint16_t)x * 16 + 16;
    sprites[id].y = (uint16_t)(y + 2) * 16 + 16;
    sprites[id].tile = tile;
    sprites[id].flags = flip ? SPRITE_FLIP_X : 0;
}
/* Presentation-only state. Game cells, clock phases and collisions are never
   written here. DDA distributes exactly sixteen pixels across the predicted
   display frames to the next service, including this draw frame. */
static uint16_t drawn_x, drawn_y;
static uint32_t drawn_frame;
static uint8_t advance_draw, enemy_duration, shot_duration, walk_distance, walk_phase, snap_draw,
    draw_mode;
typedef struct {
    uint8_t x, y, remaining, duration, error, valid, direction, reserved;
} MotionRecord;
static MotionRecord motion_records[31];
static uint8_t motion_id, motion_x, motion_y, motion_kind, motion_frozen;

/* Exact 17-bit loop phase represented by a 16-bit low word and carry.
   75849 = 65536 + 10313. IRQ phase always fits in sixteen bits. */
typedef char assert_room_offset[(offsetof(Game, room) == 88) ? 1 : -1];
typedef char assert_phase_offset[(offsetof(Game, phase) == 16) ? 1 : -1];
typedef char assert_irq_phase_offset[(offsetof(Game, irq_phase) == 20) ? 1 : -1];
typedef char assert_move_clock_offset[(offsetof(Game, move_clock) == 136) ? 1 : -1];
typedef char assert_motion_size[(sizeof(MotionRecord) == 8) ? 1 : -1];
typedef char assert_enemy_stride[(sizeof(Enemy) == 9 && MAX_ENEMIES == 25) ? 1 : -1];
typedef char assert_enemy_fields[(offsetof(Enemy, active) == 0 && offsetof(Enemy, room) == 1
                                     && offsetof(Enemy, x) == 2 && offsetof(Enemy, y) == 3
                                     && offsetof(Enemy, type) == 4 && offsetof(Enemy, anim) == 5
                                     && offsetof(Enemy, visible) == 8)
        ? 1
        : -1];
typedef char assert_motion_fields[(offsetof(MotionRecord, x) == 0 && offsetof(MotionRecord, y) == 1
                                      && offsetof(MotionRecord, remaining) == 2
                                      && offsetof(MotionRecord, duration) == 3
                                      && offsetof(MotionRecord, error) == 4
                                      && offsetof(MotionRecord, valid) == 5
                                      && offsetof(MotionRecord, direction) == 6)
        ? 1
        : -1];
typedef char assert_sprite_count[(SPRITES == 40) ? 1 : -1];
static uint8_t predict_high, predict_ticks, predict_loops;

static uint8_t until_service(uint8_t kind) __naked
{
    (void)kind;
    __asm__(
        /* B=kind, C=frames, HL=phase low, DE=IRQ phase. */
        "ld b,a\n or a\n jr z,00940$\n cp #2\n jr z,00941$\n ld a,#1\n jr 00942$\n"
        "00940$:\n ld a,#2\n jr 00942$\n"
        "00941$:\n ld a,#8\n"
        "00942$:\n ld (_predict_loops),a\n"
        "ld a,(_g+18)\n ld (_predict_high),a\n"
        "ld a,(_g+136)\n ld (_predict_ticks),a\n"
        "ld hl,(_g+16)\n ld de,(_g+20)\n ld c,#0\n"
        "00943$:\n inc c\n"
        /* irq +=0x4180. Swap preserves phase without pushes or general constants. */
        "ex de,hl\n ld a,l\n add a,#128\n ld l,a\n ld a,h\n adc a,#65\n ld h,a\n"
        /* Unsigned irq>=0x51F4; preserve original uint16 wrap semantics. */
        "ld a,h\n cp #81\n jr c,00945$\n jr nz,00944$\n"
        "ld a,l\n cp #244\n jr c,00945$\n"
        "00944$:\n ld a,l\n sub #244\n ld l,a\n ld a,h\n sbc a,#81\n ld h,a\n"
        "ld a,(_predict_ticks)\n or a\n jr z,00945$\n dec a\n ld (_predict_ticks),a\n"
        "00945$:\n ex de,hl\n"
        /* phase +=0x8300. Low byte unchanged; carry sets 17th phase bit. */
        "ld a,h\n add a,#131\n ld h,a\n jr nc,00946$\n"
        "ld a,#1\n ld (_predict_high),a\n"
        "00946$:\n ld a,(_predict_high)\n or a\n jr z,00949$\n"
        /* high && phase low>=0x2849. */
        "ld a,h\n cp #40\n jr c,00949$\n jr nz,00947$\n"
        "ld a,l\n cp #73\n jr c,00949$\n"
        "00947$:\n ld a,l\n sub #73\n ld l,a\n ld a,h\n sbc a,#40\n ld h,a\n"
        "xor a\n ld (_predict_high),a\n"
        "ld a,b\n cp #3\n jr z,00948$\n"
        "ld a,(_predict_loops)\n dec a\n ld (_predict_loops),a\n jr z,00950$\n jr 00949$\n"
        "00948$:\n ld a,(_predict_ticks)\n or a\n jr z,00950$\n"
        "00949$:\n ld a,c\n cp #80\n jp c,00943$\n"
        "00950$:\n ld a,c\n ret\n");
}

static void draw_retarget(void) __naked
{
    __asm__(
        /* Pointer to packed8byte record; one saved base for common coordinate tail. */
        "ld a,(_motion_id)\n ld l,a\n ld h,#0\n add hl,hl\n add hl,hl\n add hl,hl\n"
        "ld de,#_motion_records\n add hl,de\n push hl\n"
        "ld a,(_snap_draw)\n or a\n jp nz,00978$\n"
        "ld a,(_motion_x)\n sub (hl)\n ld b,a\n inc hl\n"
        "ld a,(_motion_y)\n sub (hl)\n ld c,a\n"
        /* HL now y1. Move to valid5. B/C retain byte-wrapped signed dx/dy. */
        "inc hl\n inc hl\n inc hl\n inc hl\n"
        "ld a,(hl)\n or a\n jp z,00977$\n"
        "ld a,(_motion_frozen)\n or a\n jp nz,00977$\n"
        /* (uint8)(delta+1)<3 accepts exactly -1,0,+1, including signed wraps. */
        "ld a,b\n inc a\n cp #3\n jp nc,00977$\n"
        "ld a,c\n inc a\n cp #3\n jp nc,00977$\n"
        "ld a,b\n or c\n jp z,00979$\n"
        /* Direction bits right1,left2,down4,up8. */
        "ld d,#0\n ld a,b\n or a\n jr z,00971$\n"
        "ld d,#1\n bit 7,b\n jr z,00971$\n ld d,#2\n"
        "00971$:\n ld a,c\n or a\n jr z,00973$\n"
        "ld a,d\n bit 7,c\n jr nz,00972$\n or #4\n jr 00974$\n"
        "00972$:\n or #8\n jr 00974$\n"
        "00973$:\n ld a,d\n"
        "00974$:\n inc hl\n ld (hl),a\n dec hl\n dec hl\n dec hl\n"
        /* HL=duration3. Preserve it across until_service's HL/DE/BC clobbers. */
        "push hl\n ld a,(_motion_kind)\n cp #3\n jr z,00975$\n cp #1\n jr z,00976$\n"
        "call _until_service\n jr 00970$\n"
        "00975$:\n ld a,(_enemy_duration)\n or a\n jr nz,00970$\n"
        "ld a,#3\n call _until_service\n ld (_enemy_duration),a\n jr 00970$\n"
        "00976$:\n ld a,(_shot_duration)\n or a\n jr nz,00970$\n"
        "ld a,#1\n call _until_service\n ld (_shot_duration),a\n"
        "00970$:\n pop hl\n ld (hl),a\n dec hl\n ld (hl),#16\n"
        "inc hl\n inc hl\n ld (hl),#0\n jr 00979$\n"
        /* Invalid/frozen/jump: currently HL=valid5; set valid1 and remaining0. */
        "00977$:\n ld (hl),#1\n dec hl\n dec hl\n dec hl\n ld (hl),#0\n jr 00979$\n"
        /* Snap: HL is original record base. Clear remaining and valid; no prediction. */
        "00978$:\n inc hl\n inc hl\n ld (hl),#0\n"
        "inc hl\n inc hl\n inc hl\n ld a,(_scene_sprites_ready)\n ld (hl),a\n"
        "00979$:\n pop hl\n ld a,(_motion_x)\n ld (hl),a\n inc hl\n ld a,(_motion_y)\n ld (hl),a\n "
        "ret\n");
}

/* Hot helper. Input actor ID=A. Return exact advanced pixel amount=A and
 * write drawn_x/y. Eight-byte record offsets: remaining2,duration3,error4,
 * valid5,direction6. Duration1..80, error<duration, so error+16 fits a byte.
 * Loop iterations <=16; no divisions, stack frame, indexed loads or C spills.
 */
static uint8_t draw_advance(uint8_t id) __naked
{
    (void)id;
    __asm__("ld l,a\n ld h,#0\n add hl,hl\n add hl,hl\n add hl,hl\n"
            "ld de,#_motion_records\n add hl,de\n"
            /* Convert target cell x to16-bit pixel x without multiply/helper calls. */
            "ld a,(hl)\n ld e,a\n and #15\n rlca\n rlca\n rlca\n rlca\n"
            "ld (_drawn_x),a\n ld a,e\n and #240\n rrca\n rrca\n rrca\n rrca\n ld (_drawn_x+1),a\n"
            "inc hl\n ld a,(hl)\n ld e,a\n and #15\n rlca\n rlca\n rlca\n rlca\n"
            "ld (_drawn_y),a\n ld a,e\n and #240\n rrca\n rrca\n rrca\n rrca\n ld (_drawn_y+1),a\n"
            "inc hl\n ld b,(hl)\n ld c,#0\n ld a,b\n or a\n jr z,00909$\n"
            "ld a,(_advance_draw)\n or a\n jr z,00904$\n"
            /* HL=remaining; D=duration; A=error+16; C=amount. */
            "inc hl\n ld d,(hl)\n inc hl\n ld a,(hl)\n add a,#16\n"
            "00901$:\n cp d\n jr c,00902$\n sub d\n inc c\n jr 00901$\n"
            "00902$:\n ld (hl),a\n dec hl\n dec hl\n ld a,b\n sub c\n ld (hl),a\n ld b,a\n"
            /* HL=remaining. Apply residual pixel distance against target coordinate. */
            "00904$:\n ld a,b\n or a\n jr z,00909$\n"
            "inc hl\n inc hl\n inc hl\n inc hl\n ld a,(hl)\n push af\n"
            "ld e,b\n ld d,#0\n ld hl,(_drawn_x)\n"
            "bit 0,a\n jr z,00905$\n or a\n sbc hl,de\n jr 00906$\n"
            "00905$:\n bit 1,a\n jr z,00906$\n add hl,de\n"
            "00906$:\n ld (_drawn_x),hl\n pop af\n ld hl,(_drawn_y)\n"
            "bit 2,a\n jr z,00907$\n or a\n sbc hl,de\n jr 00908$\n"
            "00907$:\n bit 3,a\n jr z,00908$\n add hl,de\n"
            "00908$:\n ld (_drawn_y),hl\n"
            "00909$:\n ld a,c\n ret\n");
}

/* Public hot wrapper uses globals as cold-call argument mailboxes, avoiding
 * a C function whose unused cold branches still cause17 stack spill slots.
 * These bytes are presentation state only; IRQ handlers must not call this
 * renderer (same non-reentrant constraint as original drawn_x/y globals).
 */
static uint8_t draw_motion(uint8_t id, uint8_t x, uint8_t y, uint8_t kind) __naked
{
    (void)id;
    (void)x;
    (void)y;
    (void)kind;
    __asm__("ld (_motion_id),a\n ld a,l\n ld (_motion_x),a\n"
            "ld hl,#2\n add hl,sp\n ld a,(hl)\n ld (_motion_y),a\n"
            "inc hl\n ld a,(hl)\n ld (_motion_kind),a\n"
            "ld a,(_snap_draw)\n or a\n jr nz,00921$\n"
            "ld a,(_motion_frozen)\n or a\n jr nz,00921$\n"
            /* Check x/y and valid with one record address calculation. */
            "ld a,(_motion_id)\n ld l,a\n ld h,#0\n add hl,hl\n add hl,hl\n add hl,hl\n"
            "ld de,#_motion_records\n add hl,de\n"
            "ld a,(_motion_x)\n cp (hl)\n jr nz,00921$\n"
            "inc hl\n ld a,(_motion_y)\n cp (hl)\n jr nz,00921$\n"
            "inc hl\n inc hl\n inc hl\n inc hl\n ld a,(hl)\n or a\n jr nz,00922$\n"
            "00921$:\n call _draw_retarget\n"
            "00922$:\n ld a,(_motion_id)\n call _draw_advance\n"
            /* Caller return address remains atop the two byte stack arguments. */
            "pop hl\n inc sp\n inc sp\n push hl\n ret\n");
}

static void pixel_sprite(uint8_t id, uint16_t x, uint16_t y, uint8_t tile, uint8_t flip) __naked
{
    (void)id;
    (void)x;
    (void)y;
    (void)tile;
    (void)flip;
    __asm__("ld c,a\n ld a,(_sprite_count)\n inc a\n ld (_sprite_count),a\n"
            "ld a,c\n inc a\n ld (_sprite_span),a\n"
            "ld l,c\n ld h,#0\n add hl,hl\n add hl,hl\n add hl,hl\n"
            /* Preserve x inDE while building spritebase through BC (id no longer needed). */
            "ld bc,#_sprites\n add hl,bc\n"
            "ld a,e\n add a,#16\n ld e,a\n jr nc,00988$\n inc d\n"
            "00988$:\n inc hl\n inc hl\n ld (hl),e\n inc hl\n ld (hl),d\n inc hl\n push hl\n"
            /* Pushed tile pointer adds2bytes, so y begins SP+4. */
            "ld hl,#4\n add hl,sp\n ld e,(hl)\n inc hl\n ld d,(hl)\n"
            "ld a,e\n add a,#48\n ld e,a\n jr nc,00989$\n inc d\n"
            "00989$:\n inc hl\n ld b,(hl)\n inc hl\n ld a,(hl)\n"
            "ld c,#0\n or a\n jr z,00990$\n ld c,#8\n"
            "00990$:\n pop hl\n ld (hl),b\n inc hl\n ld (hl),c\n"
            "dec hl\n dec hl\n dec hl\n dec hl\n dec hl\n ld (hl),e\n inc hl\n ld (hl),d\n"
            "pop hl\n pop bc\n pop bc\n jp (hl)\n");
}

static void reset_motion(void)
{
    memset(motion_records, 0, sizeof(motion_records));
    walk_distance = walk_phase = 0;
    draw_mode = g.mode;
    snap_draw = 1;
    drawn_frame = g.frames - 1;
}
static const uint8_t* const enemy_frames[8]
    = { ART_ENEMIES_FLOOR_1, ART_ENEMIES_FLOOR_2, ART_ENEMIES_FLOOR_3, ART_ENEMIES_FLOOR_4,
          ART_ENEMIES_FLOOR_5, ART_ENEMIES_FLOOR_6, ART_ENEMIES_FLOOR_7, ART_ENEMIES_FLOOR_8 };
/* Same record-order renderer with direct strides. Cold retargeting and the
   normal sprite writer still perform every visible actor's interpolation. */
static void draw_enemies(void) __naked
{
    __asm__("push iy\n ld iy,#_enemies\n ld b,#25\n ld c,#0\n"
            "00931$:\n ld a,8(iy)\n or a\n jr z,00933$\n ld a,0(iy)\n or a\n jr z,00933$\n ld "
            "a,(_g+88)\n cp 1(iy)\n jr nz,00933$\n"
            "push bc\n push iy\n ld d,#3\n ld e,3(iy)\n push de\n ld l,2(iy)\n ld a,c\n inc a\n "
            "call _draw_motion\n pop iy\n pop bc\n"
            "ld a,4(iy)\n and #7\n add a,a\n ld l,a\n ld h,#0\n ld de,#_enemy_frames\n add hl,de\n "
            "ld e,(hl)\n inc hl\n ld d,(hl)\n ld l,5(iy)\n ld h,#0\n add hl,de\n ld a,(hl)\n"
            "push bc\n push iy\n ld d,a\n xor a\n push af\n inc sp\n ld a,d\n push af\n inc sp\n "
            "ld hl,(_drawn_y)\n push hl\n ld de,(_drawn_x)\n ld a,c\n add a,#2\n call "
            "_pixel_sprite\n pop iy\n pop bc\n jr 00934$\n"
            "00933$:\n ld a,c\n inc a\n ld l,a\n ld h,#0\n add hl,hl\n add hl,hl\n add hl,hl\n ld "
            "de,#_motion_records+5\n add hl,de\n ld (hl),#0\n"
            "00934$:\n ld de,#9\n add iy,de\n inc c\n dec b\n jp nz,00931$\n pop iy\n ret\n");
}
#ifdef ZDUNGEON_TEST
/* Same-line stress arrangement follows normal interpolation, without adding
   a slow C scan unrelated to normal actor rendering. No game state writes. */
static void stress_line(void) __naked
{
    __asm__("ld hl,#_sprites\n ld de,#6\n ld b,#40\n00941$:\n ld (hl),#240\n inc hl\n ld (hl),#0\n "
            "inc hl\n ld a,(hl)\n inc hl\n or (hl)\n dec hl\n jr nz,00942$\n ld (hl),#16\n inc "
            "hl\n ld (hl),#0\n inc hl\n ld (hl),#30\n dec hl\n dec hl\n00942$:\n add hl,de\n djnz "
            "00941$\n ret");
}
#endif
typedef char assert_sprite_flip[(SPRITE_FLIP_X == 8) ? 1 : -1];
typedef char assert_sprite_size[(sizeof(gfx_sprite) == 8) ? 1 : -1];
typedef char assert_sprite_x[(offsetof(gfx_sprite, x) == 2) ? 1 : -1];
typedef char assert_sprite_y[(offsetof(gfx_sprite, y) == 0) ? 1 : -1];
typedef char assert_sprite_tile[(offsetof(gfx_sprite, tile) == 4) ? 1 : -1];
typedef char assert_sprite_flags[(offsetof(gfx_sprite, flags) == 5) ? 1 : -1];
/* Hidden records require zero coordinates. Tile/flags are replaced by every
   live sprite write; options remain their initial zero throughout. */
static void clear_sprite_positions(void) __naked
{
    __asm__("ld hl,#_sprites\n ld de,#5\n ld b,#40\n xor a\n00961$:\n ld (hl),a\n inc hl\n ld "
            "(hl),a\n inc hl\n ld (hl),a\n inc hl\n ld (hl),a\n add hl,de\n djnz 00961$\n ret");
}
static void prepare_sprites(void)
{
    uint8_t i;
    if (draw_mode != g.mode) {
        reset_motion();
    }
    motion_frozen = g.paused || g.mode != PLAY;
    advance_draw = drawn_frame != g.frames;
    drawn_frame = g.frames;
    enemy_duration = shot_duration = 0;
    clear_sprite_positions();
    sprite_count = sprite_span = 0;
    if (g.mode == TITLE) {
        sprite(0, 19, 16, 16, 0);
        sprite(1, 19, 17, 17, 0);
        sprite(2, 9, 17, 20, 0);
        sprite(3, 29, 17, 22, 0);
    }
    if (g.mode == PLAY || g.mode == TRANSPORT || g.mode == DEAD) {
        uint8_t show = g.mode != DEAD || (g.timer & 4);
        static const uint8_t* const walk_upper[3]
            = { ART_HERO_WALK_UP_UPPER, ART_HERO_WALK_RIGHT_UPPER, ART_HERO_WALK_DOWN_UPPER };
        static const uint8_t* const walk_lower[3]
            = { ART_HERO_WALK_UP_LOWER, ART_HERO_WALK_RIGHT_LOWER, ART_HERO_WALK_DOWN_LOWER };
        static const uint8_t* const stand_upper[3]
            = { ART_HERO_STAND_UP_UPPER, ART_HERO_STAND_RIGHT_UPPER, ART_HERO_STAND_DOWN_UPPER };
        static const uint8_t* const stand_lower[3]
            = { ART_HERO_STAND_UP_LOWER, ART_HERO_STAND_RIGHT_LOWER, ART_HERO_STAND_DOWN_LOWER };
        static const uint8_t* const fire_upper[3]
            = { ART_HERO_FIRE_UP_UPPER, ART_HERO_FIRE_RIGHT_UPPER, ART_HERO_FIRE_DOWN_UPPER };
        static const uint8_t* const fire_lower[3]
            = { ART_HERO_FIRE_UP_LOWER, ART_HERO_FIRE_RIGHT_LOWER, ART_HERO_FIRE_DOWN_LOWER };
        uint8_t moved = draw_motion(0, g.x, g.y, 0), direction = g.face == 3 ? 1 : g.face, upper,
                lower;
        if (moved) {
            walk_distance += moved;
            walk_phase = (walk_distance >> 3) & 3;
        }
        if (g.previous & FIRE) {
            upper = fire_upper[direction][0];
            lower = fire_lower[direction][0];
        } else if (moved || motion_records[0].remaining) {
            upper = walk_upper[direction][walk_phase];
            lower = walk_lower[direction][walk_phase];
        } else {
            upper = stand_upper[direction][0];
            lower = stand_lower[direction][0];
        }
        if (show && (!g.protection || (g.frames & 4))) {
            pixel_sprite(0, drawn_x, drawn_y, upper, g.face == 3);
            pixel_sprite(1, drawn_x, drawn_y + 16, lower, g.face == 3);
        }
        draw_enemies();
        /* Projectiles retain cell-step drawing: four simultaneous interpolated
           shots exceeded the measured legal-load frame budget. Logic is unchanged. */
        for (i = 0; i < 4; ++i)
            if (shots[i].active)
                sprite(27 + i, shots[i].x, shots[i].y, shots[i].owner == 1 ? 28 : 29, 0);
        for (i = 0; i < 5; ++i)
            if (effects[i].active)
                sprite(31 + i, effects[i].x, effects[i].y,
                    ART_SHOTS_EFFECTS_BURST[(12 - effects[i].timer) / 4], 0);
    }

    if (g.final_active && (g.mode == PLAY || g.mode == TRANSPORT)) {
        draw_motion(30, g.final_x, g.final_y, 2);
        pixel_sprite(36, drawn_x, drawn_y, ART_SPECIALS_FINAL_UPPER[(g.loops / 4) & 1], 0);
        pixel_sprite(37, drawn_x, drawn_y + 16, ART_SPECIALS_FINAL_LOWER[(g.loops / 4) & 1], 0);
    } else
        motion_records[30].valid = 0;
#ifdef ZDUNGEON_TEST
    /* The same-line GPU fixture follows normal actor interpolation, so its
       timing includes every visible actor rather than bypassing drawing. */
    if (test_mode == 3) {
        stress_line();
        sprite_count = sprite_span = SPRITES;
    }
#endif
    snap_draw = 0;
    if (sprite_count > g.max_sprites)
        g.max_sprites = sprite_count;
}
void video_frame(void)
{
    uint8_t erase_count = 0, k, title_upload = 0, already_uploaded = scene_sprites_ready;
    scene_sprites_ready = 0;
    if (!already_uploaded)
        prepare_sprites();
    if ((g.hud_dirty || marks_dirty) && g.mode != TITLE && g.mode != ENDING && g.mode != GAMEOVER) {
        hud();
        g.hud_dirty = 0;
    }
    if (g.item_dirty) {
        uint8_t k;
        for (k = 0; k < current_room->count; ++k) {
            RoomObject* o = current_room->objects + k;
            uint8_t bit;
            if (o->kind < 1 || o->kind == 4 || o->kind > 5)
                continue;
            bit = 1 << (o->kind == 5 ? 3 : o->kind - 1);
            if ((g.item_dirty & bit) && erase_count < 4) {
                erase_offset[erase_count] = (uint16_t)(o->y + 2) * 80 + o->x;
                erase_tile[erase_count++] = room_tiles[(uint16_t)(o->y + 2) * 40 + o->x];
                if (o->kind == 5) {
                    erase_offset[erase_count] = (uint16_t)(o->y + 2) * 80 + o->x + 1;
                    erase_tile[erase_count++] = room_tiles[(uint16_t)(o->y + 2) * 40 + o->x + 1];
                }
            }
        }
        g.item_dirty = 0;
    }
    if (zvb_ctrl_status & 2) {
        ++g.overruns;
#ifdef ZDUNGEON_TEST
#ifdef EMBER_TEST
        room_iteration_flags |= 1;
#endif
        if (late_prepare < 255)
            ++late_prepare;
#endif
    }
    if (g.mode == TITLE && ++title_flash_clock == 30) {
        title_flash_clock = 0;
        title_start_visible ^= 1;
        title_upload = 1;
    }
    gfx_wait_vblank(&video);
    if (title_upload)
        upload(0x1000 + 15 * 80 + 10,
            title_start_visible ? title_start_line : blank_row, sizeof(title_start_line));
    {
        if (!already_uploaded) {
            uint8_t span = sprite_span > previous_span ? sprite_span : previous_span;
            if (span)
                upload(0x2800, sprites, (uint16_t)span * sizeof(gfx_sprite));
            previous_span = sprite_span;
        }
    }
    for (k = 0; k < erase_count; ++k)
        upload(erase_offset[k], erase_tile + k, 1);
    if (ui_dirty)
        hud_commit();
    if (!(zvb_ctrl_status & 2)) {
        ++g.overruns;
#ifdef ZDUNGEON_TEST
#ifdef EMBER_TEST
        room_iteration_flags |= 2;
#endif
        if (late_upload < 255)
            ++late_upload;
#endif
    }
    gfx_wait_end_vblank(&video);
}
void video_exit(void)
{
    uint8_t i;
    gfx_sprite hide = { 0, 0, 0, 0, 0 };
    if (video_ready)
        for (i = 0; i < 128; ++i)
            gfx_sprite_render(&video, i, &hide);
    video_ready = 0;
    ioctl(DEV_STDOUT, CMD_RESET_SCREEN, 0);
    gfx_enable_screen(1);
}

#ifdef ZDUNGEON_TEST
uint16_t video_upload_stack_used(void)
{
    uint16_t i = 0;
    while (i < 256 && upload_stack[i] == 0xa5)
        ++i;
    return 256 - i;
}
uint8_t video_upload_guard(void)
{
    uint8_t i;
    for (i = 0; i < 32; ++i)
        if (upload_stack[i] != 0xa5)
            return 0;
    return 1;
}
#endif
