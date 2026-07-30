# IMX800 multi-mode (pmOS) — CamX RE secondary modes

**Goal B (2026-07-29):** lower-res preview modes to cut CMA load vs full 4096×3072.

**Source blob:** `dumps/android/camera-imx800-re-20260729-092459/com.qti.sensormodule.fp5_imx800.bin`  
**Decoder:** `lab/camera/kernel/parse_camx_imx800.py`  
**Tables:** `imx800_mode_tables.h` (auto-generated; do not hand-edit)

## Mode map (exported to driver)

| Driver order | CamX idx | Size | Bin | FLL | LLP | RAW10/frame | 4-buffer CMA | Role |
|-------------:|---------:|------|-----|----:|----:|------------:|-------------:|------|
| **0 (default)** | **4** | **1920×1080** | 4×4 (`0x0901=0x44`) | 1584 | 5520 | **~2.5 MiB** | **~10 MiB** | Preview / UX |
| 1 | 1 | 4096×3072 | 2×2 (`0x22`) | 7484 | 9776 | ~15.0 MiB | ~60 MiB | Hi-res still (mode-1) |
| 2 | 5 | 1280×720 | 4×4 (`0x44`) | 984 | 5520 | ~1.1 MiB | ~4.4 MiB | Ultra-light |

Other CamX tables not exported (still in bin): mode 0 = 8192×6144 QCFA full; modes 2–3 = 4384×2464 16:9 crop; modes 6–8 variants of 4096×3072.

**CMA win:** default preview buffers drop from ~60 MiB (4× full-res) to ~10 MiB (4× 1080p) — about **6× less** per open.

## Regenerate tables

```bash
cd /path/to/fp5
python3 lab/camera/kernel/parse_camx_imx800.py --list-modes
python3 lab/camera/kernel/parse_camx_imx800.py --export-dir lab/camera/kernel
```

## Build / install (dual-boot safe)

Cross-build against the lab 7.1.2 tree (matches pmOS vermagic):

```bash
cd lab/camera/kernel
make -C build/linux-sc7280 M=$PWD ARCH=arm64 LLVM=1 modules
cp imx800.ko ../../../artifacts/camera-modules/
scp artifacts/camera-modules/imx800.ko fp5-pmos-usb:/tmp/
ssh fp5-pmos-usb 'printf %s 147147 | sudo -S sh -c "
  pkill -x snapshot 2>/dev/null || true
  # stop WirePlumber if media busy
  rmmod imx800
  cp /tmp/imx800.ko /usr/local/lib/fp5-camera/
  insmod /usr/local/lib/fp5-camera/imx800.ko
"'
```

Optional module params:

| Param | Default | Notes |
|-------|---------|--------|
| `default_camx_mode` | -1 (table default = 1080p) | Force CamX 4 / 1 / 5 at probe |
| `rotation` | 270 | Match imx858 |
| `try_stream` | 1 | Program CamX tables on s_stream |

**Never use `qbootctl`.** Soft bootloader from pmOS: `scripts/to-fastboot-from-pmos.sh` (OOT sdam + reboot-arg).

## Proven on device (2026-07-29)

| Test | Result |
|------|--------|
| Enum sizes | 1920×1080, 4096×3072, 1280×720 |
| 1080p STREAMON | `s_stream on: camx=4 1920x1080 ret=0`, MODE_SELECT=1 |
| 1080p frame | 2 592 000 B, nonzero ratio ~0.91 |
| 4K STREAMON | `s_stream on: camx=1 4096x3072 ret=0` |
| 4K frame | 15 728 640 B, nonzero ratio ~0.99 |

Artifacts: `dumps/pmos/imx800-multimode-20260729/` (`cap1080.sh`, `cap4k.sh`, sample raw).

## Capture helpers (raw RDI)

```sh
# on phone after modules loaded
sudo sh /tmp/cap1080.sh   # or lab dumps copy
sudo sh /tmp/cap4k.sh
```

Pipeline: CSIPHY0 → CSID0 → VFE0 RDI0, `pBAA` (packed SBGGR10). After `media-ctl` on intermediate pads, **re-set the sensor format last** — camss may walk enum sizes and leave hi-res active.

## Snapshot / libcamera

- Driver default format is **1920×1080** so soft-ISP viewfinder can allocate smaller buffers.
- libcamera may still pick the largest mode for stills; both sizes are advertised via `enum_frame_size`.
- If Snapshot still grabs 4K, force lower stream size or use raw path scripts above; CMA fragmentation is much less painful with 1080p default.

## Dual-boot notes

- Work is OOT module only + install under `/usr/local/lib/fp5-camera/` — no boot_b flash required for mode work.
- Hybrid DTB + C-PHY camss already on slot B; Android slot A unchanged.
- Known Android soft-bootloader recovery path remains valid (see `LIVE-SESSION.md` / `scripts/to-fastboot-from-pmos.sh`).
