#!/bin/bash
# Build an aarch64 Ubuntu 24.04 ext4 rootfs + extract kernel & initrd.
# All heavy work runs inside Docker containers (arm64).
#
# Output: $OUTDIR/vmlinuz, $OUTDIR/initrd.img, $OUTDIR/rootfs.ext4
#
# Usage:
#   ./scripts/loophole/make-rootfs.sh
#   OUTDIR=/some/path ./scripts/loophole/make-rootfs.sh

set -euo pipefail

OUTDIR="${OUTDIR:-/tmp/qemu-assets}"
ROOTFS_SIZE="${ROOTFS_SIZE:-2G}"

mkdir -p "${OUTDIR}"

echo "=== Step 1: Create Ubuntu container with kernel + packages ==="
CID=$(docker create --platform linux/arm64 ubuntu:24.04 bash -c '
set -ex
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y linux-image-generic systemd-sysv openssh-server

# Set root password to "root"
echo "root:root" | chpasswd

# Allow root login on serial console
mkdir -p /etc/systemd/system/serial-getty@ttyAMA0.service.d
cat > /etc/systemd/system/serial-getty@ttyAMA0.service.d/override.conf <<UNIT
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --noclear %I 115200 linux
UNIT
systemctl enable serial-getty@ttyAMA0.service 2>/dev/null || true

# fstab
cat > /etc/fstab <<FSTAB
/dev/vda    /        ext4    defaults,noatime  0 1
FSTAB

echo "loophole-vm" > /etc/hostname

# Clean up apt cache to save space
apt-get clean
rm -rf /var/lib/apt/lists/*
')
echo "Container: ${CID}"
docker start -a "${CID}"

echo "=== Step 2: Copy kernel + initrd out ==="
# Copy /boot out to find kernel version
docker cp "${CID}:/boot" "${OUTDIR}/boot-tmp"
KVER=$(ls "${OUTDIR}/boot-tmp/vmlinuz-"* | head -1 | sed 's|.*vmlinuz-||')
echo "Kernel version: ${KVER}"
cp "${OUTDIR}/boot-tmp/vmlinuz-${KVER}" "${OUTDIR}/vmlinuz"
cp "${OUTDIR}/boot-tmp/initrd.img-${KVER}" "${OUTDIR}/initrd.img"
rm -rf "${OUTDIR}/boot-tmp"

echo "=== Step 3: Export filesystem ==="
docker export "${CID}" -o "${OUTDIR}/rootfs.tar"
docker rm "${CID}" > /dev/null

echo "=== Step 4: Create ext4 from tarball using mkfs.ext4 -d ==="
docker run --rm --platform linux/arm64 \
  -v "${OUTDIR}:/work" \
  ubuntu:24.04 bash -c '
set -ex
apt-get update && apt-get install -y e2fsprogs

mkdir -p /tmp/rootfs
tar xf /work/rootfs.tar -C /tmp/rootfs

for d in dev proc run sys tmp; do mkdir -p /tmp/rootfs/$d; done

rm -f /work/rootfs.ext4
mkfs.ext4 -d /tmp/rootfs -F /work/rootfs.ext4 '"${ROOTFS_SIZE}"'

rm -rf /tmp/rootfs /work/rootfs.tar
'

echo ""
echo "=== Done! Assets in ${OUTDIR}/: ==="
ls -lh "${OUTDIR}/"
