# sc7280 CSIPHY C-PHY

Mainline `qcom-camss` accepts C-PHY on newer platforms, but **sc7280** still
needs a correct Gen2 3PH register sequence. Using the sdm845 table results in
a live media graph and sensor stream-on with **no RDI frames**.

## Integration sketch

In `camss-csiphy-3ph-1-0.c` (or equivalent):

1. Add `lane_regs_sc7280_3ph[]` from `lane_regs_sc7280_3ph.c`.
2. For `CAMSS_7280` + `V4L2_MBUS_CSI2_CPHY`, point `regs->lane_regs` at it.
3. Ensure CSID `PHY_TYPE_SEL` is C-PHY and the lane mask uses **odd bits**
   for trios (3 trios → mask `0x2a`).

Stock in-tree camss will log `Unsupported bus type` for C-PHY until this (or
an equivalent) lands.

## Proven on

- SoC: SM7325 / sc7280 (Fairphone 5)
- CSIPHY compatible: `qcom,csiphy-v1.2.1`
- Sensor: IMX800, 3-trio C-PHY, 700 MHz `link-frequencies`
