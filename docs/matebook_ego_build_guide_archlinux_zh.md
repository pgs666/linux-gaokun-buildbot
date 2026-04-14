[English](matebook_ego_build_guide_archlinux_en.md) | 中文

# 华为 MateBook E Go 2023 Arch Linux ARM 手动构建指南

> **目标设备**：Huawei MateBook E Go 2023（代号 `gaokun3`）  
> **平台**：Qualcomm Snapdragon 8cx Gen3（`SC8280XP`）  
> **目标系统**：Arch Linux ARM GNOME、`systemd-boot` 引导、Btrfs 根文件系统  
> **推荐宿主**：Arch Linux ARM 或其他 arm64 Linux 主机  
> **仓库假设路径**：本文默认仓库位于 `~/gaokun/linux-gaokun-buildbot`

## 准备说明

本文继续沿用项目内已有内容：

- `patches/`
- `defconfig/`
- `dts/`
- `tools/`
- `firmware/`

初始 rootfs 来自：

- `http://fl.us.mirror.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz`

如果宿主本身是 arm64，可以原生构建。  
如果宿主是 x86_64，请先准备可用的 aarch64 交叉工具链，并在编译前额外导出 `CROSS_COMPILE`。

## 第一步：准备工作目录

安装宿主依赖：

```bash
sudo pacman -S --needed base-devel git bc bison flex cpio rsync kmod \
    openssl libelf pahole ccache parted dosfstools btrfs-progs \
    curl python zstd
```

准备源码和工作目录：

```bash
mkdir -p ~/gaokun/matebook-build-archlinux

cd ~/gaokun
if [ ! -d "mainline-linux" ]; then
    git clone --depth 1 --branch v7.0 \
        https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git \
        mainline-linux
fi
```

设置环境变量：

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

## 第二步：编译内核

先编译标准内核：

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

如果需要 EL2，可继续在同一棵源码上构建第二套输出：

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

## 第三步：构建 RootFS

下载并解包 Arch Linux ARM rootfs：

```bash
mkdir -p $ROOTFS_DIR
curl -fsSL \
    http://fl.us.mirror.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz \
    -o $WORKDIR/ArchLinuxARM-aarch64-latest.tar.gz
sudo bsdtar -xpf $WORKDIR/ArchLinuxARM-aarch64-latest.tar.gz -C $ROOTFS_DIR
```

配置镜像源并挂载 chroot 所需目录：

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

安装 Arch Linux ARM 软件包：

```bash
sudo chroot $ROOTFS_DIR /bin/bash
```

在 chroot 中执行：

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

卸载虚拟文件系统：

```bash
sudo umount $ROOTFS_DIR/dev/pts
sudo umount $ROOTFS_DIR/dev
sudo umount $ROOTFS_DIR/proc
sudo umount $ROOTFS_DIR/sys
sudo umount $ROOTFS_DIR/run
```

安装内核、固件和本地工具：

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

## 第四步：制作可启动镜像

镜像布局沿用 Fedora 流程里的 Btrfs 子卷结构：

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

挂载镜像、同步 rootfs 并 chroot：

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

在 chroot 中启用服务、生成 initramfs 和 BLS：

```bash
sudo mount --bind /dev /mnt/ego-archlinux/dev
sudo mount --bind /dev/pts /mnt/ego-archlinux/dev/pts
sudo mount -t proc proc /mnt/ego-archlinux/proc
sudo mount -t sysfs sys /mnt/ego-archlinux/sys
sudo mount -t tmpfs tmpfs /mnt/ego-archlinux/run
sudo chroot /mnt/ego-archlinux /bin/bash
```

在 chroot 中执行：

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
help() { echo "将 gaokun3 DSP 固件加入 initramfs"; }
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

如果启用了 EL2，再为第二套内核重复 DTB、initramfs 和 `kernel-install add` 步骤即可。

## 参考

- [Arch Linux ARM](https://archlinuxarm.org/)
- [linux-gaokun-buildbot Fedora 指南](matebook_ego_build_guide_fedora44_zh.md)
- [linux-gaokun-buildbot Ubuntu 指南](matebook_ego_build_guide_ubuntu26.04_zh.md)
