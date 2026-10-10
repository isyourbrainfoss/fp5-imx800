# Fairphone 5 — main rear camera (Sony IMX800)

> **Note**
> This is very experimental. Assume nothing here is written, or necessarily read through or fully understood, by a human unless stated, and that includes this text. Use it at your own risk, but you're encouraged to reuse any parts you find useful. The human work here is mainly ideas, testing in reality, and persistence with a vision.


Linux bring-up for the **main rear** sensor on Fairphone 5 (Qualcomm SM7325 /
sc7280): V4L2 sensor driver, device tree, and CSIPHY **C-PHY** sequence.

Status: **experimental**. Raw frames and soft-ISP preview work with the pieces
below integrated into a mobile Linux kernel (e.g. mainline-based or distro
sc7280 tree). Not an official Fairphone or distribution package.

## Hardware summary

| Item | Value |
|------|--------|
| Sensor | Sony IMX800 @ CCI0 / I2C `0x1a` |
| Formats | 10-bit **BGGR** (SBGGR10), C-PHY 3-trio on CSIPHY0 |
| Link frequency | 700 MHz (timer-friendly; higher rates need more work) |
| Reset | TLMM GPIO 20 (XCLR), active low |
| MCLK | 24 MHz (`CAM_CC_MCLK0`) |
| Rotation | 270° CW (matches other FP5 rear sensors) |

## Sensor modes (CamX RE)

Mode tables are decoded from Android CamX BinaryParameter
(`com.qti.sensormodule.fp5_imx800.bin`). Regenerator:
`driver/parse_camx_imx800.py` (expects the bin from a rooted vendor image;
not redistributed here).

| Default order | CamX idx | Size | Bin | ~RAW10/frame | Role |
|--------------:|---------:|------|-----|-------------:|------|
| **0** | **4** | **1920×1080** | 4×4 | **~2.5 MiB** | Preview / low CMA |
| 1 | 1 | 4096×3072 | 2×2 | ~15 MiB | Hi-res still |
| 2 | 5 | 1280×720 | 4×4 | ~1.1 MiB | Ultra-light |

Default pad format is **1080p** so viewfinder buffers are ~6× smaller than
always-4K (4 buffers ≈ 10 MiB vs ≈ 60 MiB). Details: `driver/IMX800-MODES.md`.

## Repository layout

```
driver/     imx800 multi-mode V4L2 subdev + mode tables + CamX parser
dt/         Board DT fragment (sensor + CSIPHY0 C-PHY endpoint)
camss/      sc7280 C-PHY lane register table for qcom-camss
libcamera/  Soft-IPA tuning sketch (black level / CCM)
scripts/    Load + multi-size raw capture helpers
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
media-ctl -d /dev/media0 -p | grep -A8 imx800
# enum modes (use the imx800 subdev node from media-ctl):
v4l2-ctl -d /dev/v4l-subdevX --list-subdev-framesizes pad=0,code=0x3007

# raw stills (RDI) — default 1080p, or pass WxH
sudo ./scripts/capture-raw.sh /tmp/imx800-1080.raw 1920x1080   # ~2592000 B
sudo ./scripts/capture-raw.sh /tmp/imx800-4k.raw 4096x3072     # ~15728640 B
sudo ./scripts/capture-raw.sh /tmp/imx800-720.raw 1280x720     # ~1152000 B

# libcamera raw (example):
cam -c <imx800-index> --stream role=raw,width=1920,height=1080 -C1 -F/tmp/raw-#.bin
```

Optional demosaic preview (lab quality): `scripts/raw10_to_jpeg.py`.

Userspace preview typically uses **libcamera** soft ISP + PipeWire. Install
`libcamera/imx800.yaml` where your libcamera IPA looks for sensor tuning
(often `/usr/share/libcamera/ipa/simple/`).

**Note:** some soft-ISP viewfinder paths still open the largest sensor mode and
downscale; prefer `role=raw` with an explicit size to keep CMA low.

## Module parameters

| Param | Default | Notes |
|-------|---------|--------|
| `default_camx_mode` | -1 (1080p table default) | Force CamX 4 / 1 / 5 |
| `rotation` | 270 | Degrees clockwise |
| `try_stream` | 1 | Program CamX tables on s_stream |

## Known limitations

- Exposure/gain are placeholders; soft AWB/CCM only (no full CamX AE).
- No AF / OIS / full QCFA 8192×6144 path yet.
- Full-res stills still stress **CMA** if opened often without release.
- If the session manager enumerates cameras before modules load, restart
  WirePlumber (or equivalent) after inserting modules.

## Upstream

DT, `imx800`, and the sc7280 C-PHY table are intended to move toward the Linux
kernel / distro kernel packages. This tree is the working bring-up reference.

## License

GPL-2.0-only (see `LICENSE` and file SPDX headers).
