/* SPDX-License-Identifier: GPL-3.0-only
 * Adapted from MIDI clock contributions by ChanceTheMaker and keremimo (2026).
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Audio ISR state: the external clock's transport and its tempo (fx.c beat_samples). Input timestamps keep USB/TRS
 * jitter outside the render loop. */
static struct {
    uint32_t last_ms, start_ms, tempo_ms;
    uint8_t mode, have_pulse, tempo_valid, tempo_n;
} midi_clock;

static __attribute__((noinline)) void midi_clock_transport(uint32_t status, uint32_t ms)
{
    if (status == 0xFAu || status == 0xFBu) {      /* Start, Continue */
        midi_clock.have_pulse = midi_clock.tempo_valid = 0;
        midi_clock.start_ms = ms;
        clock_start();
    } else if (status == 0xFCu) {
        clock_stop();
    }
}

static __attribute__((noinline)) void midi_clock_pulse(uint32_t ms)
{
    if (!midi_clock.tempo_valid) {
        midi_clock.tempo_valid = 1;
        midi_clock.tempo_ms = ms;
        midi_clock.tempo_n = 0;
    } else if (++midi_clock.tempo_n == 6u) {
        uint32_t dt = ms - midi_clock.tempo_ms;
        midi_clock.tempo_ms = ms;
        midi_clock.tempo_n = 0;
        /* Six clocks are a quarter of a beat. Reject gaps and corrupt bursts. */
        if (dt >= 62u && dt <= 375u) {
            midi_beat_samples = (uint32_t)FS * dt / 250u;
            song.g[G_BPM] = (int16_t)clamp((int32_t)((15000u + dt / 2u) / dt), 40, 240);
        }
    }
    if (song.playing)
        midi_clock.have_pulse = 1;
    midi_clock.last_ms = ms;
}

