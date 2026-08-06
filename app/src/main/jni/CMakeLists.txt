cmake_minimum_required(VERSION 3.10)
project(vdrm_consumer)

add_library(vdrm_consumer SHARED native_consumer.c)
target_include_directories(vdrm_consumer PRIVATE .)
target_link_libraries(vdrm_consumer log android aaudio EGL GLESv2 dl)

# Root proxy daemon. Must be a PIE executable (a shared lib's entry point is
# not runnable — exec would SIGILL). We disguise it as libvdr2d.so so the
# Gradle packaging puts it into lib/<abi>/, then the app copies it out and
# execs it via su.
add_executable(vdr2d vdr2d.c)
set_target_properties(vdr2d PROPERTIES
    OUTPUT_NAME "libvdr2d"
    PREFIX ""
    SUFFIX ".so")
target_include_directories(vdr2d PRIVATE .)

# AGP only packages jniLibs/ and cmake SHARED targets into the APK
# deterministically; a cmake EXECUTABLE (even disguised as .so) is not
# guaranteed to be picked up. Copy the daemon into jniLibs so the app
# can always extract it (MainActivity.ensureDaemon).
add_custom_command(TARGET vdr2d POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/arm64-v8a
    COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:vdr2d>
            ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/arm64-v8a/libvdr2d.so)