
# define the target build
ADD_LIBRARY(drivers STATIC ahci.c ata.c e1000.c ps2keyboard.c)

TARGET_INCLUDE_DIRECTORIES(drivers SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src")
TARGET_INCLUDE_DIRECTORIES(drivers SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src/uthash_impl")
TARGET_INCLUDE_DIRECTORIES(drivers SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src/kernel")
TARGET_INCLUDE_DIRECTORIES(drivers SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../pdclib/include" "${CMAKE_CURRENT_SOURCE_DIR}/../pdclib/include/pdclib" "${CMAKE_CURRENT_SOURCE_DIR}/../pdclib_impl/include")

