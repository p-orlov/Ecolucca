/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Engine table: ANALOG, the one engine. */
#include "dsp.c"

#include "eng_analog.c"

static const engine_t *const ENGINES[NENGINES] = {
    &ENG_ANALOG,                 /* 0 */
};

/* a part's engine number as an index (the audio paths: a compare, cheaper than % NENGINES; a bad number: 0) */
static inline uint32_t eng_idx(uint32_t e) { return e < NENGINES ? e : 0u; }

#define PRESET_DEF 1u            /* ANALOG's SOFT PAD: the sound at power-on (without an autosave) */

