cmake_minimum_required(VERSION 3.10)
project(vdrm_consumer)

add_library(vdrm_consumer SHARED native_consumer.c)
target_include_directories(vdrm_consumer PRIVATE .)
target_link_libraries(vdrm_consumer log android aaudio EGL GLESv2 dl)

# Root proxy daemon: built as a shared lib only so the APK packs it; the app
# copies it to its files dir and execs it via su. Plain libc, no Android deps.
add_library(vdr2d SHARED vdr2d.c)
target_include_directories(vdr2d PRIVATE .)