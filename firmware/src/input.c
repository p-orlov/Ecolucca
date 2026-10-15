/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Keyboard, scale and MIDI IN: what plays the part (the keyboard and MIDI parts of the former seq.c).
 * Everything here runs in the audio ISR, once per CTL-sample block, and ends in trk_note_on / trk_note_off.
 * MIDI IN (G_ROUTE): channel 1 only, or every channel (ROUTE_SEL). Its CC1 (mod wheel), CC11 (expression) and
 * channel aftertouch go to the part's modulation matrix (mod.c). */

#define KB_SILENT 255u
static uint32_t kb_prev;
static volatile uint8_t kb_boot_hold;    /* #137: set from power-on to the end of the splash (main.c): the keys sound nothing,
                                          * and one held across its end stays silent until it is let go and pressed again */
static uint8_t kb_note[27], kb_trk[27];  /* per key: the note it started and on which part */
static uint8_t kb_down[27];              /* per key: its note sounds (key_on .. key_off) */
static uint8_t kb_vel = 100;             /* the keys' velocity */
static volatile uint8_t kb_asleep;       /* SCREEN OFF (ui_eco.c): a key pressed now only wakes the screen */
static uint8_t last_note = 60;
static volatile uint8_t panic_req;       /* bit per part: release every sounding note (a sound load) */
static volatile uint8_t midi_seen;       /* bit per part: a MIDI IN note-on played it since the UI last looked */

static uint32_t trk_index(const track_t *t) { return (uint32_t)(t - trk); }
static uint32_t trk_midi_ch(uint32_t i) { (void)i; return 0u; }   /* the keys -> MIDI OUT: channel 1 */

static uint32_t scale_mask(const track_t *t)
{
    return SCALE_MASK[clamp(t->p[P_SCALE], 0, sizeof SCALE_MASK / sizeof SCALE_MASK[0] - 1)];
}

/* note n moved down onto the part's scale (ROOT, SCALE) */
static int32_t scale_snap(const track_t *t, int32_t n)
{
    uint32_t mask = scale_mask(t), guard = 12;
    while (guard-- && !((mask >> (uint32_t)((n - t->p[P_ROOT] + 120) % 12)) & 1u))
        n--;
    return n;
}

enum { QN_OFF, QN_SNAP, QN_WHITE };     /* P_QUANT (its values 3 and 4, the sequencer's, play as SNAP) */

/* key k's note with the octave buttons at oct: QNT, ROOT, SCALE, TRN (KB_SILENT: a black key under WHITE) */
static uint32_t kb_pitch(const track_t *t, uint32_t k, int32_t oct)
{
    static const int8_t DEGREE[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};
    int32_t n = 53 + (int32_t)k;
    if (t->p[P_QUANT] == QN_SNAP || t->p[P_QUANT] > QN_WHITE)   /* SNAP: every key, rounded down */
        return (uint32_t)clamp(scale_snap(t, n + 12 * oct + t->p[P_TRANS]), 0, 127);
    if (t->p[P_QUANT] == QN_WHITE) {                    /* WHITE: white keys walk the scale, black keys are silent */
        uint32_t mask = scale_mask(t), i;
        int32_t count = 0, degree = DEGREE[n % 12], wo;
        if (degree < 0)
            return KB_SILENT;
        /* C4 is the root. Walk scale degrees on successive white keys, including
         * below C4; scales with 5, 6, 8 or 12 notes still have no duplicated degrees. */
        degree += (n / 12 - 5) * 7;
        for (i = 0; i < 12u; i++)
            count += (mask >> i) & 1u;
        wo = degree / count;
        degree %= count;
        if (degree < 0) {
            degree += count;
            wo--;
        }
        for (i = 0; i < 12u; i++)
            if ((mask >> i) & 1u) {
                if (!degree)
                    break;
                degree--;
            }
        n = 60 + t->p[P_ROOT] + 12 * wo + (int32_t)i;
    }
    return (uint32_t)clamp(n + 12 * oct + t->p[P_TRANS], 0, 127);
}

static uint32_t kb_map(const track_t *t, uint32_t k) { return kb_pitch(t, k, song.octave); }

static int midi_note_held(const track_t *t, uint32_t note);
static void input_on(track_t *t, uint32_t note, uint32_t vel)
{
    last_note = (uint8_t)note;
    if (eco_keys)
        eco_key(note, vel, 1);
    else
        trk_note_on(t, note, vel);
}
static void input_off(track_t *t, uint32_t note)
{
    if (midi_note_held(t, note))
        return;
    if (eco_keys)
        eco_key(note, 0, 0);
    else
        trk_note_off(t, note);
}

/* a key held on part t sounds note */
static int midi_local_held(const track_t *t, uint32_t note)
{
    uint32_t k;
    for (k = 0; k < 27u; k++)
        if (kb_down[k] && kb_trk[k] == trk_index(t) && kb_note[k] == note)
            return 1;
    return 0;
}

/* key k plays kb_note[k] on part t. A note another key holds already sounds: it is not started again (nor sent
 * to MIDI OUT) */
static void key_on(uint32_t k, track_t *t)
{
    uint32_t x = kb_note[k], mc = trk_midi_ch(trk_index(t)), vel = kb_vel;
    if (!midi_local_held(t, x)) {
        input_on(t, x, vel);
        midi_out_event(0x09u | (0x90u | mc) << 8 | x << 16 | vel << 24);
    }
    kb_down[k] = 1;
}

/* key k is up: its note ends, unless another key still holds it */
static void key_off(uint32_t k, track_t *t)
{
    uint32_t x = kb_note[k], mc = trk_midi_ch(trk_index(t));
    kb_down[k] = 0;
    if (midi_local_held(t, x))
        return;
    input_off(t, x);
    midi_out_event(0x08u | (0x80u | mc) << 8 | x << 16);
}

#ifdef FM1_INPUT_LAT
/* the press latency stats (hal/fm1_input.h fm1_in_stat, console `inp`): from the first scan that saw
 * key k closed to its note-on here, and to the note's first sample leaving the DMA (kb_out_tick:
 * audio.c, the TIMER4 tick at which the block being rendered starts to play) */
static uint32_t kb_out_tick;
static __attribute__((noinline)) void kb_lat(uint32_t k)
{
    uint32_t t0 = fm1_in.note_t0[k], d = fm1_ticks() - t0, o = kb_out_tick - t0;
    fm1_in_stat.kb_n++;
    fm1_in_stat.kb_sum += d;
    fm1_in_stat.dac_sum += o;
    if (d > fm1_in_stat.kb_max)
        fm1_in_stat.kb_max = d;
    if (o > fm1_in_stat.dac_max)
        fm1_in_stat.dac_max = o;
}
#endif

static void keyboard_block(void)
{
    uint32_t cur = fm1_in.notes, ch = cur ^ kb_prev, k;
    if (!ch)
        return;
    if (kb_boot_hold) {                           /* #137: under the splash; their release is silent too */
        for (k = 0; k < 27u; k++)
            if ((ch & cur) >> k & 1u)
                kb_note[k] = KB_SILENT;
        kb_prev = cur;
        return;
    }
    for (k = 0; k < 27u; k++) {
        if (!((ch >> k) & 1u))
            continue;
        if ((cur >> k) & 1u) {                    /* the selected part; the key-up goes to the same one */
            kb_trk[k] = song.sel;
            if (kb_asleep) {                      /* (SCREEN OFF: the wake press is swallowed) */
                kb_note[k] = KB_SILENT;
                continue;
            }
            kb_note[k] = (uint8_t)kb_map(&trk[kb_trk[k]], k);
            if (kb_note[k] == KB_SILENT)
                continue;
            key_on(k, &trk[kb_trk[k]]);
#ifdef FM1_INPUT_LAT
            kb_lat(k);
#endif
        } else {
            if (kb_note[k] == KB_SILENT || !kb_down[k])
                continue;
            key_off(k, &trk[kb_trk[k] % NTRK]);
        }
    }
    kb_prev = cur;
}

/* the external clock's transport (midi_clock.c): it sets the tempo the LFO SYNC, ENV SYNC and the delay follow */
static void clock_start(void)
{
    song.tick = 0;
    song.playing = 1;
}
static void clock_stop(void) { song.playing = 0; }

/* MIDI in: the part a channel plays, -1 none */
static uint8_t midi_route;                 /* the G_ROUTE events_block last saw (a change: midi_route_block) */
static int32_t midi_slot(uint32_t ch) { return song.g[G_ROUTE] == ROUTE_SEL || ch == 0u ? 0 : -1; }
static int midi_in_block(uint32_t ch) { return midi_slot(ch) >= 0; }
static track_t *midi_track(uint32_t ch)
{
    (void)ch;
    return TSEL;
}

#include "midi_control.c"
#include "midi_clock.c"

/* everything that happens between two rendered blocks */
static void events_block(uint32_t n)
{
    uint32_t i, pr;
    uint32_t clock_mode = (uint32_t)song.g[G_CLOCK];
    if (midi_clock.mode != clock_mode) {
        clock_stop();
        memset(&midi_clock, 0, sizeof midi_clock);
        midi_clock.mode = (uint8_t)clock_mode;
        midi_beat_samples = 0;
    }
    if (midi_route != (uint8_t)song.g[G_ROUTE]) {     /* MIDI IN changed: a block lets go of the other channels */
        midi_route = (uint8_t)song.g[G_ROUTE];
        if (midi_route != ROUTE_SEL)
            midi_route_block();
    }
    pr = panic_req;
    panic_req = 0;
    if (midi_in_overflow) {                            /* a lost note-off must never leave a held note */
        mi_r = mi_w;
        memset(midi_sel_on, 0, sizeof midi_sel_on);
        memset(midi_ch, 0, sizeof midi_ch);
        memset(midi_owners, 0, sizeof midi_owners);
        pr |= (1u << NTRK) - 1u;
        RING_PUBLISH();
        midi_in_overflow = 0;
    }
    for (i = 0; i < NTRK; i++) {
        track_t *t = &trk[i];
        if ((pr >> i) & 1u) {
            midi_forget_track(i);
            trk_all_off(t);
            eco_keys_up();
        }
        engine_block(t);                              /* engine switch: fade, then switch (voice.c) */
    }
    keyboard_block();
    while (mi_r != mi_w) {                            /* USB-MIDI (and TRS) in */
        uint32_t at = mi_r % MQ, pkt = midi_in_q[at], status = (pkt >> 8) & 0xFFu;
        uint32_t st = status & 0xF0u, ch = status & 0x0Fu;
        uint32_t d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        uint32_t source = midi_in_source[at] ? midi_in_source[at] : 1u, ms = midi_in_ms[at];
        mi_r++;
        if (status >= 0xF8u) {
            if (clock_mode == source) {
                if (status == 0xF8u)
                    midi_clock_pulse(ms);
                else
                    midi_clock_transport(status, ms);
            }
        } else
            midi_event(st, ch, d1, d2);
    }
    eco_block(n);
    if (clock_mode && song.playing) {
        uint32_t last = midi_clock.have_pulse ? midi_clock.last_ms : midi_clock.start_ms;
        if (fm1_ms - last > 500u) {
            clock_stop();                             /* cable loss: the transport stops */
            midi_clock.tempo_valid = 0;
        }
    }
    if (song.playing)
        song.tick++;
}
