cmake_minimum_required(VERSION 3.21)
project(ESPWatchUpperComputer VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Widgets Network SerialPort)

qt_add_executable(ESPWatchUpperComputer
    main.cpp
    mainwindow.cpp
    mainwindow.h
)

target_link_libraries(ESPWatchUpperComputer PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::SerialPort
)

set_target_properties(ESPWatchUpperComputer PROPERTIES
    WIN32_EXECUTABLE TRUE
)
