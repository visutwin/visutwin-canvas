# Security Policy

## Supported versions

VisuTwin Canvas is in alpha and has no release branches yet. Security fixes land
on `main` only; please reproduce against the latest `main` before reporting.

| Version | Supported |
|---|---|
| `main` | Yes |
| Anything older | No |

## Reporting a vulnerability

**Please do not report security vulnerabilities through public issues,
discussions or pull requests.**

Report them privately instead, by either:

- GitHub's private vulnerability reporting: the **Report a vulnerability**
  button on the repository's
  [Security tab](https://github.com/visutwin/visutwin-canvas/security/advisories/new), or
- email to **ale@visutwin.com**.

Please include as much of the following as you can:

- the kind of issue (for example a buffer overflow in an asset parser, an
  out-of-bounds GPU read, a crash on malformed input);
- the affected source files and the commit you tested;
- the platform and backend (`metal` or `vulkan`, macOS or Linux);
- a minimal file or step-by-step reproduction (a crafted glTF, PLY, KTX2, DDS
  or OBJ file is ideal) and, if possible, a sanitizer trace from the `sanitize`
  preset;
- the impact as you understand it.

## What to expect

- An acknowledgement within **5 working days**.
- An assessment and, for a confirmed issue, a planned fix within **30 days**,
  with progress updates along the way.
- Credit in the fix's advisory, unless you prefer to stay anonymous.

Please give us a reasonable chance to fix the issue before disclosing it
publicly.

## Scope

In scope: the engine library in this repository, including its asset loaders
and parsers (glTF/GLB, OBJ, STL, PLY, KTX2/Basis, DDS, images and fonts), the
shader toolchain and the Metal and Vulkan backends.

Out of scope: vulnerabilities in third-party dependencies (report those to
their projects; vcpkg pins the versions we use), and the example applications,
which are demos and not meant to load untrusted input.
