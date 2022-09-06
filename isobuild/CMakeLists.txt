
add_custom_command(OUTPUT os.iso
                   DEPENDS ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/os.bin 
                   COMMAND mkdir -p iso/boot/grub
		   COMMAND cp ${CMAKE_CURRENT_SOURCE_DIR}/grub.cfg iso/boot/grub/
		   COMMAND cp ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/os.bin iso/boot/
		   COMMAND grub-mkrescue -o os.iso iso/
)

add_custom_target(os_iso ALL 
                  DEPENDS os.iso
		  COMMAND mv os.iso ${CMAKE_BINARY_DIR})

add_custom_target(run
                  DEPENDS os_iso 
                  COMMAND sudo qemu-system-x86_64 -machine q35
                                             -bios /usr/share/ovmf/OVMF.fd
                                             #-enable-kvm -cpu host
                                             -smp 4
                                             -m 4G
                                             -boot d
                                             #use -vga line or -nographic one, not both
                                             -vga cirrus -monitor stdio -serial file:${CMAKE_BINARY_DIR}/output.log
                                             #-nographic
                                             -drive file=${CMAKE_BINARY_DIR}/os.iso,if=none,media=cdrom,format=raw,id=boot
					     #-drive file=/home/cmason/archlinux-2022.09.03-x86_64.iso,if=none,media=cdrom,format=raw,id=boot
                                             #-drive file=${CMAKE_BINARY_DIR}/fs.qcow2,if=none,media=disk,format=qcow2,id=extra
                                             -drive file=${CMAKE_BINARY_DIR}/ext2.img,if=none,media=disk,format=raw,id=extra
                                             -device ahci,id=ahci
                                             -device ide-cd,drive=boot,bus=ahci.0
                                             -device ide-hd,drive=extra,bus=ahci.1
                                             -usb
                                             -device usb-mouse
                                             #-netdev user,id=osnet,hostfwd=tcp::5555-:22
                                             -netdev tap,br=br0,helper=/usr/lib/qemu/qemu-bridge-helper,id=osnet
                                             -device e1000,netdev=osnet
                                             #-device e1000e,netdev=osnet,romfile=${CMAKE_CURRENT_SOURCE_DIR}/efi-e1000e.rom
                                             -k en-us
                                             -d int -D int.log
)

add_custom_target(gdb
                  DEPENDS os_iso 
                  COMMAND sudo qemu-system-x86_64 -machine q35
                                             -bios /usr/share/ovmf/OVMF.fd
                                             #-enable-kvm
                                             #-cpu host
                                             -smp 4
                                             -m 4G
                                             -boot d
                                             #use -vga line or -nographic one, not both
                                             -vga cirrus -monitor stdio -serial file:${CMAKE_BINARY_DIR}/output.log
                                             #-nographic 
                                             -serial file:${CMAKE_BINARY_DIR}/output.log
                                             -drive file=${CMAKE_BINARY_DIR}/os.iso,if=none,media=cdrom,format=raw,id=boot
                                             #-drive file=${CMAKE_BINARY_DIR}/fs.qcow2,if=none,media=disk,format=qcow2,id=extra
                                             -drive file=${CMAKE_BINARY_DIR}/ext2.img,if=none,media=disk,format=raw,id=extra
                                             -device ahci,id=ahci
                                             -device ide-cd,drive=boot,bus=ahci.0
                                             -device ide-hd,drive=extra,bus=ahci.1
                                             -usb
                                             -device usb-mouse
					     #-netdev user,id=osnet
					     -netdev tap,br=br0,helper=/usr/lib/qemu/qemu-bridge-helper,id=osnet
                                             -device e1000,netdev=osnet
                                             #-device e1000e,netdev=osnet,romfile=${CMAKE_CURRENT_SOURCE_DIR}/efi-e1000e.rom
                                             -k en-us
                                             -s -S
)
