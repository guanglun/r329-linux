#!/bin/bash

KERNEL_PATH=.

scp $KERNEL_PATH/arch/arm64/boot/Image root@$R329:/boot/Image
#scp $KERNEL_PATH/arch/arm64/boot/dts/allwinner/sun50i-r329-maixsense.dtb root@r329.local:/boot/dtb
