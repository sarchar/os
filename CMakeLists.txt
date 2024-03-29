# So CMake can tell whether or not it can process this file
CMAKE_MINIMUM_REQUIRED(VERSION 2.8.0)
SET(CMAKE_C_COMPILER_WORKS TRUE)
SET(CMAKE_CXX_COMPILER_WORKS TRUE)
PROJECT(os LANGUAGES C ASM)
 
# Configure build tools
SET(CMAKE_SYSTEM_NAME Generic)
SET(CMAKE_SYSTEM_PROCESOR x86)


# Configure compiler and assembler
#SET(CMAKE_C_COMPILER x86_64-elf-gcc)
SET(CMAKE_C_COMPILER_WORKS TRUE)
SET(CMAKE_CXX_COMPILER_WORKS TRUE)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    SET(CMAKE_ASM_COMPILE_OBJECT "nasm -felf64 <SOURCE> -o <OBJECT> -g -DDEBUG")
    SET(CMAKE_C_FLAGS "-std=gnu11 -ffreestanding -g3 -Wall -Wextra -mcmodel=large -mno-red-zone -falign-functions=16" CACHE STRING "" FORCE)
else()
    SET(CMAKE_C_FLAGS "-std=gnu11 -ffreestanding -O2 -Wall -Wextra -mcmodel=large -mno-red-zone -falign-functions=16" CACHE STRING "" FORCE)
    SET(CMAKE_ASM_COMPILE_OBJECT "nasm -felf64 <SOURCE> -o <OBJECT>")
endif()

# Prevent -rdynamic from being passed to the linker
set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "-z max-page-size=0x1000")
set(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "-z max-page-size=0x1000")

# Output kernel and various libraries to predictable location
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Include the kernel code
ADD_SUBDIRECTORY(src/drivers)
ADD_SUBDIRECTORY(src/fs)
ADD_SUBDIRECTORY(src/kernel)
ADD_SUBDIRECTORY(src/lai_impl)
ADD_SUBDIRECTORY(src/net)
ADD_SUBDIRECTORY(src/pdclib_impl)
 
# Include the isobuild project
ADD_SUBDIRECTORY(isobuild)


