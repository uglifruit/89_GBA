#!/usr/bin/env bash
# build.sh — build the GBA multiboot payload and emit gba_payload.h for the RP2040 firmware.
#
# Produces payload.mb (a multiboot ROM image: header + code, linked for EWRAM @ 0x02000000,
# padded to a 16-byte multiple) and converts it to ../gba_payload.h as a C byte array.
#
# Toolchain resolution (no devkitPro install required on this machine):
#   compiler : $DEVKITARM, else arm-none-eabi-gcc on PATH, else the Pico SDK's copy at
#              ~/.pico-sdk/toolchain/*/bin (the GBA is ARM7TDMI/armv4t — GCC targets it fine).
#   logo/csum: devkitPro `gbafix` if found, else the bundled dependency-free ./gbafix.py.
#              (The GBA BIOS validates the Nintendo logo + header complement, so this is
#               mandatory for real hardware.)
set -euo pipefail
cd "$(dirname "$0")"

# --- pick a compiler prefix ---
if [[ -n "${DEVKITARM:-}" && -x "${DEVKITARM}/bin/arm-none-eabi-gcc" ]]; then
    PREFIX="${DEVKITARM}/bin/arm-none-eabi-"
elif command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    PREFIX="$(command -v arm-none-eabi-gcc)"; PREFIX="${PREFIX%arm-none-eabi-gcc}arm-none-eabi-"
else
    # Fall back to the Pico SDK's bundled toolchain.
    TC="$(ls -d "$HOME"/.pico-sdk/toolchain/*/bin 2>/dev/null | head -1 || true)"
    if [[ -n "$TC" && -x "$TC/arm-none-eabi-gcc.exe" ]]; then
        PREFIX="$TC/arm-none-eabi-"
    else
        echo "error: no arm-none-eabi toolchain found (devkitARM, PATH, or Pico SDK)." >&2
        exit 1
    fi
fi

# handle .exe suffix on Windows toolchains
gcc_bin="${PREFIX}gcc"; [[ -x "$gcc_bin" ]] || gcc_bin="${PREFIX}gcc.exe"
oc_bin="${PREFIX}objcopy"; [[ -x "$oc_bin" ]] || oc_bin="${PREFIX}objcopy.exe"

# --- pick a gbafix ---
if command -v gbafix >/dev/null 2>&1; then
    GBAFIX=(gbafix)
elif [[ -n "${DEVKITPRO:-}" && -x "${DEVKITPRO}/tools/bin/gbafix" ]]; then
    GBAFIX=("${DEVKITPRO}/tools/bin/gbafix")
else
    GBAFIX=(python ./gbafix.py)   # bundled fallback
fi

ARCH="-mcpu=arm7tdmi -mthumb-interwork -marm"
echo "compiler: $gcc_bin"
"$gcc_bin" --version | head -1
echo "gbafix:   ${GBAFIX[*]}"

# --- compile + link ---
rm -rf build && mkdir build
"$gcc_bin" $ARCH -x assembler-with-cpp -c crt0.s -o build/crt0.o
"$gcc_bin" $ARCH -O2 -ffreestanding -fno-builtin -c main.c -o build/main.o
"$gcc_bin" $ARCH -nostdlib -T multiboot.ld build/crt0.o build/main.o -o build/payload.elf

# --- raw binary ---
"$oc_bin" -O binary build/payload.elf build/payload.mb

# --- insert Nintendo logo + fix header checksum ---
"${GBAFIX[@]}" build/payload.mb

# --- emit the C header (also pads to a 16-byte multiple) ---
python ./bin2h.py build/payload.mb ../gba_payload.h gba_payload
echo "wrote ../gba_payload.h"
