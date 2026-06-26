#!/usr/bin/env bash
# package-release.sh -- pack the prebuilt dist/ into a per-platform release
# archive. Used ONLY by CI on a v* tag; the repo tree and the Makefile stay
# version-free, so local builds and the e2e tests are unaffected.
#
# Version + platform live in the FOLDER name (and a VERSION file); the files
# inside keep their stable, unversioned names. This is deliberately NOT a
# per-file rename:
#   * import libraries (.dll.a) and macOS dylib install names embed the
#     unversioned library name, so a filename rename would mis-resolve them.
#     Keeping the names stable lets the whole bundle ship intact -- nothing is
#     omitted, nothing is rewritten.
#   * the mcode wrapper (vga_monitor_pkg_mcode.vhdl) names the library in its
#     `foreign` strings; a stable filename keeps that valid across versions.
#   * upgrading is repointing one folder path, not editing every
#     -l / -sv_lib / --load / -m reference; multiple versions coexist as
#     sibling folders.
#
#   ci/package-release.sh <version> <platform-label> [dist-dir] [out-dir]
#       version: semver without the leading 'v' (e.g. 1.4.0)

set -euo pipefail

VER="${1:?usage: package-release.sh <version> <platform> [dist] [out]}"
PLAT="${2:?missing platform label}"
DIST="${3:-build/dist}"
OUT="${4:-release}"

STAGE="$OUT/vga-monitor-$VER-$PLAT"
rm -rf "$STAGE"
mkdir -p "$STAGE"

# Copy the dist tree verbatim (unversioned filenames preserved).
cp -a "$DIST"/. "$STAGE"/

[ -f LICENSE ] && cp LICENSE "$STAGE/"
printf '%s\n' "$VER" > "$STAGE/VERSION"

tar -C "$OUT" -czf "$OUT/vga-monitor-$VER-$PLAT.tar.gz" "vga-monitor-$VER-$PLAT"
echo "packaged $OUT/vga-monitor-$VER-$PLAT.tar.gz"
ls -l "$STAGE"
