#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Builds the Windows driver from Linux.
#
# This works, and the reason it works is worth stating, because the received
# wisdom is that it cannot be done. A user-mode driver of this kind is an
# ordinary Windows DLL. It links against no driver runtime at all: both the
# framework and the display extension are bound at load time through function
# tables, so the only things needed from the driver kit are two small static
# stubs and a handful of headers. Everything else is the ordinary Windows SDK.
#
# All of it is published on NuGet, which is a plain HTTPS file server, and a
# NuGet package is a zip file. So the whole toolchain is: download four
# archives, unpack them, and compile with clang.
#
#   Microsoft.Windows.WDK.x64          the two stubs and the driver headers
#   Microsoft.Windows.SDK.CPP          the shared Windows headers
#   Microsoft.Windows.SDK.CPP.x64      the 64 bit import libraries
#   Microsoft.Windows.SDK.BuildTools   the shader compiler, run under Wine
#
# What this does not do is sign anything or build an installable catalog.
# Those need Windows tools with no equivalent here, and the install script
# does both on the machine where the driver is actually used.
#
#   ./cross-build.sh            build
#   ./cross-build.sh --clean    start from nothing

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
WORK="$ROOT/build/cross"
SDK="$WORK/sdk"
OUT="$ROOT/build/driver/x64/Release"

# One version for everything. The kit and the SDK have to agree, and all of
# these are published together under the same number.
KIT_VERSION="${KIT_VERSION:-10.0.26100.6584}"
WDF_VERSION="2.33"
IDDCX_VERSION="1.2"

log() { printf '\n==> %s\n' "$*"; }

if [ "${1:-}" = "--clean" ]; then
  rm -rf "$WORK"
fi

for tool in clang-cl lld-link curl unzip; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "error: $tool is not installed." >&2
    echo "On Debian or Ubuntu: apt install clang lld curl unzip" >&2
    exit 1
  }
done

# ---------------------------------------------------------------- toolchain

fetch_package() {
  local name="$1" version="$2" dest="$3"
  if [ -d "$dest" ]; then
    return
  fi
  log "Fetching $name $version"
  local url="https://api.nuget.org/v3-flatcontainer/${name,,}/${version}/${name,,}.${version}.nupkg"
  mkdir -p "$dest"
  curl --fail --location --silent --show-error "$url" -o "$WORK/pkg.zip"
  unzip -q -o "$WORK/pkg.zip" -d "$dest"
  rm -f "$WORK/pkg.zip"
}

mkdir -p "$WORK"
fetch_package "Microsoft.Windows.SDK.CPP"        "$KIT_VERSION" "$SDK/sdk-headers"
fetch_package "Microsoft.Windows.SDK.CPP.x64"    "$KIT_VERSION" "$SDK/sdk-libs"
fetch_package "Microsoft.Windows.WDK.x64"        "$KIT_VERSION" "$SDK/wdk"
fetch_package "Microsoft.Windows.SDK.BuildTools" "$KIT_VERSION" "$SDK/buildtools"

# Each package nests its contents differently, so every root is located by
# finding a file known to be inside it rather than by assuming a shape. The
# header package puts things under a versioned directory; the library
# package does not.
SDK_INC="$(dirname "$(dirname "$(find "$SDK/sdk-headers" -type f -iname 'windows.h' -path '*/um/*' -print -quit)")")"
SDK_UM_LIB="$(dirname "$(find "$SDK/sdk-libs" -type f -iname 'kernel32.lib' -print -quit)")"
SDK_UCRT_LIB="$(dirname "$(find "$SDK/sdk-libs" -type f -iname 'libucrt.lib' -print -quit)")"

# The kit's own headers and the two stub libraries.
#
# Found by searching, because these packages have rearranged themselves
# between versions, but pinned to the versions this driver targets: the
# package carries nine of each, and taking whichever turns up first would
# silently build against the wrong framework.
WDK_IDDCX_INC="$(find "$SDK/wdk" -type d -path "*um/iddcx/$IDDCX_VERSION" -print -quit)"
WDK_WDF_INC="$(find "$SDK/wdk" -type d -path "*wdf/umdf/$WDF_VERSION" -print -quit)"
IDDCX_STUB="$(find "$SDK/wdk" -type f -ipath "*um/x64/iddcx/$IDDCX_VERSION/iddcxstub.lib" -print -quit)"
WDF_STUB="$(find "$SDK/wdk" -type f -ipath "*wdf/umdf/x64/$WDF_VERSION/WdfDriverStubUm.lib" -print -quit)"

fail_missing() {
  echo "error: $1 was not found in the driver kit package." >&2
  echo "Look under $SDK/wdk; the layout may have changed." >&2
  exit 1
}
[ -d "$SDK_INC/um" ]    || { echo "error: no SDK headers found under $SDK/sdk-headers" >&2; exit 1; }
[ -d "$SDK_UM_LIB" ]    || { echo "error: no SDK import libraries found under $SDK/sdk-libs" >&2; exit 1; }
[ -d "$SDK_UCRT_LIB" ]  || { echo "error: no C runtime libraries found under $SDK/sdk-libs" >&2; exit 1; }
[ -n "$WDK_IDDCX_INC" ] || fail_missing "the display extension headers, version $IDDCX_VERSION"
[ -n "$WDK_WDF_INC" ]   || fail_missing "the framework headers, version $WDF_VERSION"
[ -n "$IDDCX_STUB" ]    || fail_missing "iddcxstub.lib, version $IDDCX_VERSION"
[ -n "$WDF_STUB" ]      || fail_missing "WdfDriverStubUm.lib, version $WDF_VERSION"

echo "  SDK headers:               $SDK_INC"
echo "  SDK libraries:             $SDK_UM_LIB"
echo "  display extension headers: $WDK_IDDCX_INC"
echo "  framework headers:         $WDK_WDF_INC"
echo "  stubs:                     $(basename "$IDDCX_STUB"), $(basename "$WDF_STUB")"

# Windows headers include each other with inconsistent capitalisation, which
# only matters on a case sensitive filesystem. Symlinking every name to its
# lowercase form costs nothing and saves a long afternoon.
if [ ! -f "$WORK/.case-fixed" ]; then
  log "Making the headers case insensitive"
  find "$SDK_INC" "$SDK/wdk" -type f \
    \( -name '*.h' -o -name '*.H' \) -print0 2>/dev/null |
  while IFS= read -r -d '' f; do
    lower="$(dirname "$f")/$(basename "$f" | tr '[:upper:]' '[:lower:]')"
    [ "$f" = "$lower" ] || [ -e "$lower" ] || ln -s "$(basename "$f")" "$lower"
  done
  find "$SDK/sdk-libs" -type f -name '*.Lib' -print0 2>/dev/null |
  while IFS= read -r -d '' f; do
    lower="${f%.Lib}.lib"
    [ -e "$lower" ] || ln -s "$(basename "$f")" "$lower"
  done
  touch "$WORK/.case-fixed"
fi

# ------------------------------------------------------------------- shader

SHADER_OUT="$WORK/convert_cs.h"
if [ ! -f "$SHADER_OUT" ]; then
  FXC="$(find "$SDK/buildtools" -type f -name 'fxc.exe' -path '*x64*' -print -quit)"
  if [ -z "$FXC" ]; then
    echo "error: fxc.exe was not in the build tools package." >&2
    exit 1
  fi
  if ! command -v wine >/dev/null 2>&1; then
    echo "error: wine is needed to run the shader compiler." >&2
    echo "There is no native Linux compiler for this shader model: the" >&2
    echo "modern one emits a different bytecode that Direct3D 11 will not" >&2
    echo "accept. On Debian or Ubuntu: apt install wine64" >&2
    exit 1
  fi
  log "Compiling the conversion shader"
  WINEDEBUG=-all wine "$FXC" /nologo /T cs_5_0 /E main \
    /Vn kConvertComputeShader /Fh "$SHADER_OUT" \
    "$ROOT/src/render/convert_cs.hlsl"
fi

# -------------------------------------------------------------------- build

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

INCLUDES=(
  -imsvc "$SDK_INC/um"
  -imsvc "$SDK_INC/shared"
  -imsvc "$SDK_INC/ucrt"
  -imsvc "$SDK_INC/winrt"
  -imsvc "$SDK_INC/cppwinrt"
  -imsvc "$WDK_IDDCX_INC"
  -imsvc "$WDK_WDF_INC"
  -I "$WORK"
)


# IDDCX_VERSION_* are not derived from the include path and have to be told.
# UMDF_USING_NTSTATUS keeps the status codes consistent between the framework
# headers and the ordinary Windows ones.
DEFINES=(
  -DUNICODE -D_UNICODE
  -DIDDCX_VERSION_MAJOR=1 -DIDDCX_VERSION_MINOR=2
  -DUMDF_VERSION_MAJOR=2 -DUMDF_VERSION_MINOR=33
  -DUMDF_USING_NTSTATUS
  -D_WIN32_WINNT=0x0A00
)

# The driver kit headers are not clean at high warning levels and never have
# been, so warnings are not errors here. The tool and the tests are built
# with warnings as errors and cover the same code.
FLAGS=(
  --target=x86_64-pc-windows-msvc
  /std:c++17 /EHsc /O2 /MT /W3 /GS-
  /clang:-fno-strict-aliasing
)

for source in "${SOURCES[@]}"; do
  obj="$WORK/obj/$(basename "${source%.cpp}").obj"
  clang-cl "${FLAGS[@]}" "${DEFINES[@]}" "${INCLUDES[@]}" \
    /c "$ROOT/$source" /Fo"$obj"
done

log "Linking"
lld-link \
  /DLL /NOLOGO /MACHINE:X64 \
  /OUT:"$OUT/usbdisplaydd.dll" \
  /LIBPATH:"$SDK_UM_LIB" \
  /LIBPATH:"$SDK_UCRT_LIB" \
  "$WORK"/obj/*.obj \
  "$IDDCX_STUB" \
  "$WDF_STUB" \
  d3d11.lib dxgi.lib setupapi.lib hid.lib winusb.lib \
  advapi32.lib ole32.lib user32.lib gdi32.lib kernel32.lib \
  libcmt.lib libcpmt.lib libucrt.lib \
  /ENTRY:_DllMainCRTStartup

# The INF carries a version that must move for Windows to replace an
# installed copy, so it is stamped the same way the Windows build does.
STAMP="$(date +%m/%d/%Y),$(date +%H.%M.%S).0"
sed "s|^DriverVer *=.*|DriverVer = $STAMP|" \
  "$ROOT/inf/usbdisplaydd.inf" > "$OUT/usbdisplaydd.inf"

log "Built $OUT/usbdisplaydd.dll"
ls -la "$OUT/usbdisplaydd.dll"
