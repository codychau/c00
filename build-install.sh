#!/usr/bin/env bash
# ServerToolbox 编译安装脚本
# 用法:
#   sudo ./build-install.sh          # 推荐：自动安装系统依赖并编译安装到当前用户
#   ./build-install.sh               # 依赖已装好的情况下直接编译安装
#
# 安装目标: ~/.local/bin/server-toolbox 和 ~/.local/bin/llama-proxy

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# 确定目标用户（支持用 sudo 运行，仍安装到调用者自己的家目录）
if [[ -n "${SUDO_USER:-}" && "${SUDO_USER:-}" != "root" ]]; then
    TARGET_USER="$SUDO_USER"
else
    TARGET_USER="$(id -un)"
fi

TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
BIN_DIR="$TARGET_HOME/.local/bin"
PREFIX="$TARGET_HOME/.local"

echo "==> 目标用户: $TARGET_USER"
echo "==> 安装目录: $BIN_DIR"

# 需要系统安装的开发依赖
DEPS=(qt6-base-dev cmake g++ libtomlplusplus-dev cargo rustc)

if command -v apt-get >/dev/null 2>&1; then
    if [[ "$(id -u)" -eq 0 ]]; then
        echo "==> 安装系统依赖 (apt-get)..."
        apt-get update
        apt-get install -y --no-install-recommends "${DEPS[@]}"
    else
        echo "==> 尝试用 sudo 安装系统依赖 (apt-get)..."
        sudo env DEBIAN_FRONTEND=noninteractive apt-get update
        sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${DEPS[@]}"
    fi
else
    echo "!! 未检测到 apt-get，请自行安装系统依赖: ${DEPS[*]}" >&2
fi

# 编译 C++ 主程序
# 若 build/ 缓存记录的源码路径与当前源码路径不一致，说明源码被移动/复制过，需清掉重建
CACHE_SRC="$(grep -m1 'CMAKE_HOME_DIRECTORY:INTERNAL=' build/CMakeCache.txt 2>/dev/null | cut -d= -f2 || true)"
CUR_SRC="$(cd "$SCRIPT_DIR" && pwd)"
if [[ -n "$CACHE_SRC" && "$CACHE_SRC" != "$CUR_SRC" ]]; then
    echo "==> 检测到 build/ 缓存源码路径不匹配 ($CACHE_SRC)"
    echo "    -> 删除旧 build/ 后重新配置"
    rm -rf build
fi

echo "==> 配置 CMake ..."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX"
echo "==> 编译主程序 ..."
cmake --build build -j"$(nproc)"

# 编译 Rust 代理子项目
echo "==> 编译 Rust 代理 (llama-proxy) ..."
cargo build --release --manifest-path proxy-rs/Cargo.toml

# 安装（如果以 root 运行，则显式切换到目标用户执行，保证归属正确）
echo "==> 安装到 $BIN_DIR ..."
if [[ "$(id -u)" -eq 0 ]]; then
    install -d -o "$TARGET_USER" -g "$TARGET_USER" "$BIN_DIR"
    install -m 0755 -o "$TARGET_USER" -g "$TARGET_USER" \
        build/server-toolbox "$BIN_DIR/server-toolbox"
    install -m 0755 -o "$TARGET_USER" -g "$TARGET_USER" \
        proxy-rs/target/release/llama-proxy "$BIN_DIR/llama-proxy"
else
    mkdir -p "$BIN_DIR"
    install -m 0755 build/server-toolbox "$BIN_DIR/server-toolbox"
    install -m 0755 proxy-rs/target/release/llama-proxy "$BIN_DIR/llama-proxy"
fi

echo
echo "==> 安装完成！"
ls -l "$BIN_DIR/server-toolbox" "$BIN_DIR/llama-proxy"
echo
echo "PATH 中未包含 $BIN_DIR 的话，可执行:"
echo "    export PATH=\"$BIN_DIR:\$PATH\""
echo "然后运行: server-toolbox"