cmake_minimum_required(VERSION 3.19)
project(qt_hama_gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_PREFIX_PATH "C:/Qt/6.10.1/msvc2022_64")
find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(qt_hama_gui main.cpp)
target_link_libraries(qt_hama_gui PRIVATE Qt6::Widgets dcamapi)
target_sources(qt_hama_gui PRIVATE
    dcam_controller.cpp
    frame_grabber.cpp
)

target_include_directories(qt_hama_gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Hamamatsu_DCAMSDK4_v25056964/dcamsdk4/inc
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_directories(qt_hama_gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Hamamatsu_DCAMSDK4_v25056964/dcamsdk4/lib/win64
)
