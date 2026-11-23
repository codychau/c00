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

# nasrs 是自研引擎（RAID0 + 软件镜像 RAID1），NAS 页那两条路线全靠它。
#
# 这一步原来**没有**：脚本手工铺 AppDir，只拷了 server-toolbox 和
# llama-proxy，而 CMakeLists 里的 install(PROGRAMS … nasrs …) 根本不会被这条
# 路径触发。后果是 AppImage 里点「nasrs 镜像 RAID1」只会得到一句
# "nasrs 引擎不在程序旁边" —— 功能在包里是死的，而构建日志里一行警告都没有。
# 所以这里默认**缺了就终止**：少一个引擎属于"产出了一个残废包"，
# 不该靠有人恰好看见 stderr 那行字来发现。
if [ -x nasrs/target/release/nasrs ]; then
    cp nasrs/target/release/nasrs "$APPDIR/usr/bin/"
elif [ "${ALLOW_MISSING_NASRS:-0}" = "1" ]; then
    echo "!! 未找到 nasrs，NAS 页两个自研引擎在 AppImage 里都不可用（ALLOW_MISSING_NASRS=1 已确认）" >&2
else
    echo "!! 未找到 nasrs/target/release/nasrs —— 先跑 cargo build --release --manifest-path nasrs/Cargo.toml" >&2
    echo "   确实要打一个不含自研引擎的包，就设 ALLOW_MISSING_NASRS=1 再来" >&2
    exit 1
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

# ── 3b. 内置 emoji 字体 ────────────────────────────────────
# 界面里的 📊💾🖥️📂⚙️ 等图标全靠 emoji 字体，而多数服务器发行版默认不装
# （Ubuntu server 就完全没有，fc-list 里零 emoji 覆盖），图标会渲染成 □。
# AppImage 的意义是换机器也能跑，所以把字体一起带上，不依赖宿主的字体配置。
# EMOJI_FONT=none 可关闭（包能小 9.5 MB）；EMOJI_FONT=/path/to.ttf 可指定。
FONTDIR="$APPDIR/usr/share/fonts/truetype/noto"
FONT_NAME=NotoColorEmoji.ttf
FONT_LIC="$APPDIR/usr/share/doc/noto-color-emoji/copyright"
EMOJI_FONT="${EMOJI_FONT:-auto}"
FONT_OK=0

if [ "$EMOJI_FONT" != "none" ]; then
    if [ -n "${EMOJI_FONT:-}" ] && [ "$EMOJI_FONT" != "auto" ] && [ -f "$EMOJI_FONT" ]; then
        src="$EMOJI_FONT"
    else
        # 按可信度依次找：系统包 → 用户目录 → 本仓库下载缓存
        src=""
        for c in \
            /usr/share/fonts/truetype/noto/$FONT_NAME \
            "$HOME/.local/share/fonts/$FONT_NAME" \
            build/fonts/$FONT_NAME; do
            [ -f "$c" ] && src="$c" && break
        done
        # RPM 系（openEuler / Fedora / RHEL）字体目录布局与 Debian 完全不同：
        # 装在 /usr/share/fonts/google-noto-emoji/ 下。原来只写死 Debian 那条
        # 路径，于是在这类机器上"找不到 emoji 字体"，打出来的包图标全是 □，
        # 而打包机（容器）里明明装着字体 —— 不是缺字体，是找字体的写法只认一家。
        if [ -z "$src" ]; then
            src="$(find /usr/share/fonts -iname "$FONT_NAME" 2>/dev/null | head -1 || true)"
        fi
        # 最后交给 fontconfig 自己回答（别名不只有 NotoColorEmoji 一种写法）
        if [ -z "$src" ] && command -v fc-list >/dev/null 2>&1; then
            src="$(fc-list : file family | grep -iE 'emoji|NotoColorEmoji' \
                   | head -1 | cut -d: -f1 || true)"
        fi
        # 都没有就从 deb 里取（apt 的国内镜像比 GitHub 快得多）
        if [ -z "$src" ] && command -v apt-get >/dev/null 2>&1; then
            echo "    本机无 emoji 字体，尝试用 apt 取 fonts-noto-color-emoji"
            mkdir -p build/fonts
            deb="$(ls build/fonts/fonts-noto-color-emoji_*.deb 2>/dev/null | head -1 || true)"
            if [ -z "$deb" ]; then
                ( cd build/fonts && apt-get download fonts-noto-color-emoji >/dev/null 2>&1 ) || true
                deb="$(ls build/fonts/fonts-noto-color-emoji_*.deb 2>/dev/null | head -1 || true)"
            fi
            if [ -n "$deb" ]; then
                rm -rf build/fonts/x && mkdir -p build/fonts/x
                dpkg-deb -x "$deb" build/fonts/x 2>/dev/null || true
                src="$(find build/fonts/x -name "$FONT_NAME" 2>/dev/null | head -1 || true)"
            fi
        fi
    fi

    if [ -n "$src" ] && [ -f "$src" ]; then
        mkdir -p "$FONTDIR" "$(dirname "$FONT_LIC")"
        cp -L "$src" "$FONTDIR/$FONT_NAME"
        # 让 fontconfig 认这个字体，需要三件事，缺一不可：
        #   1) 一份主配置（AppRun 用 FONTCONFIG_CONFIG 指过去）
        #   2) 配置里用 <dir> 登记字体目录
        #   3) 一个可写的 <cachedir>
        # 踩过的两个坑：
        #  - 只设 FONTCONFIG_PATH 没用 —— 它是"配置文件目录"，不是字体目录。
        #  - <dir> 写绝对路径打包机路径，AppImage 挂到别处就失效；
        #    prefix="relative" 的基准是配置**所在目录的上一级**，所以字体目录
        #    必须是 usr/share/fonts/... 而配置放 usr/share/fontconfig/ 下，
        #    相对路径写成 ../fonts/... 才指得对。
        mkdir -p "$APPDIR/usr/share/fontconfig" "$APPDIR/usr/share/fontconfig/cache"
        # 预热缓存：fontconfig 的 <cachedir> 里一旦有"配置未变"的有效缓存，
        # 就不会重新扫描。这里在打包机上先扫一次，把包内字体写进缓存再打包，
        # 解压后无需 fc-cache 即可命中（打包机与目标机路径不同也没关系，
        # 缓存里记的是 relative 解析后的实际字体文件）。
        cat > "$APPDIR/usr/share/fontconfig/$APP-emoji.conf" <<FCONF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <include ignore_missing="yes">/etc/fonts/fonts.conf</include>
  <dir prefix="relative">../fonts/truetype/noto</dir>
  <cachedir prefix="relative">cache</cachedir>
  <match target="pattern">
    <test qual="any" name="family"><string>emoji</string></test>
    <edit name="family" mode="assign" binding="same"><string>Noto Color Emoji</string></edit>
  </match>
</fontconfig>
FCONF
        # 预热字体缓存（有 fc-cache 才做；没有也不影响，只是首次启动慢一点）
        if command -v fc-cache >/dev/null 2>&1; then
            FONTCONFIG_FILE="$APPDIR/usr/share/fontconfig/$APP-emoji.conf" \
                fc-cache -f "$FONTDIR" >/dev/null 2>&1 \
                && echo "    + 字体缓存已预热" \
                || echo "    （fc-cache 预热未成功，首次启动会现场扫描）"
        fi
        echo "    + 字体 $(basename "$src") ($(du -h "$FONTDIR/$FONT_NAME" | cut -f1))"
        # 许可证文本必须随附字体一起走（OFL 与 Apache-2.0 都这么要求）。
        # 踩过的两个坑，都是"只认 Debian 布局"：
        #  - RPM 系把许可证放在 /usr/share/licenses/<包名>/LICENSE，不是
        #    /usr/share/doc/…/copyright，写死 Debian 路径就永远找不到。
        #  - apt 分支把 deb 解到 build/fonts/x，而这里原来找的是 build/fontpkg —
        #    那个目录从来不创建，于是 Debian 上"字体找到了、许可证没找到"，
        #    自检必然失败（表现是每次打包都报缺许可证，没人看出根因是路径写错）。
        lic=""
        for c in \
            /usr/share/doc/fonts-noto-color-emoji/copyright \
            /usr/share/licenses/google-noto-emoji-fonts/LICENSE \
            /usr/share/licenses/google-noto-emoji-color-fonts/LICENSE; do
            [ -f "$c" ] && lic="$c" && break
        done
        if [ -z "$lic" ]; then
            lic="$(find build/fonts/x /usr/share/licenses -maxdepth 3 \
                   \( -iname 'LICENSE' -o -iname 'copyright' -o -iname 'OFL*' \) \
                   -path '*emoji*' 2>/dev/null | head -1 || true)"
        fi
        if [ -n "$lic" ]; then
            cp "$lic" "$FONT_LIC"
            # 不写死"SIL OFL 1.1"：openEuler 随包的是 Apache-2.0（Noto Color Emoji
            # 早期是 Apache，后来才改 OFL），写死等于把打包机上真实的许可证说成别的
            echo "    + 许可证文本随附 ($(basename "$lic"))"
        else
            echo "!! 未找到字体许可证文本，再分发字体要求随附，自检会失败" >&2
        fi
        FONT_OK=1
    else
        echo "!! 找不到 emoji 字体：AppImage 里的图标在没装字体的机器上会是 □" >&2
        echo "   （要跳过这项检查设 EMOJI_FONT=none；要自带设 EMOJI_FONT=/path/to.ttf）" >&2
    fi
fi

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
# 随包的 emoji 字体：让 fontconfig 改用包内主配置。该配置 include 系统的
# /etc/fonts/fonts.conf，宿主字体照常生效，只是多登记包内这一个字体目录。
# 注意别用 FONTCONFIG_PATH —— 它是"配置文件目录"，指过去而配置里没有 <dir>
# 登记字体目录的话，字体根本不会被发现（这里就绕了这个弯）。
if [ -f "$here/usr/share/fontconfig/server-toolbox-emoji.conf" ]; then
    export FONTCONFIG_CONFIG="$here/usr/share/fontconfig/server-toolbox-emoji.conf"
fi
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

# appimagetool 默认要从 GitHub 现下 type2-runtime，内网/弱网下这一步经常失败
# （实测报 "server returned status code 0"）。runtime 只有 1 MB，提前存一份
# 用 --runtime-file 传进去就不联网了。取法：
#   curl -L -o build/runtime-x86_64 \\
#     https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64
RUNTIME_ARGS=()
if [ -f "${RUNTIME_FILE:-build/runtime-x86_64}" ]; then
    RUNTIME_ARGS=(--runtime-file "${RUNTIME_FILE:-build/runtime-x86_64}")
    echo "    用本地 runtime: ${RUNTIME_FILE:-build/runtime-x86_64}"
else
    echo "    未找到本地 runtime，appimagetool 将尝试联网下载"
fi

# ARCHFLAGS=x86_64 避免工具去猜机器名
ARCHFLAGS=x86_64 "$TOOL" --appimage-extract-and-run "${RUNTIME_ARGS[@]}" \
    "$APPDIR" "$OUT" 2>&1 | tail -6

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
if [ "$EMOJI_FONT" != "none" ]; then
    [ "$FONT_OK" = 1 ]                     || fail "emoji 字体没能打包进去"
    [ -f "$FONT_LIC" ]                     || fail "缺字体许可证文本（再分发字体要求随附）"
fi
# 自研引擎：NAS 页的 RAID0 与软件镜像两条路线全用它。少了它包照样能起来，
# 界面只在选那两条时说一句"引擎不在程序旁边"—— 一个能启动的残废包，
# 靠"启动自检通过"是发现不了的，所以这里点名查。
[ -x "$APPDIR/usr/bin/nasrs" ]           || fail "缺 nasrs 引擎（NAS 页两条自研路线全不可用）"
echo "    ✅ AppDir 结构完整（AppRun / desktop / 图标 / Qt 库 / 平台插件 / emoji 字体 / nasrs）"

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
