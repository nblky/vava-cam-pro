include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
set(DTLS_BACKEND openssl CACHE STRING "myvava DTLS backend" FORCE)
set(ENABLE_DTLS ON CACHE BOOL "" FORCE)
foreach(feature ENABLE_TCP ENABLE_WS ENABLE_PROXY_CODE ENABLE_OSCORE
        WITH_OBSERVE_PERSIST ENABLE_AF_UNIX ENABLE_Q_BLOCK
        ENABLE_EXAMPLES ENABLE_DOCS ENABLE_TESTS ENABLE_THREAD_SAFE)
  set(${feature} OFF CACHE BOOL "" FORCE)
endforeach()
set(ENABLE_ASYNC ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# Pin the complete upstream source, including its license; do not use a floating branch.
set(myvava_coap_url https://codeload.github.com/obgm/libcoap/tar.gz/851533c3cf63d16984d370ce39d586ecb3694971)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libcoap-4.3.5b.tar.gz")
  set(myvava_coap_url "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libcoap-4.3.5b.tar.gz")
endif()
FetchContent_Declare(myvava_coap
  URL ${myvava_coap_url}
  URL_HASH SHA256=a7df2e2d688f0dd21511cd6351cc97fc7cf083ea1958b125582159967787dd24)
FetchContent_MakeAvailable(myvava_coap)
include("${CMAKE_CURRENT_SOURCE_DIR}/patch-libcoap.cmake")
# Upstream is built with its own warning policy, not this project's -Werror.
target_compile_options(coap-3 PRIVATE -Wno-error)
add_library(myvava-sdk STATIC wire.c client.c event_wire.c event_spool.c file_wire.c sealed_file.c media.c resolver.c)
target_include_directories(myvava-sdk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(myvava-sdk PUBLIC coap-3 Threads::Threads)
target_include_directories(myvava-sdk PRIVATE ${PROJECT_SOURCE_DIR}/shared)
install(FILES THIRD_PARTY_NOTICES.txt OPENSSL-LICENSE.txt DESTINATION share/licenses/myvava-sdk)
install(FILES ${PROJECT_SOURCE_DIR}/third_party/minimp4/minimp4.h ${PROJECT_SOURCE_DIR}/third_party/minimp4/README.md DESTINATION share/licenses/myvava-sdk/minimp4)
