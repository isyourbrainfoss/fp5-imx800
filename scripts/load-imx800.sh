#!/bin/sh
# Load bring-up modules: OOT camss (C-PHY) + imx800.
# Paths and names are examples — adjust for your distro.
set -e
MODDIR="${MODDIR:-/usr/local/lib/fp5-imx800}"

modprobe videobuf2-common 2>/dev/null || true
modprobe videobuf2-v4l2 2>/dev/null || true
modprobe videobuf2-dma-sg 2>/dev/null || true

# Replace in-tree camss if it lacks C-PHY for this SoC
if lsmod | grep -q '^qcom_camss' && ! grep -q lane_regs_sc7280_3ph /proc/kallsyms 2>/dev/null; then
	rmmod imx800 2>/dev/null || true
	rmmod imx858 s5kjn1 dw9719 2>/dev/null || true
	rmmod qcom_camss 2>/dev/null || true
fi

if ! lsmod | grep -q '^qcom_camss'; then
	insmod "$MODDIR/qcom-camss.ko"
fi

modprobe imx858 2>/dev/null || true
modprobe s5kjn1 2>/dev/null || true

if ! lsmod | grep -q '^imx800'; then
	insmod "$MODDIR/imx800.ko" try_stream=1 rotation=270
fi

if [ -f "$MODDIR/imx800.yaml" ]; then
	mkdir -p /usr/share/libcamera/ipa/simple
	cp -f "$MODDIR/imx800.yaml" /usr/share/libcamera/ipa/simple/imx800.yaml
fi

echo "ok: $(ls /dev/video2 2>/dev/null || echo 'no /dev/video2 yet')"
