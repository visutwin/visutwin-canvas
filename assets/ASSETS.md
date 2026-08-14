# Public Example Assets

All assets in this directory are safe for open-source distribution.
Most are CC-0 (public domain). Several models are CC-BY 4.0 and require
attribution — see THIRD_PARTY_NOTICES for details.

Many assets are mirrored from the PlayCanvas engine example set so the
VisuTwin examples can visually match their PlayCanvas counterparts. Assets
copied from PlayCanvas that carry a Sketchfab/Khronos CC-BY license keep
their original `<name>.txt` license sibling next to the file.

## Asset Inventory

### textures/ — complete
- `checkboard.png` — procedural 256x256 checker (Apache-2.0, generated)
- `colors.png` — procedural 256x256 gradient (Apache-2.0, generated)
- `hatch-0.jpg` — procedural 256x256 diagonal hatch (Apache-2.0, generated)
- `seaside-rocks01-color.jpg` — Rock026 from ambientCG (CC-0)
- `seaside-rocks01-normal.jpg` — Rock026 from ambientCG (CC-0)
- `seaside-rocks01-gloss.jpg` — Rock026 from ambientCG (CC-0)
- `seaside-rocks01-height.jpg` — Rock026 from ambientCG (CC-0)
- `playcanvas.png` / `playcanvas-grey.png` — PlayCanvas logo textures (CC-0)
- `snowflake.png`, `spark.png`, `particles-numbers.png` — particle sprites from PlayCanvas (CC-0)
- `normal-map.png` — tiling normal map from PlayCanvas examples (CC-0)

### fonts/ — complete
- `courier.json` + `courier.png` — Courier MSDF bitmap-font atlas from PlayCanvas (CC-0)
- `arial.json` + `arial.png` — Arial MSDF bitmap-font atlas from PlayCanvas (CC-0 atlas; Arial typeface is Monotype-proprietary)

### animations/bitmoji/ — complete
- `idle.glb`, `walk.glb`, `run.glb`, `jump-flip.glb`, `win-dance.glb` — Bitmoji locomotion clips from PlayCanvas (CC-0)

### hdri/ — complete
- `cannon-2k.hdr` — Cannon outdoor HDRI from Poly Haven (CC-0)
- `kloofendal-2k.hdr` — Kloofendal partly cloudy HDRI from Poly Haven (CC-0)

### models/ — complete
- `a_beautiful_game.glb` — ABeautifulGame chess set from Khronos glTF-Sample-Assets (CC-0, ASWF)
- `antique_camera.glb` — AntiqueCamera from Khronos glTF-Sample-Assets (CC-BY 4.0, UX3D)
- `ClearCoatTest.glb` — ClearCoatTest from Khronos glTF-Sample-Assets (CC-BY 4.0, Analytical Graphics/Ed Mackey)
- `box_textured.glb` — BoxTextured from Khronos glTF-Sample-Assets (CC-BY 4.0, Cesium)
- `fox.glb` — Fox animated model from Khronos glTF-Sample-Assets (mesh CC-0, animation CC-BY 4.0)
- `toy_car.glb` — ToyCar from Khronos glTF-Sample-Assets (CC-0)
- `da_vinci_workshop.glb` — Da Vinci Workshop from Sketchfab (CC-0)
- `leonardo_da_vinci.glb` — Leonardo da Vinci from Sketchfab (CC-0)
- `oceanic_currents.glb` — Oceanic Currents from Sketchfab (CC-0)
- `metric_tensor_riemann.glb` — Metric Tensor from Sketchfab (CC-0)
- `miller_indices_problem_2.glb` — Miller Indices from Sketchfab (CC-0)

Mirrored from PlayCanvas examples (for visual parity):
- `statue.glb` — PlayCanvas statue (CC-0)
- `geometry-camera-light.glb` — GLB with embedded camera + lights (CC-0)
- `playcanvas-cube.glb` — PlayCanvas logo cube (CC-0)
- `bitmoji.glb` — Bitmoji character for anim state graph (CC-0)
- `chess-board.glb` — Chess Board by Idmental, Sketchfab (**CC-BY 4.0**, see .txt)
- `terrain.glb` — Low-poly terrain, Sketchfab (**CC-BY 4.0**, see .txt)
- `robot-arm.glb` — Black Honey Robotic Arm, Sketchfab (**CC-BY 4.0**, Draco, see .txt)
- `glass-table.glb` — Low-poly glass table, Sketchfab (**CC-BY 4.0**, see .txt)
- `pbr-house.glb` — House 03 PBR, Sketchfab (**CC-BY 4.0**, see .txt)
- `house.glb` — House scene w/ generated UV1 for lightmapping, Sketchfab (**CC-BY 4.0**, see .txt)
- `SunglassesKhronos.glb` — Sunglasses from Khronos glTF-Sample-Assets (**CC-BY 4.0**)
- `apartment.glb` — Mirror's Edge Apartment by Aurélien Martel, Sketchfab (**CC-BY-NC 4.0 — NON-COMMERCIAL ONLY**, see .txt)
- `love.glb` — Love neon sign 02 by daysena, Sketchfab (**CC-BY 4.0**, see .txt)
- `cat.glb` — Egyptian Cat Statue by Ankledot, Sketchfab (**CC-BY 4.0**, webp textures re-encoded to png, see .txt)
- `biker.compressed.ply` — SuperSplat compressed gsplat from PlayCanvas (CC-0)
- `skull.compressed.ply` — SuperSplat compressed gsplat w/ SH from PlayCanvas (CC-0)

### cubemaps/ — complete
- `helipad-env-atlas.png` — 512x512 RGBP atlas generated from cannon-2k.hdr (CC-0, derived)
- `table-mountain-env-atlas.png` — 512x512 RGBP atlas generated from kloofendal-2k.hdr (CC-0, derived)

Generated with `tools/generate-env-atlas` using the engine's EnvLighting CPU pipeline.

## Sources

- [Poly Haven](https://polyhaven.com/) — HDRIs (CC-0)
- [ambientCG](https://ambientcg.com/) — PBR textures (CC-0)
- [Khronos glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) — reference models (mixed CC-0/CC-BY 4.0)
- Sketchfab — CC-0 models downloaded manually
