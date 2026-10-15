/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Flash: the settings record (settings_persist.c: palette, LEDS, the panel table and MENU's prefs, in the layout the
 * earlier firmware wrote) and the autosave of the sound (the part's parameters and the song's globals). Main loop
 * only. */
#define FELUCCA_FAVORITES 1
static struct { uint8_t factory[16][32]; uint32_t user, filter; } favorites;   /* the record's prefs bytes */
#define ui_prefs (favorites.factory[15][30])   /* MENU's flags, as the earlier firmware stored them */
#define PREF_ANIM_OFF 2u                       /* no power-on LED sweep (main.c boot_leds) */
#define PREF_USB_FIXED 8u                      /* USB audio at the full level, MASTER after (fx.c fx_usb_fixed) */
#define PREF_SERIAL_OFF 64u                    /* MENU > USB SERIAL OFF: no serial console (ui_eco.c usb_serial_apply) */
#define ui_scr (favorites.factory[15][27])     /* MENU > SCREEN OFF, its stored code (ui_eco.c SCR_CODE) */
#include "settings_persist.c"

#if FELUCCA_FLASH
static persist_t persist_saved __attribute__((section(".pool")));
static uint8_t persist_pending;                 /* 1 requested, 2 waiting after a flash error */
static uint32_t persist_retry_ms;
#endif

static void persist_boot(void)                    /* before settings_init / panel_init */
{
#if FELUCCA_FLASH
    persist_t p;
    uint32_t f = irq_save();
    int n;
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;       /* the expected 1 MiB part, else stay RAM-only */
    irq_restore(f);
    if (!flash_ok)
        return;
    n = st_load(OBJ_SETTINGS, &p, sizeof p);
    if (settings_import(&p, n))
        persist_saved = p;
#endif
}

static void settings_poll(void)
{
#if FELUCCA_FLASH
    persist_t p;
    if (!persist_pending || !flash_ok || (persist_pending == 2u && (uint32_t)(fm1_ms - persist_retry_ms) < 1000u))
        return;
    p = persist_saved;
    settings_export(&p);
    if (!memcmp(&p, &persist_saved, sizeof p)) {
        persist_pending = 0;
        return;                                    /* unchanged: no erase cycle */
    }
    if (st_save(OBJ_SETTINGS, &p, sizeof p) == 0) {
        persist_saved = p;
        persist_pending = 0;
    } else {
        persist_pending = 2;
        persist_retry_ms = fm1_ms;
    }
#endif
}

static void settings_save(void)
{
#if FELUCCA_FLASH
    persist_pending = 1;
#endif
    settings_poll();
}

/* The autosave: the part's parameters, its preset and the song's globals in OBJ_AUTOSAVE (A/B at 0xE5000 / 0xE6000),
 * put back at power-on. An autosave of the earlier firmware (a project) has no SOUND_MAGIC: ignored. Flash wear: a
 * write only when the sound changed, has stayed the same for AS_IDLE_MS with no key or button down and no voice
 * sounding (an erase stops the audio for ~40 ms), and at least AS_GAP_MS after the one before (AS_RETRY_MS after a
 * failed write). Never restored after a crash, a hang or a failed boot (main.c) */
#define SOUND_MAGIC 0x45434F31u                  /* "ECO1" */
#define AS_POLL_MS 250u
#define AS_IDLE_MS 10000u
#define AS_GAP_MS 60000u
#define AS_RETRY_MS 30000u
typedef struct {
    uint32_t magic;
    uint16_t np, ng;                             /* P_COUNT, G_COUNT it was written with */
    int16_t p[P_COUNT];
    int16_t g[G_COUNT];
    uint8_t preset, rsv[3];
} sound_store_t;
_Static_assert(sizeof(sound_store_t) <= ST_PAYLOAD_MAX, "the sound does not fit one flash sector");
static struct {
    uint32_t saved, seen;                        /* the signature written last (or loaded at power-on), seen last */
    uint32_t t, last, poll;                      /* fm1_ms: seen changed / busy, the last write, the last check */
    uint8_t wrote, err;                          /* a write since power-on (last is valid); the last one failed */
} as;

static uint32_t as_mix(uint32_t h, const void *p, uint32_t n)   /* FNV-1a, continued */
{
    const uint8_t *b = p;
    while (n--) {
        h ^= *b++;
        h *= 16777619u;
    }
    return h;
}

static uint32_t autosave_sig(void)
{
    const track_t *t = &trk[0];
    uint32_t h = as_mix(2166136261u, t->p, sizeof t->p);
    h = as_mix(h, &t->preset, 1);
    return as_mix(h, song.g, sizeof song.g);
}

/* nothing going on that an erase (the audio stopped ~40 ms) would be heard in */
static int autosave_quiet(void)
{
    uint32_t i;
    if (fm1_in.notes || fm1_in.buttons)
        return 0;
    for (i = 0; i < NVOICE; i++)
        if (trk[0].v[i].active)
            return 0;
    return 1;
}

/* main loop, every pass (after settings_poll) */
static void autosave_poll(void)
{
#if FELUCCA_FLASH
    static sound_store_t s;
    uint32_t sig;
    if (!flash_ok || fm1_ms - as.poll < AS_POLL_MS)
        return;
    as.poll = fm1_ms;
    sig = autosave_sig();
    if (sig != as.seen || !autosave_quiet()) {
        as.seen = sig;
        as.t = fm1_ms;
        return;
    }
    if (sig == as.saved || fm1_ms - as.t < AS_IDLE_MS ||
        (as.wrote && fm1_ms - as.last < (as.err ? AS_RETRY_MS : AS_GAP_MS)))
        return;
    as.last = fm1_ms;
    as.wrote = 1;
    s.magic = SOUND_MAGIC;
    s.np = P_COUNT;
    s.ng = G_COUNT;
    memcpy(s.p, trk[0].p, sizeof s.p);
    memcpy(s.g, song.g, sizeof s.g);
    s.preset = trk[0].preset;
    as.err = st_save(OBJ_AUTOSAVE, &s, sizeof s) != 0;
    if (!as.err)
        as.saved = sig;
#endif
}

/* power-on, after the defaults (main.c): the autosave becomes the sound when `allowed` (no crash, hang or failed boot
 * before); one that does not load (none, torn, another format) is ignored. 1 = restored */
static int autosave_boot(int allowed)
{
    int rc = 0;
#if FELUCCA_FLASH
    static sound_store_t s;
    if (flash_ok && allowed && st_load(OBJ_AUTOSAVE, &s, sizeof s) == (int)sizeof s && s.magic == SOUND_MAGIC &&
        s.np == P_COUNT && s.ng == G_COUNT) {
        track_t *t = &trk[0];
        uint32_t i;
        fm1_irq_off();                           /* the audio ISR must not see half a sound */
        for (i = 0; i < P_COUNT; i++)
            t->p[i] = (int16_t)param_fit(param_desc_of(0, i), s.p[i]);
        for (i = 0; i < G_COUNT; i++)
            song.g[i] = (int16_t)param_fit(&GP[i], s.g[i]);
        t->preset = (uint8_t)(s.preset % ENGINES[0]->npresets);
        fm1_irq_on();
        rc = 1;
    }
#else
    (void)allowed;
#endif
    as.saved = as.seen = autosave_sig();
    as.t = fm1_ms;
    return rc;
}
