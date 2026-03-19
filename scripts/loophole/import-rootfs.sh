#!/bin/bash
# Import a rootfs.ext4 into a loophole volume, then boot QEMU.
#
# Usage:
#   ./scripts/loophole/import-rootfs.sh
#
# Prereqs:
#   - Run make-rootfs.sh first to produce /tmp/qemu-assets/{vmlinuz,initramfs,rootfs.ext4}
#   - Build QEMU with loophole support

set -euo pipefail

OUTDIR="${OUTDIR:-/tmp/qemu-assets}"
PROFILE="${PROFILE:-local}"
VOLUME="${VOLUME:-qemu-rootfs}"
QEMU="${QEMU:-$(dirname "$0")/../../build/qemu-system-aarch64}"

echo "=== Step 1: Create + attach volume (background) ==="
loophole device create "${VOLUME}" -s 512MB -p "${PROFILE}" &
OWNER_PID=$!

# Wait for the owner process to be reachable
echo "Waiting for volume owner..."
for i in $(seq 1 30); do
    if loophole -p "${PROFILE}" status "${VOLUME}" >/dev/null 2>&1; then
        break
    fi
    if ! kill -0 "${OWNER_PID}" 2>/dev/null; then
        echo "ERROR: owner process exited"
        exit 1
    fi
    sleep 0.5
done

echo "=== Step 2: Import rootfs ==="
loophole device dd if="${OUTDIR}/rootfs.ext4" of="${VOLUME}:" -p "${PROFILE}"

echo "=== Step 3: Shut down volume owner ==="
loophole -p "${PROFILE}" shutdown "${VOLUME}"
wait "${OWNER_PID}" 2>/dev/null || true

echo "=== Step 4: Boot QEMU ==="
echo "Starting QEMU with loophole volume '${VOLUME}'..."
exec "${QEMU}" \
    -M virt \
    -accel hvf \
    -cpu host \
    -m 1G \
    -kernel "${OUTDIR}/vmlinuz" \
    -initrd "${OUTDIR}/initramfs" \
    -append "root=/dev/vda rw console=ttyAMA0" \
    -drive driver=loophole,volume="${VOLUME}",if=virtio \
    -nographic
