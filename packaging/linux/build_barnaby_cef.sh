#!/usr/bin/env bash
set -euo pipefail

jobs="${FLATPAK_BUILDER_N_JOBS:-}"
if [[ -z "${jobs}" ]]; then
  jobs="$(nproc)"
fi

splash_cflags=()
read -r -a splash_cflags <<<"$(pkg-config --cflags x11 cairo gdk-pixbuf-2.0)"
splash_libs=()
read -r -a splash_libs <<<"$(pkg-config --libs x11 cairo gdk-pixbuf-2.0)"

common_flags=(
  -std=c++20
  -O2
  -DNDEBUG
  -Wall
  -Wextra
  -pedantic
  -Wno-deprecated-declarations
  -Wno-missing-field-initializers
  -Wno-unused-parameter
  -DWRAPPING_CEF_SHARED
  -I.
  -Icef
  "${splash_cflags[@]}"
)

mkdir -p build/cef_wrapper

compile_one() {
  local src="$1"
  local obj="build/cef_wrapper/${src//\//_}"
  obj="${obj%.cc}.o"
  c++ "${common_flags[@]}" -c "$src" -o "$obj"
}

running=0
while IFS= read -r src; do
  compile_one "$src" &
  running=$((running + 1))
  if (( running >= jobs )); then
    wait -n
    running=$((running - 1))
  fi
done < <(find cef/libcef_dll -name '*.cc' | sort)
wait

c++ "${common_flags[@]}" -c src/linux/BarnabyCef.cc -o build/BarnabyCef.o

mapfile -t wrapper_objects < <(find build/cef_wrapper -name '*.o' | sort)
c++ -o build/Barnaby \
  build/BarnabyCef.o \
  "${wrapper_objects[@]}" \
  cef/Release/libcef.so \
  "${splash_libs[@]}" \
  -pthread \
  -Wl,-rpath,/app/lib/barnaby/cef
