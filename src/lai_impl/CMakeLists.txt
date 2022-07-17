
file(GLOB_RECURSE lai_core_files "${CMAKE_CURRENT_SOURCE_DIR}/../lai/core/*.c" "${CMAKE_CURRENT_SOURCE_DIR}/../lai/core/*.h")
file(GLOB_RECURSE lai_helper_files "${CMAKE_CURRENT_SOURCE_DIR}/../lai/helpers/*.c" "${CMAKE_CURRENT_SOURCE_DIR}/../lai/helpers/*.h")

# define the target build
ADD_LIBRARY(lailib STATIC ${lai_core_files} ${lai_helper_files} laihost.c)

TARGET_INCLUDE_DIRECTORIES(lailib SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../lai/include")
TARGET_INCLUDE_DIRECTORIES(lailib SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../pdclib/include" "${CMAKE_CURRENT_SOURCE_DIR}/../pdclib/include/pdclib" "${CMAKE_CURRENT_SOURCE_DIR}/../pdclib_impl/include")
TARGET_INCLUDE_DIRECTORIES(lailib SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../kernel")

