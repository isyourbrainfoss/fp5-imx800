# Fairphone 5 — main rear camera (Sony IMX800)

Linux bring-up for the **main rear** sensor on Fairphone 5 (Qualcomm SM7325 /
sc7280): V4L2 sensor driver, device tree, and CSIPHY **C-PHY** sequence.

Status: **experimental**. Raw frames and soft-ISP preview work with the pieces
below integrated into a mobile Linux kernel (e.g. mainline-based or distro
sc7280 tree). Not an official Fairphone or distribution package.

## Hardware summary

| Item | Value |
|------|--------|
| Sensor | Sony IMX800 @ CCI0 / I2C `0x1a` |
| Mode | 4096×3072, 10-bit **BGGR**, 2×2 bin (QCFA native 8192×6144) |
| CSI | **C-PHY**, 3 trios, CSIPHY0 |
| Link frequency | 700 MHz (timer-friendly; higher rates need more work) |
| Reset | TLMM GPIO 20 (XCLR), active low |
| MCLK | 24 MHz (`CAM_CC_MCLK0`) |

## Repository layout

```
driver/     imx800 V4L2 subdev + mode tables (OOT-friendly)
dt/         Board DT fragment (sensor + CSIPHY0 C-PHY endpoint)
camss/      sc7280 C-PHY lane register table for qcom-camss
libcamera/  Soft-IPA tuning sketch (black level / CCM)
scripts/    Example load + raw capture helpers
```

## What you need in the kernel

1. **Device tree** — merge `dt/imx800.dtsi` (adapt regulator/clock phandles).
2. **Sensor driver** — build `driver/` as a module against your kernel, or
   integrate under `drivers/media/i2c/`.
3. **CSIPHY C-PHY** — in-tree camss on sc7280 rejects C-PHY or uses a wrong
   sequence; apply `camss/` (see `camss/README.md`). Without this you get
   stream-on but **no frames**.

## Quick validation

```sh
# after modules + DT are live
v4l2-ctl -d /dev/v4l-subdevX --list-ctrls   # chip path via media-ctl -p
media-ctl -d /dev/media0 -p | grep -A5 imx800
# raw still (RDI):
sudo ./scripts/capture-raw.sh /tmp/imx800.raw
# expect ~15728640 bytes (packed 10-bit)
```

Userspace preview typically uses **libcamera** soft ISP + PipeWire. Install
`libcamera/imx800.yaml` where your libcamera IPA looks for sensor tuning
(often `/usr/share/libcamera/ipa/simple/`).

## Known limitations

- One sensor mode (no high-res QCFA path, no AF/OIS yet).
- Exposure/gain are placeholders; soft AWB/CCM only.
- Large full-res buffers stress **CMA** (fragmentation after many open/close).
- If the session manager enumerates cameras before modules load, restart
  WirePlumber (or equivalent) after inserting modules.

## Upstream

DT, `imx800`, and the sc7280 C-PHY table are intended to move toward the Linux
kernel / distro kernel packages. This tree is the working bring-up reference.

## License

GPL-2.0-only (see `LICENSE` and file SPDX headers).
