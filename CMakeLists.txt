# So CMake can tell whether or not it can process this file
CMAKE_MINIMUM_REQUIRED(VERSION 2.8.0)
PROJECT(os LANGUAGES C ASM)
 
# Configure build tools
SET(CMAKE_SYSTEM_NAME Generic)
SET(CMAKE_SYSTEM_PROCESOR x86)


# Configure assembler
SET(CMAKE_ASM_COMPILE_OBJECT "nasm -felf32 <SOURCE> -o <OBJECT>")

# Configure C compiler
SET(CMAKE_C_COMPILER i686-elf-gcc)
SET(CMAKE_C_COMPILER_WORKS TRUE)
SET(CMAKE_C_FLAGS "-std=gnu99 -ffreestanding -O2 -Wall -Wextra" CACHE STRING "" FORCE)
#SET(CMAKE_C_SIZEOF_DATA_PTR 4)

# Prevent -rdynamic from being passed to the linker
set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")
set(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "")

# Output kernel and various libraries to predictable location
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Include the kernel code
ADD_SUBDIRECTORY(src/kernel)
 
# Include the isobuild project
ADD_SUBDIRECTORY(isobuild)


