#include <zvb_sound.h>
#include "game.h"
#include "input.h"
#include "sound.h"
#include "world.h"
#include "video.h"

/* Fresh ZVB synthesis, never TED data. VOICE0: one-shot effects, priority
 * death > ending > extra life > pickup > kill > shot. VOICE1: accepted
 * title/ending melody. VOICE2: soft footsteps, independent of all effects.
 * VOICE3: transport entry/wait pulses; arrival remains an effect on VOICE0.
 * A higher/equal-priority effect may replace VOICE0; steps never replace it.
 * All audio-owned state below is excluded from the gameplay comparison. */
/* PLAYER_SHOT_ZAP: all player-shot timbre controls live here.
 * Eight 16.768 ms intervals = 134.144 ms (unless interrupted/paused).
 * A gently asymmetric square gives the buzz; quieter than the old shot.
 * Each interval drops by DROP_HZ: 1200 -> about 185 Hz, no extra voice.
 * Keep START_HZ > DROP_HZ * (FRAMES - 1), and FRAMES between 2 and 254. */
#define PLAYER_SHOT_ZAP_START_HZ 1200UL
#define PLAYER_SHOT_ZAP_DROP_HZ 145UL
#define PLAYER_SHOT_ZAP_FRAMES 8
#define PLAYER_SHOT_ZAP_VOLUME VOL_25
#define PLAYER_SHOT_ZAP_WAVEFORM (WAV_SQUARE | DUTY_CYCLE_37_5)
#define PLAYER_SHOT_ZAP_START_DIV SOUND_FREQ_TO_DIV(PLAYER_SHOT_ZAP_START_HZ)
#define PLAYER_SHOT_ZAP_DROP_DIV SOUND_FREQ_TO_DIV(PLAYER_SHOT_ZAP_DROP_HZ)
typedef char assert_player_shot_zap_bounds[(PLAYER_SHOT_ZAP_FRAMES >= 2
    && PLAYER_SHOT_ZAP_FRAMES <= 254
    && PLAYER_SHOT_ZAP_START_DIV > PLAYER_SHOT_ZAP_DROP_DIV * (PLAYER_SHOT_ZAP_FRAMES - 1)) ? 1 : -1];
static uint16_t shot_divider;
static uint8_t duration, priority, note, beat, done;
static uint8_t step_duration, foot, transport_duration, transport_clock;
static uint8_t held_mask, master_muted;
#ifdef ZDUNGEON_TEST
/* Eight most recent actual hardware voice starts: frame16, divider16,
 * voice mask, event, per-voice volume, waveform. Host drains every frame. */
volatile uint16_t audio_sequence;
volatile uint8_t audio_starts[64];
volatile uint8_t audio_held = 15;
static void trace_start(
    uint8_t voice, uint8_t event, uint16_t divider, uint8_t volume, uint8_t waveform)
{
    volatile uint8_t* p = audio_starts + ((audio_sequence & 7) << 3);
    p[0] = g.frames;
    p[1] = g.frames >> 8;
    p[2] = divider;
    p[3] = divider >> 8;
    p[4] = voice;
    p[5] = event;
    p[6] = volume;
    p[7] = waveform;
    ++audio_sequence;
}
#else
#define trace_start(v, e, d, a, w) ((void)0)
#endif
/* The profile measured repeated SDK mapping/hold calls on the two busy
 * preparation misses. Own the four-voice hold mask here and map once per
 * transaction; the SDK remains unchanged. No audio write is deferred. */
static void hold(uint8_t voices, uint8_t value)
{
    uint8_t next = value ? held_mask | voices : held_mask & ~voices;
    if (next == held_mask)
        return;
    held_mask = next;
    zvb_map_peripheral(ZVB_PERI_SOUND_IDX);
    zvb_peri_sound_hold = next;
#ifdef ZDUNGEON_TEST
    audio_held = next;
#endif
}
static void start(uint8_t voice, uint8_t event, uint16_t divider, uint8_t volume, uint8_t waveform)
{
    /* Same hold -> volume -> frequency/wave -> unhold ordering as before,
     * with one peripheral mapping and no nested SDK hold transactions. */
    zvb_map_peripheral(ZVB_PERI_SOUND_IDX);
    zvb_peri_sound_hold = held_mask | voice;
    zvb_peri_sound_select = voice;
    zvb_peri_sound_volume = volume;
    zvb_peri_sound_freq_low = divider;
    zvb_peri_sound_freq_high = divider >> 8;
    zvb_peri_sound_wave = waveform;
    held_mask &= ~voice;
    zvb_peri_sound_hold = held_mask;
#ifdef ZDUNGEON_TEST
    audio_held = held_mask;
#endif
    trace_start(voice, event, divider, volume, waveform);
}
static const uint16_t melody[8] = { SOUND_FREQ_TO_DIV(392UL), SOUND_FREQ_TO_DIV(466UL),
    SOUND_FREQ_TO_DIV(587UL), SOUND_FREQ_TO_DIV(523UL), SOUND_FREQ_TO_DIV(698UL),
    SOUND_FREQ_TO_DIV(587UL), SOUND_FREQ_TO_DIV(466UL), SOUND_FREQ_TO_DIV(392UL) };
void sound_mute(void)
{
    hold(VOICE0 | VOICE1 | VOICE2 | VOICE3, 1);
    zvb_sound_set_volume(VOL_0);
    master_muted = 1;
    duration = priority = step_duration = transport_duration = 0;
    transport_clock = beat = 0;
}
void sound_init(void)
{
    zvb_sound_initialize(1);
    held_mask = 0;
    sound_mute();
    zvb_sound_set_channels(VOICE0 | VOICE1 | VOICE2 | VOICE3, VOICE0 | VOICE1 | VOICE2 | VOICE3);
    zvb_sound_set_volume(VOL_50);
    master_muted = 0;
    note = done = foot = 0;
}
void sound_step(void)
{
    /* $1B93 original alternates a noise gate on movement input. Requested
     * adaptation: a fresh, alternating-colour noise click for EACH successful
     * step; blocked input is silent. Two frames, 25% voice vs 75% effects. */
    if (g.mode != PLAY || g.paused)
        return;
    foot ^= 1;
    start(
        VOICE2, 10, foot ? SOUND_FREQ_TO_DIV(310UL) : SOUND_FREQ_TO_DIV(430UL), VOL_25, WAV_NOISE);
    step_duration = 3; /* Tick below consumes the start frame: two full intervals. */
}
void sound_event(uint8_t event)
{
    static const uint16_t tones[8] = { 0, PLAYER_SHOT_ZAP_START_DIV, SOUND_FREQ_TO_DIV(180UL),
        SOUND_FREQ_TO_DIV(880UL), SOUND_FREQ_TO_DIV(110UL), SOUND_FREQ_TO_DIV(660UL),
        SOUND_FREQ_TO_DIV(784UL), SOUND_FREQ_TO_DIV(1046UL) };
    static const uint8_t priorities[8] = { 0, 1, 2, 3, 7, 4, 6, 5 };
    if (event == 8) {
        start(VOICE3, 8, SOUND_FREQ_TO_DIV(560UL), VOL_50, WAV_NOISE);
        transport_duration = 3;
        transport_clock = 0;
        return;
    }
    if (!event || event > 7)
        return;
    g.sound_events |= 1 << event; /* Existing audio-only coverage field. */
    if (duration && priorities[event] < priority)
        return;
    if (event == 1) {
        shot_divider = PLAYER_SHOT_ZAP_START_DIV;
        start(VOICE0, event, shot_divider, PLAYER_SHOT_ZAP_VOLUME, PLAYER_SHOT_ZAP_WAVEFORM);
        /* The start iteration consumes one tick before the first interval. */
        duration = PLAYER_SHOT_ZAP_FRAMES + 1;
    } else {
        start(VOICE0, event, tones[event], VOL_75, event == 2 || event == 4 ? WAV_NOISE : WAV_TRIANGLE);
        duration = event == 4 ? 30 : 8;
    }
    priority = priorities[event];
    if (event == 6)
        note = beat = done = 0;
}
void sound_tick(void)
{
    if (g.paused)
        return;
    if (master_muted) {
        zvb_sound_set_volume(VOL_50);
        master_muted = 0;
    }
    if (duration && --duration == 0) {
        hold(VOICE0, 1);
        priority = 0;
    }
    /* Priority 1 is exclusively the player shot. Higher-priority effects
       retain their original parameters and immediately stop this sweep. */
    if (priority == 1 && duration && duration < PLAYER_SHOT_ZAP_FRAMES) {
        shot_divider -= PLAYER_SHOT_ZAP_DROP_DIV;
        zvb_map_peripheral(ZVB_PERI_SOUND_IDX);
        zvb_peri_sound_select = VOICE0;
        zvb_peri_sound_freq_low = shot_divider;
        zvb_peri_sound_freq_high = shot_divider >> 8;
    }
    if (step_duration && --step_duration == 0)
        hold(VOICE2, 1);
    if (g.mode != PLAY && step_duration) {
        hold(VOICE2, 1);
        step_duration = 0;
    }
    if (transport_duration && --transport_duration == 0)
        hold(VOICE3, 1);
    if (g.mode == TRANSPORT) {
        /* Original $29CD/$2A9F transition noise spans entry and arrival.
         * Fresh pulse texture follows this port's documented 45-frame wait;
         * directed selection may wait indefinitely, so use a soft sparse tail. */
        if (++transport_clock == 8) {
            transport_clock = 0;
            start(VOICE3, 9, SOUND_FREQ_TO_DIV(380UL), VOL_25, WAV_NOISE);
            transport_duration = 2;
        }
    } else {
        transport_clock = 0;
        if (transport_duration) {
            hold(VOICE3, 1);
            transport_duration = 0;
        }
    }
    if ((g.mode == TITLE || g.mode == ENDING) && !done) {
        if (beat == 0) {
            /* Pausing an interrupted final title note clears beat, not note.
               Finish that tune instead of reading beyond the note table. */
            if (note >= sizeof(melody) / sizeof(melody[0])) {
                done = 1;
                hold(VOICE1, 1);
                return;
            }
            start(VOICE1, 11, melody[note], VOL_25, WAV_TRIANGLE);
            beat = note == 7 ? 24 : 12;
            ++note;
            g.sound_events |= 1;
        }
        if (--beat == 0 && note == 8) {
            done = 1;
            hold(VOICE1, 1);
        }
    } else
        hold(VOICE1, 1);
}
