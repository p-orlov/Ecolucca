/* SPDX-License-Identifier: GPL-3.0-only */
/* The ecosystem's host bench: the firmware's sound and ecosystem sources (no UI, no hardware) driven by a scenario of
 * key presses, faster than real time. Out, in DIR: out.wav (what the FM-1 would play), cells.csv (every living cell
 * each LOG_TICKS ticks), events.csv (seeds, feeds, notes, deaths); on stdout a summary and a hash of all three (the same
 * scenario and seed: the same hash).
 *   build/host/eco_bench SCENARIO [--seed N] [--out DIR] [--secs S] [--set LABEL=V ...]
 * --set: a global parameter by its label (params.c GP), e.g. --set TEMP=80 (the regulators: TEMP MUT FOOD DENS LIFE
 * MOVE).
 * SCENARIO: one event a line, "TIME key NOTE down [VEL]" or "TIME key NOTE up" (TIME in seconds; NOTE a MIDI number
 * or a name, C4 = 60; VEL 100 if left out); '#' starts a comment. Without --secs it runs 20 s past the last line.
 * Exit 1 when the scenario presses keys and nothing is seeded or played. ECO_NICHE=1 in the environment: every 5 s the
 * bands' cells, the cells overlapping and the niches' energy on stdout. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define __attribute__(x)
#define memset felucca_memset
#define memcpy felucca_memcpy
#define memcmp felucca_memcmp
#include "felucca_tables.h"
#include "../firmware/src/libc.c"
#undef memset
#undef memcpy
#undef memcmp
static struct { volatile uint32_t notes, buttons; } fm1_in;
static void bench_event(int kind, const void *c);
#define ECO_EVENT(kind, c) bench_event((kind), (c))
#include "../firmware/src/core.h"
#include "../firmware/src/engines.c"
#include "../firmware/src/params.c"
#include "../firmware/src/mod.c"
#include "../firmware/src/voice.c"
#include "../firmware/src/fx.c"
#include "../firmware/src/eco.c"
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
#include "../firmware/src/usb.c"
#include "../firmware/src/input.c"

#define LOG_TICKS 10u                                   /* cells.csv: every 200 ms */
static FILE *f_ev, *f_cell;
static uint64_t now;                                    /* samples rendered */
static uint32_t hash = 2166136261u, n_ev[12];

static void hmix(const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--) {
        hash ^= *b++;
        hash *= 16777619u;
    }
}
static void out_line(FILE *f, const char *s)
{
    fputs(s, f);
    hmix(s, strlen(s));
}

static void bench_event(int kind, const void *cp)
{
    static const char *const NAME[12] = {"seed", "feed", "note", "hunger", "age", "bud", "drift", "frag", "take",
                                         "eaten", "turn", "sprout"};
    const cell_t *c = cp;
    char line[200];
    n_ev[kind]++;
    if (c)
        snprintf(line, sizeof line, "%.4f,%s,%u,%u,%u,%u,%d,%d,%d,%d\n", (double)now / FS, NAME[kind], c->id,
                 c->parent, c->col, c->row, c->g.pitch16, c->g.pan, c->e, c->m);
    else
        snprintf(line, sizeof line, "%.4f,%s,,,,,,,,\n", (double)now / FS, NAME[kind]);
    out_line(f_ev, line);
}

static void log_cells(void)
{
    char line[200];
    uint32_t i;
    for (i = 0; i < ECO_CELLS; i++) {
        const cell_t *c = &eco.cell[i];
        if (!c->id)
            continue;
        snprintf(line, sizeof line, "%.2f,%u,%u,%u,%u,%d,%d,%d,%d,%d,%d,%u,%u\n", (double)now / FS, c->id, c->parent,
                 c->col, c->row, c->g.pitch16, c->g.pan, ECO_DIV[c->g.div], c->e, c->b, c->m, c->age >> 8, c->open);
        out_line(f_cell, line);
    }
}

typedef struct { double t; uint32_t note, vel, down, line; } sev_t;
static sev_t sev[4096];
static uint32_t nsev;

static int parse_note(const char *s)
{
    static const int8_t BASE[7] = {9, 11, 0, 2, 4, 5, 7};   /* A B C D E F G */
    int n;
    if (*s >= '0' && *s <= '9')
        return atoi(s);
    if (*s < 'A' || *s > 'G')
        return -1;
    n = BASE[*s++ - 'A'];
    if (*s == '#')
        n++, s++;
    else if (*s == 'b')
        n--, s++;
    return n + 12 * (atoi(s) + 1);
}
static int sev_cmp(const void *a, const void *b)
{
    const sev_t *x = a, *y = b;
    return x->t < y->t ? -1 : x->t > y->t ? 1 : (int)x->line - (int)y->line;
}
static int load_scenario(const char *path)
{
    char line[256], what[16], note[16], dir[16];
    FILE *f = fopen(path, "r");
    uint32_t ln = 0;
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f)) {
        double t;
        int vel = 100, n, k;
        char *h = strchr(line, '#');
        ln++;
        if (h)
            *h = 0;
        k = sscanf(line, "%lf %15s %15s %15s %d", &t, what, note, dir, &vel);
        if (k <= 0)
            continue;
        n = parse_note(note);
        if (k < 4 || strcmp(what, "key") || n < 0 || n > 127 || (strcmp(dir, "down") && strcmp(dir, "up")) ||
            nsev == NELEM(sev)) {
            fprintf(stderr, "%s:%u: expected \"TIME key NOTE down [VEL]\" or \"TIME key NOTE up\"\n", path, ln);
            fclose(f);
            return -1;
        }
        sev[nsev].t = t;
        sev[nsev].note = (uint32_t)n;
        sev[nsev].vel = (uint32_t)clamp(vel, 1, 127);
        sev[nsev].down = !strcmp(dir, "down");
        sev[nsev].line = ln;
        nsev++;
    }
    fclose(f);
    qsort(sev, nsev, sizeof sev[0], sev_cmp);
    return 0;
}

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t data = frames * 4u, v;
    uint16_t s;
    fwrite("RIFF", 1, 4, f);
    v = 36u + data;
    fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v = 16;
    fwrite(&v, 4, 1, f);
    s = 1;
    fwrite(&s, 2, 1, f);
    s = 2;
    fwrite(&s, 2, 1, f);
    v = FS;
    fwrite(&v, 4, 1, f);
    v = FS * 4u;
    fwrite(&v, 4, 1, f);
    s = 4;
    fwrite(&s, 2, 1, f);
    s = 16;
    fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data, 4, 1, f);
}

/* the part as at power-on (main.c felucca_init): the defaults, then ANALOG's preset PRESET_DEF */
static void bench_part(void)
{
    static const uint8_t FX_DEF[4] = {0, 24, 28, 36};
    track_t *t = &trk[0];
    const preset_t *pr = &ENGINES[0]->presets[PRESET_DEF];
    uint32_t i;
    for (i = 0; i < G_COUNT; i++)
        song.g[i] = GP[i].def;
    for (i = 0; i < P_E0; i++)
        t->p[i] = TP[i].def;
    for (i = 0; i < 8u; i++)
        t->p[P_E0 + i] = pr->e[i];
    for (i = 0; i < 4u; i++)
        t->p[P_ATK + i] = pr->env[i];
    t->p[P_ED_FLT] = pr->fenv;
    t->p[P_VOICE] = pr->mono ? V_LEGATO : V_POLY;
    for (i = 0; i < 4u; i++)
        t->p[P_DIST + i] = (int16_t)(pr->fx[i] ? pr->fx[i] - 1 : FX_DEF[i]);
    t->engine = t->eng_req = 0;
    song.master_q12 = MASTER_FULL;
}

int main(int argc, char **argv)
{
    const char *scen = 0, *dir = "build/eco", *set[16];
    uint32_t seed = 1, k, ev = 0, live_max = 0, last_tick = 0, downs = 0, nset = 0;
    double secs = 0;
    uint64_t end;
    int32_t out[2u * CTL], peak = 0;
    char path[512];
    FILE *f_wav;
    for (k = 1; k < (uint32_t)argc; k++) {
        if (!strcmp(argv[k], "--seed") && k + 1u < (uint32_t)argc)
            seed = (uint32_t)strtoul(argv[++k], 0, 0);
        else if (!strcmp(argv[k], "--out") && k + 1u < (uint32_t)argc)
            dir = argv[++k];
        else if (!strcmp(argv[k], "--secs") && k + 1u < (uint32_t)argc)
            secs = atof(argv[++k]);
        else if (!strcmp(argv[k], "--set") && k + 1u < (uint32_t)argc && nset < NELEM(set))
            set[nset++] = argv[++k];
        else
            scen = argv[k];
    }
    if (!scen || load_scenario(scen)) {
        fprintf(stderr, "usage: eco_bench SCENARIO [--seed N] [--out DIR] [--secs S] [--set LABEL=V ...]\n");
        return 2;
    }
    snprintf(path, sizeof path, "mkdir -p '%s'", dir);
    if (system(path))
        return 2;
    snprintf(path, sizeof path, "%s/out.wav", dir);
    f_wav = fopen(path, "wb");
    snprintf(path, sizeof path, "%s/events.csv", dir);
    f_ev = fopen(path, "w");
    snprintf(path, sizeof path, "%s/cells.csv", dir);
    f_cell = fopen(path, "w");
    if (!f_wav || !f_ev || !f_cell)
        return 2;
    out_line(f_ev, "t,event,id,parent,col,row,pitch16,pan,e,m\n");
    out_line(f_cell, "t,id,parent,col,row,pitch16,pan,div,e,b,m,age,open\n");
    bench_part();
    for (k = 0; k < nset; k++) {
        const char *eq = strchr(set[k], '=');
        uint32_t g;
        for (g = 0; eq && g < G_COUNT; g++)
            if (strlen(GP[g].label) == (size_t)(eq - set[k]) && !strncmp(GP[g].label, set[k], (size_t)(eq - set[k])))
                break;
        if (!eq || g == G_COUNT) {
            fprintf(stderr, "--set %s: no such global parameter\n", set[k]);
            return 2;
        }
        song.g[g] = (int16_t)clamp(atoi(eq + 1), GP[g].min, GP[g].max);
    }
    eco_seed_rng(seed);
    end = (uint64_t)((secs > 0 ? secs : (nsev ? sev[nsev - 1u].t : 0) + 20.0) * FS);
    wav_header(f_wav, 0);
    while (now < end) {
        uint32_t i, live = 0;
        while (ev < nsev && (uint64_t)(sev[ev].t * FS) <= now) {
            eco_key(sev[ev].note, sev[ev].vel, (int)sev[ev].down);
            downs += sev[ev].down;
            ev++;
        }
        mix_block(out, CTL);
        for (i = 0; i < 2u * CTL; i++) {
            int32_t x = clamp(out[i], -32768, 32767);
            int16_t s = (int16_t)x;
            peak = x > peak ? x : -x > peak ? -x : peak;
            fwrite(&s, 2, 1, f_wav);
            hmix(&s, 2);
        }
        now += CTL;
        if (eco.ticks != last_tick) {
            last_tick = eco.ticks;
            if (last_tick % LOG_TICKS == 0u)
                log_cells();
            if (getenv("ECO_NICHE") && last_tick % 250u == 0u) {
                uint32_t x, y, n[ECO_COLS] = {0};
                for (x = 0; x < ECO_CELLS; x++)
                    if (eco.cell[x].id)
                        n[eco.cell[x].moving ? eco_col(eco.cell[x].to_pitch) : eco.cell[x].col]++;
                printf("%6.1f bands %u %u %u %u %u", (double)now / FS, n[0], n[1], n[2], n[3], n[4]);
                for (x = 0, y = 0; x < ECO_CELLS; x++) {
                    const cell_t *a = &eco.cell[x];
                    uint32_t j;
                    for (j = x + 1u; j < ECO_CELLS; j++) {
                        const cell_t *b = &eco.cell[j];
                        int32_t dx = (a->g.pan - b->g.pan) * ECO_FW / 64, rr = eco_radius(a) + eco_radius(b) - 2;
                        int32_t dy = (eco_y16(a) - eco_y16(b)) * ECO_FH / eco.span16;
                        y += a->id && b->id && dx * dx + dy * dy < rr * rr;
                    }
                }
                printf(" overlap %u", y);
                for (x = 0; x < ECO_COLS; x++) {
                    printf(" |");
                    for (y = 0; y < ECO_ROWS; y++)
                        printf(" %4d", eco.niche[x][y].E / 16);
                }
                printf("\n");
            }
        }
        for (i = 0; i < ECO_CELLS; i++)
            live += eco.cell[i].id != 0;
        live_max = live > live_max ? live : live_max;
    }
    fseek(f_wav, 0, SEEK_SET);
    wav_header(f_wav, (uint32_t)now);
    fclose(f_wav);
    fclose(f_ev);
    fclose(f_cell);
    {
        uint32_t i, live = 0;
        for (i = 0; i < ECO_CELLS; i++)
            live += eco.cell[i].id != 0;
        printf("%s, seed %u, %.1f s: seeds %u, sprouts %u, feeds %u, buds %u, drifts %u, takes %u, turns %u, eaten %u, "
               "notes %u, deaths %u hunger / %u age, cells at most %u, at the end %u, peak %d\nhash %08x\n", scen, seed,
               (double)now / FS, n_ev[EV_SEED], n_ev[EV_SPROUT], n_ev[EV_FEED], n_ev[EV_BUD], n_ev[EV_DRIFT],
               n_ev[EV_TAKE], n_ev[EV_TURN], n_ev[EV_EATEN], n_ev[EV_NOTE], n_ev[EV_HUNGER], n_ev[EV_AGE], live_max,
               live, peak, hash);
    }
    return downs && (!n_ev[EV_SEED] || !n_ev[EV_NOTE]) ? 1 : 0;
}
