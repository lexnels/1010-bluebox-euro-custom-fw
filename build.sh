#!/bin/sh
# Compile the code caves: CPU meter out/cm7 (M7 side) and out/cm4 (M4 side), reverb + delay out/hall7 (M7), delay params out/dly4 (M4), master bus out/mst7 (M7), USB out mode out/usb7 (M7).
set -e
cd "$(dirname "$0")"
mkdir -p out
COMMON="-mthumb -mfloat-abi=hard -Os -ffreestanding -nostdlib -fno-builtin -Wall -Wextra -Werror -Wl,-e,0"
arm-none-eabi-gcc -mcpu=cortex-m7 -mfpu=fpv5-d16 $COMMON -T src/cm7.ld -o out/cm7.elf src/cm7_cpu.c
arm-none-eabi-gcc -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 $COMMON -T src/cm4.ld -o out/cm4.elf src/cm4_cpu.c
arm-none-eabi-gcc -mcpu=cortex-m7 -mfpu=fpv5-d16 $(echo "$COMMON" | sed 's/-Os/-O2/') -T src/hall7.ld -o out/hall7.elf src/hall_m7.c
arm-none-eabi-objcopy -O binary -j .cave out/cm7.elf out/cm7.bin
arm-none-eabi-objcopy -O binary -j .cave out/cm4.elf out/cm4.bin
arm-none-eabi-objcopy -O binary -j .cave out/hall7.elf out/hall7.bin
arm-none-eabi-size -A out/cm7.elf out/cm4.elf out/hall7.elf | grep -E 'cave|elf'
arm-none-eabi-gcc -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 $COMMON -T src/dly4.ld -o out/dly4.elf src/dly_m4.c
arm-none-eabi-objcopy -O binary -j .cave out/dly4.elf out/dly4.bin
arm-none-eabi-gcc -mcpu=cortex-m7 -mfpu=fpv5-d16 $(echo "$COMMON" | sed 's/-Os/-O2/') -T src/mst7.ld -o out/mst7.elf src/mst_m7.c
arm-none-eabi-objcopy -O binary -j .cave out/mst7.elf out/mst7.bin
arm-none-eabi-gcc -mcpu=cortex-m7 -mfpu=fpv5-d16 $COMMON -fno-tree-loop-distribute-patterns -T src/usb7.ld -o out/usb7.elf src/usb_m7.c
arm-none-eabi-objcopy -O binary -j .cave out/usb7.elf out/usb7.bin
arm-none-eabi-size -A out/dly4.elf out/mst7.elf out/usb7.elf | grep -E 'cave|elf'
