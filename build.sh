#!/usr/bin/env bash
# Builds the Geronimo Mod Loader on Linux, cross-compiled with clang-cl, an LLVM linker and llvm-rc
# against the MSVC CRT and Windows SDK unpacked by xwin. Same layout as build.bat:
#
#   build/game/                      mirrors <game>/Geronimo/Binaries/Win64/
#     version.dll                    doorstop (src/doorstop)
#     doorstop_config.ini
#     GML/core/GML.dll               the loader (src)
#     GML/plugins/<Name>/<Name>.dll  from plugins/<Name>/ if you keep your own plugins here
#     GML/patchers/<Name>.dll        from patchers/<Name>/ likewise
#   build/gmlcheck.exe               offline checker (tools), run it with wine
#   build/examples/<Name>/<Name>.dll example plugins (examples) - never installed
#   build/tests/<Name>/<Name>.dll    test plugins/patchers (tests) - never installed
#
# Usage:  ./build.sh            build
#         ./build.sh install    build and copy into the game (game must be closed)
#
# One-time setup (Arch):  pacman -S clang llvm
#                         cargo install xwin --locked
#                         xwin --accept-license splat --output ~/.xwin
# The linker is lld-link, lld or ld.lld (pacman -S lld) if installed, else the rust-lld that
# ships with every rustup toolchain.
# XWIN points elsewhere if the splat isn't in ~/.xwin. GML_GAME_DIR is your
# .../Geronimo/Binaries/Win64 if the game should not be looked up in the Steam libraries.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OUT=$ROOT/build
G=$OUT/game
XWIN=${XWIN:-$HOME/.xwin}
APPID=2019620

die() {
  echo "$*" >&2
  exit 1
}

case ${1:-} in
  "" | install) ;;
  *) die "usage: ./build.sh [install]" ;;
esac

find_linker() {
  local candidate
  for candidate in lld-link lld ld.lld rust-lld; do
    if command -v "$candidate" >/dev/null; then
      command -v "$candidate"
      return
    fi
  done
  if command -v rustc >/dev/null; then
    for candidate in "$(rustc --print sysroot)"/lib/rustlib/*/bin/rust-lld; do
      if [[ -x $candidate ]]; then
        echo "$candidate"
        return
      fi
    done
  fi
  die "no LLVM linker found: pacman -S lld, or install a Rust toolchain with rustup (it ships rust-lld)"
}

for tool in clang-cl llvm-rc; do
  command -v "$tool" >/dev/null || die "$tool not found: pacman -S clang llvm"
done
[[ -d $XWIN/crt/lib/x86_64 && -d $XWIN/sdk/lib/um/x86_64 ]] ||
  die "no MSVC CRT / Windows SDK in $XWIN: xwin --accept-license splat --output $XWIN"

LINKER=$(find_linker)
mkdir -p "$OUT/toolchain"
ln -sfn "$LINKER" "$OUT/toolchain/lld-link"
PATH=$OUT/toolchain:$PATH

SYSINCLUDE=("$XWIN/crt/include" "$XWIN/sdk/include/ucrt" "$XWIN/sdk/include/um" "$XWIN/sdk/include/shared")
CFLAGS=(--target=x86_64-pc-windows-msvc -fuse-ld=lld /nologo /std:c++20 /O2 /MT /EHsc /W3 /Zi
  /DUNICODE /D_UNICODE "/I$ROOT/include")
for d in "${SYSINCLUDE[@]}"; do CFLAGS+=(/imsvc "$d"); done
LFLAGS=(/DEBUG /INCREMENTAL:NO /ignore:4099 "/libpath:$XWIN/crt/lib/x86_64" "/libpath:$XWIN/sdk/lib/um/x86_64"
  "/libpath:$XWIN/sdk/lib/ucrt/x86_64")
RCFLAGS=(/nologo)
for d in "${SYSINCLUDE[@]}"; do RCFLAGS+=(/I "$d"); done

# A plugin/patcher DLL from the .cpp files in folder $1, written to $2. If the folder has a
# resource script named after the DLL (<Name>.rc), it is compiled in: files it lists as RCDATA
# ship inside the DLL (GML_API::PluginResource). Paths in it are relative to the folder.
compile() {
  local dir=$1 dll=$2 name res=()
  name=$(basename "$dll" .dll)
  if [[ -f $dir/$name.rc ]]; then
    llvm-rc "${RCFLAGS[@]}" /I "$dir" /FO "$OUT/obj/$name.res" "$dir/$name.rc"
    res=("$OUT/obj/$name.res")
  fi
  clang-cl "${CFLAGS[@]}" /LD "/Fo$OUT/obj/" "$dir"/*.cpp "${res[@]}" "/Fe$dll" /link "${LFLAGS[@]}"
}

mirror() {
  rm -rf "$2"
  mkdir -p "$(dirname "$2")"
  cp -a "$1" "$2"
}
merge() {
  mkdir -p "$2"
  cp -a "$1/." "$2/"
}

game_dir() {
  if [[ -n ${GML_GAME_DIR:-} ]]; then
    echo "$GML_GAME_DIR"
    return
  fi
  local root lib acf installdir
  for root in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
    [[ -f $root/steamapps/libraryfolders.vdf ]] || continue
    while read -r lib; do
      acf=$lib/steamapps/appmanifest_$APPID.acf
      [[ -f $acf ]] || continue
      installdir=$(sed -n 's/^\s*"installdir"\s*"\(.*\)"/\1/p' "$acf")
      echo "$lib/steamapps/common/$installdir/Geronimo/Binaries/Win64"
      return
    done < <(sed -n 's/^\s*"path"\s*"\(.*\)"/\1/p' "$root/steamapps/libraryfolders.vdf")
  done
  die "Geronimo not found in any Steam library: set GML_GAME_DIR to its Geronimo/Binaries/Win64"
}

shopt -s nullglob
mkdir -p "$OUT/obj" "$G/GML/core" "$G/GML/plugins" "$G/GML/patchers"

echo "=== doorstop (version.dll)"
clang-cl "${CFLAGS[@]}" /LD "/Fo$OUT/obj/" "$ROOT"/src/doorstop/*.cpp "/Fe$G/version.dll" \
  /link "/DEF:$ROOT/src/doorstop/version.def" "${LFLAGS[@]}" "/PDB:$OUT/version.pdb"
cp "$ROOT/doorstop_config.ini" "$G/"

echo "=== core (GML/core/GML.dll)"
llvm-rc "${RCFLAGS[@]}" /I "$ROOT/src" /FO "$OUT/obj/GML.res" "$ROOT/src/GML.rc"
clang-cl "${CFLAGS[@]}" /LD "/Fo$OUT/obj/" "$ROOT"/src/*.cpp "$OUT/obj/GML.res" "/Fe$G/GML/core/GML.dll" \
  /link "${LFLAGS[@]}" /OPT:REF "/PDB:$G/GML/core/GML.pdb"

echo "=== gmlcheck.exe"
clang-cl "${CFLAGS[@]}" "/Fo$OUT/obj/" "$ROOT/tools/gmlcheck.cpp" "$ROOT/src/hook.cpp" "$ROOT/src/scan.cpp" \
  "/Fe$OUT/gmlcheck.exe" /link "${LFLAGS[@]}"

echo "=== GML Manager skipped: WPF on .NET Framework 4.8 builds on Windows only"

echo "=== plugins"
for m in "$ROOT"/plugins/*/; do
  m=${m%/}
  n=$(basename "$m")
  mkdir -p "$G/GML/plugins/$n"
  compile "$m" "$G/GML/plugins/$n/$n.dll"
  for sub in Paks Assets; do
    if [[ -d $m/$sub ]]; then mirror "$m/$sub" "$G/GML/plugins/$n/$sub"; fi
  done
done

echo "=== patchers"
for m in "$ROOT"/patchers/*/; do
  m=${m%/}
  compile "$m" "$G/GML/patchers/$(basename "$m").dll"
done

for k in examples tests; do
  echo "=== $k (built, never installed)"
  for m in "$ROOT/$k"/*/; do
    m=${m%/}
    n=$(basename "$m")
    mkdir -p "$OUT/$k/$n"
    compile "$m" "$OUT/$k/$n/$n.dll"
  done
done

find "$OUT" -type f \( -name '*.exp' -o -name '*.lib' -o -name '*.ilk' \) -delete

if [[ ${1:-} == install ]]; then
  GAME=$(game_dir)
  [[ -f $GAME/Geronimo-Win64-Shipping.exe ]] || die "$GAME doesn't hold Geronimo-Win64-Shipping.exe"
  if pgrep -f 'Geronimo-Win64-Shipping\.exe' >/dev/null; then die "Close Geronimo first."; fi
  echo "=== install to $GAME"
  cp "$G/version.dll" "$GAME/"
  if [[ ! -f $GAME/doorstop_config.ini ]]; then cp "$G/doorstop_config.ini" "$GAME/"; fi
  mirror "$G/GML/core" "$GAME/GML/core"
  merge "$G/GML/plugins" "$GAME/GML/plugins"
  for m in "$G"/GML/plugins/*/; do
    if [[ -d ${m}Paks ]]; then mirror "${m}Paks" "$GAME/GML/plugins/$(basename "$m")/Paks"; fi
  done
  merge "$G/GML/patchers" "$GAME/GML/patchers"
fi
echo "=== done"
