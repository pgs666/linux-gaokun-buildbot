#!/usr/bin/env bash
set -euo pipefail

. "$(dirname "$0")/lib/common_image.sh"

: "${GAOKUN_DIR:?missing GAOKUN_DIR}"
: "${WORKDIR:?missing WORKDIR}"
: "${ROOTFS_DIR:?missing ROOTFS_DIR}"
: "${ARTIFACT_DIR:?missing ARTIFACT_DIR}"
: "${IMAGE_FILE:?missing IMAGE_FILE}"
: "${IMAGE_SIZE:?missing IMAGE_SIZE}"

BUILD_EL2="${BUILD_EL2:-false}"
KREL="$(cat "$WORKDIR/kernel-release.txt")"
KREL_EL2=""
if [[ "$BUILD_EL2" == "true" && -f "$WORKDIR/kernel-release-el2.txt" ]]; then
  KREL_EL2="$(cat "$WORKDIR/kernel-release-el2.txt")"
fi

EFI_END_MIB=1025
truncate -s "$IMAGE_SIZE" "$IMAGE_FILE"
parted -s "$IMAGE_FILE" mklabel gpt
parted -s "$IMAGE_FILE" mkpart EFI fat32 1MiB "${EFI_END_MIB}MiB"
parted -s "$IMAGE_FILE" set 1 esp on
parted -s "$IMAGE_FILE" mkpart rootfs btrfs "${EFI_END_MIB}MiB" 100%

LOOP="$(sudo losetup --show -fP "$IMAGE_FILE")"
sudo mkfs.vfat -F32 -n EFI "${LOOP}p1"
sudo mkfs.btrfs -f -L rootfs "${LOOP}p2"

EFI_UUID="$(sudo blkid -s UUID -o value "${LOOP}p1")"
ROOT_UUID="$(sudo blkid -s UUID -o value "${LOOP}p2")"

MNT=/mnt/ego-archlinux
cleanup() {
  set +e
  sudo umount "$MNT/dev/pts" 2>/dev/null || true
  sudo umount "$MNT/boot/efi" 2>/dev/null || true
  sudo umount "$MNT/var" 2>/dev/null || true
  sudo umount "$MNT/home" 2>/dev/null || true
  sudo umount "$MNT/dev" 2>/dev/null || true
  sudo umount "$MNT/proc" 2>/dev/null || true
  sudo umount "$MNT/sys" 2>/dev/null || true
  sudo umount "$MNT/run" 2>/dev/null || true
  sudo umount "$MNT" 2>/dev/null || true
  sudo losetup -d "$LOOP" 2>/dev/null || true
}
trap cleanup EXIT

sudo mkdir -p "$MNT"
sudo mount "${LOOP}p2" "$MNT"
sudo btrfs subvolume create "$MNT/@"
sudo btrfs subvolume create "$MNT/@home"
sudo btrfs subvolume create "$MNT/@var"
sudo umount "$MNT"
sudo mount -o subvol=@ "${LOOP}p2" "$MNT"
sudo mkdir -p "$MNT/home"
sudo mount -o subvol=@home "${LOOP}p2" "$MNT/home"
sudo mkdir -p "$MNT/var"
sudo mount -o subvol=@var "${LOOP}p2" "$MNT/var"
sudo mkdir -p "$MNT/boot/efi"
sudo mount "${LOOP}p1" "$MNT/boot/efi"

sudo rsync -aHAX "$ROOTFS_DIR/" "$MNT/"
install_common_image_assets "$MNT" "$GAOKUN_DIR"

sudo tee "$MNT/etc/fstab" >/dev/null <<EOF
UUID=${ROOT_UUID}  /         btrfs  subvol=@,compress=zstd:1,ssd,noatime  0  0
UUID=${ROOT_UUID}  /home     btrfs  subvol=@home,compress=zstd:1,ssd,noatime  0  0
UUID=${ROOT_UUID}  /var      btrfs  subvol=@var,compress=zstd:1,ssd,noatime  0  0
UUID=${EFI_UUID}   /boot/efi vfat   defaults,nofail,x-systemd.device-timeout=10s  0  2
EOF

sudo mount --bind /dev "$MNT/dev"
sudo mount --bind /dev/pts "$MNT/dev/pts"
sudo mount -t proc proc "$MNT/proc"
sudo mount -t sysfs sys "$MNT/sys"
sudo mount -t tmpfs tmpfs "$MNT/run"

sudo chroot "$MNT" /usr/bin/env KREL="$KREL" KREL_EL2="$KREL_EL2" BUILD_EL2="$BUILD_EL2" ROOT_UUID="$ROOT_UUID" /bin/bash -euxo pipefail <<'CHROOT_EOF'
echo "archlinux" > /etc/hostname
id -u user >/dev/null 2>&1 || useradd -m -s /bin/bash -G wheel user
echo "user:user" | chpasswd
mkdir -p /etc/sudoers.d
echo "%wheel ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/wheel-nopasswd
chmod 440 /etc/sudoers.d/wheel-nopasswd

sed -i 's/^#\(en_US.UTF-8 UTF-8\)/\1/' /etc/locale.gen
sed -i 's/^#\(zh_CN.UTF-8 UTF-8\)/\1/' /etc/locale.gen
locale-gen
cat > /etc/locale.conf <<'EOF'
LANG=zh_CN.UTF-8
LC_MESSAGES=zh_CN.UTF-8
EOF

mkdir -p /var/lib/AccountsService/users
cat > /var/lib/AccountsService/users/user <<'EOF'
[User]
Language=zh_CN.UTF-8
EOF
cat > /var/lib/AccountsService/users/gdm <<'EOF'
[User]
Language=zh_CN.UTF-8
SystemAccount=true
EOF

install -d -m 0755 /home/user/.config
install -Dm644 /usr/local/share/gaokun/monitors.xml /home/user/.config/monitors.xml
chown user:user /home/user/.config/monitors.xml

systemctl enable gdm NetworkManager sshd huawei-touchpad.service \
  gdm-monitor-sync.service patch-nvm-bdaddr.service || true

mkdir -p /etc/initcpio/install /etc/initcpio/hooks
cat > /etc/initcpio/install/gaokun3-firmware <<'EOF'
build() {
  add_file /usr/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcadsp8280.mbn
  add_file /usr/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qccdsp8280.mbn
  add_file /usr/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcslpi8280.mbn
  add_file /usr/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/audioreach-tplg.bin
}

help() {
  echo "Add gaokun3 DSP firmware files to the initramfs"
}
EOF
cat > /etc/initcpio/hooks/gaokun3-firmware <<'EOF'
run_hook() { :; }
EOF

cat > /etc/mkinitcpio.conf <<'EOF'
MODULES=(btrfs nvme phy_qcom_qmp_pcie phy_qcom_qmp_combo phy_qcom_qmp_usb phy_qcom_snps_femto_v2 usb_storage uas typec pci_pwrctrl_pwrseq ath11k ath11k_pci i2c_hid_of)
BINARIES=()
FILES=()
HOOKS=(base systemd autodetect modconf block filesystems keyboard fsck gaokun3-firmware)
COMPRESSION="zstd"
EOF

install -d /etc/kernel
cat > /etc/kernel/install.conf <<'EOF'
layout=bls
EOF

cat > /etc/kernel/cmdline <<EOF
root=UUID=$ROOT_UUID rootflags=subvol=@ clk_ignore_unused pd_ignore_unused arm64.nopauth iommu.passthrough=0 iommu.strict=0 pcie_aspm.policy=powersupersave efi=noruntime fbcon=rotate:1 usbhid.quirks=0x12d1:0x10b8:0x20000000 consoleblank=0 loglevel=4 psi=1
EOF

run_kernel_install() {
  local krel="$1"
  local dtb="$2"
  local initramfs="$3"
  local conf_root

  install -d "/boot/dtb-$krel/qcom"
  cp "/usr/lib/modules/$krel/dtb/qcom/$dtb" "/boot/dtb-$krel/qcom/$dtb"

  conf_root="$(mktemp -d)"
  cat > "$conf_root/install.conf" <<'EOF'
layout=bls
EOF
  printf '%s\n' "$(cat /etc/kernel/cmdline)" > "$conf_root/cmdline"
  printf 'qcom/%s\n' "$dtb" > "$conf_root/devicetree"

  kernel-install --entry-token=machine-id remove "$krel" || true
  KERNEL_INSTALL_CONF_ROOT="$conf_root" \
    kernel-install --verbose --make-entry-directory=yes --entry-token=machine-id add \
    "$krel" "/usr/lib/modules/$krel/vmlinuz" "$initramfs"
  rm -rf "$conf_root"
}

mkinitcpio -k "$KREL" -g "/boot/initramfs-$KREL.img"
if [[ "$BUILD_EL2" == "true" && -n "$KREL_EL2" ]]; then
  mkinitcpio -k "$KREL_EL2" -g "/boot/initramfs-$KREL_EL2.img"
fi

rm -f /etc/machine-id
systemd-machine-id-setup
MACHINE_ID="$(cat /etc/machine-id)"

bootctl --no-variables --esp-path=/boot/efi install

run_kernel_install "$KREL" "sc8280xp-huawei-gaokun3.dtb" "/boot/initramfs-$KREL.img"
if [[ "$BUILD_EL2" == "true" && -n "$KREL_EL2" ]]; then
  run_kernel_install "$KREL_EL2" "sc8280xp-huawei-gaokun3-el2.dtb" "/boot/initramfs-$KREL_EL2.img"
fi

cat > /boot/efi/loader/loader.conf <<EOF
default ${MACHINE_ID}-${KREL}.conf
timeout 5
console-mode keep
editor no
EOF
CHROOT_EOF

if [[ "$BUILD_EL2" == "true" && -n "$KREL_EL2" ]]; then
  install_el2_efi_payloads "$MNT" "$GAOKUN_DIR"
fi

sync

trap - EXIT
cleanup
