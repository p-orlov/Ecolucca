/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* The UI (main loop only). The screen, top to bottom: a header (the page, the octave, the cells living, the voices
 * sounding, the CPU load, the tempo), the field (the ecosystem's 5 x 5 niches: stereo position across, pitch up,
 * shaded by their energy; the cells as dots, size their mass, colour their pitch (a rainbow), brightness their
 * energy, a ring while a note sounds; the notes held as bars) and the editor (four values on KNOB 1..4, the pages, the
 * preset). GLO opens the menu over the field and the editor.
 * Controls: SELECT the tempo (in the menu its row), PRESETS the preset, KNOB 1..4 the values (in the menu KNOB 1 the
 * row's value), GLO the menu (HOME closes it), EDIT ENV LFO FX SCL ARP their pages (a tap again: the next of its
 * pages; SCL ARP: the ecosystem's regulators), PLAY a tap pauses the ecosystem / lets it go on, a hold clears it,
 * OCT- / OCT+ the octave. */

/* ----------------------------------------------------------- state --- */
static struct {
    uint8_t force;               /* redraw everything */
    uint8_t uboot;               /* main.c: the UBOOT countdown, seconds left (0 none) */
    uint8_t page;                /* UI_PAGES */
    uint8_t menu, mrow;          /* the menu open, its row */
    uint8_t serial;              /* USB SERIAL changed in the menu: applied as it closes (one re-enumeration) */
    uint8_t hot;                 /* the knob turned last + 1 (0 none), until hot_t + HOT_MS */
    uint32_t hot_t;
    char msg[24];                /* ui_message: in the header until msg_t + MSG_MS */
    uint32_t msg_t;
} ui;
#define HOT_MS 1500u
#define MSG_MS 1500u

static void ui_message(const char *s)
{
    str_cpy(ui.msg, s, sizeof ui.msg);
    ui.msg_t = fm1_ms | 1u;
}

static void str_cat(char *d, const char *s, unsigned max)
{
    unsigned n = str_len(d);
    if (n < max)
        str_cpy(d + n, s, max - n);
}

/* ------------------------------------------------------- the sound --- */
/* a preset load keeps these (the part's place in the mix and its scale) */
static int sound_kept(uint32_t id)
{
    return id == P_LEVEL || id == P_PAN || id == P_ROOT || id == P_SCALE || id == P_QUANT || id == P_TRANS;
}

static void track_defaults(track_t *t)
{
    uint32_t i;
    for (i = 0; i < P_E0; i++)
        t->p[i] = TP[i].def;
    for (i = 0; i < 8u; i++)
        t->p[P_E0 + i] = ENGINES[0]->edit[i].def;
}

/* preset pi of ANALOG: the sound only (not sound_kept); its notes released */
static void sound_preset(track_t *t, uint32_t pi)
{
    static const uint8_t FX_DEF[4] = {0, 24, 28, 36};
    const engine_t *e = ENGINES[0];
    const preset_t *pr = &e->presets[pi % e->npresets];
    uint32_t i;
    fm1_irq_off();                                     /* the audio ISR must not see half a sound */
    t->eng_req = 0;
    t->preset = (uint8_t)(pi % e->npresets);
    for (i = 0; i < P_E0; i++)
        if (!sound_kept(i))
            t->p[i] = TP[i].def;
    for (i = 0; i < 8u; i++)
        t->p[P_E0 + i] = pr->e[i];
    t->p[P_ATK] = pr->env[0];
    t->p[P_DEC] = pr->env[1];
    t->p[P_SUS] = pr->env[2];
    t->p[P_REL] = pr->env[3];
    t->p[P_ED_FLT] = pr->fenv;
    t->p[P_VOICE] = pr->mono ? V_LEGATO : V_POLY;      /* mono presets keep the legato feel */
    for (i = 0; i < 4u; i++)                           /* the sends */
        t->p[P_DIST + i] = (int16_t)(pr->fx[i] ? pr->fx[i] - 1 : FX_DEF[i]);
    t->p[P_DIST] = 0;                                  /* (no DIST: the cells' voices bypass it) */
    fm1_irq_on();
    panic_req |= (uint8_t)(1u << trk_index(t));        /* MONO / POLY may change: release what sounds */
}

/* ----------------------------------------------------- the pages --- */
#define PG(g) (0x80u | (g))                            /* a page's global parameter (song.g) */
typedef struct {
    const char *title;
    uint8_t btn;                 /* the button that opens it (panel.c B_*) */
    uint8_t id[4];               /* P_* or PG(G_*) on KNOB 1..4, 0xFF none */
} ui_page_t;
static const ui_page_t UI_PAGES[] = {
    {"OSC", B_EDIT, {P_E0, P_E1, P_E2, P_E3}},             /* WAVE DTN MIX NOIS */
    {"FILTER", B_EDIT, {P_FTYPE, P_E4, P_E5, P_E7}},       /* TYPE CUT RES KTR */
    {"VOICE", B_EDIT, {P_VOICE, P_GLIDE, P_SPRD, P_LEVEL}},
    {"ENV", B_ENV, {P_ATK, P_DEC, P_SUS, P_REL}},
    {"ENV MOD", B_ENV, {P_ED_FLT, P_ED_PIT, P_ED_SHP, P_E6}},   /* ENV -> FLT PIT SHP, and the drive */
    {"LFO", B_LFO, {P_LRATE, P_LWAVE, P_LD_PIT, P_LD_FLT}},
    {"FX", B_FX, {P_CHOR, P_DLY, P_REV, 0xFF}},
    {"ECO", B_SCL, {PG(G_TEMP), PG(G_FOOD), PG(G_DENS), PG(G_LIFE)}},
    {"PREDATORS", B_SCL, {PG(G_PRED), 0xFF, 0xFF, 0xFF}},
    {"EVOLVE", B_ARP, {PG(G_MUT), PG(G_MOVE), PG(G_DECAY), PG(G_SLIDE)}},
    {"RHYTHM", B_ARP, {PG(G_RATE), PG(G_BREAK), PG(G_CHAOS), 0xFF}},
    {"RANGE", B_ARP, {PG(G_LOW), PG(G_HIGH), PG(G_ESCALE), 0xFF}},
};
#define NUI_PAGES NELEM(UI_PAGES)

static int16_t *page_val(uint32_t id) { return id & 0x80u ? &song.g[id & 0x7Fu] : &TSEL->p[id]; }
static const param_desc_t *page_desc(uint32_t id) { return id & 0x80u ? &GP[id & 0x7Fu] : track_desc(TSEL, id); }

static void page_button(uint32_t b)                    /* its first page, a tap again the next of its pages */
{
    uint32_t i, n = NUI_PAGES;
    for (i = 1; i <= n; i++) {
        uint32_t p = (UI_PAGES[ui.page].btn == b ? ui.page + i : i - 1u) % n;
        if (UI_PAGES[p].btn == b) {
            ui.page = (uint8_t)p;
            break;
        }
    }
    ui.menu = 0;
    ui.force = 1;
}

static void knob_turn(uint32_t k, int32_t s)
{
    track_t *t = TSEL;
    uint32_t id = UI_PAGES[ui.page].id[k];
    if (id == 0xFFu)
        return;
    *page_val(id) = (int16_t)param_turn(page_desc(id), *page_val(id), s);
    if (id == P_VOICE)
        panic_req |= (uint8_t)(1u << trk_index(t));    /* (MONO / POLY: no voice left from the other mode) */
    ui.hot = (uint8_t)(k + 1u);
    ui.hot_t = fm1_ms;
}

/* ------------------------------------------------------ SCREEN OFF --- */
/* the stored code of NEVER 5 15 30 60 MIN, as the earlier firmware wrote it (its 1.1.5 codes read back too) */
static const uint8_t SCR_CODE[5] = {0, 2, 1, 4, 7};
static const uint8_t SCR_DEC[8] = {0, 2, 1, 0, 3, 0, 0, 4};
static const uint8_t SCR_MIN[5] = {0, 5, 15, 30, 60};
static const char *const SCR_NAME[5] = {"NEVER", "5 MIN", "15 MIN", "30 MIN", "60 MIN"};
enum { SCR_ON, SCR_OFF, SCR_WAKE, SCR_SLPOUT, SCR_SHOW };
static struct {
    uint8_t st;                  /* SCR_*: SCR_WAKE asked, SCR_SLPOUT sent, SCR_SHOW the frame that redraws */
    uint8_t eat;                 /* the wake gesture: panel input swallowed until all is let go */
    uint32_t idle;               /* fm1_ms of the last panel input */
    uint32_t t;                  /* fm1_ms of the last panel command (SLPIN, SLPOUT) */
} scrn;
static uint32_t scr_get(void) { return ui_scr < NELEM(SCR_DEC) ? SCR_DEC[ui_scr] : 0u; }
static void scr_wake_now(void)                         /* main.c: the crash screen, UPDATE, UBOOT */
{
    if (scrn.st != SCR_ON)
        lcd_wake_now();
    scrn.st = SCR_ON;
    scrn.idle = fm1_ms;
    kb_asleep = 0;
}
/* every pass: 1 = the pass is swallowed (the screen dark or waking, or the wake gesture going on) */
static int scr_input(uint32_t pressed, uint32_t notes)
{
    uint32_t held = fm1_in.buttons | fm1_in.notes, k;
    int32_t moved = 0;
    if (pressed | notes | held | panel_moved)
        scrn.idle = fm1_ms;
    panel_moved = 0;
    if (midi_seen) {                                   /* a MIDI note keeps it on, wakes it; nothing swallowed */
        midi_seen = 0;
        scrn.idle = fm1_ms;
        if (scrn.st == SCR_OFF)
            scrn.st = SCR_WAKE;
    }
    if (scrn.st == SCR_ON && !scrn.eat)
        return 0;
    for (k = 0; k < NE; k++)                           /* (every knob's turns go) */
        moved |= panel_enc(k);
    panel_moved = 0;
    if ((pressed | notes | held | (uint32_t)moved) && scrn.st == SCR_OFF)
        scrn.st = SCR_WAKE;
    if (scrn.st != SCR_ON || held || moved)
        return 1;
    scrn.eat = 0;                                      /* all let go: input again */
    kb_asleep = 0;
    return 0;
}
/* every frame: 1 = draw it */
static int scr_frame(void)
{
    uint32_t lim = SCR_MIN[scr_get()] * 60000u;
    if (ui.uboot) {                                    /* (never dark under the countdown) */
        scrn.idle = fm1_ms;
        if (scrn.st == SCR_OFF)
            scrn.st = SCR_WAKE;
    }
    switch (scrn.st) {
    case SCR_ON:
        if (!lim || (int32_t)(fm1_ms - scrn.idle) < (int32_t)lim)
            return 1;
        lcd_power(0);
        scrn.st = SCR_OFF;
        scrn.t = fm1_ms;
        scrn.eat = 1;
        kb_asleep = 1;
        break;
    case SCR_WAKE:
        if (fm1_ms - scrn.t >= 120u) {
            lcd_power(1);
            scrn.st = SCR_SLPOUT;
            scrn.t = fm1_ms;
        }
        break;
    case SCR_SLPOUT:
        if (fm1_ms - scrn.t >= 120u) {
            scrn.st = SCR_SHOW;
            ui.force = 1;
            return 1;
        }
        break;
    case SCR_SHOW:
        return 1;
    }
    return 0;
}
static void scr_shown(void)                            /* after the frame: the redrawn page on */
{
    if (scrn.st != SCR_SHOW)
        return;
    lcd_power(2);
    scrn.st = SCR_ON;
    scrn.idle = fm1_ms;
}

/* ------------------------------------------------------- settings --- */
static void palette_apply(void) { palette_set(settings.palette % NPALETTES); }   /* (panel.c settings_init) */

/* MENU > USB SERIAL: OFF re-enumerates as audio + MIDI only (usb.c usb_cdc_switch); at boot before USB starts
 * (main.c) and as the menu closes */
static void usb_serial_apply(void)
{
#if FELUCCA_CDC
    usb_cdc_switch(FELUCCA_CDC_DEFAULT && !(ui_prefs & PREF_SERIAL_OFF));
#endif
}

/* ----------------------------------------------------------- menu --- */
enum { MR_KEYS, MR_MODE, MR_ROOT, MR_MIDI, MR_CLOCK, MR_BPM, MR_TUNE, MR_DTIME, MR_DFDBK, MR_RTYPE, MR_RSIZE, MR_COLOR,
       MR_LEDS, MR_SCROFF, MR_SPKEQ, MR_SERIAL, MR_COUNT };
static const char *const MENU_LABEL[MR_COUNT] = {
    "KEYS", "MODE", "SCALE ROOT", "MIDI IN", "CLOCK", "TEMPO", "TUNE", "DELAY TIME", "DELAY FDBK", "REVERB",
    "REVERB SIZE", "COLOR", "LEDS", "SCREEN OFF", "SPEAKER EQ", "USB SERIAL"};
static const uint8_t MENU_G[MR_COUNT] = {0xFF, G_EMODE, G_EROOT, G_ROUTE, G_CLOCK, G_BPM, G_TUNE, G_DTIME, G_DFDBK,
                                         G_RTYPE, G_RSIZE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};   /* the song's globals, 0xFF a setting */
static const char *const SPK_EQ[3] = {"FLAT", "LOWCUT", "BASS+"};
static const char *const ON_OFF[2] = {"OFF", "ON"};

static void menu_value(uint32_t row, char *b, unsigned max)
{
    uint32_t g = MENU_G[row];
    if (g != 0xFFu) {
        const char *unit;
        char v[12];
        param_format(&GP[g], song.g[g], v, &unit);
        str_cpy(b, v, max);
        if (g == G_TUNE)
            unit = "ct";
        if (unit[0]) {
            str_cat(b, " ", max);
            str_cat(b, unit, max);
        }
        return;
    }
    str_cpy(b, row == MR_KEYS ? (eco_keys ? "ECO" : "PLAY") :
               row == MR_COLOR ? UI_PALETTES[settings.palette % NPALETTES].name : row == MR_LEDS ? LEDS_NAME[settings_leds % LEDS_COUNT] :
               row == MR_SCROFF ? SCR_NAME[scr_get()] : row == MR_SPKEQ ? SPK_EQ[settings.lowcut % 3u] :
               ON_OFF[!(ui_prefs & PREF_SERIAL_OFF)], max);
}

static void menu_turn(uint32_t row, int32_t s)
{
    uint32_t g = MENU_G[row];
    if (g != 0xFFu) {
        song.g[g] = (int16_t)param_turn(&GP[g], song.g[g], s);
        return;
    }
    switch (row) {
    case MR_KEYS:                                      /* ECO: the keys seed and feed; PLAY: they play the sound */
        eco_keys ^= 1u;
        panic_req |= 1u;
        return;
    case MR_COLOR:
        settings.palette = (uint32_t)((int32_t)(settings.palette % NPALETTES) + (s > 0 ? 1 : (int32_t)NPALETTES - 1)) %
                           NPALETTES;
        palette_apply();
        ui.force = 1;
        break;
    case MR_LEDS:
        settings_leds = (uint8_t)leds_step(settings_leds, s);
        break;
    case MR_SCROFF:
        ui_scr = SCR_CODE[clamp((int32_t)scr_get() + s, 0, 4)];
        scrn.idle = fm1_ms;
        break;
    case MR_SPKEQ:
        settings.lowcut = (uint32_t)clamp((int32_t)settings.lowcut + s, 0, 2);
        fx_lowcut = (uint8_t)settings.lowcut;
        break;
    case MR_SERIAL:
        ui_prefs ^= PREF_SERIAL_OFF;
        ui.serial = 1;
        break;
    }
    settings_save();
}

static void menu_close(void)
{
    ui.menu = 0;
    ui.force = 1;
    if (ui.serial) {
        ui.serial = 0;
        usb_serial_apply();
    }
}

/* ---------------------------------------------------------- input --- */
static void button_press(uint32_t b)
{
    switch (b) {
    case B_GLO:
        if (ui.menu)
            menu_close();
        else {
            ui.menu = 1;
            ui.force = 1;
        }
        break;
    case B_HOME:
        if (ui.menu)
            menu_close();
        break;
    case B_EDIT: case B_ENV: case B_LFO: case B_FX: case B_SCL: case B_ARP:
        page_button(b);
        break;
    case B_OCTDN: case B_OCTUP: {
        uint32_t other = panel.btn[b == B_OCTDN ? B_OCTUP : B_OCTDN];
        if ((fm1_in.buttons >> other) & 1u)            /* both held: main.c's UBOOT gesture, no octave */
            break;
        song.octave = (int8_t)clamp(song.octave + (b == B_OCTUP ? 1 : -1), -3, 3);
        break;
    }
    }
}

/* PLAY: a tap pauses the ecosystem or lets it go on, a hold of a second clears it (eco.c) */
static void play_button(uint32_t pressed, uint32_t rel)
{
    static uint32_t t0;                                /* fm1_ms of the press, 0 none or the hold done */
    uint32_t bit;
    if (panel.btn[B_PLAY] >= 32u)
        return;
    bit = 1u << panel.btn[B_PLAY];
    if (pressed & bit)
        t0 = fm1_ms | 1u;
    if (t0 && (fm1_in.buttons & bit) && fm1_ms - t0 >= 1000u) {
        t0 = 0;
        eco_clear = 1;
        ui_message("CLEARED");
    }
    if (t0 && (rel & bit)) {
        t0 = 0;
        eco_paused ^= 1u;
        ui_message(eco_paused ? "PAUSED" : "PLAY");
    }
}

static void ui_input(void)
{
    uint32_t rel, pressed = fm1_input_edges(&rel), notes = fm1_input_note_edges(), id;
    int32_t s, k;
    track_t *t = TSEL;
    if (scr_input(pressed, notes))
        return;
    for (id = 0; pressed >> id; id++)
        if ((pressed >> id) & 1u)
            button_press(panel_btn_of(id));
    play_button(pressed, rel);
    s = panel_enc(EN_SELECT);
    if (s) {
        if (ui.menu)
            ui.mrow = (uint8_t)clamp((int32_t)ui.mrow + s, 0, MR_COUNT - 1);
        else
            song.g[G_BPM] = (int16_t)param_turn(&GP[G_BPM], song.g[G_BPM], s);
    }
    s = panel_enc(EN_PRESET);
    if (s && !ui.menu) {
        int32_t n = ENGINES[0]->npresets;
        sound_preset(t, (uint32_t)(((int32_t)t->preset + s % n + n) % n));
        ui_message(ENGINES[0]->presets[t->preset].name);
    }
    (void)panel_enc(EN_ALGO);                          /* (no role yet: its turns go) */
    for (k = 0; k < 4; k++) {
        s = panel_enc((uint32_t)(EN_K1 + k));
        if (!s)
            continue;
        if (!ui.menu)
            knob_turn((uint32_t)k, s);
        else if (!k)
            menu_turn(ui.mrow, s);
    }
}

/* ----------------------------------------------------------- LEDs --- */
static uint8_t led_pos[FM1_NKEY];                      /* each LED: column << 3 | row, 0xFF none */
static void led_init(void)
{
    uint32_t id, c, r;
    for (id = 0; id < FM1_NKEY; id++)
        led_pos[id] = 0xFF;
    for (c = 0; c < FM1_NCOL; c++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][c] >= 0 && FM1_KEYMAP[r][c] < (int8_t)FM1_NKEY)
                led_pos[FM1_KEYMAP[r][c]] = (uint8_t)(c << 3 | r);
}
static void led_put(uint8_t *a, uint32_t id)
{
    uint8_t q = id < FM1_NKEY ? led_pos[id] : 0xFFu;
    if (q != 0xFFu)
        a[q >> 3] |= (uint8_t)(1u << (q & 7u));
}

/* lit: the page's button (GLO in the menu), OCT- / OCT+ off the middle octave, the keys held. MENU > LEDS: DIM HI /
 * DIM LO the others glow, OFF they are dark, INV turns it around */
static void ui_leds(void)
{
    uint8_t nl[FM1_NCOL] = {0}, nd[FM1_NCOL] = {0};
    uint32_t k, c, mode = settings_leds;
    static uint8_t ready;
    if (fm1_led_anim_on())                             /* the power-on sweep has them (main.c boot_leds) */
        return;
    if (!ready) {
        led_init();
        ready = 1;
    }
    led_put(nl, panel.btn[ui.menu ? B_GLO : UI_PAGES[ui.page].btn]);
    if (!eco_paused)
        led_put(nl, panel.btn[B_PLAY]);
    if (song.octave < 0)
        led_put(nl, panel.btn[B_OCTDN]);
    if (song.octave > 0)
        led_put(nl, panel.btn[B_OCTUP]);
    for (k = 0; k < 27u; k++) {
        if ((fm1_in.notes >> k) & 1u)
            led_put(nl, 14u + k);
        led_put(nd, 14u + k);
    }
    for (k = 0; k < NB; k++)
        led_put(nd, panel.btn[k]);
    for (c = 0; c < FM1_NCOL; c++) {
        if (mode == LEDS_INV) {
            nl[c] = (uint8_t)(nd[c] & ~nl[c]);
            nd[c] = 0;
        } else if (mode == LEDS_OFF)
            nd[c] = 0;
        else
            nd[c] &= (uint8_t)~nl[c];
    }
    fm1_led_dim_level(mode == LEDS_DIM_LO);
    for (c = 0; c < FM1_NCOL; c++) {
        fm1_led_dim[c] = nd[c];
        fm1_led_breath[c] = 0;
        fm1_led_mid[c] = 0;
    }
    for (c = 0; c < FM1_NCOL; c++)
        fm1_led[c] = nl[c];
}

/* -------------------------------------------------------- drawing --- */
#define HDR_H 20
#define FIELD_Y HDR_H
#define FIELD_H 124                                    /* (gfx.c CV_MAX: a 240-wide canvas at most 124 rows) */
#define EDIT_Y (FIELD_Y + FIELD_H)
#define EDIT_H (240 - EDIT_Y)
#define FIELD_LO (ECO_LO_NOTE * 16)                    /* C2 .. C7: the colours' scale, 1/16 semitone */
#define FIELD_SPAN ((int32_t)ECO_COLS * 12 * 16)
#define FIELD_MS 40u                                   /* the field's frame rate: 25 / s */

static int32_t field_x(int32_t pan) { return 120 + pan * (120 - 6) / 64; }   /* L at the left */
static int32_t field_y(int32_t p16)
{
    int32_t lo = eco.span16 ? eco.lo16 : FIELD_LO, span = eco.span16 ? eco.span16 : FIELD_SPAN;   /* (eco_range) */
    return FIELD_H - 1 - clamp((p16 - lo) * (FIELD_H - 1) / span, 0, FIELD_H - 1);
}
/* a pitch's colour: a rainbow from violet (C2) to red (C7) */
static uint16_t field_hue(int32_t p16)
{
    int32_t h = 1151 - clamp((p16 - FIELD_LO) * 1151 / FIELD_SPAN, 0, 1151), f = h & 255;
    switch (h >> 8) {
    case 0: return RGB(255, f, 0);                     /* red -> yellow */
    case 1: return RGB(255 - f, 255, 0);               /* -> green */
    case 2: return RGB(0, 255, f);                     /* -> cyan */
    case 3: return RGB(0, 255 - f, 255);               /* -> blue */
    default: return RGB(f, 0, 255);                    /* -> violet */
    }
}

static void cv_disc(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t dy, dx;
    for (dy = -r; dy <= r; dy++) {
        for (dx = r; dx > 0 && dx * dx + dy * dy > r * r; dx--)
            ;
        cv_rect(cx - dx, cy + dy, 2 * dx + 1, 1, c);
    }
}

static void draw_splash(void)
{
    lcd_fill(0, 0, 240, 240, T_BG);
    draw_text_box(0, 100, 240, &AF_M, "FELUCCA", T_TEXT, 1);
    draw_text_box(0, 124, 240, &AF_S, FELUCCA_VERSION, T_MID, 1);
}

static uint32_t cells_living(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < ECO_CELLS; i++)
        n += eco.cell[i].id != 0;
    return n;
}

static uint32_t voices_sounding(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += trk[0].v[i].active;
    return n;
}

static void draw_header(void)
{
    static uint32_t peak, late, late_t;                /* the peak load held (/256); audio.c's late halves, the last */
    char b[40], n[12];
    const char *title = ui.menu ? "MENU" : UI_PAGES[ui.page].title;
    uint32_t p = audio_peak_q8, hot;
    audio_peak_q8 = 0;
    peak = p > peak - peak / 8u ? p : peak - peak / 8u;
    if (felucca_dbg.late != late) {
        late = felucca_dbg.late;
        late_t = fm1_ms | 1u;
    }
    hot = peak >= 230u || (late_t && fm1_ms - late_t < 2000u);   /* 90 % or a half rendered late: audible */
    if (ui.msg_t && fm1_ms - ui.msg_t < MSG_MS)
        title = ui.msg;
    else
        ui.msg_t = 0;
    cv_begin(240, HDR_H, T_BG);
    cv_text_fit(6, CAP_IN(M, HDR_H), &AF_M, title, T_TEXT, T_BG, 110);
    b[0] = 0;
    if (song.octave) {
        str_cat(b, "OCT", sizeof b);
        if (song.octave > 0)
            str_cat(b, "+", sizeof b);
        fmt_int(n, song.octave);
        str_cat(b, n, sizeof b);
        str_cat(b, " ", sizeof b);
    }
    fmt_int(n, (int32_t)cells_living());
    str_cat(b, n, sizeof b);
    str_cat(b, "C ", sizeof b);
    fmt_int(n, (int32_t)voices_sounding());
    str_cat(b, n, sizeof b);
    str_cat(b, "V ", sizeof b);
    fmt_int(n, (int32_t)(song.cpu_q8 * 100u / 256u));  /* the CPU: average / peak */
    str_cat(b, n, sizeof b);
    str_cat(b, "/", sizeof b);
    fmt_int(n, (int32_t)(peak * 100u / 256u));
    str_cat(b, n, sizeof b);
    str_cat(b, late_t && fm1_ms - late_t < 2000u ? "%! " : "% ", sizeof b);
    fmt_int(n, song.g[G_BPM]);
    str_cat(b, n, sizeof b);
    cv_text_r(234, CAP_IN(S, HDR_H), &AF_S, b, hot ? T_ACCENT : T_MID, T_BG);
    cv_rect(0, HDR_H - 1, 240, 1, T_LINE);
    cv_blit(0, 0);
}

/* the field: the niches (shaded by their energy), the notes held (keys, MIDI IN) as bars, the cells (size: mass,
 * colour: pitch, brightness: energy, a ring while their note's gate is open), and the keys' own notes (MENU > KEYS
 * PLAY) */
static uint32_t field_age[NVOICE], field_flash[NVOICE];
static void draw_field(void)
{
    const track_t *t = &trk[0];
    int32_t pan = t->p[P_PAN], spr = (t->p[P_SPRD] + 1) >> 1, n;
    uint32_t i, ch, x, y;
    cv_begin(240, FIELD_H, T_BG);
    for (x = 0; x < ECO_COLS; x++)                     /* a niche column (an octave): a band of the screen, C2 lowest */
        for (y = 0; y < ECO_ROWS; y++) {
            int32_t e = clamp(eco.niche[x][y].E * 100 / EQ(400), 0, 100);
            int32_t x0 = (int32_t)y * 240 / (int32_t)ECO_ROWS, y0 = (int32_t)(ECO_COLS - 1u - x) * FIELD_H / (int32_t)ECO_COLS;
            int32_t x1 = (int32_t)(y + 1u) * 240 / (int32_t)ECO_ROWS, y1 = (int32_t)(ECO_COLS - x) * FIELD_H / (int32_t)ECO_COLS;
            if (e)
                cv_rect(x0, y0, x1 - x0, y1 - y0, ux_mix(T_BG, T_TINT, e));
            for (i = 0; i < eco.frag[x][y].n; i++)     /* a genome fragment: a dot a trait */
                cv_rect(x0 + 3 + (int32_t)i * 4, y1 - 5, 2, 2, T_MID);
            if (y)
                cv_rect(x0, 0, 1, FIELD_H, T_LINE);
            if (x)
                cv_rect(0, y1, 240, 1, T_LINE);
        }
    for (i = 0; i < 27u; i++)
        if (kb_down[i])
            cv_rect(0, field_y(kb_note[i] * 16) - 1, 240, 3, T_SEL);
    for (ch = 0; ch < 16u; ch++)
        for (n = 0; n < 128; n++)
            if (midi_notes[ch][n])
                cv_rect(0, field_y(n * 16) - 1, 240, 3, T_SEL);
    for (i = 0; i < ECO_CELLS; i++) {
        const cell_t *c = &eco.cell[i];
        int32_t r, cx, cy;
        if (!c->id)
            continue;
        cx = field_x(c->g.pan);
        cy = field_y(eco_y16(c));
        r = eco_radius(c);
        if (c->open)
            cv_disc(cx, cy, r + 2, T_ACCENT);
        cv_disc(cx, cy, r, ux_mix(T_BG, field_hue(c->hue), 30 + clamp(c->e, 0, ECO_E_FOUND) * 70 / ECO_E_FOUND));
        if (c->g.pred >= 64u) {                        /* a predator: hollow (the more the wider), a red eye */
            cv_disc(cx, cy, r * c->g.pred / 384, T_BG);
            cv_disc(cx, cy, 1, RGB(255, 40, 40));
        }
    }
    for (i = 0; i < NVOICE; i++) {
        const voice_t *v = &t->v[i];
        int32_t lvl, vx, vy, r;
        if (!v->active || v->p)
            continue;
        if (v->age != field_age[i]) {                  /* a new note on this voice: its flash */
            field_age[i] = v->age;
            field_flash[i] = fm1_ms;
        }
        lvl = clamp(v->env_out, 0, 32767);
        vx = field_x(spr ? clamp(pan + (v->side ? spr : -spr), -64, 63) : pan);
        vy = field_y(v->pitch_cur);
        r = 2 + lvl * 6 / 32767;
        if (fm1_ms - field_flash[i] < 120u)
            cv_disc(vx, vy, r + 3, T_ACCENT);
        cv_disc(vx, vy, r, ux_mix(T_BG, v->gate ? field_hue(v->pitch_cur) : T_MID, 30 + lvl * 70 / 32767));
    }
    cv_blit(0, FIELD_Y);
}

/* the editor: the page's four values (label, value, unit, gauge), the pages, the preset */
static void draw_edit(void)
{
    const track_t *t = TSEL;
    const ui_page_t *pg = &UI_PAGES[ui.page];
    uint32_t k;
    cv_begin(240, EDIT_H, T_BG);
    for (k = 0; k < 4u; k++) {
        const param_desc_t *d;
        const char *unit;
        char val[12];
        int32_t x = (int32_t)k * 60, v, span;
        uint16_t ink;
        if (pg->id[k] == 0xFFu)
            continue;
        d = page_desc(pg->id[k]);
        v = *page_val(pg->id[k]);
        param_format(d, v, val, &unit);
        ink = ui.hot == k + 1u && fm1_ms - ui.hot_t < HOT_MS ? T_ACCENT : T_THEME;
        cv_text_in(x, 6, 60, &AF_S, d->label, T_MID, T_BG);
        cv_text_in(x, 22, 60, &AF_M, val, T_TEXT, T_BG);
        cv_text_in(x, 42, 60, &AF_S, unit, T_MID, T_BG);
        span = d->max > d->min ? d->max - d->min : 1;
        cv_rect(x + 8, 62, 44, 3, T_LINE);
        cv_rect(x + 8, 62, (v - d->min) * 44 / span, 3, ink);
    }
    for (k = 0; k < NUI_PAGES; k++)
        cv_rect(8 + (int32_t)k * 9, 82, 5, 5, k == ui.page ? T_THEME : T_DIM);
    cv_text_fit(240 - 6 - 120, 76, &AF_S, ENGINES[0]->presets[t->preset % ENGINES[0]->npresets].name, T_MID, T_BG, 120);
    cv_blit(0, EDIT_Y);
}

/* the menu: six rows over the field, the selected one's value under them */
static void draw_menu(void)
{
    uint32_t r, top = ui.mrow > 2u ? ui.mrow - 2u : 0u;
    char b[24];
    if (top + 6u > MR_COUNT)
        top = MR_COUNT - 6u;
    cv_begin(240, FIELD_H, T_BG);
    for (r = 0; r < 6u; r++) {
        uint32_t row = top + r;
        int32_t y = (int32_t)r * 20 + 2;
        uint16_t bg = row == ui.mrow ? T_SEL : T_BG, fg = row == ui.mrow ? T_INK : T_TEXT;
        if (row >= MR_COUNT)
            break;
        cv_rect(0, y, 240, 20, bg);
        cv_text_on(8, y + CAP_IN(S, 20), &AF_S, MENU_LABEL[row], fg, bg);
        menu_value(row, b, sizeof b);
        cv_text_r(232, y + CAP_IN(S, 20), &AF_S, b, fg, bg);
    }
    cv_blit(0, FIELD_Y);
    cv_begin(240, EDIT_H, T_BG);
    menu_value(ui.mrow, b, sizeof b);
    cv_text_in(0, 20, 240, &AF_M, b, T_TEXT, T_BG);
    cv_text_in(0, 60, 240, &AF_S, "SELECT ROW   KNOB 1 VALUE   GLO CLOSE", T_MID, T_BG);
    cv_blit(0, EDIT_Y);
}

/* what the editor shows, hashed: it is drawn again when this changes */
static uint32_t edit_sig(void)
{
    const ui_page_t *pg = &UI_PAGES[ui.page];
    uint32_t k, h = ui.page * 131u + TSEL->preset * 7u + (ui.hot && fm1_ms - ui.hot_t < HOT_MS ? ui.hot : 0u);
    for (k = 0; k < 4u; k++)
        h = h * 31u + (pg->id[k] == 0xFFu ? 0u : (uint32_t)(uint16_t)*page_val(pg->id[k]));
    return h;
}
static uint32_t menu_sig(void)
{
    char b[24];
    uint32_t h = ui.mrow, i;
    menu_value(ui.mrow, b, sizeof b);
    for (i = 0; b[i]; i++)
        h = h * 31u + (uint8_t)b[i];
    return h;
}

static void ui_draw(void)
{
    static uint32_t t_field, t_hdr, sig, uboot;
    uint32_t s;
    if (!scr_frame())
        return;
    if (ui.uboot) {                                    /* main.c: OCT- + OCT+ held, the UBOOT countdown */
        if (ui.uboot != uboot || ui.force) {
            char b[16] = "UBOOT IN ", n[4];
            fmt_int(n, ui.uboot);
            str_cat(b, n, sizeof b);
            lcd_fill(0, 0, 240, 240, T_BG);
            draw_text_box(0, 110, 240, &AF_M, b, T_THEME, 1);
            uboot = ui.uboot;
            ui.force = 0;
        }
        scr_shown();
        return;
    }
    uboot = 0;
    if (ui.force) {
        ui.force = 0;
        t_hdr = fm1_ms - 1000u;
        t_field = fm1_ms - 1000u;
        sig = ~0u;
    }
    if (fm1_ms - t_hdr >= 250u || (ui.msg_t && fm1_ms - ui.msg_t < 30u)) {
        t_hdr = fm1_ms;
        draw_header();
    }
    if (ui.menu) {
        s = menu_sig();
        if (s != sig) {
            sig = s;
            draw_menu();
        }
    } else {
        if (fm1_ms - t_field >= FIELD_MS) {
            t_field = fm1_ms;
            draw_field();
        }
        s = edit_sig();
        if (s != sig) {
            sig = s;
            draw_edit();
        }
    }
    scr_shown();
}
