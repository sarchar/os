
# define the target build
ADD_LIBRARY(net STATIC arp.c icmp.c ipv4.c net.c)

TARGET_INCLUDE_DIRECTORIES(net PRIVATE "${CMAKE_SOURCE_DIR}/src")
TARGET_INCLUDE_DIRECTORIES(net PRIVATE "${CMAKE_SOURCE_DIR}/src/uthash_impl")
TARGET_INCLUDE_DIRECTORIES(net PRIVATE "${CMAKE_SOURCE_DIR}/src/kernel")
TARGET_INCLUDE_DIRECTORIES(net PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../pdclib/include" "${CMAKE_CURRENT_SOURCE_DIR}/../pdclib/include/pdclib" "${CMAKE_CURRENT_SOURCE_DIR}/../pdclib_impl/include")

