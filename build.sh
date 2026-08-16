#!/bin/bash
# Cross-build 3c589.device using the amiga-build-container toolchain.
# Output: 3c589/Devs/networks/3c589.device (updated in place), build/3c589.lha
set -e
cd "$(dirname "$0")"

IMAGE=${IMAGE:-ghcr.io/rondoval/amiga-build-container:gcc-v16.1}

mkdir -p build

docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/work" -w /work "$IMAGE" sh -ec '
   CFLAGS="-O2 -fomit-frame-pointer -Wall -Wno-parentheses -Wno-pointer-sign -IInclude"
   DEVICE=3c589/Devs/networks/3c589.device

   OBJS=""
   for f in device unit request; do
      m68k-amigaos-gcc $CFLAGS -c 3c589/Source/$f.c -o build/$f.o
      OBJS="$OBJS build/$f.o"
   done
   m68k-amigaos-gcc -nostartfiles $OBJS -o $DEVICE -lamiga
   m68k-amigaos-strip $DEVICE

   # Aminet-style archive, straight from the tree: 3c589.info + the drawer
   rm -f build/3c589.lha
   lha aq2 build/3c589.lha 3c589.info 3c589

   ls -la $DEVICE build/3c589.lha
'
