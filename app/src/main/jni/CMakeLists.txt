cmake_minimum_required(VERSION 3.10)
project(vdrm_consumer)

add_library(vdrm_consumer SHARED native_consumer.c)
target_include_directories(vdrm_consumer PRIVATE .)
target_link_libraries(vdrm_consumer log android aaudio EGL GLESv2 dl)

add_library(vdr2_helper SHARED vdr2_helper.c)
target_compile_options(vdr2_helper PRIVATE -fPIE)
target_link_options(vdr2_helper PRIVATE -Wl,-e,main -Wl,-pie)
target_link_libraries(vdr2_helper c)