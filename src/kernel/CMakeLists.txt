
# custom command to convert iso8859-1 to an ELF object for embedding into the kernel
add_custom_command(OUTPUT font.o
                   DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/font.psf
                   COMMAND cd ${CMAKE_CURRENT_SOURCE_DIR} && x86_64-elf-objcopy -O elf64-x86-64 -B i386 -I binary font.psf ${CMAKE_CURRENT_BINARY_DIR}/font.o
)

# don't compile font.o like normal
SET_SOURCE_FILES_PROPERTIES(font.o
    PROPERTIES
    EXTERNAL_OBJECT true
    GENERATED true
)

# compile a data "library"
#ADD_LIBRARY(data STATIC font.o)
#SET_TARGET_PROPERTIES(data PROPERTIES LINKER_LANGUAGE C)

# define the target build
ADD_EXECUTABLE(os.bin boot.asm bootmem.c efifb.c idt.c interrupts.c interrupts.asm kernel.c multiboot2.c palloc.c terminal.c font.o)

# includes
TARGET_INCLUDE_DIRECTORIES(os.bin SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src")

# libs
TARGET_LINK_LIBRARIES(os.bin gcc)

# configure linker flags
#SET_TARGET_PROPERTIES(os.bin PROPERTIES LINK_FLAGS "-T ${CMAKE_SOURCE_DIR}/linker.ld -ffreestanding -O2 -nostdlib -lgcc")
SET_TARGET_PROPERTIES(os.bin PROPERTIES LINK_FLAGS "-T ${CMAKE_SOURCE_DIR}/linker.ld -ffreestanding -g3 -nostdlib")

# be sure to link static files
#TARGET_LINK_LIBRARIES(os.bin font)

