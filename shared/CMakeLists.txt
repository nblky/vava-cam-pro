add_library(vava_protocol STATIC protocol.c registry.c policy.c h264.c)
target_include_directories(vava_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
