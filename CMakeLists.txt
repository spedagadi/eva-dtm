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
find_package(SDL2_image REQUIRED)
find_package(glfw3 REQUIRED)
find_package(Vulkan REQUIRED)
find_package(cairo REQUIRED)

set(AVERMEDIA_INLCUDES
        "${CMAKE_SOURCE_DIR}/3rdparty/avermedia/include")
set(AVERMEDIA_LIBS
        "${CMAKE_SOURCE_DIR}/3rdparty/avermedia/lib/AVerCapAPI.lib")

if (WIN32)
    add_executable(eva-dtm src/main.c src/window.c
            src/window_glfw.c
            src/avermedia/AVerCapAPI_Pro.c
            src/avermedia/avermedia_capture.c)
else ()
    add_executable(eva-dtm src/main.c src/window.c
            src/window_glfw.c)
endif ()

target_include_directories(eva-dtm PUBLIC
        "${CMAKE_SOURCE_DIR}/include/"
        "${CMAKE_SOURCE_DIR}/avermedia/"
        ${AVERMEDIA_INLCUDES}
        ${LIBPLACEBO_INCLUDE_DIRS}
        ${SDL2_INCLUDE_DIRS}
        ${SDL2_IMAGE_INCLUDE_DIRS}
        ${CAIRO_INCLUDE_DIRS})

if (WIN32)
    target_link_libraries(eva-dtm PUBLIC
            ${LIBPLACEBO_LIBRARIES}
            ${SDL2_LIBRARIES}
            SDL2_image
            #${SDL2_IMAGE_LIBRARY}
            glfw
            ${AVERMEDIA_LIBS}
            PkgConfig::LIBAV
            cairo)
else ()
    target_link_libraries(eva-dtm PUBLIC
            ${LIBPLACEBO_LIBRARIES}
            ${SDL2_LIBRARIES}
            SDL2_image
            glfw
            PkgConfig::LIBAV
            cairo)
endif ()
