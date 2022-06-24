# define the target build
ADD_EXECUTABLE(os.bin boot.asm kernel.c)

# configure linker flags
SET_TARGET_PROPERTIES(os.bin PROPERTIES LINK_FLAGS "-T ${CMAKE_SOURCE_DIR}/linker.ld -ffreestanding -O2 -nostdlib -lgcc")

