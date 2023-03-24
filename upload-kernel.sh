#!/bin/bash

KERNEL_PATH=.
export R329=192.168.11.31
scp $KERNEL_PATH/arch/arm64/boot/Image root@$R329:/boot/vmlinuz-5.10.109-rt65-sun50iw11
#scp $KERNEL_PATH/arch/arm64/boot/dts/allwinner/sun50i-r329-maixsense.dtb root@r329.local:/boot/dtb
