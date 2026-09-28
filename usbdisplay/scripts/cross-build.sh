#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Builds the Windows driver from Linux.
#
# This works, and it is worth saying why, because the received wisdom is
# that it cannot be done. A user-mode driver of this kind is an ordinary
# Windows library. It links against no driver runtime at all: both the
# framework and the display extension are bound at load time through
# function tables, so the only things needed from the driver kit are two
# small static stubs and a handful of headers.
#
# Everything else is the ordinary Microsoft toolchain, which comes from two
# places:
#
#   xwin    the compiler's own runtime and the Windows SDK, downloaded from
#           Microsoft's installer feed and laid out for a case sensitive
#           filesystem. That last part matters more than it sounds: Windows
#           headers include each other with inconsistent capitalisation, so
#           without the symlinks it adds, the first include fails.
#
#   NuGet   the driver kit, which is not part of that feed but is published
#           as an ordinary archive.
#
# The shader is not compiled here. Its compiler runs only on Windows and
# Direct3D 11 accepts no other form of it, so the compiled bytecode is
# committed to the tree instead; see src/render/generated/convert_cs.h.
#
# What this does not do is sign anything or produce an installable catalog.
# Those need Windows tools with no equivalent here, and the install script
# does both on the machine where the driver is actually used.
#
#   ./cross-build.sh            build
#   ./cross-build.sh --clean    discard the downloaded toolchain first

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
WORK="$ROOT/build/cross"
XWIN="$WORK/toolchain"
WDK="$WORK/wdk"
OUT="$ROOT/build/driver/x64/Release"

# The kit package carries several versions of each framework, so the ones
# this driver targets are named rather than guessed at: picking the wrong
# one builds against a different framework and fails in ways that look
# nothing like a version mismatch.
KIT_VERSION="${KIT_VERSION:-10.0.26100.6584}"
XWIN_VERSION="${XWIN_VERSION:-0.10.0}"
WDF_VERSION="2.33"
IDDCX_VERSION="1.2"

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[ "${1:-}" = "--clean" ] && rm -rf "$WORK"

# Finding a clang new enough for the Microsoft standard library.
#
# That library refuses to compile with a compiler it does not recognise, and
# the refusal is a static assertion naming the version it wants rather than
# anything subtle. The version required moves forward with the toolchain, so
# this asks each candidate what it is rather than assuming: distributions
# commonly ship several side by side, with the newest under a suffixed name
# and something older as the default.
#
# clang-cl is the same binary as clang under another name, and packages do
# not always install the extra name, so clang is asked for its
# Microsoft-compatible mode directly when needed.
MINIMUM_CLANG="${MINIMUM_CLANG:-19}"

clang_major() {
  "$1" --version 2>/dev/null | sed -n 's/.*version \([0-9][0-9]*\).*/\1/p' | head -1
}

CLANG=""
BEST_VERSION=0
for candidate in clang-22 clang-21 clang-20 clang-19 clang; do
  command -v "$candidate" >/dev/null 2>&1 || continue
  version="$(clang_major "$candidate")"
  [ -n "$version" ] || continue
  if [ "$version" -ge "$MINIMUM_CLANG" ] && [ "$version" -gt "$BEST_VERSION" ]; then
    CLANG="$candidate"
    BEST_VERSION="$version"
  fi
done

if [ -z "$CLANG" ]; then
  installed=""
  for candidate in clang clang-16 clang-17 clang-18 clang-19 clang-20; do
    if command -v "$candidate" >/dev/null 2>&1; then
      installed="$installed $candidate ($(clang_major "$candidate"))"
    fi
  done
  die "no clang $MINIMUM_CLANG or newer was found.
The Microsoft standard library refuses anything older, by static assertion.
Found:${installed:- nothing}
On Debian or Ubuntu, the LLVM project publishes current packages:
  wget -qO- https://apt.llvm.org/llvm.sh | sudo bash -s -- $MINIMUM_CLANG"
fi

# The matching linker, preferring the one from the same release.
LLD=""
for candidate in "lld-link-$BEST_VERSION" "lld-$BEST_VERSION" lld-link ld.lld lld; do
  command -v "$candidate" >/dev/null 2>&1 || continue
  LLD="$candidate"
  break
done
[ -n "$LLD" ] || die "the LLVM linker is not installed. On Debian or Ubuntu: apt install lld-$BEST_VERSION"

if [ "${LLD#lld-link}" != "$LLD" ]; then
  LLD_LINK=("$LLD")
else
  LLD_LINK=("$LLD" -flavor link)
fi
CLANG_CL=("$CLANG" --driver-mode=cl)

for tool in curl tar unzip; do
  command -v "$tool" >/dev/null 2>&1 || die "$tool is not installed"
done

mkdir -p "$WORK"

# ------------------------------------------------------- compiler and SDK

if [ ! -f "$XWIN/crt/include/excpt.h" ]; then
  log "Fetching the Microsoft toolchain"

  XWIN_BIN="$(command -v xwin 2>/dev/null || true)"
  if [ -z "$XWIN_BIN" ]; then
    XWIN_BIN="$WORK/bin/xwin"
    if [ ! -x "$XWIN_BIN" ]; then
      mkdir -p "$WORK/bin"
      name="xwin-$XWIN_VERSION-x86_64-unknown-linux-musl"
      curl --fail --location --silent --show-error \
        "https://github.com/Jake-Shadle/xwin/releases/download/$XWIN_VERSION/$name.tar.gz" \
        | tar -xz -C "$WORK/bin" --strip-components=1 "$name/xwin"
      chmod +x "$XWIN_BIN"
    fi
  fi

  # Only the 64 bit desktop pieces, a fraction of the whole and all this
  # driver targets.
  "$XWIN_BIN" --accept-license --arch x86_64 --variant desktop \
    --cache-dir "$WORK/xwin-cache" splat --output "$XWIN"
  rm -rf "$WORK/xwin-cache"
fi

[ -f "$XWIN/crt/include/excpt.h" ] || die "the compiler runtime headers are missing from $XWIN"
[ -d "$XWIN/sdk/include/um" ]      || die "the SDK headers are missing from $XWIN"

# --------------------------------------------------------- the driver kit

if [ ! -d "$WDK" ]; then
  log "Fetching the driver kit"
  mkdir -p "$WDK"
  name="microsoft.windows.wdk.x64"
  curl --fail --location --silent --show-error \
    "https://api.nuget.org/v3-flatcontainer/$name/$KIT_VERSION/$name.$KIT_VERSION.nupkg" \
    -o "$WORK/wdk.zip"
  # Only the parts this driver needs: a few megabytes of a hundred megabyte
  # archive.
  ( cd "$WDK" && unzip -q -o "$WORK/wdk.zip" \
      "c/Include/*/um/iddcx/$IDDCX_VERSION/*" \
      "c/Include/wdf/umdf/$WDF_VERSION/*" \
      "c/Lib/*/um/x64/iddcx/$IDDCX_VERSION/*" \
      "c/Lib/wdf/umdf/x64/$WDF_VERSION/*" )
  rm -f "$WORK/wdk.zip"
fi

# Two things have to be repaired in the kit before a compiler that is not
# Microsoft's can read it. Both are invisible on Windows and both stop the
# build dead anywhere else.
if [ ! -f "$WDK/.prepared" ]; then
  log "Preparing the driver kit"

  # One: nothing here agrees on capitalisation. The kit's headers ask each
  # other for names that differ from the files on disk, they ask the SDK
  # for names that differ too, and this project's own sources use a third
  # spelling again. On Windows none of that matters; on a case sensitive
  # filesystem every one of them is a missing file.
  #
  # Rather than chase who asks for what, every header is given a lowercase
  # alias, so any request that differs only in case resolves. That covers
  # the callers that cannot be seen from here, which is what the previous
  # attempt got wrong: it read the includes in the kit's own headers and so
  # never learned that this project asks for iddcx.h while the file is
  # called IddCx.h.
  find "$WDK" -type f -name '*.h' -print0 |
  while IFS= read -r -d '' header; do
    directory="$(dirname "$header")"
    name="$(basename "$header")"
    lower="$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]')"
    [ "$name" = "$lower" ] && continue
    [ -e "$directory/$lower" ] || ln -s "$name" "$directory/$lower"
  done

  # And the reverse, for names the kit asks of the SDK: there the file is
  # lowercase and the request is not. The SDK is not ours to reorganise, so
  # only the specific spellings asked for are added.
  find "$WDK" -type f -name '*.h' -print0 |
  while IFS= read -r -d '' header; do
    sed -n 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*["<]\([A-Za-z0-9_.]*\.h\)[">].*/\1/p' \
      "$header" |
    while IFS= read -r wanted; do
      lower="$(printf '%s' "$wanted" | tr '[:upper:]' '[:lower:]')"
      [ "$wanted" = "$lower" ] && continue
      for directory in "$(dirname "$header")" \
                       "$XWIN/sdk/include/um" \
                       "$XWIN/sdk/include/shared" \
                       "$XWIN/sdk/include/ucrt"; do
        [ -e "$directory/$wanted" ] && break
        if [ -f "$directory/$lower" ]; then
          ln -s "$lower" "$directory/$wanted" 2>/dev/null || true
          break
        fi
      done
    done
  done

  # Two: an enumeration is declared ahead of its definition with an
  # underlying type, and defined without one. The Microsoft compiler
  # accepts the mismatch; clang rejects it outright, and there is no flag
  # for it because the check is not a diagnostic that can be switched off.
  #
  # Removing the type from the declaration makes the two agree. It changes
  # nothing about the resulting code: an enumeration of those values has
  # that underlying type anyway. There is exactly one of these in the
  # framework headers, and the pattern is precise enough that it cannot
  # match anything else.
  find "$WDK" -type f -name '*.h' -print0 |
  xargs -0 sed -i 's/^\([[:space:]]*enum[[:space:]][[:space:]]*_[A-Za-z0-9_]*\)[[:space:]]*:[[:space:]]*int[[:space:]]*;/\1;/'

  touch "$WDK/.prepared"
fi

IDDCX_INC="$(find "$WDK" -type d -path "*um/iddcx/$IDDCX_VERSION" -print -quit)"
WDF_INC="$(find "$WDK" -type d -path "*wdf/umdf/$WDF_VERSION" -print -quit)"
IDDCX_STUB="$(find "$WDK" -type f -ipath "*iddcx/$IDDCX_VERSION/iddcxstub.lib" -print -quit)"
WDF_STUB="$(find "$WDK" -type f -ipath "*umdf/x64/$WDF_VERSION/WdfDriverStubUm.lib" -print -quit)"

[ -n "$IDDCX_INC" ]  || die "the display extension headers, version $IDDCX_VERSION, were not in the kit"
[ -n "$WDF_INC" ]    || die "the framework headers, version $WDF_VERSION, were not in the kit"
[ -n "$IDDCX_STUB" ] || die "iddcxstub.lib, version $IDDCX_VERSION, was not in the kit"
[ -n "$WDF_STUB" ]   || die "WdfDriverStubUm.lib, version $WDF_VERSION, was not in the kit"

echo "  compiler:  $CLANG (version $BEST_VERSION)"
echo "  linker:    ${LLD_LINK[*]}"
echo "  toolchain: $XWIN"
echo "  kit:       $IDDCX_INC"

# ------------------------------------------------------------------ build

log "Compiling"
mkdir -p "$WORK/obj" "$OUT"

SOURCES=(
  src/core/proto.cpp
  src/core/usb.cpp
  src/core/open_device.cpp
  src/core/macrosilicon.cpp
  src/render/rect.cpp
  src/render/damage.cpp
  src/render/convert.cpp
  src/render/converter.cpp
  src/driver/log.cpp
  src/driver/settings.cpp
  src/driver/sender.cpp
  src/driver/pipeline.cpp
  src/driver/device.cpp
  src/driver/driver.cpp
)

# -imsvc rather than -I, so these count as system headers and their warnings
# stay out of the way. The Microsoft headers are not clean at the level this
# project holds its own code to.
INCLUDES=(
  -imsvc "$XWIN/crt/include"
  -imsvc "$XWIN/sdk/include/ucrt"
  -imsvc "$XWIN/sdk/include/um"
  -imsvc "$XWIN/sdk/include/shared"
  -imsvc "$XWIN/sdk/include/winrt"
  -imsvc "$IDDCX_INC"
  -imsvc "$WDF_INC"
  -I "$ROOT/src/render/generated"
)

# IDDCX_VERSION_* are not derived from the include path and have to be
# stated. UMDF_USING_NTSTATUS keeps status codes consistent between the
# framework headers and the ordinary Windows ones.
DEFINES=(
  -DUNICODE -D_UNICODE
  -D_WIN32_WINNT=0x0A00
  -DIDDCX_VERSION_MAJOR=1 -DIDDCX_VERSION_MINOR=2
  -DUMDF_VERSION_MAJOR=2 -DUMDF_VERSION_MINOR=33
  -DUMDF_USING_NTSTATUS
)

FLAGS=(
  --target=x86_64-pc-windows-msvc
  /std:c++17 /EHsc /O2 /MT /W3 /GS-
  -Wno-unused-command-line-argument
  -Wno-microsoft-enum-value
  -Wno-ignored-attributes
  -Wno-nonportable-include-path
  # The framework hands a driver a handle it may never need to use again;
  # keeping it is deliberate rather than an oversight.
  -Wno-unused-private-field
)

for source in "${SOURCES[@]}"; do
  obj="$WORK/obj/$(basename "${source%.cpp}").obj"
  "${CLANG_CL[@]}" "${FLAGS[@]}" "${DEFINES[@]}" "${INCLUDES[@]}" \
    /c "$ROOT/$source" /Fo"$obj"
done

# ntdll is in the list because the framework stub calls a kernel debug
# print that lives there and nowhere else. Leaving it out fails at the very
# last step with one unresolved symbol.
log "Linking"
"${LLD_LINK[@]}" \
  /DLL /NOLOGO /MACHINE:X64 \
  /OUT:"$OUT/usbdisplaydd.dll" \
  /LIBPATH:"$XWIN/crt/lib/x86_64" \
  /LIBPATH:"$XWIN/sdk/lib/um/x86_64" \
  /LIBPATH:"$XWIN/sdk/lib/ucrt/x86_64" \
  "$WORK"/obj/*.obj \
  "$IDDCX_STUB" "$WDF_STUB" \
  d3d11.lib dxgi.lib setupapi.lib hid.lib winusb.lib \
  advapi32.lib ole32.lib user32.lib gdi32.lib kernel32.lib ntdll.lib

# The version in the INF must move for Windows to replace an installed
# copy, so it is stamped the way the Windows build does.
sed "s|^DriverVer *=.*|DriverVer = $(date +%m/%d/%Y),$(date +%H.%M.%S).0|" \
  "$ROOT/inf/usbdisplaydd.inf" > "$OUT/usbdisplaydd.inf"

log "Built $OUT/usbdisplaydd.dll"
ls -la "$OUT/usbdisplaydd.dll"
