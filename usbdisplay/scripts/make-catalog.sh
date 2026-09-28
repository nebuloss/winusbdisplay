#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Generate and sign a Windows driver catalog, on Linux.
#
# This is the step everybody says needs Windows, and it does not. A catalog
# is a PKCS#7 SignedData whose content is a Microsoft certificate trust list,
# content type OID 1.3.6.1.4.1.311.10.1: a DER structure listing every file
# in the package by name and hash. Nothing in it is secret and nothing in it
# is undocumented enough to stop two existing projects from producing one:
#
#   LINBIT/generate-cat-file  builds the unsigned catalog. C99, no library
#                             dependencies, GPL-2.0, and used in production
#                             by WinDRBD to ship a *kernel* driver, which is
#                             a far harder audience than this package.
#   osslsigncode              puts an Authenticode signature on it. It has
#                             been able to sign .cat files since 2022.
#
# Neither can replace the other: osslsigncode signs catalogs but will not
# create one, and generate-cat-file creates them but does not sign.
#
# The catalog is named from the INF's own CatalogFile line rather than passed
# in. Windows matches the two by name and a mismatch fails at install time
# with a message about the package being unsigned, which sends you looking at
# certificates instead of at a typo.
#
# Member hashes are SHA1, because that is what the catalog format has always
# used for them and what Windows 10 and 11 still accept. That is not the
# signature: the signature is SHA256, and it is the signature that carries
# the trust decision.

set -euo pipefail

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

DIR=
HWID=
CERT=
KEY=

while [ $# -gt 0 ]; do
  case "$1" in
    --dir)  DIR=$2;  shift 2 ;;
    --hwid) HWID=$2; shift 2 ;;
    --cert) CERT=$2; shift 2 ;;
    --key)  KEY=$2;  shift 2 ;;
    *) die "unknown argument: $1" ;;
  esac
done

[ -n "$DIR" ]  || die "usage: $0 --dir <package> --hwid <id> [--cert pem --key pem]"
[ -n "$HWID" ] || die "a hardware id is required; it is recorded in the catalog"
[ -d "$DIR" ]  || die "$DIR is not a directory"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOLS="$ROOT/build/cross/catalog"

# Pinned rather than tracking the branch. This produces a signed artefact
# that users are asked to trust, so what it was built from should be a fact
# about the release and not about the day it was built.
CATGEN_REPO=https://github.com/LINBIT/generate-cat-file
CATGEN_REF=65ebf4de79a42b0a1104fbef37b1c207ff13e1e9

command -v osslsigncode >/dev/null 2>&1 || {
  [ -z "$CERT" ] || die "osslsigncode is not installed and signing was asked for"
}

if [ ! -x "$TOOLS/generate-cat-file" ]; then
  log "Fetching the catalog generator"
  rm -rf "$TOOLS"
  mkdir -p "$(dirname "$TOOLS")"
  git clone --quiet "$CATGEN_REPO" "$TOOLS"
  git -C "$TOOLS" checkout --quiet "$CATGEN_REF" 2>/dev/null ||
    log "warning: $CATGEN_REF is not in the clone, using the default branch"
  make -C "$TOOLS" >/dev/null
  chmod +x "$TOOLS/gencat.sh"
fi

INF=$(ls "$DIR"/*.inf 2>/dev/null | head -n 1) || true
[ -n "${INF:-}" ] || die "no INF in $DIR; a catalog covers a package, not a file"

# Carriage returns: INFs are CRLF and the name would otherwise carry one.
CAT=$(sed -n 's/^[Cc]atalog[Ff]ile[[:space:]]*=[[:space:]]*//p' "$INF" | tr -d '\r' | head -n 1)
[ -n "$CAT" ] || die "$INF names no CatalogFile, so nothing can be matched to it"

# Every file in the package directory is a member, which is what Windows
# verifies: it hashes each file the INF installs and looks for that hash.
# A member missing from the catalog is a file Windows refuses to copy.
MEMBERS=()
for file in "$DIR"/*; do
  case "$file" in
    *.cat) continue ;;
  esac
  [ -f "$file" ] && MEMBERS+=("$file")
done
[ ${#MEMBERS[@]} -gt 0 ] || die "$DIR has nothing to put in a catalog"

# Windows 10 and 11, 64 bit. The older OS strings the generator defaults to
# would claim support for Windows 7 and 8, which this driver does not have:
# the display extension it binds to did not exist then.
#
# No ARM64 entry, deliberately. The INF lists NTarm64 but nothing in the
# release is built for it, so claiming it in the catalog would only move the
# failure later.
OS_STRING=_v100_X64
OS_ATTR=2:10.0

log "Cataloguing $DIR as $CAT"
for member in "${MEMBERS[@]}"; do printf '    member: %s\n' "$(basename "$member")"; done

"$TOOLS/gencat.sh" -o "$DIR/$CAT" -h "$HWID" \
  -O "$OS_STRING" -A "$OS_ATTR" "${MEMBERS[@]}"

[ -s "$DIR/$CAT" ] || die "the catalog came out empty"

# Worth checking rather than trusting: an empty or truncated catalog is
# still a file, and the next thing to look at it is Windows.
if command -v openssl >/dev/null 2>&1; then
  openssl asn1parse -inform der -in "$DIR/$CAT" >/dev/null ||
    die "the catalog is not valid DER"
  openssl asn1parse -inform der -in "$DIR/$CAT" |
    grep -q '1.3.6.1.4.1.311.10.1' ||
    die "the catalog is DER but not a certificate trust list"
fi

if [ -z "$CERT" ]; then
  log "Unsigned catalog at $DIR/$CAT"
  exit 0
fi

[ -n "$KEY" ] || die "--cert without --key"

# The catalog is signed, and so is every program in the package. Windows
# only requires the catalog, but an unsigned executable next to a signed
# one is the sort of thing that gets a release quarantined.
#
# Signing happens after cataloguing, which looks wrong: signing changes the
# file, so its hash should no longer match. It does match, because the hash
# recorded for an executable is not of the bytes on disk. It skips the
# checksum and the certificate table, the two areas signing writes to, which
# is the whole reason the generator carries a separate program to compute
# it. Do this in the other order and the signature is the thing that breaks.
log "Signing"
for file in "$DIR/$CAT" "$DIR"/*.dll "$DIR"/*.exe; do
  [ -f "$file" ] || continue
  osslsigncode sign -h sha256 \
    -certs "$CERT" -key "$KEY" \
    -n 'usbdisplay' -i 'https://github.com/winusbdisplay' \
    -in "$file" -out "$file.signed" >/dev/null
  mv "$file.signed" "$file"
  printf '    signed: %s\n' "$(basename "$file")"
done

osslsigncode verify -CAfile "$CERT" -in "$DIR/$CAT" >/dev/null 2>&1 ||
  log "note: the signature does not chain to a trusted root, which is expected"

log "Signed catalog at $DIR/$CAT"
