# Public Example Assets

Most assets in this directory are CC-0 (public domain). Several models are
CC-BY 4.0 and require attribution, and one (`models/apartment.glb`) is CC-BY-NC
4.0, which permits non-commercial use only. These assets carry their own licences
and are not covered by the project's Apache-2.0 grant — see THIRD_PARTY_NOTICES
for details.

## Asset Inventory

### textures/ — complete
- `checkboard.png` — procedural 1024x1024 checker, 4x4 cells of dark grey
  (45,45,47)/(51,51,53) (Apache-2.0, generated). Its pixels match upstream's
  example checkboard exactly, and must: examples multiply it by diffuse colours
  far above 1 (render-to-texture uses (3,4,2)), so a lighter stand-in renders
  their ground white. `checkboard.ktx2` is an older, unused encoding.
- `colors.png` — procedural 256x256 gradient (Apache-2.0, generated)
- `hatch-0.jpg` — procedural 256x256 diagonal hatch (Apache-2.0, generated). UNUSED
  today, and it does NOT match upstream's hatch texture (mean 189/255 against 225).
  Replace it with upstream's before porting `shaders/shader-hatch`, or that port will
  be measured against the wrong art.
- `seaside-rocks01-color.jpg`, `-normal.jpg`, `-gloss.jpg`, `-height.jpg` — 1024x1024,
  byte-identical to the upstream set.
  LICENCE TO CONFIRM. The pixels must stay as they are: gloss drives the
  environment-atlas refraction lookup's mip (`level = (1 - gloss) * 5`), and a
  substitute averaging 0.51 instead of this set's 0.75 sampled a prefiltered level,
  rendering `refraction`'s capsules a flat opaque wash instead of glass. Used by
  `refraction` and `area-light`.
- `playcanvas.png` / `playcanvas-grey.png` — logo textures (CC-0 assets, but the
  depicted logo is a third-party mark: branding, not reusable art)
- `snowflake.png`, `spark.png`, `particles-numbers.png` — particle sprites (CC-0)
- `normal-map.png` — tiling normal map (CC-0)
- `heart.png` — light-cookie / decal texture; provenance unconfirmed, see
  THIRD_PARTY_NOTICES. Used by `lights`, `clustered-spot-shadows`, `mesh-decals`.
- `bricks076a/` — ambientCG "Bricks 076 A" (CC-0), converted to webp; carries its
  own `Bricks076A-textures.txt`. Used by `parallax-mapping`.
- `lightmap-pools.tga`, `lut-teal-orange.tga` — no recorded origin and no
  generating tool in this repository. `lut-teal-orange.tga` is the 3D-LUT test
  strip used by `ambient-occlusion-davinci`; `lightmap-pools.tga` is unused.

### fonts/ — complete
- `courier.json` + `courier.png` — Courier MSDF bitmap-font atlas (CC-0)
- `liberation-sans.json` + `liberation-sans.png` + `.txt` — SDF atlas of the 95
  printable ASCII glyphs, generated from Liberation Sans 2.1.5 (**SIL OFL 1.1**, so
  the copyright notice and licence must accompany it — they are in the `.txt`).
  Liberation Sans is metric-compatible with Arial, so every advance matches the
  Arial atlas this replaced. Used by `raycast`, `post-processing` and `anisotropy`.

### animations/bitmoji/ — complete
- `idle.glb`, `walk.glb`, `run.glb`, `jump-flip.glb`, `win-dance.glb` — Bitmoji locomotion clips (CC-0)

### hdri/ — complete
- `cannon-2k.hdr` — Cannon outdoor HDRI from Poly Haven (CC-0)
- `kloofendal-2k.hdr` — Kloofendal partly cloudy HDRI from Poly Haven (CC-0)

### models/ — complete
- `a_beautiful_game.glb` — ABeautifulGame chess set from Khronos glTF-Sample-Assets
  (**CC-BY 4.0** — © 2020 Academy Software Foundation for the model, © 2022 Ed Mackey
  for the glTF conversion)
- `antique_camera.glb` — AntiqueCamera from Khronos glTF-Sample-Assets (CC-0, UX3D,
  created by Maximillan Kamps, see .txt)
- `ClearCoatTest.glb` — ClearCoatTest from Khronos glTF-Sample-Assets (**CC-BY 4.0**,
  © 2020 Analytical Graphics, Inc., created by Ed Mackey, see .txt)
- `box_textured.glb` — BoxTextured from Khronos glTF-Sample-Assets (**CC-BY 4.0**,
  © 2017 Cesium, see .txt)
- `fox.glb` — Fox animated model from Khronos glTF-Sample-Assets (mesh CC-0 PixelMannen;
  rig + animation **CC-BY 4.0** tomkranis; glTF conversion **CC-BY 4.0** @AsoboStudio
  and @scurest, see .txt)
- `toy_car.glb` — ToyCar from Khronos glTF-Sample-Assets (CC-0)
- `da_vinci_workshop.glb` — Da Vinci Workshop from Sketchfab (CC-0)
- `leonardo_da_vinci.glb` — Leonardo da Vinci from Sketchfab (CC-0)
- `oceanic_currents.glb` — Oceanic Currents from Sketchfab (CC-0)
- `metric_tensor_riemann.glb` — Metric Tensor from Sketchfab (CC-0)
- `miller_indices_problem_2.glb` — Miller Indices from Sketchfab (CC-0)

Mirrored from the upstream examples (for visual parity):
- `statue.glb` — statue (CC-0)
- `geometry-camera-light.glb` — GLB with embedded camera + lights (CC-0)
- `playcanvas-cube.glb` — logo cube (CC-0; the logo is a third-party mark)
- `bitmoji.glb` — Bitmoji character for anim state graph (CC-0)
- `chess-board.glb` — Chess Board by Idmental, Sketchfab (**CC-BY 4.0**, see .txt)
- `terrain.glb` — Low-poly terrain, Sketchfab (**CC-BY 4.0**, see .txt)
- `robot-arm.glb` — Black Honey Robotic Arm, Sketchfab (**CC-BY 4.0**, Draco, see .txt)
- `glass-table.glb` — Low-poly glass table, Sketchfab (**CC-BY 4.0**, see .txt)
- `pbr-house.glb` — House 03 PBR, Sketchfab (**CC-BY 4.0**, see .txt)
- `house.glb` — House scene w/ generated UV1 for lightmapping, Sketchfab (**CC-BY 4.0**, see .txt)
- `SunglassesKhronos.glb` — **PROVENANCE UNCONFIRMED** (see .txt). The Khronos
  glTF-Sample-Assets collection has no "Sunglasses" model, so the filename's implied
  source is wrong, and no licence is documented anywhere for it.
- `apartment.glb` — Mirror's Edge Apartment by Aurélien Martel, Sketchfab (**CC-BY-NC 4.0 — NON-COMMERCIAL ONLY**, see .txt)
- `love.glb` — Love neon sign 02 by daysena, Sketchfab (**CC-BY 4.0**, see .txt)
- `laboratory.glb` — Laboratory by Sketchfab (**CC-BY 4.0**, see .txt)
- `dry-sand-terrain.glb` — FREE Dry Sand Terrain by josevega, Sketchfab (**CC-BY 4.0**,
  webp texture re-encoded to jpeg, see .txt) — this engine has no webp decoder
  (`tools/glb_reencode_webp.py`). Same mesh and exporter otherwise, so that re-encode
  is the only difference from the upstream copy.
- `cat.glb` — Egyptian Cat Statue by Ankledot, Sketchfab (**CC-BY 4.0**, webp textures re-encoded to png, see .txt)
- `tamiya-dt03.compressed.ply` — Tamiya DT-03 by Simon Bethke, SuperSplat
  (**CC-BY 4.0**, see .txt). Used by `gsplat-example`. Downloaded as a 154 MB
  uncompressed 3DGS PLY and converted with `tools/ply_to_compressed_ply.py`
  (thinned to 500k splats, SH dropped) — rerun that script to regenerate.
- `sh_sphere.ply`, `compressed_rings.ply`, `torus_splats.ply` — procedurally
  generated splat test assets (Apache-2.0, generated). Used by `gsplat-tier2`.

### cubemaps/ — complete
- `helipad-env-atlas.png`, `table-mountain-env-atlas.png`, `morning-env-atlas.png` —
  512x512 RGBP atlases, byte-identical to the upstream set so the examples light
  exactly as theirs do
- `xmas_faces/*.png` — six light-cookie cubemap faces. Used by `lights`.

**Provenance unconfirmed** for all of the above. They are mirrored byte-for-byte from
the upstream example set, whose repository is MIT-licensed but carries no per-asset
attribution for them, so the original HDR sources are undocumented. `tools/generate-env-atlas`
produces CC-0 replacements where that matters for a release — see THIRD_PARTY_NOTICES.

## Sources

- [Poly Haven](https://polyhaven.com/) — HDRIs (CC-0)
- [ambientCG](https://ambientcg.com/) — PBR textures (CC-0)
- [Khronos glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) — reference models (mixed CC-0/CC-BY 4.0)
- Sketchfab — CC-0 models downloaded manually
