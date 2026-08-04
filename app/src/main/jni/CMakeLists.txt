cmake_minimum_required(VERSION 3.10)
project(vdrm_consumer)

add_library(vdrm_consumer SHARED native_consumer.c)
target_include_directories(vdrm_consumer PRIVATE .)
target_link_libraries(vdrm_consumer log android aaudio EGL GLESv2 dl)

add_executable(vdr2_helper vdr2_helper.c)
target_link_options(vdr2_helper PRIVATE -static)
target_link_libraries(vdr2_helper c)
set_target_properties(vdr2_helper PROPERTIES
    OUTPUT_NAME "libvdr2_helper.so"
)