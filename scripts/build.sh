#!/usr/bin/env bash
#
# Build the repeater firmware and collect flashable images in dist/.
#
#   scripts/build.sh            build both Zigbee and Thread
#   scripts/build.sh zigbee     build only the Zigbee image
#   scripts/build.sh thread     build only the Thread image
#
# Needs an ESP-IDF v6.1 environment (". $IDF_PATH/export.sh"). Each protocol gets
# its own build directory (build-<protocol>) and sdkconfig, so they never mix.
#
# Output per protocol, in dist/:
#   esp-802154-repeater-<protocol>-<version>-full.bin   merged image, flash at 0x0
#   esp-802154-repeater-<protocol>-<version>-app.bin    application only, flash at 0x20000
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="${ROOT}/dist"
CHIP="esp32c6"
APP_OFFSET="0x20000" # must match partitions.csv

die() { echo "error: $*" >&2; exit 1; }

command -v idf.py >/dev/null 2>&1 || die "idf.py not found, run '. \$IDF_PATH/export.sh' first"

if [ "$#" -eq 0 ]; then
    set -- zigbee thread
fi
for proto in "$@"; do
    case "${proto}" in
        zigbee|thread) ;;
        *) die "unknown protocol '${proto}' (expected: zigbee, thread)" ;;
    esac
done

# Same version string as the one embedded in the app (git describe), for the file names.
VERSION="$(git -C "${ROOT}" describe --always --tags --dirty 2>/dev/null || echo unknown)"

mkdir -p "${DIST}"

for proto in "$@"; do
    build_dir="${ROOT}/build-${proto}"
    name="esp-802154-repeater-${proto}-${VERSION}"

    echo "=== Building ${proto} firmware (${VERSION}) ==="
    idf.py -C "${ROOT}" -B "${build_dir}" \
        -DSDKCONFIG="${build_dir}/sdkconfig" \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.${proto}" \
        build

    # Sanity check: exactly the requested stack is enabled.
    if [ "${proto}" = zigbee ]; then
        grep -q '^CONFIG_ZB_ENABLED=y' "${build_dir}/sdkconfig" || die "Zigbee not enabled in ${proto} build"
    else
        grep -q '^CONFIG_OPENTHREAD_ENABLED=y' "${build_dir}/sdkconfig" || die "OpenThread not enabled in ${proto} build"
    fi

    (cd "${build_dir}" && python -m esptool --chip "${CHIP}" merge-bin -o "${DIST}/${name}-full.bin" @flash_args)
    cp "${build_dir}/esp_802154_repeater.bin" "${DIST}/${name}-app.bin"
done

(cd "${DIST}" && sha256sum ./*.bin > SHA256SUMS)

echo
echo "=== Images in ${DIST} ==="
ls -l "${DIST}"
echo
echo "Flash a full image with:"
echo "  python -m esptool --chip ${CHIP} -p <PORT> write-flash 0x0 dist/<image>-full.bin"
echo "or only the application (keeps bootloader/partition table) with:"
echo "  python -m esptool --chip ${CHIP} -p <PORT> write-flash ${APP_OFFSET} dist/<image>-app.bin"
