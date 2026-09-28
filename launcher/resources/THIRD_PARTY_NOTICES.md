# Third-Party Notice Inventory

This file describes the third-party software and notice delivery used by MojoRecomp.
The complete text set is stored under `resources/licenses/`, embedded into the
launcher at build time, and can be materialized by the user from **Support & FAQ ->
Licenses & Notices**. The materialized copy is written under
`%LOCALAPPDATA%\MojoRecomp\licenses\`, keeping the application directory
single-executable while still making the notices directly accessible.

The dependency-text inventory is generated from the locked Windows dependency
graphs. Original MojoRecomp code is separately licensed under ISC; that grant
does not apply to the third-party material inventoried here, generated/original
game code, game data, or game artwork. The current artwork set and its provenance
are tracked in `launcher/public/art/SOURCES.md`.

## Verified Native / Recompilation Dependencies

### XenonRecomp

- License: MIT.
- Pinned commit: `ddd128bcca99fe8bfbb99bea583c972351fa6ace`.
- Local source: `thirdparty/XenonRecomp-src/LICENSE.md`.
- Copyright notice in the pinned license: `Copyright (c) 2025 hedge-dev and contributors`.

The runtime links selected XenonUtils functionality from this checkout. The final
runtime link map confirms that XEX/image code, TinySHA1, and tiny-AES-c content are
present. libmspack LZX is imported from the separate replaceable DLL described below.
XenonRecomp's GNU-binutils-derived `disasm` library is built by the local
recompilation toolchain but contributes no object code to the shipped COT runtime in
the audited link map.

### XenosRecomp

- License: MIT.
- Pinned commit: `990d03b28a27b50277ee5d8d942e1c5f873869d1`.
- Local source: `thirdparty/XenosRecomp-src/LICENSE.md`.
- Copyright notice in the pinned license: `Copyright (c) 2025 hedge-dev and contributors`.

MojoRecomp compiles the required Xenos shader-recompiler source directly into the COT
runtime. The runtime intentionally does not link XenosRecomp's smol-v/zstd cache
pipeline.

### NVIDIA FXAA 3.11

- Component: FXAA 3.11 Quality algorithm by Timothy Lottes / NVIDIA.
- License: BSD-3-Clause-style NVIDIA redistribution terms.
- Use in MojoRecomp: the Vulkan presenter contains an adapted Quality edge-orientation,
  span-search and subpixel implementation for the `FXAA` and `FXAA Extreme` modes.
- Preserved notice: `resources/licenses/NVIDIA-FXAA-LICENSE.txt`.

### SDL3

- License: SDL's zlib-style license notice.
- Audited headers identify version `3.5.0`.
- Local notice source: `thirdparty/sdl3/include/SDL3/SDL_copying.h`.
- The pinned notice identifies `Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>`.

### Runtime support libraries

The audited runtime also incorporates or compiles against the following material,
whose texts are preserved under `resources/licenses/`:

- `{fmt}` `11.0.2` - MIT;
- `toml++` - MIT;
- SIMDe headers - MIT-style terms from the pinned checkout;
- TinySHA1 - permissive notice preserved from the pinned XenonRecomp copy;
- tiny-AES-c - Unlicense;
- libmspack LZX decompressor, pinned through XenonRecomp at commit
  `305907723a4e7ab2018e58040059ffb5e77db837` - LGPL 2.1. It is shipped as
  the replaceable `mojorecomp-lzx.dll`, not linked into `cot-runtime.exe`;
- LLVM compiler-rt builtins from Clang/LLVM `22.1.8` - Apache-2.0 WITH
  LLVM-exception material preserved from the matching upstream tag;
- xxHash is an explicit link input of the current runtime build. The audited map
  contains no xxHash object contribution, but its BSD-2-Clause license is retained
  conservatively in the notice payload.

### FFmpeg libraries used by the COT runtime

MojoRecomp no longer depends on the earlier opaque archived FFmpeg libraries for its
public build path. `setup.bat` checks out the public `wmarti/FFmpeg` repository at
commit `0604b464c7cb4ebc94940cf1f324a3b26b87717c` and rebuilds the narrow Windows
x86_64 library set through `tools/ffmpeg-rexglue/` into the ignored
`thirdparty/ffmpeg-build/` directory. The selected libavcodec and libavutil objects
are combined into the replaceable `mojorecomp-ffmpeg.dll`. No FFmpeg object code is
linked into `cot-runtime.exe`.

The pinned source configuration is:

```text
--toolchain=msvc --arch=x86_64 --disable-everything --disable-programs --disable-all --disable-x86asm --disable-autodetect --disable-network --enable-avcodec --enable-avformat --enable-avutil --enable-decoder='mp3,mp3float,wmav2,xmaframes' --enable-parser=mpegaudio --enable-demuxer='asf,mp3' --enable-protocol=file
```

The corresponding generated configuration explicitly records `CONFIG_GPL=0`,
`CONFIG_NONFREE=0`, `CONFIG_VERSION3=0`, and `CONFIG_GPLV3=0`.

The headers identify `libavcodec 58.134.100` and `libavutil 56.70.100`, matching the
FFmpeg 4.4 API generation. The LGPL 2.1 text and FFmpeg license summary are included
in the notice payload.

Each release produces one `MojoRecomp-LGPL-Sources-<version>.zip` archive beside the
portable binary. It contains the exact pinned upstream source, maintained MojoRecomp
build recipe, and applicable LGPL material for both FFmpeg and libmspack. Its hash is
listed in the release `SHA256SUMS.txt` file.

Compatible modified builds can be placed under
`%LOCALAPPDATA%\MojoRecomp\lgpl-overrides\` using the shipped filenames
`mojorecomp-ffmpeg.dll` and `mojorecomp-lzx.dll`. The launcher copies an override
into the active versioned runtime launch directory instead of the official component
copy. Removing the override restores the verified official library on the next launch.

### extract-xiso

- Upstream: XboxDev/extract-xiso.
- Bundled utility version: `2.7.1`.
- The pinned upstream `LICENSE.TXT` describes a modified Berkeley/BSD-style license
  and requires preservation of its copyright/conditions/disclaimer for binary
  redistribution.
- The matching license text is preserved in the development dependency/notice
  material and is embedded in the launcher's materializable notice payload.

## Launcher Dependencies

The current pinned launcher dependency metadata reports, among other direct
dependencies:

- Tauri: Apache-2.0 OR MIT;
- Tauri Build: Apache-2.0 OR MIT;
- Tauri Dialog Plugin: Apache-2.0 OR MIT;
- reqwest: MIT OR Apache-2.0;
- serde / serde_json: MIT OR Apache-2.0;
- sha2: MIT OR Apache-2.0;
- semver: MIT OR Apache-2.0;
- toml: MIT OR Apache-2.0;
- zip: MIT;
- Svelte: MIT;
- Vite: MIT;
- `@tauri-apps/api`: Apache-2.0 OR MIT;
- `@tauri-apps/plugin-dialog`: MIT OR Apache-2.0.

`scripts/generate-third-party-notices.mjs` resolves the locked Windows Cargo graph
and the installed Windows npm graph, separates runtime from build-time dependencies,
collects package-provided license/notice files, and fails if a required package has
no resolved notice text. Duplicate text bodies are hash-deduplicated while package
metadata retains the applicable license expression and source URL.

The current generated inventories are:

- `Cargo-ThirdPartyNotices.txt`: 321 packages (301 runtime, 20 build-time), 196
  unique license/notice bodies;
- `Npm-ThirdPartyNotices.txt`: 45 installed Windows packages (21 runtime, 24
  build-time), 37 unique license/notice bodies.

The MojoRecomp Launcher package itself is intentionally excluded from these
third-party inventories because original launcher code is covered separately by
the project's ISC License.

## DXC / DXIL

The Windows launcher embeds `dxcompiler.dll` and `dxil.dll` from the pinned
`XenosRecomp-src/thirdparty/dxc-bin` tree and materializes them internally at
runtime.

The XenosRecomp `dxc-bin` submodule is pinned at
`737ac9f51c3d06f0cd1ee7e14dc065bafd52310d`, whose history updates the Windows
payload to DXC `1.8.2407`. The shipped DLL identities are:

- `dxcompiler.dll` `1.8.2407.7` (`416fab6b5`);
- `dxil.dll` `101.8.2407.12` (`release/github-release-1.8.2407`, `57a0a4370`).

The matching DirectXShaderCompiler `v1.8.2407` license and third-party notice text
are now included as `DXC-LICENSE.txt` and `DXC-ThirdPartyNotices.txt`.

## Vulkan Headers

The runtime compiles against Vulkan-Headers `1.4.357` (`VK_HEADER_VERSION 357`).
The matching upstream `v1.4.357` dual Apache-2.0/MIT license material is preserved
in the launcher notice payload.

## Notice Delivery

The source notice directory and root project license are compiled into the Tauri
launcher rather than copied as loose files beside `mojorecomp-launcher.exe`. The
Support page exposes a **Licenses & Notices** action that materializes the project
`LICENSE`, this summary, and every embedded third-party license/notice file to the
launcher's LocalAppData notice directory and opens that directory in Explorer. Rust
tests verify that the project license, generated Cargo/npm inventories, and key
native notices are embedded and can be materialized.

Packages whose installed Cargo/npm archive does not expose a usable root license
file are resolved through the audited texts in
`resources/license-overrides/`. Their locked-package mappings, upstream/canonical
sources, and hashes are documented in `resources/license-overrides/SOURCES.md`;
release validation pins the override file hashes.

## Project License

Original MojoRecomp code is licensed under the ISC License in the repository
root unless an individual file states different terms. The license applies only
to material whose copyright belongs to the MojoRecomp project. It does not
relicense third-party components, generated guest/PPC output or original game
code, game data, launcher game artwork, names, characters, logos, trademarks, or
other material owned by third parties.

The launcher embeds the project ISC text separately from this third-party
inventory. **Licenses & Notices** materializes `LICENSE`, this summary, and all
embedded third-party license/notice texts under
`%LOCALAPPDATA%\MojoRecomp\licenses\`.
