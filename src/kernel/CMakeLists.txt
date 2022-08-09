
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
ADD_EXECUTABLE(os.bin acpi.c ap_boot.asm apic.c boot.asm bootmem.c efifb.c hpet.c idt.c interrupts.c interrupts.asm kalloc.c kernel.c multiboot2.c paging.c palloc.c pci.c serial.c terminal.c font.o)

# includes
TARGET_INCLUDE_DIRECTORIES(os.bin SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/uthash_impl")

# libs
TARGET_LINK_LIBRARIES(os.bin gcc drivers fs pdclib lailib)

# configure linker flags
#SET_TARGET_PROPERTIES(os.bin PROPERTIES LINK_FLAGS "-T ${CMAKE_SOURCE_DIR}/linker.ld -ffreestanding -O2 -nostdlib -lgcc")
SET_TARGET_PROPERTIES(os.bin PROPERTIES LINK_FLAGS "-T ${CMAKE_SOURCE_DIR}/linker.ld -ffreestanding -g3 -nostdlib")

# be sure to link static files
#TARGET_LINK_LIBRARIES(os.bin font)

