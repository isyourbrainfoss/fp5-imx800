#!/bin/sh
# Capture one raw frame from IMX800 via V4L2 (qcom-camss RDI path).
# Usage: capture-raw.sh [outfile] [WxH]
#   sizes: 1280x720 | 1920x1080 (default) | 4096x3072
set -e
MD="${MD:-/dev/media0}"
VID="${VID:-/dev/video2}"
OUT="${1:-/tmp/imx800-frame.raw}"
SIZE="${2:-1920x1080}"
W="${SIZE%x*}"
H="${SIZE#*x}"

media-ctl -d "$MD" -l '"msm_csiphy0":1->"msm_csid0":0[1]'
media-ctl -d "$MD" -l '"msm_csid0":1->"msm_vfe0_rdi0":0[1]'

for e in \
  '"imx800 16-001a":0' \
  '"msm_csiphy0":0' '"msm_csiphy0":1' \
  '"msm_csid0":0' '"msm_csid0":1' \
  '"msm_vfe0_rdi0":0' '"msm_vfe0_rdi0":1'
do
  media-ctl -d "$MD" --set-v4l2 "${e}[fmt:SBGGR10_1X10/${W}x${H} field:none]" || true
done
# Re-force sensor last — camss may walk enum and leave another size active.
media-ctl -d "$MD" --set-v4l2 '"imx800 16-001a":0[fmt:SBGGR10_1X10/'"${W}x${H}"' field:none]'

v4l2-ctl -d "$VID" --set-fmt-video=width="$W",height="$H",pixelformat=pBAA
rm -f "$OUT"
timeout 12 v4l2-ctl -d "$VID" --stream-mmap --stream-count=1 --stream-to="$OUT"
ls -la "$OUT"
# packed RAW10 size check
python3 -c "
from pathlib import Path
b=Path('$OUT').read_bytes()
exp=$W*$H*10//8
nz=sum(1 for x in b if x)
print('size',len(b),'exp',exp,'nonzero',nz,'OK' if len(b)==exp and nz>256 else 'CHECK')
"
