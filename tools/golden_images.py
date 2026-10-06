#!/usr/bin/env python3
"""Golden-image regression test for the example applications.

Renders a fixed set of deterministic examples with one backend, downscales each
frame 4x (box average) and compares it with the reference committed under
tests/golden/<backend>/<density>x/, the set for the display's pixel density. LOCAL ONLY: it needs a GPU and a window, so no CI preset
runs it; ctest runs it through the `golden` label (see the `golden` test presets).

    tools/golden_images.py --examples-dir build-examples/examples --backend metal
    tools/golden_images.py --examples-dir build-examples/examples --backend metal --update
    tools/golden_images.py ... --only clearcoat,gsplat

Every case runs under VISUTWIN_FIXED_DT, so an animated example reaches the same
state at the same frame in every run. There is one reference set PER DISPLAY
DENSITY: the example harness logs it ("Display pixel density 2"), and a capture is
compared with tests/golden/<backend>/<density>x/ (2x on a Retina display, 1x on a
standard one). The examples render at one pixel per point with the same MSAA on every
display, so the two sets hold the same images today; they stay separate so that a
density-dependent change is caught rather than averaged. VISUTWIN_MAX_PIXEL_RATIO and
VISUTWIN_ANTIALIAS, which change what a case renders, are removed from the examples'
environment. --update writes the set for the density it runs at. A density
with no set, or a capture whose size is not its reference's, is SKIPPED; if every
case is skipped the exit code is 77, which ctest reports as a skip.

A mismatch writes the capture, the reference and a difference image to --out.

Needs Python 3 with numpy and Pillow.
"""
import argparse
import json
import re
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile
import time

try:
    import numpy as np
    from PIL import Image
except ImportError as missing:
    sys.exit(f"golden_images.py needs numpy and Pillow in {sys.executable} ({missing}); "
             "install them there, or point CMake at another interpreter with -DPython3_EXECUTABLE=...")

ROOT = pathlib.Path(__file__).resolve().parent.parent
GOLDEN_DIR = ROOT / "tests" / "golden"

# Deterministic under a fixed timestep, and between them the features most often
# broken: shadows and cascades, clearcoat and the env atlas, DOF and the camera frame,
# baked lightmaps and text, splats, SSAO and clustered omni shadows, dynamic batching,
# clustered local lights.
CASES = [
    {"example": "shadow-cascades", "frame": 90},
    {"example": "clearcoat", "frame": 60},
    {"example": "depth-of-field", "frame": 90},
    {"example": "lightmap-sources", "frame": 120},
    {"example": "gsplat", "frame": 90},
    {"example": "ambient-occlusion", "frame": 90},
    {"example": "dynamic-batching", "frame": 150},
    {"example": "clustered-lighting", "frame": 90},
    # Moving spot lights over the clustered shadow atlas: the one case that catches
    # a per-rect atlas clear that stops clearing (shadows from every past light
    # position accumulate — Vulkan's depth-only pipelines forced LessEqual until
    # 2026-09-24). Every static scene renders the atlas the same either way.
    {"example": "clustered-spot-shadows", "frame": 60},
]

DOWNSCALE = 4
# A pixel "differs" when a channel moves more than PIXEL_THRESHOLD counts in the
# downscaled image; a case fails when more than MAX_DIFFERING_FRACTION of its pixels
# differ (a LOCAL change: an edge, an object, a shadow) or the mean absolute
# difference exceeds MAX_MEAN_DIFFERENCE (a GLOBAL one: exposure, a curve, a factor).
# Calibrated 2026-09-24 on Metal: seven of eight cases came back bit-identical run to
# run and the worst noise was 0.004 mean (ambient-occlusion). A probe that multiplied
# every lit colour by 1.03 moved no pixel past 12 counts but the mean by 0.26-1.17, so
# the mean limit has to sit well under that: 0.1 is ~25x the noise and catches it
# everywhere, where 0.5 let half of it through. Downscaling averages away the isolated
# pixels some Vulkan examples flip from run to run.
PIXEL_THRESHOLD = 12
MAX_DIFFERING_FRACTION = 0.002
MAX_MEAN_DIFFERENCE = 0.1

SKIP_EXIT_CODE = 77


def example_binary(examples_dir: pathlib.Path, example: str) -> pathlib.Path:
    name = f"visutwin-{example}"
    bundle = examples_dir / f"{name}.app" / "Contents" / "MacOS" / name
    return bundle if bundle.exists() else examples_dir / name


def capture(binary: pathlib.Path, backend: str, frame: int, out_png: pathlib.Path,
            log_path: pathlib.Path, timeout: float) -> bool:
    """Run one example until its screenshot is written, then stop it."""
    env = dict(os.environ)
    # A reference set stands for what its display density renders by default.
    env.pop("VISUTWIN_MAX_PIXEL_RATIO", None)
    env.pop("VISUTWIN_ANTIALIAS", None)
    env.update({
        "VISUTWIN_BACKEND": backend,
        "VISUTWIN_FIXED_DT": "0.0166667",
        "VISUTWIN_SCREENSHOT": str(out_png),
        "VISUTWIN_SCREENSHOT_FRAME": str(frame),
    })
    with open(log_path, "wb") as log:
        process = subprocess.Popen([str(binary)], env=env, stdout=log, stderr=subprocess.STDOUT,
                                   cwd=str(ROOT), start_new_session=True)
        deadline = time.time() + timeout
        last_size = -1
        try:
            while time.time() < deadline:
                if process.poll() is not None:
                    break
                if out_png.exists():
                    size = out_png.stat().st_size
                    if size > 0 and size == last_size:
                        return True
                    last_size = size
                time.sleep(0.25)
            return out_png.exists() and out_png.stat().st_size > 0
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()


def pixel_ratio_label(log_path: pathlib.Path) -> str:
    """The reference set for the display a run rendered on: "2x", "1x" ... from the harness's
    "Display pixel density D" line; "unknown" when the log has none."""
    match = re.search(r"Display pixel density ([0-9.]+)", log_path.read_text(errors="replace"))
    if not match:
        return "unknown"
    return f"{float(match.group(1)):g}x"


def downscale(image: Image.Image) -> Image.Image:
    width = image.width - image.width % DOWNSCALE
    height = image.height - image.height % DOWNSCALE
    return image.convert("RGB").crop((0, 0, width, height)).reduce(DOWNSCALE)


def compare(capture_img: Image.Image, reference_img: Image.Image):
    a = np.asarray(capture_img, dtype=np.int16)
    b = np.asarray(reference_img, dtype=np.int16)
    difference = np.abs(a - b)
    per_pixel = difference.max(axis=2)
    differing = float((per_pixel > PIXEL_THRESHOLD).mean())
    mean = float(difference.mean())
    ok = differing <= MAX_DIFFERING_FRACTION and mean <= MAX_MEAN_DIFFERENCE
    heat = Image.fromarray(np.clip(per_pixel * 4, 0, 255).astype(np.uint8))
    return ok, differing, mean, heat


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--examples-dir", required=True, type=pathlib.Path,
                        help="the build's examples directory (holds the visutwin-<name>.app bundles)")
    parser.add_argument("--backend", required=True, choices=["metal", "vulkan"])
    parser.add_argument("--update", action="store_true", help="capture and store new references")
    parser.add_argument("--only", default="", help="comma-separated example names")
    parser.add_argument("--out", type=pathlib.Path, default=None,
                        help="where failing captures go (default: <examples-dir>/golden-failures)")
    parser.add_argument("--timeout", type=float, default=90.0, help="seconds per example")
    args = parser.parse_args()

    only = {name for name in args.only.split(",") if name}
    cases = [case for case in CASES if not only or case["example"] in only]
    backend_dir = GOLDEN_DIR / args.backend
    manifests = {}   # reference set -> its manifest, read once, written back by --update

    def manifest_for(label: str) -> dict:
        if label not in manifests:
            path = backend_dir / label / "manifest.json"
            manifests[label] = json.loads(path.read_text()) if path.exists() else {}
        return manifests[label]

    out_dir = args.out or (args.examples_dir / "golden-failures")

    failures, skipped, passed = [], [], []
    with tempfile.TemporaryDirectory(prefix="visutwin-golden-") as scratch_name:
        scratch = pathlib.Path(scratch_name)
        for case in cases:
            example = case["example"]
            binary = example_binary(args.examples_dir, example)
            if not binary.exists():
                failures.append(f"{example}: no binary at {binary}")
                continue
            shot = scratch / f"{example}.png"
            log = scratch / f"{example}.log"
            if not capture(binary, args.backend, case["frame"], shot, log, args.timeout):
                failures.append(f"{example}: no screenshot within {args.timeout:.0f} s (log below)\n"
                                + log.read_text(errors="replace")[-2000:])
                continue
            full = Image.open(shot)
            small = downscale(full)
            label = pixel_ratio_label(log)
            reference_dir = backend_dir / label
            manifest = manifest_for(label)

            if args.update:
                if label == "unknown":
                    failures.append(f"{example}: the log names no pixel ratio, so no reference set to store in")
                    continue
                reference_dir.mkdir(parents=True, exist_ok=True)
                small.save(reference_dir / f"{example}.png", optimize=True)
                manifest[example] = {"capture_size": [full.width, full.height], "frame": case["frame"]}
                print(f"  stored  {label}/{example}  ({full.width}x{full.height} -> {small.width}x{small.height})")
                continue

            reference_path = reference_dir / f"{example}.png"
            if not reference_path.exists() or example not in manifest:
                if any((backend_dir / other / f"{example}.png").exists()
                       for other in ("1x", "2x") if other != label):
                    skipped.append(f"{example}: rendered at {label}, which has no reference set "
                                   f"(run --update on a {label} display to create one)")
                else:
                    failures.append(f"{example}: no reference (run with --update to create one)")
                continue
            expected_size = manifest[example]["capture_size"]
            if [full.width, full.height] != expected_size:
                skipped.append(f"{example}: captured at {full.width}x{full.height}, the {label} reference at "
                               f"{expected_size[0]}x{expected_size[1]} (the window changed size)")
                continue
            ok, differing, mean, heat = compare(small, Image.open(reference_path))
            line = f"{example} ({label}): {differing * 100:.3f}% of pixels differ, mean |d| {mean:.3f}"
            if ok:
                passed.append(line)
            else:
                out_dir.mkdir(parents=True, exist_ok=True)
                small.save(out_dir / f"{example}-capture.png")
                shutil.copy(reference_path, out_dir / f"{example}-reference.png")
                heat.save(out_dir / f"{example}-difference.png")
                failures.append(f"{line}  (limits {MAX_DIFFERING_FRACTION * 100:.1f}% / {MAX_MEAN_DIFFERENCE}); "
                                f"images in {out_dir}")

    if args.update:
        for label, manifest in manifests.items():
            if manifest and label != "unknown":
                path = backend_dir / label / "manifest.json"
                path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
                print(f"references for {args.backend} at {label} written to {path.parent}")
        for line in failures:
            print(f"  FAIL  {line}")
        return 1 if failures else 0

    for line in passed:
        print(f"  ok    {line}")
    for line in skipped:
        print(f"  skip  {line}")
    for line in failures:
        print(f"  FAIL  {line}")
    if failures:
        return 1
    if skipped and not passed:
        return SKIP_EXIT_CODE
    return 0


if __name__ == "__main__":
    sys.exit(main())
