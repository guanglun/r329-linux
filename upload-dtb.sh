#!/bin/bash

KERNEL_PATH=.

#scp $KERNEL_PATH/arch/arm64/boot/Image root@r329:/boot/Image
scp $KERNEL_PATH/arch/arm64/boot/dts/allwinner/sun50i-r329-maixsense.dtb root@$R329:/boot/dtb-5.14.0-rc4-sun50iw11/allwinner/sun50i-r329-maixsense.dtb

	# cs-gpios = <&pio 7 4 GPIO_ACTIVE_HIGH>;
	# status = "okay";

	# spidev@0 {
	# 	status = "okay";

	# 	compatible = "spidev";
	# 	/* spi default max clock 100Mhz */
	# 	spi-max-frequency = <100000000>;
	# 	reg = <0>;
	# };