#!/bin/bash

CROSS_COMPILE_PATH=/home/guanglun/workspace/fourier-r329-g0/armbian-build/cache/toolchain/gcc-arm-8.3-2019.03-x86_64-aarch64-linux-gnu/bin

#cp config/kernel/linux-sun50iw11-edge.config $KERNEL_PATH/.config
#cd $KERNEL_PATH && make ARCH=arm64 CROSS_COMPILE=$CROSS_COMPILE_PATH/aarch64-linux-gnu- clean
#cd $KERNEL_PATH && make ARCH=arm64 CROSS_COMPILE=$CROSS_COMPILE_PATH/aarch64-linux-gnu- -j12
make -j12 KDEB_PKGVERSION=21.08.0-trunk BRANCH=edge LOCALVERSION=-sun50iw11 KBUILD_DEBARCH=arm64 ARCH=arm64 CROSS_COMPILE=$CROSS_COMPILE_PATH/aarch64-linux-gnu- menuconfig
#cd $KERNEL_PATH && make ARCH=arm64 CROSS_COMPILE=$CROSS_COMPILE_PATH/aarch64-linux-gnu- menuconfig