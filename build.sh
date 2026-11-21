#!/usr/bin/env bash
# Cross-compile Loupe.exe from Linux/macOS with Zig (pip install ziglang, or a zig binary on PATH).
set -euo pipefail
cd "$(dirname "$0")"
./sync-version.sh
if [[ -z "${ZIG:-}" ]]; then
  if command -v zig >/dev/null 2>&1; then ZIG=zig; else ZIG="python3 -m ziglang"; fi
fi
TARGET=x86_64-windows-gnu
OBJ=build/webp
mkdir -p "$OBJ" dist

# libwebp (decoder + demux), compiled once and cached
pids=()
for c in third_party/libwebp/src/dec/*.c third_party/libwebp/src/demux/*.c third_party/libwebp/src/utils/*.c third_party/libwebp/src/dsp/*.c; do
  o="$OBJ/$(echo "${c#third_party/libwebp/src/}" | tr / _).o"
  if [[ ! "$o" -nt "$c" ]]; then
    $ZIG cc -target $TARGET -O2 -DNDEBUG -Ithird_party/libwebp -ffunction-sections -fdata-sections -c "$c" -o "$o" &
    pids+=($!)
  fi
done
for p in "${pids[@]:-}"; do [[ -n "$p" ]] && wait "$p"; done

$ZIG c++ -target $TARGET -std=c++17 -O2 -s -municode \
  -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN \
  -Ithird_party/libwebp -ffunction-sections -fdata-sections \
  -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers \
  ${EXTRA_FLAGS:-} src/*.cpp res/loupe.rc "$OBJ"/*.o -o "${OUT:-dist/Loupe.exe}" \
  -Wl,--subsystem,windows -Wl,--gc-sections \
  -ld2d1 -ldwrite -ld3d11 -ldxgi -lole32 -loleaut32 -luuid -ldwmapi -lshcore -lshell32 -luser32 -lgdi32 -ladvapi32 \
  -lshlwapi -lwindowscodecs -lcomctl32 -luxtheme -lwinhttp -lbcrypt -lversion -lcomdlg32 -lpropsys
rm -f dist/Loupe.pdb
ls -la "${OUT:-dist/Loupe.exe}"
