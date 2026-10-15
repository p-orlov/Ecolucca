#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
# Host tests of the Felucca sources (no hardware). Run from the repo root after ./build.sh:
#   tests/run_tests.sh
# storage (A/B, torn writes), keys / buttons / encoders / LED scan (hal/fm1_input.h), the TRS MIDI parser, USB audio
# and the USB descriptor layouts, the M-UPGRADE entry and the update loader, the UI fonts and themes, the render loops'
# target cost (tests/target_budget.py; BUDGET_UPDATE=1 rewrites tests/target_budget.txt), the installer CLI, and the
# browser emulator's build (when emcc is there). SANITIZE=0 skips the ASan / UBSan runs.
set -e
export AC79_SDK="${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}"
cd "$(dirname "$0")/.."
OUT=build/host
mkdir -p "$OUT"
CC="${CC:-cc} -O1 -Wall -Wno-unused-function"
fail=0
run() { echo "== $1"; shift; "$@" || fail=1; }

$CC -o "$OUT/storage_test" tests/storage_test.c
run "flash storage (A/B, torn writes)" "$OUT/storage_test"

$CC -o "$OUT/input_test" tests/input_test.c
run "keys and buttons: fast press, long release, bouncy contacts (one note each), glitches, encoders" "$OUT/input_test"

$CC -o "$OUT/midi_uart_test" tests/midi_uart_test.c
run "TRS MIDI parser" "$OUT/midi_uart_test"

HALF=$(sed -n 's/^#define HALF_FRAMES \([0-9]*\).*/\1/p' firmware/src/core.h)
$CC -O2 -DT_CDC=1 -DHALF_FRAMES=$HALF -o "$OUT/uac_test" tests/uac_test.c -lm
run "USB audio input: descriptors (with CDC), ring and packets, 44.1 -> 48 kHz resampler, 48 kHz stream" "$OUT/uac_test"
$CC -O2 -DT_CDC=0 -DHALF_FRAMES=$HALF -o "$OUT/uac_test_nocdc" tests/uac_test.c -lm
run "USB audio input: descriptors (without CDC), ring and packets, resampler, 48 kHz stream" "$OUT/uac_test_nocdc"

# USB descriptor layouts (#67): CDC UAC LAYOUT CDC-presented 48K
for v in 1.1.0.1.1 1.1.1.1.1 1.1.2.1.1 1.1.3.1.1 1.1.0.0.1 1.1.2.0.1 0.1.0.1.1 1.1.0.1.0 1.1.0.0.0 1.1.2.1.0 0.1.0.1.0 \
         1.0.0.1.1 1.0.1.1.1 1.0.0.0.1 0.0.0.1.1; do
    IFS=. read -r t_cdc t_uac t_lay t_on t_48 <<EOF
$v
EOF
    $CC -DT_CDC="$t_cdc" -DT_UAC="$t_uac" -DT_LAYOUT="$t_lay" -DT_ON="$t_on" -DT_48K="$t_48" -o "$OUT/usb_desc_test" \
        tests/usb_desc_test.c
    run "USB descriptors: CDC $t_cdc (presented $t_on), UAC $t_uac (48 kHz $t_48), layout $t_lay" "$OUT/usb_desc_test"
done
# FELUCCA_UAC_AS_FIRST=0 (#67): the AC collection MIDI first (the kernel AppleUSBAudio of macOS <= 15 stops before
# the audio streaming interface there)
for v in 1.1.0.1.1 1.1.0.0.1 1.1.0.1.0 1.1.0.0.0 0.1.0.1.1; do
    IFS=. read -r t_cdc t_uac t_lay t_on t_48 <<EOF
$v
EOF
    $CC -DT_CDC="$t_cdc" -DT_UAC="$t_uac" -DT_LAYOUT="$t_lay" -DT_ON="$t_on" -DT_48K="$t_48" -DT_ASF=0 \
        -o "$OUT/usb_desc_test" tests/usb_desc_test.c
    run "USB descriptors (AC collection MIDI first): CDC $t_cdc (presented $t_on), UAC $t_uac (48 kHz $t_48)" \
        "$OUT/usb_desc_test"
done

[ -f build/felucca.fwsc ] || { echo "run ./build.sh first"; exit 1; }

$CC -DOWN_PKG=1 -o "$OUT/ota_test" tests/ota_test.c
run "M-UPGRADE entry (own loader)" "$OUT/ota_test" build/felucca.fwsc

head -c 200000 build/felucca.bin > "$OUT/old_app.bin"
python3 tools/fm1pkg_make.py "$OUT/old_app.bin" build/loader/ota.bin "$OUT/old.fwsc" >/dev/null
$CC -o "$OUT/ldr_test" tests/ldr_test.c
run "update loader: other app -> this build" "$OUT/ldr_test" "$OUT/old.fwsc" build/felucca.fwsc

if [ -f build/gen/felucca_tables.h ]; then
    $CC -O1 -w -Ibuild/gen -Ifirmware/src -o "$OUT/theme_test" tests/theme_test.c -lm
    run "themes: contrast, text blending and font metrics" "$OUT/theme_test"
    $CC -O1 -w -Ibuild/gen -Ifirmware/src -o "$OUT/text_ref_test" tests/text_ref_test.c -lm
    run "text: pens, kerning and pixels equal the reference renderer" "$OUT/text_ref_test"
    if python3 -c "import PIL" 2>/dev/null; then
        run "text spacing: glyph gaps against the font's own (16x supersampled), S M <= 0.25 px, L <= 0.5 px" \
            python3 tests/text_spacing_test.py build/gen/ui_fonts.h
    fi
    $CC -O2 -w -Ibuild/gen -Ifirmware/src -Ifirmware/hal -o "$OUT/eco_bench" tests/eco_bench.c
    run "ecosystem bench: seeds, notes (tests/eco/basic.txt -> build/eco/basic)" \
        "$OUT/eco_bench" tests/eco/basic.txt --seed 7 --out build/eco/basic
    eco_buds() { "$OUT/eco_bench" tests/eco/bud.txt --seed 7 --out build/eco/bud | grep -q 'buds [1-9]'; }
    run "ecosystem bench: a fed cell buds (tests/eco/bud.txt -> build/eco/bud)" eco_buds
    eco_drifts() { "$OUT/eco_bench" tests/eco/drift.txt --seed 7 --out build/eco/drift | grep -q 'drifts [1-9].*takes [1-9]'; }
    run "ecosystem bench: hungry cells drift to food, eaters take traits from remains (tests/eco/drift.txt)" eco_drifts
    run "ecosystem bench: DRONE mode plays (tests/eco/drift.txt -> build/eco/drone)" \
        "$OUT/eco_bench" tests/eco/drift.txt --seed 7 --out build/eco/drone --set MODE=1
    eco_preds() { "$OUT/eco_bench" tests/eco/rain.txt --seed 7 --secs 300 --out build/eco/pred --set FOOD=100 --set PRED=100 | grep -q 'turns [1-9].*eaten [1-9]'; }
    run "ecosystem bench: hungry big cells turn predators and eat (tests/eco/rain.txt, FOOD 100, PRED 100, 300 s)" eco_preds
    eco_rain() { "$OUT/eco_bench" tests/eco/rain.txt --seed 7 --secs 300 --out build/eco/rain --set FOOD=100 | grep -q 'sprouts [1-9].*at the end [1-9]'; }
    run "ecosystem bench: growing food keeps a population, spores sprout (tests/eco/rain.txt, FOOD 100, 300 s)" eco_rain
    eco_same() {
        a=$("$OUT/eco_bench" tests/eco/basic.txt --seed 7 --out build/eco/same_a | grep '^hash')
        b=$("$OUT/eco_bench" tests/eco/basic.txt --seed 7 --out build/eco/same_b | grep '^hash')
        [ -n "$a" ] && [ "$a" = "$b" ]
    }
    run "ecosystem: the same scenario and seed, the same sound and logs" eco_same
else
    echo "== skip the UI font tests (run ./build.sh once)"
fi

# ASan + UBSan: the update paths
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize=signed-integer-overflow,shift,bounds,object-size,pointer-overflow"
SAN="$SAN -fno-sanitize-recover=undefined -w"
san_ok() {
    printf 'int main(void){return 0;}\n' > "$OUT/san_probe.c" &&
        ${CC%% *} $SAN -o "$OUT/san_probe" "$OUT/san_probe.c" 2>/dev/null && "$OUT/san_probe"
}
if [ "${SANITIZE:-1}" = 0 ]; then
    echo "== skip the sanitizer runs (SANITIZE=0)"
elif ! san_ok; then
    echo "== skip the sanitizer runs (the compiler has no ASan / UBSan)"
else
    mkdir -p "$OUT/asan"
    A="$OUT/asan"
    SCC="${CC%% *} $SAN -Ibuild/gen -Ifirmware/src"
    export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
    $SCC -o "$A/ldr_test" tests/ldr_test.c
    run "ASan/UBSan: update loader (other app -> this build)" "$A/ldr_test" "$OUT/old.fwsc" build/felucca.fwsc
    $SCC -DOWN_PKG=1 -o "$A/ota_test" tests/ota_test.c
    run "ASan/UBSan: M-UPGRADE entry (own loader)" "$A/ota_test" build/felucca.fwsc
fi

run "target cost of the render loops (pi32v2 disassembly)" python3 tests/target_budget.py \
    build/felucca.dis tests/target_budget.txt

run "installer CLI (fm1_install.py) against a simulated FM-1" python3 tests/install_test.py

if ! command -v emcc >/dev/null 2>&1; then
    echo "== skip the browser emulator (no emcc: Emscripten builds web/emu)"
elif [ ! -f build/gen/felucca_tables.h ]; then
    echo "== skip the browser emulator (run ./build.sh once)"
else
    run "browser emulator: the firmware to WebAssembly (web/emu/build.sh -> build/emu)" sh web/emu/build.sh
fi

[ $fail -eq 0 ] && echo "ALL HOST TESTS PASSED" || { echo "HOST TESTS FAILED"; exit 1; }
