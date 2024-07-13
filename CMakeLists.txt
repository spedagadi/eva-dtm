cmake_minimum_required(VERSION 3.0.0)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
message("CMAKE_MODULE_PATH = ${CMAKE_MODULE_PATH} ")
project(eva-dtm VERSION 0.1.0 LANGUAGES C CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
        libavdevice
        libavfilter
        libavformat
        libavcodec
        libswresample
        libswscale
        libavutil
)
pkg_check_modules(LIBPLACEBO REQUIRED IMPORTED_TARGET
    libplacebo)
set(CMAKE_FIND_LIBRARY_SUFFIXES .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
find_package(SDL2 REQUIRED)
find_package(SDL2_image REQUIRED SDL2_image>=2.0.0)
find_package(glfw3 REQUIRED)
find_package(Vulkan REQUIRED)

add_executable(eva-dtm src/main.c src/window.c
        src/window_glfw.c)
target_include_directories(eva-dtm PUBLIC
        "${CMAKE_SOURCE_DIR}/include/"
        ${LIBPLACEBO_INCLUDE_DIRS}
        ${SDL2_INCLUDE_DIRS}
        ${SDL2_image_INCLUDE_DIRS})

target_link_libraries(eva-dtm PUBLIC
        ${LIBPLACEBO_LIBRARIES}
        ${SDL2_LIBRARIES}
        ${SSDL2_image_LIBRARIES}
        glfw
        PkgConfig::LIBAV)