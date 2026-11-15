#!/usr/bin/env bash
# 打 AppImage。
#
#   ./packaging/build-appimage.sh              # 产出 dist/server-toolbox-<版本>-x86_64.AppImage
#   VERSION=1.2.0 ./packaging/build-appimage.sh
#
# 设计要点：
# 1) 只捆绑 Qt 自己的库和插件，系统库（libc/libGL/ICU/X11…）一律不打包。
#    AppImage 的通行做法是依赖目标机已有的系统库；把 glibc 打进去反而会在
#    别的发行版上崩。Qt 是唯一必须自带的，因为版本要求严格（见 QT_VERSION）。
# 2) 不依赖 linuxdeployqt（Debian/Ubuntu 仓库里没有），自己按 ldd 递归收集。
# 3) AppRun 用 exec 而非 source，避免污染调用方环境。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

APP=server-toolbox
VERSION="${VERSION:-1.0.0}"
BUILD_DIR="${BUILD_DIR:-build}"
APPDIR="dist/AppDir"
OUT="dist/${APP}-${VERSION}-x86_64.AppImage"

QT_LIBS="$(qmake6 -query QT_INSTALL_LIBS)"
QT_PLUGINS="$(qmake6 -query QT_INSTALL_PLUGINS)"
QT_BINS="$(qmake6 -query QT_INSTALL_BINS)"

echo "==> Qt $(qmake6 -query QT_VERSION)  libs=$QT_LIBS"

# ── 1. Release 构建 ────────────────────────────────────────
if [ ! -x "$BUILD_DIR/$APP" ]; then
    echo "==> 未找到 $BUILD_DIR/$APP，先做 Release 构建"
    cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD_DIR" -j"$(nproc)"
fi

# ── 2. 铺 AppDir ───────────────────────────────────────────
echo "==> 铺 AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/plugins"

cp "$BUILD_DIR/$APP" "$APPDIR/usr/bin/"
# llama-proxy 是 AI 页要调的本地代理，一起带上
if [ -x proxy-rs/target/release/llama-proxy ]; then
    cp proxy-rs/target/release/llama-proxy "$APPDIR/usr/bin/"
else
    echo "!! 未找到 llama-proxy，AI 代理功能在 AppImage 里会不可用" >&2
fi

cp packaging/$APP.desktop "$APPDIR/$APP.desktop"
cp packaging/$APP.svg     "$APPDIR/$APP.svg"
# AppImage 规范允许根目录放 .DirIcon 作为文件管理器图标
cp packaging/$APP.svg     "$APPDIR/.DirIcon"

# ── 3. 递归收 Qt 依赖 ──────────────────────────────────────
# 系统库白名单：这些留给宿主，不打进 AppImage。
SYSTEM_PREFIXES=(
    "linux-vdso" "ld-linux" "libc.so" "libm.so" "libdl.so" "libpthread.so"
    "librt.so" "libresolv" "libnsl" "libutil.so" "libgcc_s" "libstdc++"
    "libglib" "libgobject" "libgthread" "libgmodule" "libgio" "libglib-2.0"
    "libX" "libxcb" "libXau" "libXdmcp" "libdrm" "libGL" "libEGL" "libGLESv2"
    "libexpat" "libz.so" "libpng" "libfreetype" "libfontconfig" "libdbus-1"
    "libsystemd" "libmount" "libselinux" "libpcre2" "libavahi" "libcap"
    "libaudit" "libblkid" "libuuid" "libwayland" "libxkbcommon" "libinput"
    "libmtdev" "libevdev" "libts" "libdouble-conversion" "libmd4c"
    "libgraphite2" "libharfbuzz" "libsqlite3" "libssl" "libcrypto"
    "libicu" "libb2" "liblzma" "libbz2" "libevent" "libgomp" "libatomic"
    "libasound" "libpulse" "libspeechd" "libatspi" "libatk" "libcairo"
    "libpango" "libgdib" "libjpeg" "libtiff" "libwebp" "libopenjp2"
    "libcups" "libproxy" "libdconf" "libapparmor" "libseccomp"
)

is_system() {
    local f="$1"
    for p in "${SYSTEM_PREFIXES[@]}"; do
        case "$f" in "${p}"*) return 0 ;; esac
    done
    return 1
}

copy_lib() {
    local src="$1"
    [ -e "$src" ] || return 0
    local base; base="$(basename "$src")"
    case "$base" in
        *libc.so*|*libm.so*|*ld-linux*) return 0 ;;
    esac
    is_system "$base" && return 0
    if [ ! -e "$APPDIR/usr/lib/$base" ]; then
        cp -L "$src" "$APPDIR/usr/lib/$base" 2>/dev/null || return 0
        echo "    + $base"
        # 继续递归它自己的依赖
        local dep
        for dep in $(ldd "$APPDIR/usr/lib/$base" 2>/dev/null | awk '{print $3}' | grep '^/'); do
            copy_lib "$dep"
        done
    fi
}

echo "==> 收集 Qt 库"
for dep in $(ldd "$APPDIR/usr/bin/$APP" | awk '{print $3}' | grep '^/'); do
    copy_lib "$dep"
done
# Qt 插件：平台插件缺失会直接起不来，其余按需
echo "==> 收集 Qt 插件"
for p in platforms/platforms imageformats/imageformats styles/styles; do
    d="$QT_PLUGINS/$(dirname "$p")"
    n="$(basename "$p" | cut -d/ -f1)"
    [ -d "$d" ] || continue
    mkdir -p "$APPDIR/usr/plugins/$n"
    for so in "$d"/*.so; do
        [ -e "$so" ] || continue
        # 平台插件只留 xcb / wayland / minimal（minimal 供无显示环境自检用）
        if [ "$n" = "platforms" ]; then
            case "$(basename "$so")" in
                libqxcb.so|libqwayland.so|libqminimal.so|libqoffscreen.so) ;;
                *) continue ;;
            esac
        fi
        cp -L "$so" "$APPDIR/usr/plugins/$n/" 2>/dev/null || true
        echo "    * $n/$(basename "$so")"
    done
done

# ── 4. AppRun ──────────────────────────────────────────────
echo "==> 写 AppRun"
cat > "$APPDIR/AppRun" <<'APPRUN'
#!/usr/bin/env bash
# AppImage 挂载点在 SELF（旧版为 APPDIR）
here="$(dirname "$(readlink -f "$0")")"
export QT_PLUGIN_PATH="$here/usr/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
# 用自带的 Qt 翻译，找不到就退回系统
export QT_TRANSLATION_PATH="$here/usr/translations"
# 不覆盖用户已有的 QT_QPA_PLATFORM（方便 QT_QPA_PLATFORM=offscreen 自检）
export QT_AUTO_SCREEN_SCALE_FACTOR="${QT_AUTO_SCREEN_SCALE_FACTOR:-1}"
exec "$here/usr/bin/server-toolbox" "$@"
APPRUN
chmod +x "$APPDIR/AppRun"

# ── 5. 打包 ──
TOOL="${APPIMAGETOOL:-build/appimagetool}"
if [ ! -x "$TOOL" ]; then
    echo "!! 找不到可执行的 appimagetool: $TOOL" >&2
    echo "   下载: curl -L -o build/appimagetool \\" >&2
    echo "     https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage" >&2
    echo "   然后 chmod +x build/appimagetool" >&2
    exit 1
fi

echo "==> 打包 $OUT"
rm -f "$OUT"
# ARCHFLAGS=x86_64 避免工具去猜机器名
ARCHFLAGS=x86_64 "$TOOL" --appimage-extract-and-run "$APPDIR" "$OUT" 2>&1 | tail -6

# appimagetool 有时按 AppDir 名字命名产物，兜底改名
if [ ! -f "$OUT" ]; then
    found="$(ls -t dist/*.AppImage 2>/dev/null | head -1 || true)"
    [ -n "$found" ] && mv "$found" "$OUT"
fi

# ── 6. 自检 ────────────────────────────────────────────────
# 两条经验，都是踩过才知道的：
#  a) 不能只看 timeout 的退出码。timeout 默认只发 SIGTERM，而 Qt 事件循环会
#     把它处理掉、进程继续跑，所以 124 只代表"没在期限内自己退出"，
#     不代表渲染成功。这里主动跑一次并检查存活。
#  b) 不要指望 grep "could not find or load" 之类来抓缺插件 —— Qt6 的
#     QT_PLUGIN_PATH 是**追加式**的，自带插件缺失时它会静默回退到系统 Qt，
#     日志里什么异常都没有（实测把 usr/plugins/platforms 全删光照样起得来）。
#     所以这里改成直接核对 AppDir 里的文件是否齐全，这才是真检查。
echo "==> 自检"
LOG="dist/smoke.log"

fail() { echo "!! 自检失败: $1" >&2; [ -f "$LOG" ] && sed -n '1,6p' "$LOG" >&2; exit 1; }

# 6a. AppDir 结构完整性（这是 AppImage 能不能自足的关键）
[ -x "$APPDIR/AppRun" ]                              || fail "缺 AppRun"
[ -f "$APPDIR/$APP.desktop" ]                        || fail "缺 .desktop（桌面环境不会认它）"
[ -f "$APPDIR/$APP.svg" ]                            || fail "缺图标"
for lib in libQt6Core.so.6 libQt6Gui.so.6 libQt6Widgets.so.6; do
    [ -f "$APPDIR/usr/lib/$lib" ] || fail "缺 $lib，目标机没装同版本 Qt 会起不来"
done
# 平台插件：Linux 桌面基本只有 xcb / wayland 两种活路，至少得有一个
if [ ! -f "$APPDIR/usr/plugins/platforms/libqxcb.so" ] &&
   [ ! -f "$APPDIR/usr/plugins/platforms/libqwayland.so" ]; then
    fail "缺 xcb/wayland 平台插件（注意：缺了也不会报错，Qt 会静默用系统 Qt）"
fi
echo "    ✅ AppDir 结构完整（AppRun / desktop / 图标 / Qt 库 / 平台插件）"

# 6b. 真跑一次，确认能起来且不立刻退出
export APPIMAGE_EXTRACT_AND_RUN=1   # 无 FUSE 的环境（容器/CI）需要
: > "$LOG"
timeout -s KILL 60 env QT_QPA_PLATFORM=offscreen "$OUT" > "$LOG" 2>&1 &
PID=$!
sleep 12
if kill -0 "$PID" 2>/dev/null; then
    kill -9 "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true
    ALIVE=1
else
    ALIVE=0
fi
[ "$ALIVE" = 1 ] || fail "启动后 12 秒内进程已退出"
# 兄弟控件间误用 mapFrom 的历史坑，见 doc/THEME.md
grep -q 'parent must be in parent hierarchy' "$LOG" \
    && fail "出现 mapFrom 父子链告警"
echo "    ✅ 无头启动 12 秒未崩溃，无 mapFrom 告警"

echo
echo "==> 完成"
ls -lh "$OUT" | awk '{print "    " $5, $9}'
echo "    运行: ./$OUT"
echo "    无 FUSE 的环境（容器/CI）: APPIMAGE_EXTRACT_AND_RUN=1 ./$OUT"
