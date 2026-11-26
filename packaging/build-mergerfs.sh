#!/usr/bin/env bash
# 从源码编 mergerfs，放进 live（程序按「同目录 → ../lib → /usr/local/bin → /usr/bin →
# PATH」找它，见 nascontroller.cpp::mergerfsBinaryPath）。
#
# 为什么要编：mergerfs 在 dnf 系（openEuler / Fedora）的仓库里一个都没有 —— 实测容器里
# `dnf list --available mergerfs` 直接报 No matching Packages，pacman 系也没有，Arch 只在
# AUR。页顶的「一键安装」对那两家永远装不了它，而本程序刻意不代装。所以对 dnf 系唯一
# 能落地的那条就是自己编一个二进制放在程序旁边。
#
# 为什么编得动：mergerfs 的源码自带 vendored libfuse（作者改过的 fork，Makefile 先把它
# 编成 libfuse.a 再**静态**链进 mergerfs），所以**不需要发行版的 fuse3 开发头**，只要
# g++（C++17）、make、strip。容器（openEuler 24.03）这三样都有。
#
# 为什么不抓现成的 deb 解开：Debian 的 mergerfs 二进制要求 GLIBCXX_3.4.32 / CXXABI_1.3.15，
# 本容器里的 libstdc++（gcc 12.3.1）没有那两个版本 —— 解开照样跑不起来。在**跑它的
# 那个镜像里编**，编译器与运行时同源，这条比搬运二进制可靠。宿主是 openEuler 26.09，
# 在宿主上编再放进容器会撞 glibc 版本，所以这条脚本要在容器里跑。
#
# 用法（宿主上）：
#   podman run --rm --security-opt label=disable -v ~/c00-app:/work \
#       --entrypoint bash localhost/c00-desktop:latest bash /work/src/packaging/build-mergerfs.sh
set -euo pipefail

# 版本取 Debian pool 里最新的那个 .orig.tar.gz。写死版本号是最烂的做法：
# 下一次发行版升级后这个脚本会编一个过期的东西，而"编哪个版本"这件事用户看不见。
POOL="http://deb.debian.org/debian/pool/main/m/mergerfs/"
WORK=$(mktemp -d /tmp/mergerfs-build.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

NEWEST=$(curl -sSL "$POOL" | grep -o 'mergerfs_[0-9.]*\.orig\.tar\.gz' | sort -V | tail -1)
[ -n "$NEWEST" ] || { echo "认不出 pool 里最新的源码包，脚本要改"; exit 1; }
VER=${NEWEST#mergerfs_}; VER=${VER%.orig.tar.gz}
echo "== 源码：$NEWEST（$VER）=="

curl -sSL -o "$WORK/$NEWEST" "$POOL/$NEWEST"
tar xzf "$WORK/$NEWEST" -C "$WORK"
cd "$WORK/mergerfs-$VER"

# buildtools/update-version 会先试 git describe（镜像里没有 git），失败后回落到 VERSION
# 文件 —— 这条链没有网络依赖，也不会把一个 "unknown" 版本编进产物。
./buildtools/update-version

# 默认 CXXFLAGS 是 -O0 -g（作者的调试档），24 MB；strip + -O2 之后约 1.2 MB。
make clean > /dev/null
make -j"$(nproc)" CXXFLAGS="-O2 -pipe -fstack-protector-strong -std=c++17 -MMD -MP" > "$WORK/make.log" 2>&1 \
    || { echo "编译失败"; tail -25 "$WORK/make.log"; exit 1; }
strip build/mergerfs

# 自检：不是"文件存在"，是"跑得起来且版本对得上"。
V=$(./build/mergerfs --version | head -1)
case "$V" in
    "mergerfs v$VER") ;;
    *) echo "自检失败：--version 说「$V」，期望 v$VER"; exit 1 ;;
esac

# 放进 live（程序同目录，程序按那条搜索顺序第一项就找到它）与 engine/（镜像重建那条）。
# live 在容器里是只读挂载（/c00-live），所以这里走宿主路径 /work/live。
for d in /work/live /work/engine; do
    [ -d "$d" ] || continue
    install -m 755 build/mergerfs "$d/.mergerfs.new"
    mv -f "$d/.mergerfs.new" "$d/mergerfs"
    echo "== 已放 $d/mergerfs =="
done
ls -l /work/live/mergerfs
ldd /work/live/mergerfs
