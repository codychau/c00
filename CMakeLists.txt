cmake_minimum_required(VERSION 3.20)
project(ServerToolbox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

add_executable(server-toolbox
    main.cpp
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

# ── 界面自检脚手架（可选，默认关闭，不影响正常构建）──────────
# cmake -S . -B build -DENABLE_THEME_SHOT=ON
#   board_shot  只渲染主板画板：不跑外部命令，秒级出图
#   app_shot    渲染整个窗口：会触发各页面首轮刷新（含 lspci/smartctl 等），慢一些
# 用法：QT_QPA_PLATFORM=offscreen ./build/board_shot
option(ENABLE_THEME_SHOT "Build UI screenshot helpers" OFF)
if(ENABLE_THEME_SHOT)
    # 截图脚手架的源码在 preview/ 下，而 preview/ 被 .gitignore 排除（本地产物目录）。
    # 所以脚手架只在本地可用；别人克隆后要开启会得到下面这句，
    # 而不是一堆难懂的 "Cannot find source file"。
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/preview/app_shot.cpp")
        message(FATAL_ERROR
            "ENABLE_THEME_SHOT=ON 需要 preview/ 下的截图脚手架源码，"
            "但该目录被 .gitignore 排除、不在版本库中。\n"
            "正常构建本程序不需要它 —— 去掉 -DENABLE_THEME_SHOT=ON 即可。")
    endif()

    add_executable(board_shot
        preview/board_shot.cpp
        theme.cpp                   theme.h
        motherboardwidget.cpp       motherboardwidget.h
    )
    target_link_libraries(board_shot PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(board_shot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(app_shot
        preview/app_shot.cpp
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
    target_link_libraries(app_shot PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
    target_include_directories(app_shot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(page_probe
        preview/page_probe.cpp
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
    target_link_libraries(page_probe PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
    target_include_directories(page_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# Build Rust proxy subproject
add_custom_target(build-proxy-rs
    COMMAND cargo build --release --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/proxy-rs/Cargo.toml
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/proxy-rs
)
