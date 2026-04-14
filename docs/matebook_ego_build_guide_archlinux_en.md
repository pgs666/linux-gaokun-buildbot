English | [中文](matebook_ego_build_guide_archlinux_zh.md)

# Huawei MateBook E Go 2023 Arch Linux ARM Manual Build Guide

> **Target Device**: Huawei MateBook E Go 2023 (codename `gaokun3`)  
> **Platform**: Qualcomm Snapdragon 8cx Gen3 (`SC8280XP`)  
> **Target System**: Arch Linux ARM GNOME, systemd-boot boot, Btrfs root filesystem  
> **Recommended Host**: Arch Linux ARM or another arm64 Linux host  
> **Repository Assumption**: This document assumes your current repository is at `~/gaokun/linux-gaokun-buildbot`

## Preparation Notes

This guide follows the same project layout as the Fedora and Ubuntu guides:

- `patches/`
- `defconfig/`
- `dts/`
- `tools/`
- `firmware/`

The initial rootfs comes from:

- `http://fl.us.mirror.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz`

If the host is arm64, you can build natively.  
If the host is x86_64, prepare an aarch64 cross toolchain and export `CROSS_COMPILE` before building the kernel.

## Step 1: Prepare Working Directory

Install host dependencies:

```bash
sudo pacman -S --needed base-devel git bc bison flex cpio rsync kmod \
    openssl libelf pahole ccache parted dosfstools btrfs-progs \
    curl python zstd
```

Prepare source and work directories:

```bash
mkdir -p ~/gaokun/matebook-build-archlinux

cd ~/gaokun
if [ ! -d "mainline-linux" ]; then
    git clone --depth 1 --branch v7.0 \
        https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git \
        mainline-linux
fi
```

Set environment variables:

```bash
export GAOKUN_DIR=~/gaokun/linux-gaokun-buildbot
export WORKDIR=~/gaokun/matebook-build-archlinux
export KERN_SRC=~/gaokun/mainline-linux
export KERN_OUT=~/gaokun/kernel-out
export KERN_OUT_EL2=~/gaokun/kernel-out-el2
export FW_REPO=$GAOKUN_DIR/firmware
export ROOTFS_DIR=$WORKDIR/rootfs
export IMAGE_FILE=$WORKDIR/archlinux-gaokun3.img
export CCACHE_DIR=$WORKDIR/.ccache
export CCACHE_BASEDIR=$WORKDIR
export CCACHE_NOHASHDIR=true
export CCACHE_COMPILERCHECK=content
if [ -d /usr/lib/ccache/bin ]; then
    export PATH=/usr/lib/ccache/bin:$PATH
elif [ -d /usr/lib/ccache ]; then
    export PATH=/usr/lib/ccache:$PATH
fi
```

## Step 2: Compile Kernel

Build the standard kernel first:

```bash
cd $KERN_SRC

git config user.name "local builder"
git config user.email "builder@example.com"
git am $GAOKUN_DIR/patches/*.patch

mkdir -p $KERN_OUT
ccache -z

make O=$KERN_OUT ARCH=arm64 gaokun3_defconfig
make O=$KERN_OUT ARCH=arm64 olddefconfig
make O=$KERN_OUT ARCH=arm64 -j$(nproc)
make O=$KERN_OUT ARCH=arm64 modules_prepare

KREL=$(cat $KERN_OUT/include/config/kernel.release)
echo $KREL
KREL_EL2=""
ccache -s
```

If EL2 is needed, continue on the same source tree and use a separate output directory:

```bash
rm -rf $KERN_OUT_EL2
git -C $KERN_SRC apply --index $GAOKUN_DIR/patches/el2/*.patch
git -C $KERN_SRC commit -m "Apply EL2 patches"
ccache -z

make -C $KERN_SRC O=$KERN_OUT_EL2 ARCH=arm64 gaokun3_defconfig
$KERN_SRC/scripts/config --file $KERN_OUT_EL2/.config --set-str LOCALVERSION "-gaokun3-el2"
make -C $KERN_SRC O=$KERN_OUT_EL2 ARCH=arm64 olddefconfig
make -C $KERN_SRC O=$KERN_OUT_EL2 ARCH=arm64 -j$(nproc)
make -C $KERN_SRC O=$KERN_OUT_EL2 ARCH=arm64 modules_prepare

KREL_EL2=$(cat $KERN_OUT_EL2/include/config/kernel.release)
echo $KREL_EL2
ccache -s
```

## Step 3: Build RootFS

Download and extract the Arch Linux ARM rootfs:

```bash
mkdir -p $ROOTFS_DIR
curl -fsSL \
    http://fl.us.mirror.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz \
    -o $WORKDIR/ArchLinuxARM-aarch64-latest.tar.gz
sudo bsdtar -xpf $WORKDIR/ArchLinuxARM-aarch64-latest.tar.gz -C $ROOTFS_DIR
```

Set up mirror and chroot mounts:

```bash
sudo tee $ROOTFS_DIR/etc/pacman.d/mirrorlist > /dev/null <<'EOF'
Server = http://fl.us.mirror.archlinuxarm.org/$arch/$repo
EOF

sudo cp /etc/resolv.conf $ROOTFS_DIR/etc/resolv.conf
sudo mount --bind /dev $ROOTFS_DIR/dev
sudo mount --bind /dev/pts $ROOTFS_DIR/dev/pts
sudo mount -t proc proc $ROOTFS_DIR/proc
sudo mount -t sysfs sys $ROOTFS_DIR/sys
sudo mount -t tmpfs tmpfs $ROOTFS_DIR/run
```

Install Arch Linux ARM packages:

```bash
sudo chroot $ROOTFS_DIR /bin/bash
```

Inside chroot:

```bash
pacman-key --init || true
pacman-key --populate archlinuxarm || true
pacman -Syyu --noconfirm

sed -i 's/^#\(en_US.UTF-8 UTF-8\)/\1/' /etc/locale.gen
sed -i 's/^#\(zh_CN.UTF-8 UTF-8\)/\1/' /etc/locale.gen
locale-gen
cat > /etc/locale.conf <<'EOF'
LANG=zh_CN.UTF-8
LC_MESSAGES=zh_CN.UTF-8
EOF

pacman -S --noconfirm --needed \
    gnome sudo openssh networkmanager gdm \
    mkinitcpio systemd \
    noto-fonts-cjk noto-fonts-emoji \
    i2c-tools alsa-utils pipewire pipewire-alsa wireplumber \
    fcitx5-im fcitx5-chinese-addons gnome-tweaks extension-manager \
    mpv v4l-utils vim nano ripgrep git htop fastfetch screen firefox
exit
```

Unmount the virtual filesystems:

```bash
sudo umount $ROOTFS_DIR/dev/pts
sudo umount $ROOTFS_DIR/dev
sudo umount $ROOTFS_DIR/proc
sudo umount $ROOTFS_DIR/sys
sudo umount $ROOTFS_DIR/run
```

Install kernel, firmware and local tools:

```bash
cd $KERN_SRC

KREL=$(cat $KERN_OUT/include/config/kernel.release)
echo $KREL

sudo make O=$KERN_OUT ARCH=arm64 INSTALL_MOD_PATH=$ROOTFS_DIR modules_install
sudo rm -f $ROOTFS_DIR/lib/modules/$KREL/{build,source}
sudo mkdir -p $ROOTFS_DIR/usr/lib/modules
sudo mv $ROOTFS_DIR/lib/modules/$KREL $ROOTFS_DIR/usr/lib/modules/
sudo rmdir $ROOTFS_DIR/lib/modules || true
sudo cp $KERN_OUT/arch/arm64/boot/Image \
    $ROOTFS_DIR/usr/lib/modules/$KREL/vmlinuz
sudo cp $KERN_OUT/System.map \
    $ROOTFS_DIR/usr/lib/modules/$KREL/System.map
sudo cp $KERN_OUT/.config \
    $ROOTFS_DIR/usr/lib/modules/$KREL/config
echo linux-gaokun3 | sudo tee $ROOTFS_DIR/usr/lib/modules/$KREL/pkgbase > /dev/null
sudo mkdir -p $ROOTFS_DIR/usr/lib/modules/$KREL/dtb/qcom
sudo cp $KERN_OUT/arch/arm64/boot/dts/qcom/sc8280xp-huawei-gaokun3.dtb \
    $ROOTFS_DIR/usr/lib/modules/$KREL/dtb/qcom/

if [ -n "$KREL_EL2" ]; then
    sudo make -C $KERN_SRC O=$KERN_OUT_EL2 ARCH=arm64 INSTALL_MOD_PATH=$ROOTFS_DIR modules_install
    sudo rm -f $ROOTFS_DIR/lib/modules/$KREL_EL2/{build,source}
    sudo mv $ROOTFS_DIR/lib/modules/$KREL_EL2 $ROOTFS_DIR/usr/lib/modules/
    sudo cp $KERN_OUT_EL2/arch/arm64/boot/Image \
        $ROOTFS_DIR/usr/lib/modules/$KREL_EL2/vmlinuz
    sudo cp $KERN_OUT_EL2/System.map \
        $ROOTFS_DIR/usr/lib/modules/$KREL_EL2/System.map
    sudo cp $KERN_OUT_EL2/.config \
        $ROOTFS_DIR/usr/lib/modules/$KREL_EL2/config
    echo linux-gaokun3-el2 | sudo tee $ROOTFS_DIR/usr/lib/modules/$KREL_EL2/pkgbase > /dev/null
    sudo mkdir -p $ROOTFS_DIR/usr/lib/modules/$KREL_EL2/dtb/qcom
    sudo cp $KERN_OUT_EL2/arch/arm64/boot/dts/qcom/sc8280xp-huawei-gaokun3-el2.dtb \
        $ROOTFS_DIR/usr/lib/modules/$KREL_EL2/dtb/qcom/
fi

sudo mkdir -p $ROOTFS_DIR/usr/lib/firmware
sudo cp -r $FW_REPO/. $ROOTFS_DIR/usr/lib/firmware/

sudo mkdir -p $ROOTFS_DIR/usr/local/bin
sudo mkdir -p $ROOTFS_DIR/usr/local/lib/gaokun-touchscreen-tuner
sudo mkdir -p $ROOTFS_DIR/etc/systemd/system
sudo mkdir -p $ROOTFS_DIR/usr/share/alsa/ucm2/Qualcomm/sc8280xp
sudo mkdir -p $ROOTFS_DIR/usr/share/applications

sudo cp $GAOKUN_DIR/tools/touchscreen-tuner/touchscreen-tune \
    $ROOTFS_DIR/usr/local/bin/touchscreen-tune
sudo cp $GAOKUN_DIR/tools/touchscreen-tuner/tune.py \
    $ROOTFS_DIR/usr/local/lib/gaokun-touchscreen-tuner/tune.py
sudo cp $GAOKUN_DIR/tools/touchscreen-tuner/tune-icon.svg \
    $ROOTFS_DIR/usr/local/lib/gaokun-touchscreen-tuner/tune-icon.svg
sudo cp $GAOKUN_DIR/tools/touchscreen-tuner/touchscreen-tune.desktop \
    $ROOTFS_DIR/usr/share/applications/touchscreen-tune.desktop
sudo chmod +x $ROOTFS_DIR/usr/local/bin/touchscreen-tune

sudo cp $GAOKUN_DIR/tools/touchpad/huawei-tp-activate.py \
    $ROOTFS_DIR/usr/local/bin/
sudo cp $GAOKUN_DIR/tools/touchpad/huawei-touchpad.service \
    $ROOTFS_DIR/etc/systemd/system/
sudo chmod +x $ROOTFS_DIR/usr/local/bin/huawei-tp-activate.py

sudo cp $GAOKUN_DIR/tools/monitors/gdm-monitor-sync \
    $ROOTFS_DIR/usr/local/bin/
sudo cp $GAOKUN_DIR/tools/monitors/gdm-monitor-sync.service \
    $ROOTFS_DIR/etc/systemd/system/
sudo chmod +x $ROOTFS_DIR/usr/local/bin/gdm-monitor-sync

sudo cp $GAOKUN_DIR/tools/bluetooth/patch-nvm-bdaddr.py \
    $ROOTFS_DIR/usr/local/bin/
sudo cp $GAOKUN_DIR/tools/bluetooth/patch-nvm-bdaddr.service \
    $ROOTFS_DIR/etc/systemd/system/
sudo chmod +x $ROOTFS_DIR/usr/local/bin/patch-nvm-bdaddr.py

sudo cp $GAOKUN_DIR/tools/audio/sc8280xp.conf \
    $ROOTFS_DIR/usr/share/alsa/ucm2/Qualcomm/sc8280xp/

sudo mkdir -p $ROOTFS_DIR/usr/local/share/gaokun
sudo cp -a $GAOKUN_DIR/tools/image-assets/etc/. \
    $ROOTFS_DIR/etc/
sudo cp $GAOKUN_DIR/tools/image-assets/usr/local/share/gaokun/monitors.xml \
    $ROOTFS_DIR/usr/local/share/gaokun/monitors.xml
```

## Step 4: Create Bootable Image

Follow the same Btrfs image layout used by the Fedora workflow:

```bash
cd $WORKDIR
truncate -s 12G $IMAGE_FILE

parted -s $IMAGE_FILE mklabel gpt
parted -s $IMAGE_FILE mkpart EFI fat32 1MiB 1025MiB
parted -s $IMAGE_FILE set 1 esp on
parted -s $IMAGE_FILE mkpart rootfs btrfs 1025MiB 100%

LOOP=$(sudo losetup --show -fP $IMAGE_FILE)
sudo mkfs.vfat -F32 -n EFI ${LOOP}p1
sudo mkfs.btrfs -f -L rootfs ${LOOP}p2
```

Mount the image, sync the rootfs, and chroot:

```bash
sudo mkdir -p /mnt/ego-archlinux
sudo mount ${LOOP}p2 /mnt/ego-archlinux
sudo btrfs subvolume create /mnt/ego-archlinux/@
sudo btrfs subvolume create /mnt/ego-archlinux/@home
sudo btrfs subvolume create /mnt/ego-archlinux/@var
sudo umount /mnt/ego-archlinux

sudo mount -o subvol=@ ${LOOP}p2 /mnt/ego-archlinux
sudo mkdir -p /mnt/ego-archlinux/home /mnt/ego-archlinux/var /mnt/ego-archlinux/boot/efi
sudo mount -o subvol=@home ${LOOP}p2 /mnt/ego-archlinux/home
sudo mount -o subvol=@var ${LOOP}p2 /mnt/ego-archlinux/var
sudo mount ${LOOP}p1 /mnt/ego-archlinux/boot/efi

sudo rsync -aHAX $ROOTFS_DIR/ /mnt/ego-archlinux/
```

Inside chroot, enable services, build initramfs, and generate BLS entries:

```bash
sudo mount --bind /dev /mnt/ego-archlinux/dev
sudo mount --bind /dev/pts /mnt/ego-archlinux/dev/pts
sudo mount -t proc proc /mnt/ego-archlinux/proc
sudo mount -t sysfs sys /mnt/ego-archlinux/sys
sudo mount -t tmpfs tmpfs /mnt/ego-archlinux/run
sudo chroot /mnt/ego-archlinux /bin/bash
```

Inside chroot:

```bash
echo archlinux > /etc/hostname
id -u user >/dev/null 2>&1 || useradd -m -s /bin/bash -G wheel user
echo "user:user" | chpasswd
mkdir -p /etc/sudoers.d
echo "%wheel ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/wheel-nopasswd
chmod 440 /etc/sudoers.d/wheel-nopasswd

systemctl enable gdm NetworkManager sshd huawei-touchpad.service \
    gdm-monitor-sync.service patch-nvm-bdaddr.service

mkdir -p /etc/initcpio/install /etc/initcpio/hooks
cat > /etc/initcpio/install/gaokun3-firmware <<'EOF'
build() {
  add_file /usr/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcadsp8280.mbn
  add_file /usr/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qccdsp8280.mbn
  add_file /usr/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcslpi8280.mbn
  add_file /usr/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/audioreach-tplg.bin
}
help() { echo "Add gaokun3 DSP firmware files to the initramfs"; }
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

cat > /etc/kernel/install.conf <<'EOF'
layout=bls
EOF
cat > /etc/kernel/cmdline <<EOF
root=UUID=$(blkid -s UUID -o value ${LOOP}p2) rootflags=subvol=@ clk_ignore_unused pd_ignore_unused arm64.nopauth iommu.passthrough=0 iommu.strict=0 pcie_aspm.policy=powersupersave efi=noruntime fbcon=rotate:1 usbhid.quirks=0x12d1:0x10b8:0x20000000 consoleblank=0 loglevel=4 psi=1
EOF

mkinitcpio -k $KREL -g /boot/initramfs-$KREL.img
if [ -n "$KREL_EL2" ]; then
    mkinitcpio -k $KREL_EL2 -g /boot/initramfs-$KREL_EL2.img
fi

rm -f /etc/machine-id
systemd-machine-id-setup
bootctl --no-variables --esp-path=/boot/efi install

mkdir -p /boot/dtb-$KREL/qcom
cp /usr/lib/modules/$KREL/dtb/qcom/sc8280xp-huawei-gaokun3.dtb /boot/dtb-$KREL/qcom/
kernel-install --make-entry-directory=yes --entry-token=machine-id add \
    $KREL /usr/lib/modules/$KREL/vmlinuz /boot/initramfs-$KREL.img
```

If EL2 is enabled, add the EL2 DTB, initramfs, and `kernel-install add` again for the second kernel.

## References

- [Arch Linux ARM](https://archlinuxarm.org/)
- [linux-gaokun-buildbot Fedora guide](matebook_ego_build_guide_fedora44_en.md)
- [linux-gaokun-buildbot Ubuntu guide](matebook_ego_build_guide_ubuntu26.04_en.md)
