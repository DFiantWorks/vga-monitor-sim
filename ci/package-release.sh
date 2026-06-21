#!/usr/bin/env bash
# package-release.sh -- stamp a version into the artifact filenames and pack a
# release archive. Used ONLY by CI on a v* tag; the repo tree and the Makefile
# stay version-free, so local builds and the e2e tests are unaffected.
#
# The module/entity names, the C ABI symbols, and the $system-task names inside
# every file are left untouched -- only the *filenames* gain a version token, so
# upgrading is a one-line change to a build's source list and -l/-sv_lib/--load/
# -m references, nothing else:
#   vga_monitor.sv            -> vga_monitor_v1_4_0.sv        (module: vga_monitor)
#   libvga_monitor_dpi.so     -> libvga_monitor_dpi_v1_4_0.so (-lvga_monitor_dpi_v1_4_0)
#   vga_monitor.vpi           -> vga_monitor_v1_4_0.vpi       (vvp -m vga_monitor_v1_4_0)
#
# Underscores (v1_4_0), not dots, so the name is safe in -l / -sv_lib / -m / dlopen.
#
#   ci/package-release.sh <version> <platform-label> [dist-dir] [out-dir]
#       version: semver without the leading 'v' (e.g. 1.4.0)

set -euo pipefail

VER="${1:?usage: package-release.sh <version> <platform> [dist] [out]}"
PLAT="${2:?missing platform label}"
DIST="${3:-build/dist}"
OUT="${4:-release}"

TOK="v${VER//./_}"                         # 1.4.0 -> v1_4_0
STAGE="$OUT/vga-monitor-$VER-$PLAT"
rm -rf "$STAGE"
mkdir -p "$STAGE"

for f in "$DIST"/*; do
    [ -f "$f" ] || continue
    base="$(basename "$f")"
    # A MinGW import library (.dll.a) embeds the (unversioned) DLL name inside
    # it, so a filename rename can't version it correctly -- the renamed lib
    # would still resolve to the unversioned DLL. Omit it from versioned
    # archives; downstream can load the DLL at runtime (e.g. NVC --load) or
    # regenerate an import lib with dlltool. It stays in the unversioned bundle.
    case "$base" in *.dll.a) continue ;; esac
    if [[ "$base" == *.* ]]; then
        new="${base%.*}_$TOK.${base##*.}"  # stem_vX_Y_Z.ext
    else
        new="${base}_$TOK"
    fi
    cp "$f" "$STAGE/$new"
    # A macOS dylib carries its own install name (LC_ID_DYLIB); it must match the
    # renamed leaf or -rpath resolution fails. install_name_tool exists on macOS.
    case "$new" in
        *.dylib) install_name_tool -id "@rpath/$new" "$STAGE/$new" ;;
    esac
done

[ -f LICENSE ] && cp LICENSE "$STAGE/"
printf '%s\n' "$VER" > "$STAGE/VERSION"

tar -C "$OUT" -czf "$OUT/vga-monitor-$VER-$PLAT.tar.gz" "vga-monitor-$VER-$PLAT"
echo "packaged $OUT/vga-monitor-$VER-$PLAT.tar.gz"
ls -l "$STAGE"
