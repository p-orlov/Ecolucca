/* SPDX-License-Identifier: GPL-3.0-only */
/* The ecosystem (docs/mvp_decisions.md): cells on a 5 x 5 grid of niches (pitch: the octaves C2..C7, stereo: five
 * rows), fed by the keys. In PULSE mode (G_EMODE 0) each cell plays a note once every interval of its own (a division
 * of the tempo); in DRONE mode (1) the NVOICE best fed cells hold one note each, the rest are silent. Every note is on
 * a voice of the cell's own parameters (voice.c voice_play) and is the cell's feeding window (energy and material);
 * notes and upkeep cost energy; material grows a cell's mass (louder, hungrier); a grown, fed cell buds a mutated
 * child; a cell that cannot eat where it is drifts towards food; a cell dies of hunger or age and leaves organic matter
 * that rots partly back into energy, and a fragment of its genome that loses traits as it rots; a cell eating there
 * pulls its genes towards the fragment's. The regulators (G_TEMP .. G_RATE, ui_eco.c SCL ARP) are read once a tick.
 * Runs in the audio ISR (events_block): the notes at block resolution, the rest once per ECO_TICK samples. The same
 * inputs give the same run (tests/eco_bench.c). The UI only reads it. */
#define ECO_CELLS 32u
#define ECO_COLS 5u
#define ECO_ROWS 5u
#define ECO_LO_NOTE 36                           /* C2: the colour scale's bottom (ui_eco.c); the range: G_LOW G_HIGH */
#define ECO_TICK 882u                            /* samples: 20 ms */
#define EQ(x) ((int32_t)((x) * 16))              /* energy, material and organic matter: 1/16 units */
#define ECO_M1 256                               /* the mass of an average (grown) cell */
#define ECO_M_MAX (2 * ECO_M1)

/* the initial tuning values (docs/mvp_decisions.md) */
#define ECO_E_IN EQ(40)                          /* a held key's energy into its column, per tick, at velocity 100 */
#define ECO_B_IMP EQ(200)                        /* a note-on's material into its column, at velocity 127 */
#define ECO_B_IN EQ(4)                           /* a held key's material into its column, per tick, at velocity 100 */
#define ECO_E_SPILL EQ(400)                      /* a niche's energy above this overflows into its neighbours */
#define ECO_B_SPILL EQ(100)                      /* its material, likewise */
#define ECO_E_WILD EQ(400)                       /* FOOD above 50: a niche's food grows by itself up to this, */
#define ECO_E_SOW EQ(40)                         /* then this much of it goes to one neighbour; none anywhere, it
                                                  * starts this much in a niche of the middle row */
#define ECO_E_HAS EQ(8)                          /* a niche with this much energy has food (it grows) */
#define ECO_E_SPORE EQ(300)                      /* a fragment sprouts in an empty niche this rich */
#define ECO_R_MAX EQ(5)                          /* intake ceiling per tick, an average cell */
#define ECO_RB_MAX EQ(1)                         /* material intake ceiling per tick, an average cell */
#define ECO_C_BASE EQ(0.5)                       /* upkeep per tick, an average cell */
#define ECO_C_NOTE EQ(20)                        /* a note at velocity 100 with a quarter-second gate */
#define ECO_E_FOUND EQ(800)                      /* a founder's energy */
#define ECO_MASS_E 4                             /* the organic matter a mass unit leaves */
#define ECO_E_REP EQ(1500)                       /* budding: the parent's energy at least */
#define ECO_B_REP EQ(60)                         /* budding: the parent's material at least, the child's mass ECO_M1/2 */
#define ECO_C_REP EQ(300)                        /* budding: its cost to the parent */
#define ECO_A_MIN (5u * 50u)                     /* ticks: the age to bud at */
#define ECO_COOL (3u * 50u)                      /* ticks between a cell's buds */
#define ECO_NEAR 16                              /* 1/16 semitone: a key feeds a cell this close in its niche */
#define ECO_E_HUNGRY EQ(100)                     /* a niche's energy per average cell below which its cells drift */
#define ECO_V_PAN 15                             /* drift, 1/256 pan unit per tick: a row in about 9 s */
#define ECO_V_PITCH 110                          /* drift, 1/256 of 1/16 semitone per tick: an octave in about 9 s */
#define ECO_C_PAN 12                             /* drift's cost a pan unit (1/16 energy units): a row about 19 */
#define ECO_C_PITCH 20                           /* and a pitch unit (1/16 semitone): an octave about 240 */
#define ECO_DRONE 8u                             /* DRONE: a cell's cycle, its interval gene times this */
#define ECO_PRED_STEP 128                        /* a hungry big cell's turn towards hunting (of 255): two to a full one */
#define ECO_HUNT_COOL (5u * 50u)                 /* ticks between a predator's attacks */
#define ECO_C_HUNT EQ(50)                        /* an attack's cost, caught or not */
#define ECO_TRAITS 23u                           /* the genes a fragment can carry: eco_gene */
#define ECO_FRAG_N 6u                            /* a fresh fragment's traits */
#define ECO_FW 114                               /* the field (ui_eco.c): px a half of the pan, */
#define ECO_FH 123                               /* px over the pitch range */
#define ECO_GAP 4                                /* px between a newborn and its neighbours at least */
#define ECO_STUCK 250u                           /* ticks a drift waits with no way on before it stops there */

#ifndef ECO_EVENT
#define ECO_EVENT(kind, c) ((void)0)             /* the host bench logs them (tests/eco_bench.c) */
#endif
enum { EV_SEED, EV_FEED, EV_NOTE, EV_HUNGER, EV_AGE, EV_BUD, EV_DRIFT, EV_FRAG, EV_TAKE, EV_EATEN, EV_TURN, EV_SPROUT };

static const int8_t ECO_DX[4] = {1, -1, 0, 0}, ECO_DY[4] = {0, 0, 1, -1};   /* a niche's four neighbours */

/* the interval genes, shortest first: N_DIV indices (fx.c div_samples) 1/32 16T 1/16 8T 1/8 1/4 1/2 1/1 2BAR 4BAR */
static const uint8_t ECO_DIV[10] = {3, 5, 2, 4, 1, 0, 6, 7, 8, 9};
/* a founder's interval by its octave, C2 .. C6: 1/1 1/2 1/4 1/8 1/16 (then G_RATE and a random step) */
static const uint8_t ECO_DIV_COL[ECO_COLS] = {7, 6, 5, 4, 2};
/* a band's loudness ceiling (a cell's size, eco_size) and how many cells it holds, low to high */
static const uint8_t ECO_CAP[ECO_COLS] = {120, 110, 100, 90, 80}, ECO_LIM[ECO_COLS] = {5, 6, 7, 8, 9};

typedef struct {
    int16_t pitch16;             /* 1/16 semitone */
    int8_t pan;                  /* -64..63 */
    uint8_t div;                 /* the interval: an ECO_DIV index */
    uint8_t phase;               /* the first note this far into the interval, /256 */
    uint8_t gate;                /* the gate, /256 of the interval */
    uint8_t smin, smax;          /* its size (loudness) born and grown: eco_size */
    int8_t fenv;                 /* ENV -> FILTER */
    int8_t e[8];                 /* ANALOG's P_E0..P_E7 */
    uint8_t env[4];              /* ATK DEC SUS REL */
    uint8_t len, mask;           /* the rhythm: a motif of len intervals (1..8), a note on each bit of mask */
    uint8_t rev;                 /* its REVERB send */
    uint8_t skip;                /* the chance a hit is skipped, /256 */
    uint8_t pred;                /* how much a predator (0..255): niche food x (1 - pred)^2, hunting x pred^2 */
} genome_t;

typedef struct {
    genome_t g;
    uint16_t id, parent;         /* id 0: a free slot; parent 0: a founder */
    uint8_t col, row;
    uint8_t open;                /* its note sounds: the feeding window is open */
    uint8_t moving;              /* drifting towards to_pitch, to_pan */
    uint8_t step;                /* its motif's step now */
    uint8_t run;                 /* ticks into its drift (a predator speeds up across) */
    uint8_t grid;                /* its steps on the tempo's grid (eco_block; CHAOS slips them off) */
    uint8_t glide;               /* this drift's pitch glides (else it steps through the scale's notes) */
    int16_t pos16;               /* this drift's pitch, gliding (the pitch heard: it, or its scale note) */
    int16_t home16;              /* where it was born: its pitch, */
    int8_t hpan;                 /* its pan */
    int8_t to_pan;
    int16_t to_pitch;
    uint16_t ap, ay, ag;         /* the fractions (/256) of a drift's pan and pitch unit, of a growth's mass unit */
    uint16_t as;                 /* the fraction (/256) of a pan unit going round a cell in its way (eco_round) */
    int8_t side;                 /* this drift's side round cells in its way, 0 not chosen yet */
    uint8_t stuck;               /* ticks this drift has had no way on */
    int16_t hue;                 /* its colour on the field (ui_eco.c): its line's pitch, not moved by drift */
    uint8_t preset;              /* the ANALOG preset its line's sound came from (a mutation moves to a neighbour) */
    int16_t snd[P_COUNT];        /* its sound: the part's at its line's seeding; the genome's genes over it */
    uint32_t cool;               /* until it may bud again, 1/256 tick */
    uint32_t hcool;              /* until it may attack again, 1/256 tick */
    uint16_t eat;                /* its intake ceilings' scale, /256: its rhythm's share of sounding (eco_appetite) */
    uint32_t been;               /* the niches it has been in: bit col * ECO_ROWS + row */
    int32_t e, b, m;             /* energy, material (1/16 units), mass (ECO_M1 an average cell) */
    uint32_t age;                /* 1/256 tick (the temperature speeds it up) */
    uint32_t pos;                /* samples into its interval */
    uint32_t h;                  /* its note (voice.c handle), 0 none */
} cell_t;

typedef struct { int32_t E, B, O; } niche_t;

typedef struct {                 /* a dead cell's genome fragment, in its niche */
    genome_t g;
    uint32_t mask;               /* the traits it still carries (eco_gene's), 0 none */
    uint32_t n;                  /* how many */
    int32_t o0, o;               /* the organic matter it came with, what of it has not rotted */
} frag_t;

static struct {
    cell_t cell[ECO_CELLS];
    niche_t niche[ECO_COLS][ECO_ROWS];
    frag_t frag[ECO_COLS][ECO_ROWS];
    uint16_t held[ECO_COLS];     /* the velocities of the keys held, summed per column (eco_tick, from kvel) */
    uint8_t kvel[128];           /* each key's velocity while held */
    uint8_t mode;                /* the G_EMODE the cells play in */
    int32_t lo16, span16;        /* the niches' pitch range (G_LOW .. G_HIGH, eco_range), 1/16 semitone */
    uint32_t acc;                /* samples towards the next tick */
    uint32_t clock;              /* samples since power-on, the tempo grid's origin (not paused) */
    uint32_t ticks;
    uint32_t rng;
    uint16_t next_id;
    uint32_t beat, len[NELEM(ECO_DIV)];   /* the tempo the intervals were taken at (eco_lens), the intervals */
    int32_t k;                   /* this tick's regulators: a niche's capacity (mass), */
    uint32_t mut, move;          /* the mutation's and the drift's strength (50 the tuning values') */
} eco;
static volatile uint8_t eco_keys = 1;            /* 1: the keys seed and feed (ui_eco.c MENU > KEYS), 0: they play */
static volatile uint8_t eco_paused, eco_clear;  /* ui_eco.c PLAY: a tap pauses (silent, frozen), a hold clears all */

static void eco_seed_rng(uint32_t s) { eco.rng = s ? s : 0x9E3779B9u; }
static uint32_t eco_rand(void)
{
    uint32_t s = eco.rng ? eco.rng : 0x9E3779B9u;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return eco.rng = s;
}

/* a pitch's column: five equal bands of the range */
static uint32_t eco_col(int32_t p16)
{
    return (uint32_t)clamp((p16 - eco.lo16) * (int32_t)ECO_COLS / eco.span16, 0, ECO_COLS - 1);
}
/* the range from G_LOW and G_HIGH (a semitone a column at least); changed, the cells' columns again */
static __attribute__((noinline)) void eco_range(void)
{
    int32_t lo = clamp(song.g[G_LOW], 24, 84), hi = clamp(song.g[G_HIGH], 48, 108);
    uint32_t i;
    hi = hi < lo + (int32_t)ECO_COLS ? lo + (int32_t)ECO_COLS : hi;
    if (eco.lo16 == lo * 16 && eco.span16 == (hi - lo) * 16)
        return;
    eco.lo16 = lo * 16;
    eco.span16 = (hi - lo) * 16;
    for (i = 0; i < ECO_CELLS; i++)
        if (eco.cell[i].id)
            eco.cell[i].col = (uint8_t)eco_col(eco.cell[i].g.pitch16);
}
static uint32_t eco_row(int32_t pan) { return (uint32_t)clamp((pan + 64) * (int32_t)ECO_ROWS / 128, 0, ECO_ROWS - 1); }
/* the note of G_ESCALE (from G_EROOT) nearest a pitch, 1/16 semitone; the higher of two as near */
static int32_t eco_snap(int32_t p16)
{
    uint32_t mask = SCALE_MASK[clamp(song.g[G_ESCALE], 0, NELEM(SCALE_MASK) - 1)];
    int32_t s = (p16 + 8) / 16, r = 120 - clamp(song.g[G_EROOT], 0, 11), d;
    for (d = 0; d < 12; d++) {
        if (mask >> ((uint32_t)(s + d + r) % 12u) & 1u)
            return (s + d) * 16;
        if (mask >> ((uint32_t)(s - d + r) % 12u) & 1u)
            return (s - d) * 16;
    }
    return s * 16;
}
/* the temperature: the pace of intake, upkeep, notes' cost, growth, ageing, budding and drift, x0.25 .. x1.75 (/256) */
static uint32_t eco_heat(void) { return 64u + (uint32_t)clamp(song.g[G_TEMP], 0, 100) * 192u / 50u; }
/* CHAOS: glides and slips off the tempo grid more often, goes home less often */
static uint32_t eco_chaos(void) { return (uint32_t)clamp(song.g[G_CHAOS], 0, 100); }
/* the intervals in samples, again when the tempo changes (their divides stay out of the audio ISR's every block) */
static __attribute__((noinline)) void eco_lens(void)
{
    uint32_t i;
    eco.beat = beat_samples();
    for (i = 0; i < NELEM(ECO_DIV); i++)
        eco.len[i] = div_samples(ECO_DIV[i]);
}
static uint32_t eco_len(const cell_t *c)
{
    if (eco.beat != beat_samples())
        eco_lens();
    return eco.len[c->g.div < NELEM(ECO_DIV) ? c->g.div : 0u];
}
/* where a cell on the tempo grid is in its interval of len samples at eco.clock + ahead: the grid from power-on, its
 * phase gene a quarter-interval offset */
static uint32_t eco_grid_pos(const cell_t *c, uint32_t len, uint32_t ahead)
{
    return (eco.clock + ahead + len / 4u * (c->g.phase >> 6)) % len;
}

/* a cell eats only while its note sounds: its intake ceilings scaled so it eats as much in the end as the tuning's
 * cell (every step, half the time) whatever its rhythm (its motif's notes, skips, gate); 1/4 .. 8 times */
static __attribute__((noinline)) void eco_appetite(cell_t *c)
{
    uint32_t n = c->g.len ? c->g.len : 1u, m = c->g.mask & ((1u << n) - 1u), hits = 0, d;
    for (; m; m &= m - 1u)
        hits++;
    d = hits * 256u / n * (256u - c->g.skip) / 256u * c->g.gate / 256u;
    c->eat = (uint16_t)clamp(128 * 256 / (int32_t)(d ? d : 1u), 64, 2048);
}

/* a cell's size: its loudness, from its mass (ECO_M_MAX: 120) */
static int32_t eco_size(const cell_t *c) { return 20 + c->m * 100 / ECO_M_MAX; }
static int32_t eco_m_of(int32_t size) { return (size - 20) * ECO_M_MAX / 100; }
/* the mass it grows to: its smax gene, its band's ceiling over it */
static int32_t eco_mmax(const cell_t *c)
{
    return eco_m_of(c->g.smax < ECO_CAP[c->col] ? c->g.smax : ECO_CAP[c->col]);
}
/* growing up it may change its band (its pitch); grown, it keeps it */
static int eco_young(const cell_t *c) { return c->m < eco_mmax(c); }
/* its radius on the field, px */
static int32_t eco_radius(const cell_t *c) { return clamp(2 + c->m * 3 / ECO_M1, 2, 10); }
/* its pitch on the field: on its way while it drifts */
static int32_t eco_y16(const cell_t *c) { return c->moving ? c->pos16 : c->g.pitch16; }
/* a pan and a pitch on the field, 1/16 px */
static int32_t eco_fx(int32_t pan) { return pan * ECO_FW / 4; }
static int32_t eco_fy(int32_t p16) { return (p16 - eco.lo16) * ECO_FH * 16 / eco.span16; }
/* where it is now: its niche, marked as one it has been in */
static void eco_at(cell_t *c)
{
    c->col = (uint8_t)eco_col(c->g.pitch16);
    c->row = (uint8_t)eco_row(c->g.pan);
    c->been |= 1u << (c->col * ECO_ROWS + c->row);
}
/* a band a cell may go into: within the range, room in it (cnt: its cells, those heading there counted), the cell
 * no louder than its ceiling */
static int eco_band_ok(const cell_t *c, uint32_t col, const uint8_t cnt[ECO_COLS])
{
    return col < ECO_COLS && cnt[col] < ECO_LIM[col] && eco_size(c) <= ECO_CAP[col];
}
/* a cell of radius r at pan, pitch16 would touch another (gap px apart at least; not c itself) */
static int eco_touch(const cell_t *c, int32_t r, int32_t pan, int32_t p16, int32_t gap)
{
    uint32_t i;
    for (i = 0; i < ECO_CELLS; i++) {
        const cell_t *q = &eco.cell[i];
        int32_t dx, dy, rr;
        if (!q->id || q == c)
            continue;
        dx = (q->g.pan - pan) * ECO_FW / 64;
        dy = (eco_y16(q) - p16) * ECO_FH / eco.span16;
        rr = r + eco_radius(q) + gap;
        if (dx < rr && dx > -rr && dy < rr && dy > -rr && dx * dx + dy * dy < rr * rr)
            return 1;
    }
    return 0;
}
/* a newborn's pan: from pan0 outwards, either side first by chance, the nearest where it touches none (ECO_GAP
 * apart) and (load not 0) its niche has room for its mass; -128 none */
static __attribute__((noinline)) int32_t eco_place(const cell_t *c, int32_t pan0, const int32_t load[ECO_COLS][ECO_ROWS])
{
    int32_t r = eco_radius(c), side = eco_rand() & 1u ? 3 : -3, j;
    uint32_t col = eco_col(c->g.pitch16);
    for (j = 0; j < 44; j++) {
        int32_t pan = pan0 + (j + 1) / 2 * (j & 1 ? side : -side);
        if (pan < -64 || pan > 63)
            continue;
        if (load && load[col][eco_row(pan)] + c->m > eco.k)
            continue;
        if (!eco_touch(c, r, pan, c->g.pitch16, ECO_GAP))
            return pan;
    }
    return -128;
}

/* a cell within ECO_NEAR of the note in the note's niche (where its founder would live) */
static int eco_near(uint32_t note)
{
    uint32_t i, col = eco_col((int32_t)note * 16), row = eco_row(0);
    for (i = 0; i < ECO_CELLS; i++) {
        const cell_t *c = &eco.cell[i];
        int32_t d = c->g.pitch16 - (int32_t)note * 16;
        if (c->id && c->col == col && c->row == row && d <= ECO_NEAR && d >= -ECO_NEAR)
            return 1;
    }
    return 0;
}

static cell_t *eco_free(void)
{
    uint32_t i;
    for (i = 0; i < ECO_CELLS; i++)
        if (!eco.cell[i].id)
            return &eco.cell[i];
    return 0;
}

static uint16_t eco_new_id(void)
{
    if (!++eco.next_id)
        eco.next_id = 1;
    return eco.next_id;
}

/* a G_BREAK chance of a motif: 2..8 intervals, notes on random ones (one at least); else a note every interval */
static void eco_motif(genome_t *g)
{
    g->len = 1;
    g->mask = 1;
    if (eco_rand() % 100u < (uint32_t)clamp(song.g[G_BREAK], 0, 100)) {
        g->len = (uint8_t)(2u + eco_rand() % 7u);
        g->mask = (uint8_t)(eco_rand() & ((1u << g->len) - 1u));
        if (!g->mask)
            g->mask = (uint8_t)(1u << (eco_rand() % g->len));
    }
}

/* a founder at the key's pitch, in the middle of the stereo field (beside the cells there), with the part's sound as
 * its genome, the key's velocity its size grown; its interval by its octave, G_RATE and chance, its first note at a
 * random point of it */
static __attribute__((noinline)) void eco_seed(uint32_t note, uint32_t vel)
{
    const track_t *t = &trk[0];
    cell_t *c = eco_free();
    uint32_t i, col = eco_col((int32_t)note * 16), len;
    int32_t div, pan;
    if (!c)
        return;
    memset(c, 0, sizeof *c);
    c->g.pitch16 = (int16_t)(note * 16u);
    div = ECO_DIV_COL[col] + (50 - clamp(song.g[G_RATE], 0, 100)) / 25 + (int32_t)(eco_rand() % 3u) - 1;
    c->g.div = (uint8_t)clamp(div, 0, NELEM(ECO_DIV) - 1);
    c->g.phase = (uint8_t)eco_rand();
    eco_motif(&c->g);
    c->g.skip = (uint8_t)((uint32_t)clamp(song.g[G_BREAK], 0, 100) * (eco_rand() % 128u) / 100u);
    c->g.gate = 128;
    c->g.smin = 40;
    c->g.smax = (uint8_t)clamp(vel ? (int32_t)vel : 100, 50, 127);
    c->g.rev = (uint8_t)clamp(t->p[P_REV], 0, 127);
    c->g.fenv = (int8_t)t->p[P_ED_FLT];
    for (i = 0; i < 8u; i++)
        c->g.e[i] = (int8_t)t->p[P_E0 + i];
    for (i = 0; i < 4u; i++)
        c->g.env[i] = (uint8_t)t->p[P_ATK + i];
    c->col = (uint8_t)col;
    c->m = eco_mmax(c) < ECO_M1 ? eco_mmax(c) : ECO_M1;
    pan = eco_place(c, 0, 0);
    c->g.pan = (int8_t)(pan < -64 ? 0 : pan);
    c->hpan = c->g.pan;
    c->id = eco_new_id();
    eco_at(c);
    c->e = ECO_E_FOUND;
    len = eco_len(c);
    c->pos = len - len / 256u * c->g.phase;
    c->home16 = c->g.pitch16;
    c->grid = eco_rand() % 100u >= eco_chaos();
    if (c->grid)
        c->pos = eco_grid_pos(c, len, 0);
    c->hue = c->g.pitch16;
    c->preset = t->preset;
    memcpy(c->snd, t->p, sizeof c->snd);
    eco_appetite(c);
    ECO_EVENT(EV_SEED, c);
}

/* a spore: a fragment in an empty niche this rich (ECO_E_SPORE) sprouts into a cell of its genome, born size (the
 * part's sound under it), fed from the niche */
static __attribute__((noinline)) void eco_sprout(uint32_t x, uint32_t y)
{
    frag_t *f = &eco.frag[x][y];
    cell_t *c = eco_free();
    uint32_t len;
    int32_t pan;
    if (!c)
        return;
    memset(c, 0, sizeof *c);
    c->g = f->g;
    c->g.pred = 0;                               /* a new line grazes, whatever the dead one did */
    c->m = eco_m_of(c->g.smin);
    pan = eco_place(c, c->g.pan, 0);
    if (pan >= -64)
        c->g.pan = (int8_t)pan;
    c->id = eco_new_id();
    eco_at(c);
    c->e = ECO_E_FOUND / 2;
    eco.niche[x][y].E -= ECO_E_FOUND / 2;
    c->hue = c->home16 = c->g.pitch16;
    c->hpan = c->g.pan;
    c->preset = trk[0].preset;
    memcpy(c->snd, trk[0].p, sizeof c->snd);
    len = eco_len(c);
    c->grid = eco_rand() % 100u >= eco_chaos();
    c->pos = c->grid ? eco_grid_pos(c, len, 0) : eco_rand() % len;
    eco_appetite(c);
    f->n = 0;
    f->mask = 0;
    ECO_EVENT(EV_SPROUT, c);
}

/* a key (or a MIDI note) down / up: a note with no cell near it in its niche seeds one, otherwise its column gets
 * material by its velocity; held, it feeds the column by its velocity (eco_tick) */
static void eco_key(uint32_t note, uint32_t vel, int down)
{
    uint32_t col, r;
    eco_range();
    note &= 127u;
    col = eco_col((int32_t)note * 16);
    eco.kvel[note] = (uint8_t)(down ? vel : 0u);
    if (!down)
        return;
    if (!eco_near(note)) {
        eco_seed(note, vel);
        return;
    }
    for (r = 0; r < ECO_ROWS; r++)
        eco.niche[col][r].B += ECO_B_IMP * (int32_t)vel / 127 * clamp(song.g[G_FOOD], 0, 100) / 50 / (int32_t)ECO_ROWS;
    ECO_EVENT(EV_FEED, 0);
}

/* the note on its voice, the cell's own sound and REVERB send (LEVEL: the part's, voice.c vox_mix), as loud as it is
 * big; it costs by its gate (samples; DRONE: 0, paid by the tick) */
static __attribute__((noinline)) void eco_note_on(cell_t *c, uint32_t gate)
{
    static int16_t patch[P_COUNT];
    uint32_t i;
    int32_t vel = clamp(eco_size(c), 1, 127);
    memcpy(patch, c->snd, sizeof patch);
    for (i = 0; i < 8u; i++)
        patch[P_E0 + i] = c->g.e[i];
    for (i = 0; i < 4u; i++)
        patch[P_ATK + i] = c->g.env[i];
    patch[P_ED_FLT] = c->g.fenv;
    patch[P_PAN] = c->g.pan;
    patch[P_REV] = c->g.rev;
    c->h = voice_play(&trk[0], c->g.pitch16, (uint32_t)vel, patch);
    c->open = 1;
    c->e -= (int32_t)((int64_t)ECO_C_NOTE * vel / 100 * gate / (FS / 4u) * eco_heat() >> 8);
    ECO_EVENT(EV_NOTE, c);
}

static void eco_note_off(cell_t *c)
{
    voice_stop(&trk[0], c->h);
    c->open = 0;
}

/* a death; eaten (EV_EATEN), nothing is left: the predator has it */
static __attribute__((noinline)) void eco_die(cell_t *c, int kind)
{
    niche_t *n = &eco.niche[c->col][c->row];
    frag_t *f = &eco.frag[c->col][c->row];
    int32_t o = c->m * ECO_MASS_E * 16 * 6 / 10 + (c->e > 0 ? c->e * 3 / 10 : 0);
    if (c->open)
        eco_note_off(c);                         /* (its note fades with its own release) */
    ECO_EVENT(kind, c);
    if (kind == EV_EATEN) {
        c->id = 0;
        return;
    }
    n->O += o;
    f->g = c->g;                                 /* its fragment: ECO_FRAG_N traits by chance (the niche's last) */
    f->mask = 0;
    for (f->n = 0; f->n < ECO_FRAG_N;) {
        uint32_t bit = 1u << (eco_rand() % ECO_TRAITS);
        if (!(f->mask & bit)) {
            f->mask |= bit;
            f->n++;
        }
    }
    f->o0 = f->o = o > 0 ? o : 1;
    ECO_EVENT(EV_FRAG, c);
    c->id = 0;
}

/* the genes a fragment carries, k < ECO_TRAITS: ANALOG's eight (0 the waveform), ADSR, ENV -> FILTER, gate, the size
 * grown, interval, phase, the motif's length and notes, the REVERB send, skips, predation, the size born. Not the
 * pitch and the pan: they are where a cell is */
static int32_t eco_gene(const genome_t *g, uint32_t k)
{
    return k < 8u ? g->e[k] : k < 12u ? g->env[k - 8u] : k == 12u ? g->fenv : k == 13u ? g->gate : k == 14u ? g->smax :
           k == 15u ? g->div : k == 16u ? g->phase : k == 17u ? g->len : k == 18u ? g->mask :
           k == 19u ? g->rev : k == 20u ? g->skip : k == 21u ? g->pred : g->smin;
}
static void eco_gene_set(genome_t *g, uint32_t k, int32_t v)
{
    if (k < 8u)
        g->e[k] = (int8_t)v;
    else if (k < 12u)
        g->env[k - 8u] = (uint8_t)v;
    else if (k == 12u)
        g->fenv = (int8_t)v;
    else if (k == 13u)
        g->gate = (uint8_t)v;
    else if (k == 14u)
        g->smax = (uint8_t)v;
    else if (k == 15u)
        g->div = (uint8_t)v;
    else if (k == 16u)
        g->phase = (uint8_t)v;
    else if (k == 17u)
        g->len = (uint8_t)v;
    else if (k == 18u)
        g->mask = (uint8_t)v;
    else if (k == 19u)
        g->rev = (uint8_t)v;
    else if (k == 20u)
        g->skip = (uint8_t)v;
    else if (k == 21u)
        g->pred = (uint8_t)v;
    else
        g->smin = (uint8_t)v;
    if (g->smin + 10 > g->smax)
        g->smin = (uint8_t)(g->smax - 10);
    if (!(g->mask & ((1u << g->len) - 1u)))
        g->mask |= 1u;
}
/* the n-th (from 0) trait of a mask */
static uint32_t eco_nth(uint32_t mask, uint32_t n)
{
    uint32_t k;
    for (k = 0; k < ECO_TRAITS - 1u; k++)
        if ((mask >> k & 1u) && !n--)
            break;
    return k;
}

/* a fragment rots with the niche's organic matter and drops traits by chance as it does: of ECO_FRAG_N, as many as
 * the share left (rounded up); none left, it is gone */
static void eco_frag_rot(frag_t *f, int32_t leak)
{
    uint32_t keep;
    f->o -= (int32_t)((int64_t)f->o * leak * 2 >> 16);
    keep = (uint32_t)(((int64_t)ECO_FRAG_N * f->o + f->o0 - 1) / f->o0);
    if (f->o < f->o0 / 16)
        keep = 0;
    while (f->n > keep) {
        f->mask &= ~(1u << eco_nth(f->mask, eco_rand() % f->n));
        f->n--;
    }
}

/* a cell eating where a fragment lies pulls one of its traits towards it: an eighth of the way (one step at least);
 * the discrete ones (the waveform, the interval, the motif) whole, one time in 8; a grazer takes no predation */
static void eco_absorb(cell_t *c, const frag_t *f)
{
    uint32_t k = eco_nth(f->mask, eco_rand() % f->n);
    int32_t a = eco_gene(&c->g, k), d = eco_gene(&f->g, k) - a;
    if (!d || (k == 21u && !c->g.pred))
        return;
    if (k == 0u || k == 15u || k == 17u || k == 18u) {
        if (eco_rand() % 8u)
            return;
        eco_gene_set(&c->g, k, a + d);
    } else {
        eco_gene_set(&c->g, k, a + (d / 8 ? d / 8 : d > 0 ? 1 : -1));
    }
    eco_appetite(c);
    ECO_EVENT(EV_TAKE, c);
}

static int32_t eco_step(int32_t v, int32_t lo, int32_t hi, uint32_t max)
{
    uint32_t r = eco_rand();
    int32_t d;
    max = max * eco.mut / 50u;
    if (!max)
        return v;
    d = 1 + (int32_t)((r >> 8) % max);
    return clamp(r & 1u ? v + d : v - d, lo, hi);
}

/* usually one gene by a small step, a quarter of the time two; now and then the pitch by an interval (kept within the
 * niches); the interval gene often by one; rarely the waveform. The steps and the odds: by eco.mut, none at 0 */
static void eco_mutate(genome_t *g)
{
    static const int8_t IV[6] = {-12, -7, -5, 5, 7, 12};
    uint32_t n = (eco_rand() & 3u) ? 1u : 2u;
    while (n--) {
        uint32_t k = eco_rand() % 16u;
        if (k < 7u)
            g->e[1u + k] = (int8_t)eco_step(g->e[1u + k], 0, 127, 6);
        else if (k < 11u)
            g->env[k - 7u] = (uint8_t)eco_step(g->env[k - 7u], 0, 127, 6);
        else if (k == 11u)
            g->fenv = (int8_t)eco_step(g->fenv, -64, 63, 6);
        else if (k == 12u)
            g->pan = (int8_t)eco_step(g->pan, -64, 63, 16);
        else if (k == 13u)
            g->gate = (uint8_t)eco_step(g->gate, 16, 240, 32);
        else if (k == 14u)
            g->smax = (uint8_t)eco_step(g->smax, g->smin + 10, 127, 8);
        else
            g->phase = (uint8_t)eco_step(g->phase, 0, 255, 32);
    }
    if (eco_rand() % 400u < eco.mut) {                 /* 1 in 8 at 50: the size born, the REVERB send */
        if (eco_rand() & 1u)
            g->smin = (uint8_t)eco_step(g->smin, 30, g->smax - 10, 6);
        else
            g->rev = (uint8_t)eco_step(g->rev, 0, 127, 8);
    }
    if (eco_rand() % 400u < eco.mut) {                 /* 1 in 8 at 50 */
        int32_t p = eco_snap(g->pitch16 + IV[eco_rand() % 6u] * 16);
        if (p >= eco.lo16 && p < eco.lo16 + eco.span16)
            g->pitch16 = (int16_t)p;
    }
    if (eco_rand() % 150u < eco.mut)                   /* 1 in 3 at 50 */
        g->div = (uint8_t)eco_step(g->div, 0, NELEM(ECO_DIV) - 1, 1);
    if (eco_rand() % 2000u < eco.mut) {                /* 1 in 40 at 50: the motif, a note in or out, longer, shorter */
        uint32_t k = eco_rand() % 4u, n = g->len;
        if (k < 2u)
            g->mask ^= (uint8_t)(1u << (eco_rand() % n));
        else if (k == 2u && n < 8u) {
            g->mask |= (uint8_t)((eco_rand() & 1u) << n);
            g->len = (uint8_t)(n + 1u);
        } else if (n > 1u)
            g->len = (uint8_t)(n - 1u);
        if (!(g->mask & ((1u << g->len) - 1u)))
            g->mask |= 1u;
    }
    if (eco_rand() % 1600u < eco.mut)                  /* 1 in 32 at 50: the skips */
        g->skip = (uint8_t)eco_step(g->skip, 0, 230, 24);
    if (g->pred && eco_rand() % 400u < eco.mut)        /* 1 in 8 at 50: a hunter's predation by a little */
        g->pred = (uint8_t)eco_step(g->pred, 0, 255, 16);
    if (eco_rand() % 1600u < eco.mut)                  /* 1 in 32 at 50 */
        g->e[0] = (int8_t)(eco_rand() % 7u);
    if (eco_rand() % 600u < eco.mut) {                 /* 1 in 12 at 50: a new envelope shape, a hit or a swell */
        if (eco_rand() & 1u) {
            g->env[0] = (uint8_t)(eco_rand() % 6u);
            g->env[1] = (uint8_t)(15u + eco_rand() % 36u);
            g->env[2] = 0;
            g->env[3] = (uint8_t)(10u + eco_rand() % 31u);
        } else {
            g->env[0] = (uint8_t)(40u + eco_rand() % 51u);
            g->env[2] = (uint8_t)(90u + eco_rand() % 38u);
            g->env[3] = (uint8_t)(60u + eco_rand() % 41u);
        }
    }
}

/* a child's line moves to a neighbouring ANALOG preset: its oscillator, filter, envelope and ENV -> FILTER */
static void eco_preset(cell_t *c)
{
    const engine_t *e = ENGINES[0];
    const preset_t *pr;
    uint32_t i;
    c->preset = (uint8_t)((c->preset + (eco_rand() & 1u ? 1u : e->npresets - 1u)) % e->npresets);
    pr = &e->presets[c->preset];
    for (i = 0; i < 8u; i++)
        c->g.e[i] = (int8_t)pr->e[i];
    for (i = 0; i < 4u; i++)
        c->g.env[i] = (uint8_t)pr->env[i];
    c->g.fenv = (int8_t)pr->fenv;
}

/* a mutated child, born size, beside its parent (eco_place: where it touches none and its niche has room), unless its
 * band is over its limit already (one more than ECO_LIM may be born: it heads on, eco_drift_pick); the parent pays its
 * material, 40 % of its energy and ECO_C_REP. load, cnt: the niches' mass, the bands' cells, kept up to date */
static __attribute__((noinline)) void eco_bud(cell_t *p, int32_t load[ECO_COLS][ECO_ROWS], uint8_t cnt[ECO_COLS])
{
    cell_t *c = eco_free();
    uint32_t len, d;
    int32_t pan;
    p->cool = ECO_COOL * 256u;
    if (!c)
        return;
    memset(c, 0, sizeof *c);
    c->g = p->g;
    c->preset = p->preset;
    if (eco_rand() % 2000u < eco.mut)                  /* 1 in 40 at 50 */
        eco_preset(c);
    eco_mutate(&c->g);
    eco_appetite(c);
    c->m = eco_m_of(c->g.smin);
    pan = eco_place(c, c->g.pan, load);
    if (pan < -64 || cnt[eco_col(c->g.pitch16)] > ECO_LIM[eco_col(c->g.pitch16)])
        return;
    c->g.pan = (int8_t)pan;
    c->id = eco_new_id();
    c->parent = p->id;
    eco_at(c);
    c->e = p->e * 4 / 10;
    c->hue = (int16_t)clamp(p->hue + c->g.pitch16 - p->g.pitch16, 0, 2047);   /* the parent's, but for a mutation */
    c->home16 = c->g.pitch16;
    c->hpan = c->g.pan;
    c->grid = p->grid;
    memcpy(c->snd, p->snd, sizeof c->snd);
    p->e -= c->e + ECO_C_REP;
    p->b -= ECO_B_REP;
    len = eco_len(c);
    d = (uint8_t)(c->g.phase - p->g.phase);      /* in step with its parent, but for a phase mutation */
    c->pos = (p->pos % len + len - d * len / 256u) % len;
    c->step = (uint8_t)(p->step % (c->g.len ? c->g.len : 1u));
    load[c->col][c->row] += c->m;
    cnt[c->col]++;
    ECO_EVENT(EV_BUD, c);
}

/* a cell's share of a niche's stock in its window: by mass among the windows open, at most cap, less when crowded */
static int32_t eco_share(int32_t stock, int32_t left, int32_t cap, int32_t m, int32_t open_m, int32_t load)
{
    int32_t take = (int32_t)((int64_t)stock * m / open_m);
    take = take < cap ? take : cap;
    if (load > eco.k)
        take = (int32_t)((int64_t)take * eco.k / load);
    return take < left ? take : left;
}

/* material into mass, a mass unit at most per tick (by the temperature), up to its size grown (eco_mmax); not while
 * a bigger disc would touch a cell beside it */
static __attribute__((noinline)) void eco_grow(cell_t *c, uint32_t heat)
{
    int32_t dm, cost, max = eco_mmax(c), r;
    c->ag = (uint16_t)(c->ag + heat);
    dm = c->ag >> 8;
    c->ag &= 255u;
    dm = dm < max - c->m ? dm : max - c->m;
    cost = (dm * ECO_B_REP + ECO_M1 - 1) / ECO_M1;
    if (dm <= 0 || c->b < cost)
        return;
    r = clamp(2 + (c->m + dm) * 3 / ECO_M1, 2, 10);
    if (r > eco_radius(c) && eco_touch(c, r, c->g.pan, eco_y16(c), 0))
        return;
    c->m += dm;
    c->b -= cost;
}


/* a drift begins: its pitch glides by the chance CHAOS, else steps through the scale's notes */
static void eco_move(cell_t *c, int32_t to_pitch, int32_t to_pan)
{
    c->to_pitch = (int16_t)to_pitch;
    c->to_pan = (int8_t)to_pan;
    c->pos16 = c->g.pitch16;
    c->glide = eco_rand() % 100u < eco_chaos();
    c->moving = 1;
    c->run = 0;
    c->side = 0;
    c->stuck = 0;
    c->as = 0;
    ECO_EVENT(EV_DRIFT, c);
}

/* a fed cell (half a founder's energy at least) away from where it was born heads back, by the chance 1 - CHAOS / 2
 * a look; its pitch too if it may change bands there (young, room: eco_band_ok), else across only, as a predator */
static void eco_home(cell_t *c, uint8_t cnt[ECO_COLS])
{
    int32_t to = c->home16;
    uint32_t col = eco_col(to);
    if (c->g.pred >= 128u || (col != c->col && (!eco_young(c) || !eco_band_ok(c, col, cnt))))
        to = c->g.pitch16;
    if (c->e < ECO_E_FOUND / 2 || (to == c->g.pitch16 && c->hpan == c->g.pan) || eco_rand() % 200u < eco_chaos())
        return;
    eco_move(c, to, c->hpan);
    col = eco_col(to);
    if (col != c->col) {
        cnt[c->col]--;
        cnt[col]++;
    }
}

/* the pitch a band up (dir 1) or down (-1), on the scale */
static int32_t eco_band_pitch(const cell_t *c, int32_t dir)
{
    return eco_snap(c->g.pitch16 + dir * ((eco.span16 / (int32_t)ECO_COLS + 8) & ~15));
}

/* a predator (pred from half up) keeps its pitch band: with no prey in its niche it heads across, to the row next to
 * it with the most prey by mass; prey here or none near, it goes home across (eco_home) */
static __attribute__((noinline)) void eco_chase(cell_t *c, uint8_t cnt[ECO_COLS])
{
    int32_t prey[3] = {0, 0, 0};
    uint32_t i;
    for (i = 0; i < ECO_CELLS; i++) {
        const cell_t *q = &eco.cell[i];
        if (q->id && q->col == c->col && q->m < c->m && q->row + 1u >= c->row && q->row <= c->row + 1u)
            prey[q->row + 1u - c->row] += q->m;
    }
    if (prey[1] || (!prey[0] && !prey[2])) {
        eco_home(c, cnt);
        return;
    }
    i = prey[2] > prey[0] || (prey[2] == prey[0] && (eco_rand() & 1u)) ? c->row + 1u : c->row - 1u;
    if (i >= ECO_ROWS)
        return;
    eco_move(c, c->g.pitch16, ((int32_t)i * 128 + 64) / (int32_t)ECO_ROWS - 64);
}

/* a young cell in a band over its limit (ECO_LIM) heads up a band, or down from the top, if there is room (the
 * youngest of the band: young set). A cell that cannot eat where it is (its niche crowded, or too little energy for
 * the mass there) heads for the neighbouring niche with the most energy per mass, if that is 1.5 times as rich (across,
 * its pan to the row's centre) or 4 times (up or down, its pitch by a band: dearer, young only, eco_band_ok); half
 * that for a niche it has been in, which it also likes twice as much; a niche with no room for it, never. Crowded with
 * no such niche, a band up or down with room, grown or not. A predator chases prey instead (eco_chase). cnt: the
 * bands' cells, kept up to date */
static void eco_drift_pick(cell_t *c, const int32_t load[ECO_COLS][ECO_ROWS], uint8_t cnt[ECO_COLS], int young)
{
    int32_t l0 = load[c->col][c->row], e0 = eco.niche[c->col][c->row].E, be = 0, bl = 1, bx = -1, by = 0;
    uint32_t i;
    if (c->g.pred >= 128u) {
        eco_chase(c, cnt);
        return;
    }
    if (young && cnt[c->col] > ECO_LIM[c->col]) {
        int32_t dir = c->col + 1u < ECO_COLS ? 1 : -1;
        if (!eco_band_ok(c, c->col + (uint32_t)dir, cnt))
            return;
        eco_move(c, eco_band_pitch(c, dir), c->g.pan);
        cnt[c->col]--;
        cnt[c->col + (uint32_t)dir]++;
        return;
    }
    if (l0 <= eco.k && (int64_t)e0 * ECO_M1 >= (int64_t)ECO_E_HUNGRY * l0) {
        eco_home(c, cnt);
        return;
    }
    for (i = 0; i < 4u; i++) {
        uint32_t x = c->col + (uint32_t)ECO_DX[i], y = c->row + (uint32_t)ECO_DY[i], been;
        int32_t en, ln;
        if (x >= ECO_COLS || y >= ECO_ROWS || (ECO_DX[i] && (!eco_young(c) || !eco_band_ok(c, x, cnt))))
            continue;
        ln = load[x][y] + c->m;
        if (ln > eco.k)
            continue;
        been = c->been >> (x * ECO_ROWS + y) & 1u;
        en = eco.niche[x][y].E << been;
        if ((int64_t)en * l0 * 2 > (int64_t)e0 * ln * (ECO_DX[i] ? 8 : 3) && (int64_t)en * bl > (int64_t)be * ln) {
            be = en;
            bl = ln;
            bx = (int32_t)x;
            by = (int32_t)y;
        }
    }
    for (i = 0; bx < 0 && l0 > eco.k && i < 2u; i++) {   /* crowded, nowhere to go: a band up or down, grown or not */
        uint32_t x = c->col + (uint32_t)ECO_DX[i];
        int32_t en, ln;
        if (x >= ECO_COLS || !eco_band_ok(c, x, cnt) || (ln = load[x][c->row] + c->m) > eco.k)
            continue;
        en = eco.niche[x][c->row].E;
        if ((int64_t)en * bl >= (int64_t)be * ln) {
            be = en;
            bl = ln;
            bx = (int32_t)x;
            by = (int32_t)c->row;
        }
    }
    if (bx < 0)
        return;
    eco_move(c, bx == (int32_t)c->col ? c->g.pitch16 : eco_band_pitch(c, bx - (int32_t)c->col),
             by == (int32_t)c->row ? c->g.pan : (by * 128 + 64) / (int32_t)ECO_ROWS - 64);
    cnt[c->col]--;
    cnt[bx]++;
}

/* the cell a drifting cell would run into at pan, p16: touching it there, nearer to it than now; 0 none */
static __attribute__((noinline)) cell_t *eco_bump(const cell_t *c, int32_t pan, int32_t p16)
{
    int32_t r = eco_radius(c), x = eco_fx(pan), y = eco_fy(p16), x0 = eco_fx(c->g.pan), y0 = eco_fy(c->pos16);
    uint32_t i;
    for (i = 0; i < ECO_CELLS; i++) {
        cell_t *q = &eco.cell[i];
        int32_t qx, qy, rr, d;
        if (!q->id || q == c)
            continue;
        qx = eco_fx(q->g.pan);
        qy = eco_fy(eco_y16(q));
        rr = (r + eco_radius(q)) * 16;
        if (x - qx >= rr || qx - x >= rr || y - qy >= rr || qy - y >= rr)
            continue;
        d = (x - qx) * (x - qx) + (y - qy) * (y - qy);
        if (d < rr * rr && d < (x0 - qx) * (x0 - qx) + (y0 - qy) * (y0 - qy))
            return q;
    }
    return 0;
}

/* a pitch a drifting cell may not go to: out of the range, or (grown) out of its band */
static int eco_band_out(const cell_t *c, int32_t p16)
{
    return p16 < eco.lo16 || p16 >= eco.lo16 + eco.span16 || (!eco_young(c) && eco_col(p16) != c->col);
}

/* in the way of q, a step round it at right angles to the way it was going: across (dp), by a pan unit's worth of
 * pitch; up or down (dy), by a pan unit when that much has gone into it. To its side (at first away from q's centre),
 * the other if that is blocked or out of bounds; 0 no way round now */
static int eco_round(cell_t *c, const cell_t *q, int32_t dp, int32_t dy, int32_t *pan, int32_t *p16)
{
    uint32_t t;
    if (dp) {
        int32_t k = (dp < 0 ? -dp : dp) * ECO_FW * eco.span16 / (64 * ECO_FH) + 1;
        if (!c->side)
            c->side = (int8_t)(eco_fy(*p16) >= eco_fy(eco_y16(q)) ? 1 : -1);
        for (t = 0; t < 2u; t++, c->side = (int8_t)-c->side) {
            int32_t y = *p16 + c->side * k;
            if (!eco_band_out(c, y) && !eco_bump(c, *pan, y)) {
                *p16 = y;
                return 1;
            }
        }
        return 0;
    }
    c->as = (uint16_t)(c->as + (dy < 0 ? -dy : dy) * 64 * ECO_FH * 256 / (ECO_FW * eco.span16) + 1);
    if (c->as < 256u)
        return -1;
    c->as -= 256u;
    if (!c->side)
        c->side = (int8_t)(eco_fx(*pan) >= eco_fx(q->g.pan) ? 1 : -1);
    for (t = 0; t < 2u; t++, c->side = (int8_t)-c->side) {
        int32_t p = *pan + c->side;
        if (p >= -64 && p <= 63 && !eco_bump(c, p, *p16)) {
            *pan = p;
            return 1;
        }
    }
    return 0;
}

/* jammed (no way round for a while): a grazer heads a band up or down instead, to its side round the jam first (else
 * up), if that band has room and a ceiling it fits under, grown or not; 1 it does */
static __attribute__((noinline)) int eco_escape(cell_t *c)
{
    int32_t dir = c->side ? c->side : 1;
    uint32_t t, i;
    if (c->g.pred >= 128u || eco_col(c->to_pitch) != c->col)
        return 0;                                /* (once a drift: its goal still in its band) */
    for (t = 0; t < 2u; t++, dir = -dir) {
        uint32_t col = c->col + (uint32_t)dir, n = 0;
        if (col >= ECO_COLS || eco_size(c) > ECO_CAP[col])
            continue;
        for (i = 0; i < ECO_CELLS; i++) {
            const cell_t *q = &eco.cell[i];
            n += q->id && (q->moving ? eco_col(q->to_pitch) : q->col) == col;
        }
        if (n >= ECO_LIM[col])
            continue;
        c->to_pitch = (int16_t)eco_band_pitch(c, dir);
        c->side = 0;
        c->stuck = 0;
        return 1;
    }
    return 0;
}

/* a tick of drift at speed v (/256): pan and pitch go on, at a cost by the distance (pitch dearer), the pitch heard
 * gliding or stepping through the scale's notes, the niche following; a sounding DRONE note takes the pitch. A cell in
 * its way: one way at a time if that is free, else round it (eco_round), jammed a second a band away (eco_escape).
 * Its goal under that cell, or no way on for ECO_STUCK ticks: it stays where it is, its new home */
static __attribute__((noinline)) void eco_drift(cell_t *c, uint32_t v)
{
    voice_t *vo;
    cell_t *q = 0;
    int32_t n, dp, dy, pan = c->g.pan, y = c->pos16;
    c->ap = (uint16_t)(c->ap + ((ECO_V_PAN * v >> 8) * (256u + (c->run * 3u * c->g.pred >> 8)) >> 8));
    if (c->run < 255u)
        c->run++;                                /* a predator: up to 4 times as fast across in 5 s */
    n = c->ap >> 8;
    c->ap &= 255u;
    dp = clamp(c->to_pan - pan, -n, n);
    c->ay = (uint16_t)(c->ay + (ECO_V_PITCH * v >> 8));
    n = c->ay >> 8;
    c->ay &= 255u;
    dy = clamp(c->to_pitch - y, -n, n);
    if ((dp || dy) && (q = eco_bump(c, pan + dp, y + dy)) != 0) {
        int32_t gx = eco_fx(c->to_pan) - eco_fx(q->g.pan), gy = eco_fy(c->to_pitch) - eco_fy(eco_y16(q));
        int32_t rr = (eco_radius(c) + eco_radius(q)) * 16;
        if (gx * gx + gy * gy < rr * rr || ++c->stuck >= ECO_STUCK) {
            c->moving = 0;
            c->g.pitch16 = (int16_t)(c->glide ? c->pos16 : eco_snap(c->pos16));
            c->home16 = c->g.pitch16;
            c->hpan = c->g.pan;
            eco_at(c);
            return;
        }
        if (dp && dy && !eco_bump(c, pan + dp, y))
            dy = 0;
        else if (dp && dy && !eco_bump(c, pan, y + dy))
            dp = 0;
        else {
            int r = eco_round(c, q, dp, dy, &pan, &y);
            if (r > 0)
                c->stuck = 0;
            else if (!r && c->stuck >= ECO_STUCK / 5u)
                eco_escape(c);
            dp = dy = 0;
        }
    }
    pan += dp;
    y += dy;
    if (dp || dy)
        c->stuck = 0;
    c->e -= (pan - c->g.pan < 0 ? c->g.pan - pan : pan - c->g.pan) * ECO_C_PAN;
    c->e -= (y - c->pos16 < 0 ? c->pos16 - y : y - c->pos16) * ECO_C_PITCH;
    c->g.pan = (int8_t)pan;
    c->pos16 = (int16_t)y;
    c->g.pitch16 = (int16_t)(c->glide || c->pos16 == c->to_pitch ? c->pos16 : eco_snap(c->pos16));
    if (c->g.pan == c->to_pan && c->pos16 == c->to_pitch)
        c->moving = 0;
    eco_at(c);
    if (eco.mode && c->open && (vo = voice_of(&trk[0], c->h)) != 0)
        vo->pitch16 = c->g.pitch16;
}

/* DRONE: a voice for one more held note (the others: the cells' own notes sounding) */
static int eco_voice_free(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < ECO_CELLS; i++)
        n += eco.cell[i].id && eco.cell[i].open;
    return n < NVOICE;
}

/* a step of a cell's motif: a note on its bit, not skipped by chance */
static int eco_hit(cell_t *c)
{
    uint32_t n = c->g.len ? c->g.len : 1u, s = c->step < n ? c->step : 0u;
    c->step = (uint8_t)(s + 1u < n ? s + 1u : 0u);
    if (!(c->g.mask >> s & 1u))
        return 0;
    return (eco_rand() & 255u) >= c->g.skip;
}

/* prey for a cell: one lighter than it (half or less: half), any pitch, in its niche or the four next to it, by
 * chance; 0 none */
static cell_t *eco_prey(const cell_t *p, int half)
{
    cell_t *pick = 0;
    uint32_t i, n = 0;
    for (i = 0; i < ECO_CELLS; i++) {
        cell_t *c = &eco.cell[i];
        uint32_t dx = c->col > p->col ? c->col - p->col : p->col - c->col;
        uint32_t dy = c->row > p->row ? c->row - p->row : p->row - c->row;
        if (c->id && c != p && (half ? c->m * 2 <= p->m : c->m < p->m) && dx + dy <= 1u && eco_rand() % ++n == 0u)
            pick = c;
    }
    return pick;
}

/* a grown cell that cannot eat where it is, prey near, turns further towards hunting by chance (G_PRED; at 100 one
 * look in 256, once turned one in 64: halfway is the worst place to stay); never with another predator in its band */
static void eco_turn(cell_t *c, const int32_t load[ECO_COLS][ECO_ROWS], uint32_t chance)
{
    int32_t l0 = load[c->col][c->row], e0 = eco.niche[c->col][c->row].E;
    uint32_t i;
    if (eco_young(c) || c->g.pred == 255u || (l0 <= eco.k && (int64_t)e0 * ECO_M1 >= (int64_t)ECO_E_HUNGRY * l0))
        return;
    if (eco_rand() % (c->g.pred ? 6400u : 25600u) >= chance)
        return;
    for (i = 0; i < ECO_CELLS; i++) {
        const cell_t *q = &eco.cell[i];
        if (q->id && q != c && q->col == c->col && q->g.pred >= 128u)
            return;
    }
    if (!eco_prey(c, 1))
        return;
    c->g.pred = (uint8_t)(c->g.pred + ECO_PRED_STEP < 255 ? c->g.pred + ECO_PRED_STEP : 255);
    ECO_EVENT(EV_TURN, c);
}

/* a predator's attack, while its note sounds: on a lighter cell near, caught by its skill (pred^2) and the weight gap
 * (sure from half its mass down, never at its own); it takes the prey's energy (70 %), its body as energy and
 * material. Rare: ECO_HUNT_COOL after each */
static __attribute__((noinline)) void eco_hunt(cell_t *c, int32_t load[ECO_COLS][ECO_ROWS])
{
    cell_t *q = eco_prey(c, 0);
    uint32_t gap;
    if (!q)
        return;
    c->hcool = ECO_HUNT_COOL * 256u;
    c->e -= ECO_C_HUNT;
    gap = (uint32_t)clamp(512 - 512 * q->m / c->m, 0, 256);
    if (eco_rand() % 65025u >= (uint32_t)c->g.pred * c->g.pred * gap / 256u)
        return;
    c->e += (q->e > 0 ? q->e * 7 / 10 : 0) + q->m * ECO_MASS_E * 16 * 6 / 10;
    c->b += q->b + ECO_B_REP * q->m / ECO_M1 / 2;
    load[q->col][q->row] -= q->m;
    eco_die(q, EV_EATEN);
}

/* FOOD above 50: food grows by itself where there is some (ECO_E_HAS), up to ECO_E_WILD, the faster the more FOOD,
 * material with it; a niche half full sows ECO_E_SOW into one neighbour now and then; none has any, it starts in a
 * random niche of the middle row */
static __attribute__((noinline)) void eco_wild(int32_t food)
{
    int32_t g = EQ(0.5) + (food - 50) * EQ(2.5) / 50, any = 0;   /* a tick: 0.55 .. 3 */
    uint32_t x, y, k;
    for (x = 0; x < ECO_COLS; x++)
        for (y = 0; y < ECO_ROWS; y++) {
            niche_t *n = &eco.niche[x][y];
            if (n->E < ECO_E_HAS)
                continue;
            any = 1;
            if (n->E < ECO_E_WILD) {
                n->E += g;
                n->B += g / 8;
            }
            if (n->E >= ECO_E_WILD / 2 && eco_rand() % 25u == 0u) {
                uint32_t nx, ny;
                k = eco_rand() % 4u;
                nx = x + (uint32_t)ECO_DX[k];
                ny = y + (uint32_t)ECO_DY[k];
                if (nx < ECO_COLS && ny < ECO_ROWS) {
                    n->E -= ECO_E_SOW;
                    eco.niche[nx][ny].E += ECO_E_SOW;
                }
            }
        }
    if (!any && eco_rand() % 50u == 0u)
        eco.niche[eco_rand() % ECO_COLS][ECO_ROWS / 2].E += ECO_E_SOW;
}

/* one tick: the regulators, the stocks leak and rot, the held keys feed their columns, the food grows (FOOD above
 * 50), full niches overflow, the open windows take energy and material, upkeep, age, death, growth, drift, budding */
static __attribute__((noinline)) void eco_tick(void)
{
    static const uint8_t W[ECO_ROWS] = {1, 2, 4, 2, 1};   /* a key's supply over its column's rows, /10 */
    static int32_t open_m[ECO_COLS][ECO_ROWS], load[ECO_COLS][ECO_ROWS], stock[ECO_COLS][ECO_ROWS],
        bstock[ECO_COLS][ECO_ROWS];                       /* (static: off the ISR's stack) */
    static uint8_t cnt[ECO_COLS], yng[ECO_COLS];          /* a band's cells (those heading there), its youngest + 1 */
    int32_t food = clamp(song.g[G_FOOD], 0, 100), ein = ECO_E_IN * food / 50, bin = ECO_B_IN * food / 50, leak;
    uint32_t i, x, y, heat, v, amax, turn;
    heat = eco_heat();
    eco_range();
    eco.k = clamp(song.g[G_DENS], 1, 8) * ECO_M1;
    eco.mut = (uint32_t)clamp(song.g[G_MUT], 0, 100);
    eco.move = (uint32_t)clamp(song.g[G_MOVE], 0, 100);
    amax = (uint32_t)clamp(song.g[G_LIFE], 10, 300) * 50u * 256u;
    leak = 16 + clamp(song.g[G_DECAY], 0, 100) * 296 / 100;   /* E's leak per tick, /65536: 0.25 % at 50 */
    v = heat * heat / 256u * eco.move / 50u;          /* drift by the temperature squared: x0.06 .. x3 */
    turn = (uint32_t)clamp(song.g[G_PRED], 0, 100);
    eco.ticks++;
    memset(open_m, 0, sizeof open_m);
    memset(load, 0, sizeof load);
    memset(eco.held, 0, sizeof eco.held);
    for (i = 0; i < 128u; i++)                       /* the keys held, by their columns as the range is now */
        if (eco.kvel[i])
            eco.held[eco_col((int32_t)i * 16)] += eco.kvel[i];
    for (x = 0; x < ECO_COLS; x++)
        for (y = 0; y < ECO_ROWS; y++) {
            niche_t *n = &eco.niche[x][y];
            int32_t rot = (int32_t)((int64_t)n->O * leak * 2 >> 16);   /* O_rot: 0.5 % at 50 */
            n->E -= (int32_t)((int64_t)n->E * leak >> 16);
            n->B -= (int32_t)((int64_t)n->B * leak / 5 >> 16);
            n->O -= rot;
            n->E += rot * 3 / 10;
            if (eco.frag[x][y].n)
                eco_frag_rot(&eco.frag[x][y], leak);
            for (i = 0; i < ECO_COLS; i++) {         /* the held keys: their column, a quarter to the next ones */
                int32_t k = eco.held[i] * (i == x ? 4 : i + 1u == x || x + 1u == i ? 1 : 0);
                n->E += ein * W[y] / 10 * k / 400;
                n->B += bin * W[y] / 10 * k / 400;
            }
        }
    if (food > 50)
        eco_wild(food);
    for (x = 0; x < ECO_COLS; x++)                   /* the overflow: a tenth of the excess, a quarter each way */
        for (y = 0; y < ECO_ROWS; y++) {
            const niche_t *n = &eco.niche[x][y];
            stock[x][y] = n->E > ECO_E_SPILL ? (n->E - ECO_E_SPILL) / 40 : 0;
            bstock[x][y] = n->B > ECO_B_SPILL ? (n->B - ECO_B_SPILL) / 40 : 0;
        }
    for (x = 0; x < ECO_COLS; x++)
        for (y = 0; y < ECO_ROWS; y++)
            for (i = 0; i < 4u; i++) {
                uint32_t nx = x + (uint32_t)ECO_DX[i], ny = y + (uint32_t)ECO_DY[i];
                if (nx >= ECO_COLS || ny >= ECO_ROWS)
                    continue;
                eco.niche[x][y].E -= stock[x][y];
                eco.niche[nx][ny].E += stock[x][y];
                eco.niche[x][y].B -= bstock[x][y];
                eco.niche[nx][ny].B += bstock[x][y];
            }
    memset(cnt, 0, sizeof cnt);
    memset(yng, 0, sizeof yng);
    for (i = 0; i < ECO_CELLS; i++) {
        const cell_t *c = &eco.cell[i];
        if (!c->id)
            continue;
        load[c->col][c->row] += c->m;
        if (c->open)
            open_m[c->col][c->row] += c->m;
        cnt[c->moving ? eco_col(c->to_pitch) : c->col]++;
        if (!c->moving && eco_young(c) && (!yng[c->col] || c->age < eco.cell[yng[c->col] - 1u].age))
            yng[c->col] = (uint8_t)(i + 1u);
    }
    for (x = 0; x < ECO_COLS; x++)
        for (y = 0; y < ECO_ROWS; y++)
            if (eco.frag[x][y].n && !load[x][y] && eco.niche[x][y].E >= ECO_E_SPORE && cnt[x] < ECO_LIM[x] &&
                !(eco_rand() & 63u))
                eco_sprout(x, y);
    for (x = 0; x < ECO_COLS; x++)
        for (y = 0; y < ECO_ROWS; y++) {
            stock[x][y] = eco.niche[x][y].E;
            bstock[x][y] = eco.niche[x][y].B;
        }
    for (i = 0; i < ECO_CELLS; i++) {
        cell_t *c = &eco.cell[i];
        niche_t *n;
        if (!c->id)
            continue;
        n = &eco.niche[c->col][c->row];
        if (c->open && !voice_of(&trk[0], c->h))
            c->open = 0;                             /* its voice taken (keys in PLAY, a panic): no window */
        if (c->open && open_m[c->col][c->row]) {
            int32_t om = open_m[c->col][c->row], l = load[c->col][c->row], herb = (255 - c->g.pred) * (255 - c->g.pred);
            int32_t te = eco_share(stock[c->col][c->row], n->E,
                                   ((int32_t)((ECO_R_MAX * c->m / ECO_M1) * heat >> 8) * herb >> 16) * c->eat >> 8,
                                   c->m, om, l);
            int32_t tb = eco_share(bstock[c->col][c->row], n->B,
                                   ((int32_t)((ECO_RB_MAX * c->m / ECO_M1) * heat >> 8) * herb >> 16) * c->eat >> 8,
                                   c->m, om, l);
            c->e += te;
            n->E -= te;
            c->b += tb;
            n->B -= tb;
            if (eco.mode)                            /* DRONE: the note costs by the tick */
                c->e -= (int32_t)((int64_t)ECO_C_NOTE * eco_size(c) / 100 * ECO_TICK / (FS / 4u) * heat >> 8);
            if (eco.frag[c->col][c->row].n && ((eco.ticks ^ i) & 3u) == 2u)
                eco_absorb(c, &eco.frag[c->col][c->row]);
        }
        c->e -= (int32_t)((ECO_C_BASE * c->m / ECO_M1) * heat >> 8);
        if (c->e <= 0 || (c->age += heat) > amax) {
            load[c->col][c->row] -= c->m;
            eco_die(c, c->e <= 0 ? EV_HUNGER : EV_AGE);
            continue;
        }
        eco_grow(c, heat);
        if (((eco.ticks ^ i) & 7u) == 1u)
            eco_turn(c, load, turn);
        if (c->hcool)
            c->hcool = c->hcool > heat ? c->hcool - heat : 0u;
        else if (c->g.pred && c->open && ((eco.ticks ^ i) & 7u) == 2u)
            eco_hunt(c, load);
        if (c->moving)
            eco_drift(c, v);
        else if (v && ((eco.ticks ^ i) & 7u) == 0u)    /* each cell looks around every 8th tick */
            eco_drift_pick(c, load, cnt, yng[c->col] == i + 1u);
        if (c->cool)
            c->cool = c->cool > heat ? c->cool - heat : 0u;
        else if (c->age >= ECO_A_MIN * 256u && !eco_young(c) && c->e >= ECO_E_REP && c->b >= ECO_B_REP)
            eco_bud(c, load, cnt);
    }
}

/* every block (events_block, after the keys and MIDI IN): a pause or a clearing, a mode change, the cells' notes (PULSE:
 * one an interval, its gate long; DRONE: one a cycle of ECO_DRONE intervals, held for its gate's share of it, if a
 * voice is free), then the ticks due */
static __attribute__((noinline)) void eco_block(uint32_t n)
{
    uint32_t i;
    if (eco_paused || eco_clear) {
        for (i = 0; i < ECO_CELLS; i++)
            if (eco.cell[i].id && eco.cell[i].open)
                eco_note_off(&eco.cell[i]);
        if (eco_clear) {
            eco_clear = 0;
            memset(eco.cell, 0, sizeof eco.cell);
            memset(eco.niche, 0, sizeof eco.niche);
            memset(eco.frag, 0, sizeof eco.frag);
            eco.acc = 0;
        }
        if (eco_paused)
            return;
    }
    if (eco.beat != beat_samples())
        eco_lens();
    if (eco.mode != (song.g[G_EMODE] != 0)) {
        eco.mode = song.g[G_EMODE] != 0;
        for (i = 0; i < ECO_CELLS; i++)
            if (eco.cell[i].id && eco.cell[i].open)
                eco_note_off(&eco.cell[i]);
    }
    for (i = 0; i < ECO_CELLS; i++) {
        cell_t *c = &eco.cell[i];
        uint32_t len;
        if (!c->id)
            continue;
        len = eco.len[c->g.div < NELEM(ECO_DIV) ? c->g.div : 0u] * (eco.mode ? ECO_DRONE : 1u);
        c->pos += n;
        if (c->pos >= len) {
            c->pos = c->pos - len < len ? c->pos - len : 0u;
            if (eco_rand() % 800u < (c->grid ? eco_chaos() : 100u - eco_chaos())) {
                c->grid ^= 1u;                       /* CHAOS: off the grid now and then, or back onto it */
                if (!c->grid)
                    c->pos += eco_rand() % (len / 8u + 1u);
            }
            if (c->grid)
                c->pos = eco_grid_pos(c, len, n);
            if (c->open)
                eco_note_off(c);
            if (!eco_hit(c))
                continue;                            /* a rest of its rhythm, or a skip */
            if (c->moving && eco_rand() % 100u < (uint32_t)clamp(song.g[G_SLIDE], 0, 100))
                continue;                            /* drifting: silent this time */
            if (!eco.mode)
                eco_note_on(c, len * c->g.gate / 256u);
            else if (eco_voice_free())
                eco_note_on(c, 0);
        } else if (c->open && c->pos >= len / 256u * c->g.gate) {
            eco_note_off(c);
        }
    }
    eco.clock += n;
    eco.acc += n;
    while (eco.acc >= ECO_TICK) {
        eco.acc -= ECO_TICK;
        eco_tick();
    }
}

/* the keys switched to playing (or a panic): no column fed any longer */
static void eco_keys_up(void)
{
    memset(eco.held, 0, sizeof eco.held);
    memset(eco.kvel, 0, sizeof eco.kvel);
}
