# Huawei MateBook E Go 2023 - Fedora 44 Beta 构建指南 (GRUB + GNOME)

> **目标机型**：Huawei MateBook E Go 2023（SC8280XP / gaokun3）  
> **宿主机环境**：基于 RPM/DNF 的发行版（如 Fedora），需要有 `dnf`、`parted`。若宿主机是 **arm64**（例如 arm64 WSL2 Fedora），可直接原生构建；若宿主机是 x86_64，则需要 `qemu-user-static` 与 `cross-gcc-aarch64`（或其他 aarch64 交叉工具链）。若宿主机为 Ubuntu，可通过安装 `dnf` 然后使用 `--installroot` 实现跨发行版构建。  
> **发行版**：**Fedora 44 Beta** (Rawhide 或当期发行版)  

**WSL2 需切换内核 https://github.com/Nevuly/WSL2-Linux-Kernel-Rolling/releases 以更好的支持 vfat, btrfs 等文件系统**

---

## 第一步：目录结构准备


安装必要工具（对于 Fedora arm64 宿主机）：

```bash
sudo dnf install gcc make bison flex bc openssl-devel elfutils-libelf-devel ncurses-devel dwarves git parted dosfstools btrfs-progs curl python3
```

建立工作目录：

```bash
mkdir -p ~/gaokun/matebook-build-fedora

cd ~/gaokun
# 拉取指定的 Linux 主线源码 (指定 tag 为 v7.0-rc4)
if [ ! -d "mainline-linux" ]; then
    git clone --depth 1 --branch v7.0-rc4 https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git mainline-linux
fi
```

设置环境变量：
```bash
export GAOKUN_DIR=~/gaokun
export WORKDIR=$GAOKUN_DIR/matebook-build-fedora
export KERN_SRC=$GAOKUN_DIR/mainline-linux
export KERN_OUT=$GAOKUN_DIR/kernel-out
export FW_REPO=$GAOKUN_DIR/firmware-huawei-gaokun3_minimal
export ROOTFS_DIR=$WORKDIR/rootfs
export IMAGE_FILE=$WORKDIR/fedora-44-gaokun3.img
```

---

## 第二步：内核与树内驱动编译

```bash
cd $KERN_SRC

# 应用全套补丁 (涵盖屏幕驱动代码和硬件基础支持)、defconfig 和设备树注入
git am $GAOKUN_DIR/gaokun-patches/*.patch

# 编译阶段
mkdir -p $KERN_OUT

# arm64 宿主机（含 arm64 WSL2）可直接原生构建，无需 CROSS_COMPILE
make O=$KERN_OUT ARCH=arm64 olddefconfig
make O=$KERN_OUT ARCH=arm64 -j$(nproc)

KREL=$(cat $KERN_OUT/include/config/kernel.release)
echo $KREL
```

---

## 第三步：构建基础文件系统 (Fedora GNOME Desktop)

通过 `dnf --installroot` 安装完整 Fedora GNOME 桌面。使用 `@gnome-desktop` 组拉入标准桌面环境（含 gdm、gnome-shell、nautilus、ptyxis、gnome-control-center、gnome-software、gnome-system-monitor、gnome-disk-utility、gnome-backgrounds、mesa-dri-drivers、NetworkManager 等），再补充引导工具、中文支持和常用命令行工具。

> 注：若宿主机是 Ubuntu 等非 Fedora 系统，先 `sudo apt install dnf systemd-container`。跨发行版构建有时由于 key 缺失稍微复杂，建议宿主机也使用 Fedora 容器或本系虚拟机。

```bash
mkdir -p $ROOTFS_DIR

sudo dnf --installroot=$ROOTFS_DIR --releasever=44 --forcearch=aarch64 --use-host-config -y \
    --exclude=gnome-boxes,gnome-connections,snapshot,gnome-weather,showtime,decibels,gnome-contacts,gnome-maps,simple-scan,gnome-clocks,yelp,gnome-user-docs,gnome-calculator,gnome-calendar \
    install \
    @gnome-desktop \
    passwd dnf sudo udev \
    bluez dracut dracut-network btrfs-progs \
    grub2-efi-aa64 grub2-efi-aa64-modules efibootmgr shim-aa64 \
    wpa_supplicant iw wireless-regdb \
    gnome-tweaks flatpak google-noto-sans-cjk-fonts adwaita-mono-fonts \
    fcitx5-chinese-addons fcitx5-configtool \
    parted iproute net-tools traceroute ncat telnet iputils which \
    f44-backgrounds-gnome mpv vim git openssh-server curl htop fastfetch screen nano firefox
```

### 安装内核与模块

```bash
cd $KERN_SRC

# 验证内核版本
KREL=$(cat $KERN_OUT/include/config/kernel.release)
echo $KREL

sudo make O=$KERN_OUT ARCH=arm64 \
    INSTALL_MOD_PATH=$ROOTFS_DIR modules_install

sudo rm -f $ROOTFS_DIR/lib/modules/$KREL/{build,source}

# 安装 kernel
sudo cp $KERN_OUT/arch/arm64/boot/Image \
    $ROOTFS_DIR/boot/vmlinuz-$KREL

# 创建 dtb 目录结构（GRUB 需要）
sudo mkdir -p $ROOTFS_DIR/boot/dtb-$KREL/qcom

# 安装 dtb
sudo cp $KERN_OUT/arch/arm64/boot/dts/qcom/sc8280xp-huawei-gaokun3.dtb \
    $ROOTFS_DIR/boot/dtb-$KREL/qcom/
```

安装设备专属固件：
```bash
# 使用项目内置的最小固件集覆盖到 RootFS
sudo cp -r $FW_REPO/* $ROOTFS_DIR/lib/firmware/
```

安装特定工具：
```bash
sudo mkdir -p $ROOTFS_DIR/usr/local/bin
sudo mkdir -p $ROOTFS_DIR/etc/systemd/system
sudo mkdir -p $ROOTFS_DIR/usr/share/alsa/ucm2/Qualcomm/sc8280xp

sudo cp $GAOKUN_DIR/tools/touchpad/huawei-tp-activate.py $ROOTFS_DIR/usr/local/bin/
sudo cp $GAOKUN_DIR/tools/touchpad/huawei-touchpad.service $ROOTFS_DIR/etc/systemd/system/
sudo chmod +x $ROOTFS_DIR/usr/local/bin/huawei-tp-activate.py

sudo cp $GAOKUN_DIR/tools/bluetooth/patch-nvm-bdaddr.py $ROOTFS_DIR/usr/local/bin/
sudo chmod +x $ROOTFS_DIR/usr/local/bin/patch-nvm-bdaddr.py

# Audio UCM: 华为机型复用 Lenovo X13s 的 UCM，并靠这份匹配文件选中正确配置
sudo cp $GAOKUN_DIR/tools/audio/sc8280xp.conf \
    $ROOTFS_DIR/usr/share/alsa/ucm2/Qualcomm/sc8280xp/
```

---

## 第四步：打包到可启动磁盘镜像并配置 GRUB

### 1. 镜像与分区挂载（Fedora 风格 Btrfs 子卷）

```bash
cd $WORKDIR
truncate -s 12G $IMAGE_FILE

parted -s $IMAGE_FILE mklabel gpt
parted -s $IMAGE_FILE mkpart EFI fat32 1MiB 256MiB
parted -s $IMAGE_FILE set 1 esp on
parted -s $IMAGE_FILE mkpart rootfs btrfs 256MiB 100%

LOOP=$(sudo losetup --show -fP $IMAGE_FILE)
sudo mkfs.vfat -F32 -n EFI ${LOOP}p1
sudo mkfs.btrfs -f -L rootfs ${LOOP}p2

EFI_UUID=$(sudo blkid -s UUID -o value ${LOOP}p1)
ROOT_UUID=$(sudo blkid -s UUID -o value ${LOOP}p2)

sudo mkdir -p /mnt/ego-fedora

# 创建 Fedora 常见子卷布局：@（根）与 @home（家目录）
sudo mount ${LOOP}p2 /mnt/ego-fedora
sudo btrfs subvolume create /mnt/ego-fedora/@
sudo btrfs subvolume create /mnt/ego-fedora/@home
sudo umount /mnt/ego-fedora

# 挂载子卷
sudo mount -o subvol=@ ${LOOP}p2 /mnt/ego-fedora
sudo mkdir -p /mnt/ego-fedora/home
sudo mount -o subvol=@home ${LOOP}p2 /mnt/ego-fedora/home
sudo mkdir -p /mnt/ego-fedora/boot/efi
sudo mount ${LOOP}p1 /mnt/ego-fedora/boot/efi
```

### 2. 同步数据及 fstab

```bash
sudo rsync -aHAX --info=progress2 $ROOTFS_DIR/ /mnt/ego-fedora/

sudo tee /mnt/ego-fedora/etc/fstab > /dev/null <<EOF
UUID=${ROOT_UUID}  /         btrfs  subvol=@,compress=zstd:1,ssd,noatime  0  0
UUID=${ROOT_UUID}  /home     btrfs  subvol=@home,compress=zstd:1,ssd,noatime  0  0
UUID=${EFI_UUID}   /boot/efi vfat   defaults,nofail,x-systemd.device-timeout=10s  0  2
EOF
```

### 3. chroot 初始化，生成 Dracut 与 GRUB (Dracut 替代 Initramfs)

设置并切入虚拟环境（通过 `systemd-nspawn` 或者 `mount --bind` + `chroot`。这里使用通用 `chroot`，前提是确保 qemu-user-static 与环境就绪）：

```bash
# 建议用 trap 统一清理，避免中途失败后残留挂载
cleanup_mounts() {
    sudo umount /mnt/ego-fedora/dev/pts 2>/dev/null || true
    sudo umount /mnt/ego-fedora/boot/efi 2>/dev/null || true
    sudo umount /mnt/ego-fedora/home 2>/dev/null || true
    sudo umount /mnt/ego-fedora/dev 2>/dev/null || true
    sudo umount /mnt/ego-fedora/proc 2>/dev/null || true
    sudo umount /mnt/ego-fedora/sys 2>/dev/null || true
    sudo umount /mnt/ego-fedora/run 2>/dev/null || true
    sudo umount /mnt/ego-fedora 2>/dev/null || true
}
trap cleanup_mounts EXIT

sudo mount --bind /dev /mnt/ego-fedora/dev
sudo mount --bind /dev/pts /mnt/ego-fedora/dev/pts
sudo mount -t proc proc /mnt/ego-fedora/proc
sudo mount -t sysfs sys /mnt/ego-fedora/sys
sudo mount -t tmpfs tmpfs /mnt/ego-fedora/run

sudo chroot /mnt/ego-fedora /bin/bash
```

**(在 chroot 环境中执行):**

```bash
KREL="$(ls /lib/modules/ | head -n1)"

# 简单用户信息与主机名
echo "fedora" > /etc/hostname
useradd -m -s /bin/bash -G wheel user
echo "user:user" | chpasswd
mkdir -p /etc/sudoers.d
echo "%wheel ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/wheel-nopasswd
chmod 440 /etc/sudoers.d/wheel-nopasswd

# 开启服务
systemctl enable gdm NetworkManager sshd huawei-touchpad.service

# 内核模块自动加载 (Dracut 与运行时皆需要)
mkdir -p /etc/modules-load.d
echo -e "pci-pwrctrl-pwrseq\nath11k_pci" > /etc/modules-load.d/wifi.conf
echo "btqca" > /etc/modules-load.d/bluetooth.conf
echo -e "panel-himax-hx83121a\nmsm\nhid_multitouch" > /etc/modules-load.d/display.conf
echo -e "lpasscc_sc8280xp\nsnd-soc-sc8280xp" > /etc/modules-load.d/audio.conf
echo -e "huawei-gaokun-ec\nhuawei-gaokun-battery\nucsi_huawei_gaokun" > /etc/modules-load.d/battery.conf

mkdir -p /etc/modprobe.d
echo "softdep pinctrl_sc8280xp_lpass_lpi pre: lpasscc_sc8280xp" > /etc/modprobe.d/audio-deps.conf

# 生成 initramfs: Fedora 默认使用 dracut
cat > /etc/dracut.conf.d/matebook.conf <<MODEOF
hostonly="no"
add_drivers+=" btrfs nvme phy-qcom-qmp-pcie phy-qcom-qmp-combo phy-qcom-qmp-usb phy-qcom-snps-femto-v2 usb-storage uas typec pci-pwrctrl-pwrseq ath11k ath11k_pci panel-himax-hx83121a msm i2c-hid-of lpasscc_sc8280xp snd-soc-sc8280xp pinctrl_sc8280xp_lpass_lpi "
MODEOF

dracut --force --kver $KREL

# --- 设置 GRUB：关闭 BLS，使用传统 grub.cfg 菜单 ---
ROOT_UUID="$(blkid -s UUID -o value /dev/disk/by-label/rootfs)"

cat > /etc/default/grub <<GRUBEOF
GRUB_DEFAULT=saved
GRUB_TIMEOUT=5
GRUB_DISTRIBUTOR="Fedora"
GRUB_ENABLE_BLSCFG=false
GRUB_CMDLINE_LINUX="clk_ignore_unused pd_ignore_unused arm64.nopauth iommu.passthrough=0 iommu.strict=0 pcie_aspm.policy=powersupersave modprobe.blacklist=simpledrm efi=noruntime fbcon=rotate:1 usbhid.quirks=0x12d1:0x10b8:0x20000000 consoleblank=0 loglevel=4"
GRUB_DEFAULT_DTB="qcom/sc8280xp-huawei-gaokun3.dtb"
GRUBEOF

# 生成 GRUB 主配置（传统菜单模式）
grub2-install --target=arm64-efi --efi-directory=/boot/efi --boot-directory=/boot --removable
grub2-mkconfig -o /boot/grub2/grub.cfg

# 关键修复：给 EFI 分区写入桥接 grub.cfg，强制按 rootfs UUID 找到真正菜单。
# 对于 Btrfs + @ 子卷布局，真实路径通常在 /@/boot/grub2/grub.cfg。
# 否则在部分设备上会出现“GRUB 无菜单，需要手动 configfile (hd0,gpt2)/@/boot/grub2/grub.cfg”。
mkdir -p /boot/efi/EFI/BOOT /boot/efi/EFI/fedora
cat > /boot/efi/EFI/BOOT/grub.cfg <<EOF
search --no-floppy --fs-uuid --set=root ${ROOT_UUID}
if [ -f (\$root)/@/boot/grub2/grub.cfg ]; then
    set prefix=(\$root)/@/boot/grub2
    configfile (\$root)/@/boot/grub2/grub.cfg
elif [ -f (\$root)/boot/grub2/grub.cfg ]; then
    set prefix=(\$root)/boot/grub2
    configfile (\$root)/boot/grub2/grub.cfg
else
    echo "ERROR: grub.cfg not found on rootfs UUID ${ROOT_UUID}"
    echo "Tried: /@/boot/grub2/grub.cfg and /boot/grub2/grub.cfg"
    sleep 5
fi
EOF
cp /boot/efi/EFI/BOOT/grub.cfg /boot/efi/EFI/fedora/grub.cfg

# 可选：验证 grub.cfg 已写入 devicetree
grep -n "devicetree" /boot/grub2/grub.cfg
echo "EFI bridge grub.cfg:" && cat /boot/efi/EFI/BOOT/grub.cfg

# 若仍卡启动日志，可临时追加调试参数后重建 grub.cfg：
# sed -i 's/loglevel=4/loglevel=7 ignore_loglevel rd.debug rd.shell=0/' /etc/default/grub
# grub2-mkconfig -o /boot/grub2/grub.cfg

exit
```

**(回到宿主机)**

```bash
trap - EXIT
cleanup_mounts
```

### 4. 卸载镜像
```bash
sudo losetup -d $LOOP
```

---

## 第五步：刷入设备

镜像准备就绪于 `$WORKDIR/fedora-44-gaokun3.img`。

```bash
sudo dd if=$IMAGE_FILE of=/dev/sdX bs=4M status=progress conv=fsync
```
或使用 `balenaEtcher`, `Rufus`, `gnome-disks` 等图形工具刷入。

**可选：启动后通过扩容工具（如 `gnome-disks`）将剩余空间扩展到根分区，或在终端使用 `btrfs` 命令手动扩容：**
