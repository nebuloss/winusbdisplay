#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Assemble a release, on Linux.
#
# Everything a user needs comes out of one machine: the driver, the
# installer, the console tool, the brightness control, both catalogs and the
# signature over all of them. Nothing here runs a Windows program, under
# emulation or otherwise.
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

VERSION="${1:-dev}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "$ROOT/.." && pwd)"
BUILD="$ROOT/build"
STAGE="$BUILD/package"

DRIVER="$BUILD/driver/x64/Release"

for required in \
    "$DRIVER/usbdisplaydd.dll" "$DRIVER/usbdisplaydd.inf" \
    "$BUILD/usbdisplay-setup.exe" "$BUILD/usbdisplayctl.exe" \
    "$BUILD/usbdisplaytray.exe"; do
  [ -f "$required" ] || die "$required is missing; run cross-build.sh first"
done

rm -rf "$STAGE"
mkdir -p "$STAGE/driver" "$STAGE/winusb" "$STAGE/tools"

# Each package is one directory holding its INF and every file the INF
# names, which is what a catalog covers and what pnputil is handed.
cp "$DRIVER/usbdisplaydd.dll" "$DRIVER/usbdisplaydd.inf" "$STAGE/driver/"
cp "$ROOT/inf/usbdisplay_winusb.inf" "$STAGE/winusb/"
cp "$BUILD/usbdisplayctl.exe" "$BUILD/usbdisplaytray.exe" "$STAGE/tools/"

# The installer sits at the top, where somebody unpacking the archive finds
# it without being told where to look.
cp "$BUILD/usbdisplay-setup.exe" "$STAGE/"

log "Generating a signing certificate"
KEYS="$BUILD/signing"
rm -rf "$KEYS"; mkdir -p "$KEYS"
openssl req -x509 -newkey rsa:2048 -nodes -days 1825 -sha256 \
  -subj "/CN=usbdisplay release $VERSION" \
  -addext "basicConstraints=critical,CA:FALSE" \
  -addext "keyUsage=critical,digitalSignature" \
  -addext "extendedKeyUsage=critical,codeSigning" \
  -keyout "$KEYS/key.pem" -out "$KEYS/cert.pem" 2>/dev/null

# The certificate travels with the package in the form Windows reads:
# Authenticode signatures verify against the machine's trusted stores, and
# the installer has to put it there before Windows will accept either driver.
openssl x509 -in "$KEYS/cert.pem" -outform der -out "$STAGE/usbdisplay.cer"

# The hardware ids are recorded in the catalogs. They are what ties a
# catalog to the device it was made for.
"$ROOT/scripts/make-catalog.sh" --dir "$STAGE/driver" \
  --hwid 'root\usbdisplaydd' --cert "$KEYS/cert.pem" --key "$KEYS/key.pem"
"$ROOT/scripts/make-catalog.sh" --dir "$STAGE/winusb" \
  --hwid 'USB\VID_345F&PID_9133&MI_03' --cert "$KEYS/cert.pem" --key "$KEYS/key.pem"

# Signed last, because they are not catalog members and signing a file
# changes it: doing this first would invalidate every hash above.
log "Signing the programs outside the driver packages"
for program in "$STAGE/usbdisplay-setup.exe" "$STAGE"/tools/*.exe; do
  osslsigncode sign -h sha256 -certs "$KEYS/cert.pem" -key "$KEYS/key.pem" \
    -n 'usbdisplay' -in "$program" -out "$program.signed" >/dev/null
  mv "$program.signed" "$program"
  printf '    signed: %s\n' "$(basename "$program")"
done

rm -rf "$KEYS"

cp "$ROOT/scripts/install.ps1" "$ROOT/scripts/purge.ps1" \
   "$ROOT/scripts/reattach.ps1" "$ROOT/README.md" "$STAGE/"
cp "$REPO/LICENSE" "$STAGE/"

ARCHIVE="$BUILD/usbdisplay-$VERSION.zip"
rm -f "$ARCHIVE"
(cd "$STAGE" && zip -qr "$ARCHIVE" .)

log "Packaged $ARCHIVE"
(cd "$STAGE" && find . -type f | sort | sed 's|^\./|    |')
