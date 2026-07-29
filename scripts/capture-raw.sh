#!/bin/sh
# Capture one raw frame from IMX800 via V4L2 (qcom-camss RDI path).
set -e
MD="${MD:-/dev/media0}"
VID="${VID:-/dev/video2}"
OUT="${1:-/tmp/imx800-frame.raw}"

media-ctl -d "$MD" -l '"msm_csiphy0":1->"msm_csid0":0[1]'
media-ctl -d "$MD" -l '"msm_csid0":1->"msm_vfe0_rdi0":0[1]'

for e in \
  '"imx800 16-001a":0' \
  '"msm_csiphy0":0' '"msm_csiphy0":1' \
  '"msm_csid0":0' '"msm_csid0":1' \
  '"msm_vfe0_rdi0":0' '"msm_vfe0_rdi0":1'
do
  media-ctl -d "$MD" --set-v4l2 "${e}[fmt:SBGGR10_1X10/4096x3072 field:none]" || true
done

v4l2-ctl -d "$VID" --set-fmt-video=width=4096,height=3072,pixelformat=pBAA
rm -f "$OUT"
timeout 8 v4l2-ctl -d "$VID" --stream-mmap --stream-count=1 --stream-to="$OUT"
ls -la "$OUT"
