#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Assemble a release, on Linux.
#
# The release is one file: usbdisplay-setup.exe. Everything else, the driver,
# both catalogs, the certificate, the console tool and the brightness
# control, travels inside it. A user downloads one thing and runs it.
#
# Nothing here runs a Windows program, under emulation or otherwise. The
# driver is cross compiled with clang, the catalogs are generated and signed
# with free tools, and the installer is built by NSIS, which is an ordinary
# Ubuntu package with a native Linux binary.
#
# The signing certificate is generated here and thrown away with the build.
# It proves the files have not changed since they were built and nothing
# whatever about who built them, which is said plainly in the release notes
# rather than left for somebody to work out. Trusting it is a real decision,
# and the alternative, building from source, is supported and documented.
#
#   ./package.sh <version>

set -euo pipefail

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

VERSION="${1:-0.0.0}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "$ROOT/.." && pwd)"
BUILD="$ROOT/build"
STAGE="$BUILD/package"

DRIVER="$BUILD/driver/x64/Release"

command -v makensis >/dev/null 2>&1 ||
  die "makensis is not installed; it is the 'nsis' package"
command -v osslsigncode >/dev/null 2>&1 ||
  die "osslsigncode is not installed"

for required in \
    "$DRIVER/usbdisplaydd.dll" "$DRIVER/usbdisplaydd.inf" \
    "$BUILD/driversetup.exe" "$BUILD/usbdisplayctl.exe" \
    "$BUILD/usbdisplaytray.exe"; do
  [ -f "$required" ] || die "$required is missing; run cross-build.sh first"
done

rm -rf "$STAGE"
mkdir -p "$STAGE/driver" "$STAGE/winusb" "$STAGE/tools"

# Each driver package is one directory holding its INF and every file the
# INF names, which is what a catalog covers and what pnputil is handed.
cp "$DRIVER/usbdisplaydd.dll" "$DRIVER/usbdisplaydd.inf" "$STAGE/driver/"
cp "$ROOT/inf/usbdisplay_winusb.inf" "$STAGE/winusb/"
cp "$BUILD/usbdisplayctl.exe" "$BUILD/usbdisplaytray.exe" "$STAGE/tools/"
cp "$BUILD/driversetup.exe" "$STAGE/"

log "Generating a signing certificate"
KEYS="$BUILD/signing"
rm -rf "$KEYS"; mkdir -p "$KEYS"
openssl req -x509 -newkey rsa:2048 -nodes -days 1825 -sha256 \
  -subj "/CN=usbdisplay release $VERSION" \
  -addext "basicConstraints=critical,CA:FALSE" \
  -addext "keyUsage=critical,digitalSignature" \
  -addext "extendedKeyUsage=critical,codeSigning" \
  -keyout "$KEYS/key.pem" -out "$KEYS/cert.pem" 2>/dev/null

# The certificate travels in the form Windows reads: Authenticode signatures
# verify against the machine's trusted stores, and the driver step has to put
# it there before Windows will accept either package.
openssl x509 -in "$KEYS/cert.pem" -outform der -out "$STAGE/usbdisplay.cer"

# The hardware ids are recorded in the catalogs. They are what ties a catalog
# to the device it was made for.
"$ROOT/scripts/make-catalog.sh" --dir "$STAGE/driver" \
  --hwid 'root\usbdisplaydd' --cert "$KEYS/cert.pem" --key "$KEYS/key.pem"
"$ROOT/scripts/make-catalog.sh" --dir "$STAGE/winusb" \
  --hwid 'USB\VID_345F&PID_9133&MI_03' --cert "$KEYS/cert.pem" --key "$KEYS/key.pem"

# Signed after cataloguing, because signing changes a file; see
# make-catalog.sh for why the catalog survives that and these do not belong
# in it anyway.
log "Signing the programs outside the driver packages"
for program in "$STAGE/driversetup.exe" "$STAGE"/tools/*.exe; do
  osslsigncode sign -h sha256 -certs "$KEYS/cert.pem" -key "$KEYS/key.pem" \
    -n 'usbdisplay' -in "$program" -out "$program.signed" >/dev/null
  mv "$program.signed" "$program"
  printf '    signed: %s\n' "$(basename "$program")"
done

log "Building the installer"
# Windows' version field is four numbers, so a tag like "v0.1.0" has to be
# reduced to one. Anything that is not a number becomes zero rather than
# stopping the build, because a release should not fail over a label.
NUMERIC="$(printf '%s' "$VERSION" | sed 's/^v//' |
  sed 's/[^0-9.].*//' | cut -d. -f1-3)"
case "$NUMERIC" in
  *.*.*) ;;
  *.*)   NUMERIC="$NUMERIC.0" ;;
  ?*)    NUMERIC="$NUMERIC.0.0" ;;
  *)     NUMERIC="0.0.0" ;;
esac

makensis \
  "-DVERSION=$VERSION" "-DNUMERIC=$NUMERIC" "-DPAYLOAD=$STAGE" \
  "$ROOT/installer/usbdisplay.nsi"

INSTALLER="$BUILD/usbdisplay-setup.exe"
[ -f "$INSTALLER" ] || die "makensis produced no installer"

# Signed last of all. This is the only file a user ever sees, so it is the
# one whose signature actually gets looked at.
log "Signing the installer"
osslsigncode sign -h sha256 -certs "$KEYS/cert.pem" -key "$KEYS/key.pem" \
  -n 'USB Display driver' -i 'https://github.com/nebuloss/winusbdisplay' \
  -in "$INSTALLER" -out "$INSTALLER.signed" >/dev/null
mv "$INSTALLER.signed" "$INSTALLER"

rm -rf "$KEYS"

log "Packaged $INSTALLER"
ls -la "$INSTALLER"
