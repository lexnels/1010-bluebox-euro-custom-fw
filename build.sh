#!/bin/sh
# Compile the code caves: CPU meter out/cm7 (M7 side) and out/cm4 (M4 side), hall reverb out/hall7 (M7).
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
