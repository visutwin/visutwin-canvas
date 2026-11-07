## Summary

<!-- What does this change do, and why? Link the issue it addresses (Fixes #...). -->

## Upstream

<!-- If this ports or follows upstream behaviour, name the upstream code or example.
     Mark any intentional divergence with a `DEVIATION:` comment in the code. -->

## Testing

<!-- What did you run? Tick what applies. -->

- [ ] Builds with zero warnings (`cmake --build --preset default`)
- [ ] Unit tests pass (`ctest --preset default`)
- [ ] Vulkan build and tests (`cmake --preset vulkan`, `ctest --preset vulkan`)
- [ ] Vulkan validation smoke test (`ctest --preset vulkan-smoke`)
- [ ] Golden images (`ctest --preset golden`); re-captured on both backends if the change is intended to alter rendering
- [ ] Checked affected examples on **both** Metal and Vulkan
- [ ] Added or updated a unit test for the change

## Rendering changes

<!-- Before/after screenshots (VISUTWIN_SCREENSHOT) for anything visible.
     Delete this section if nothing renders differently. -->

## Checklist

- [ ] New files carry the SPDX / copyright header
- [ ] Rules or contracts this change introduces are recorded in `AGENTS.md`, subsystem detail in `ARCHITECTURE.md`
- [ ] `FEATURES.md` updated if a user-facing feature was added or changed
