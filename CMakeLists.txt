cmake_minimum_required(VERSION 3.20)
project(ServerToolbox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

# 共享源文件清单（不含各自的 main 文件）。
# 三个可执行目标 —— server-toolbox、app_shot、page_probe —— 需要同样的页面集合；
# 之前各写一份，加一页就得改三处，漏一处就是链接期 undefined reference。
# 现在只在 APP_SOURCES 里声明一次，新页面漏加的可能性归零。
set(APP_SOURCES
    theme.cpp
    theme.h
    glasspanel.cpp
    glasspanel.h
    mainwindow.cpp
    mainwindow.h
    statuspage.cpp
    statuspage.h
    motherboardwidget.cpp
    motherboardwidget.h
    scenepage.cpp
    scenepage.h
    scenedialog.cpp
    scenedialog.h
    sceneconfig.cpp
    sceneconfig.h
    storagepage.cpp
    storagepage.h
    storagedevicepage.cpp
    storagedevicepage.h
    mountpage.cpp
    mountpage.h
    naspage.cpp
    naspage.h
    nasboardwidget.cpp
    nasboardwidget.h
    nasmodel.cpp
    nasmodel.h
    nascontroller.cpp
    nascontroller.h
    vmpage.cpp
    vmpage.h
    vmconfig.cpp
    vmconfig.h
    vmdialog.cpp
    vmdialog.h
    qemuargs.cpp
    qemuargs.h
    virtualbox.cpp
    virtualbox.h
    pcidialog.cpp
    pcidialog.h
    smartdialog.cpp
    smartdialog.h
    formatdialog.cpp
    formatdialog.h
    entertainmentpage.cpp
    entertainmentpage.h
    desktoppage.cpp
    desktoppage.h
    keybindingconfig.cpp
    keybindingconfig.h
    aipage.cpp
    aipage.h
    servicepage.cpp
    servicepage.h
    serviceeditdialog.cpp
    serviceeditdialog.h
    aboutpage.cpp
    aboutpage.h
    logger.cpp
    logger.h
    logpage.cpp
    logpage.h
    shutdowndialog.cpp
    shutdowndialog.h
    healthmonitor.cpp
    healthmonitor.h
    adminhelper.cpp
    adminhelper.h
    watchdoginstalldialog.cpp
    watchdoginstalldialog.h
)

add_executable(server-toolbox
    main.cpp
    ${APP_SOURCES}
)

target_link_libraries(server-toolbox PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)

# ── 安装 ────────────────────────────────────────────────────
# 用户级安装，无需 root：cmake --install build  →  ~/.local/bin/server-toolbox
# 默认前缀选 ~/.local 而非 /usr/local，符合"家用服务器上随手装一个"的定位；
# 想装到系统目录就显式传 -DCMAKE_INSTALL_PREFIX=/usr/local。
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Install prefix" FORCE)
endif()
install(TARGETS server-toolbox RUNTIME DESTINATION bin)
# 引擎跟着程序走: ~/.local/bin 里的 nasrs 会被 PATH 兜底找到
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/nasrs/target/release/nasrs)
    install(PROGRAMS nasrs/target/release/nasrs DESTINATION bin)
endif()

# 界面自检脚手架（preview/ 下的探针与截图工具）已整体移除。
# 原本它挂在 -DENABLE_THEME_SHOT=ON 上，源码在 preview/ 里、被 .gitignore 排除，
# 所以克隆的人拿到的是"开关在、源码没有"。现在删干净：构建本程序不需要它，
# 少一个会让人 FATAL_ERROR 的开关，也比留一堆"只有作者机器上能跑"的源码好。
# 界面改动由使用者在真实桌面上验收。

# Build Rust proxy subproject
add_custom_target(build-proxy-rs
    COMMAND cargo build --release --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/proxy-rs/Cargo.toml
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/proxy-rs
)

# 自研 RAID0 引擎（nasrs）。与 proxy-rs 同样的做法：CMake 只做转发，
# 真正的构建交给 cargo，避免把 Rust 依赖图复制进 CMake。
add_custom_target(build-nasrs
    COMMAND cargo build --release --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/nasrs/Cargo.toml
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/nasrs
    # 程序在「可执行文件同目录」找 nasrs (nascontroller.cpp::nasrsBinaryPath),
    # cargo 的产物在 target/release 下, 所以链一份过来 —— 不链就会「缺少引擎」。
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        ${CMAKE_CURRENT_SOURCE_DIR}/nasrs/target/release/nasrs ${CMAKE_BINARY_DIR}/nasrs
    DEPENDS nasrs/Cargo.toml nasrs/src/main.rs nasrs/src/raid.rs nasrs/src/nbd.rs nasrs/src/lib.rs
          nasrs/src/mirror.rs nasrs/src/sync.rs
)

# 条带映射与 NBD 编解码是纯函数，单测不需要 root、不需要设备、
# 不需要网络 —— 这是本功能唯一能在开发机上真跑起来的验证。
add_custom_target(test-nasrs
    COMMAND cargo test --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/nasrs/Cargo.toml
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/nasrs
)

