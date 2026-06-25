cmake_minimum_required(VERSION 3.20)
project(ServerToolbox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

add_executable(server-toolbox
    main.cpp
    mainwindow.cpp
    mainwindow.h
    statuspage.cpp
    statuspage.h
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
    pcidialog.cpp
    pcidialog.h
    smartdialog.cpp
    smartdialog.h
    formatdialog.cpp
    formatdialog.h
    entertainmentpage.cpp
    entertainmentpage.h
    aipage.cpp
    aipage.h
    servicepage.cpp
    servicepage.h
    aboutpage.cpp
    aboutpage.h
    logger.cpp
    logger.h
    logpage.cpp
    logpage.h
)

target_link_libraries(server-toolbox PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)
