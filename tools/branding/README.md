# Darkone83 opening-logo replacement

`darkone83-wireframe.obj` and `darkone83-outline.svg` are the user's original supplied branding assets. This is the *only planned visual change* to GoldenEye 007.

Create the Direct3D wireframe buffer using:

```powershell
py -3 tools\007xb_assets.py logo --obj branding\darkone83-wireframe.obj
```

The output `data/darkone83.lines` is a 56,208-byte geometry data file (2,424 vertices, 3,387 segments). The future classic RXDK code will need to map it to `D3DPT_LINELIST`, place it in the intro scene, and preserve the original N logo's animation timing. No such engine code has been written or tested yet.
