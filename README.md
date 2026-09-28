<div align="center">
  <img src="launcher/public/art/mojorecomp.png" width="190" alt="MojoRecomp logo">
  <h1>MojoRecomp</h1>
  <p>Native Windows ports of the Crash Bandicoot Titans games.</p>
</div>

> [!IMPORTANT]
> MojoRecomp does not include the original game. You must provide your own legally obtained Xbox 360 copy of **Crash of the Titans**.

## About

MojoRecomp is an unofficial native PC port project. It recompiles the Xbox 360 game code and provides the Windows runtime for graphics, audio, input, files, saves, and video playback. It is not an Xbox 360 emulator.

XenonRecomp translates the PowerPC game code, and XenosRecomp translates the Xbox 360 shaders. MojoRecomp provides its own Windows runtime. ReXGlue served as a technical reference during early runtime development. MojoRecomp's narrow FFmpeg build recipe and a small set of codec/parser list files are adapted from ReXGlue SDK v0.10.0 and retain its BSD-3-Clause notice.

## Status

| Component | Version | Status |
| --- | --- | --- |
| MojoRecomp Launcher | `1.0.0` | 🚧 Pre-release |
| Crash of the Titans | `0.1.0-alpha` | ✅ Playable |
| Crash: Mind over Mutant | `0.0.0-dev` | 🚧 In development |

Crash of the Titans boots, plays, saves, and supports its original audio and videos. The runtime remains an alpha release while it is tested on more hardware and game regions. Gameplay runs at the original 30 FPS.

Mind over Mutant has a launcher entry but no playable runtime or release date yet.

## Features

- Portable launcher with guided ISO setup
- Vulkan renderer and XAudio output
- Original FMV playback
- Controller and keyboard input
- Windowed and borderless fullscreen modes
- 1x, 2x, and 3x internal resolution scaling
- 4:3, 16:9, 16:10, 21:9, and 32:9 aspect ratios
- FXAA and anisotropic texture filtering
- Original game languages and an optional Brazilian Portuguese localization pack
- Separate game files, saves, settings, cache, logs, and support reports

## Requirements

- 64-bit Windows 10 or Windows 11
- A Vulkan 1.3 capable GPU with a current driver
- Microsoft Edge WebView2 Runtime
- An Xbox 360 Crash of the Titans ISO
- A controller is recommended, but keyboard input is available

### Platform support

| Operating system | Architecture | Status | Notes |
| --- | --- | --- | --- |
| Windows 10/11 | x86-64 (x64) | ✅ Supported | Official release target |
| Windows 10/11 | ARM64 | ❌ Not available | The native runtime has not been ported or tested |
| Windows | x86 (32-bit) | ❌ Unsupported | The runtime requires a 64-bit host address space |
| Linux | x86-64 (AMD64) | ❌ Not available | The launcher and runtime have not been ported or tested |
| Linux | ARM64 | ❌ Not available | The launcher and runtime have not been ported or tested |

No additional platform ports are currently announced.

## Setup

1. Extract the MojoRecomp release.
2. Open `mojorecomp-launcher.exe`.
3. Choose a location for the `MojoRecomp-Games` library.
4. Select **Crash of the Titans** and click **SET UP**.
5. Select your `.iso` file and wait for extraction and validation to finish.
6. Click **PLAY**.

The launcher does not modify the ISO. You can move the game library later from **Launcher Settings**.

## Languages

| Language | Source |
| --- | --- |
| English | Original game files |
| Deutsch | Original game files |
| Français | Original game files |
| Español | Original game files |
| Italiano | Original game files |
| Nederlands | Original game files |
| Português Brasileiro | Optional MojoRecomp localization pack with English voices |

Available original languages depend on the ISO region. The launcher identifies localization packs that still need to be installed.

## Graphics and launcher settings

| Setting | Options |
| --- | --- |
| Display mode | Windowed, Borderless fullscreen |
| Resolution scale | 1x (720p), 2x (1440p), 3x (2160p/4K) |
| Aspect ratio | 4:3, 16:9, 16:10, 21:9, 32:9 |
| VSync | On, Off |
| Anti-aliasing | Off, FXAA, FXAA Extreme |
| Texture filtering | Default, 1x, 2x, 4x, 8x, 16x |
| Logging | On, Off |

Aspect ratios other than 16:9 are experimental. Unsupported texture-filtering levels are disabled automatically.

## Controls

Controllers are detected while the game is running.

| Xbox control | Keyboard |
| --- | --- |
| Left stick | `W` `A` `S` `D` |
| Right stick | Arrow keys |
| D-pad | `Shift` + Arrow keys |
| A | `J` or `Space` |
| B | `K` |
| X | `U` |
| Y | `I` |
| Left / Right bumper | `Z` / `C` |
| Left / Right trigger | `Q` / `E` |
| Left / Right stick click | `F` / `R` |
| Start / Back | `Enter` / `Tab` |

### Debug shortcuts

| Key | Action | Debug Mode required |
| --- | --- | --- |
| `F1` × 10 | Toggle Debug Mode | ❌ No |
| `F2` | Unlock all moves | ✅ Yes |
| `F3` | Toggle 4x fast-forward | ✅ Yes |
| `F4` | Unlock all episodes | ✅ Yes |
| `F6` | Pause or resume | ✅ Yes |
| `F7` | Advance one frame while paused | ✅ Yes |
| `F9` | Toggle the performance overlay | ❌ No |

> [!CAUTION]
> Unlock shortcuts change save progress. Back up your save first through **Open Save Folder** in the launcher.

## Storage

The game library defaults to:

```text
%USERPROFILE%\Games\MojoRecomp-Games\
|-- cot\
|-- mom\
`-- .mojorecomp\
    |-- components\
    |   |-- runtime.cot\
    |   `-- runtime.mom\
    |-- downloads\
    `-- staging\
```

Versioned game runtimes and other game components follow the selected library. Downloads and component/game staging also stay inside `.mojorecomp` so changing the library keeps the managed game installation self-contained.

Saves are stored separately:

```text
%USERPROFILE%\Saved Games\MojoRecomp\cot\
```

Launcher settings, cache, logs, crash reports, diagnostics, launcher-update state, support packages, license notices, and optional LGPL overrides are stored under:

```text
%LOCALAPPDATA%\MojoRecomp\
```

The launcher verifies a library move before switching to the new location. Versioned game components move with that library. Saves remain separate. Existing development-era `games` and `userdata` folders are migration inputs only.

## Troubleshooting

The **Support & FAQ** page can open the save and log folders and create a sanitized support package. Support packages exclude saves, ISOs, and extracted game assets.

Some OBS and Streamlabs Vulkan capture hooks can corrupt the runtime during startup. MojoRecomp blocks the affected hook inside the game process. Use **Window Capture** or **Display Capture** when needed.

## Distribution

The official package is a portable Windows x64 application. Crash of the Titans also has a separately versioned runtime component containing the native runtime, its required DLLs, and the ISO extraction utility. The current pre-release launcher retains its embedded bootstrap until the public HTTPS component feed has been validated; once a verified `runtime.cot` component is active, it is the preferred runtime source.

Release packages are written to the ignored `.release\` directory with the portable launcher, runtime component, update catalog, checksums, and one versioned LGPL corresponding-source archive covering the replaceable FFmpeg and libmspack libraries.

## Building from source

The Windows build requires Git, Python, Node.js/npm, Rust/Cargo, CMake, Ninja, Visual Studio C++ tools, LLVM/Clang 22.1.8, and the dependencies listed in the third-party notices.

Main directories:

```text
config/       Recompiler configuration and function boundaries
launcher/     Tauri, Rust, Svelte, and TypeScript launcher
patches/      Maintained dependency patches
runtime/      Native runtime, renderer, audio, input, and tests
tools/        Analysis and recompilation tools
thirdparty/   Local dependencies and toolchains (not tracked)
```

Place a development copy of `default.xex` in the ignored `game\` directory. These scripts cover the development workflow:

| Script | Purpose |
| --- | --- |
| `setup.bat` | Downloads, patches, and builds the pinned development dependencies |
| `tools\analyze.bat` | Analyzes the XEX and refreshes the switch-table data |
| `tools\recompile.bat` | Translates the game code into local C++ sources |
| `build-smoke.bat` | Builds the runtime and runs a basic mapping and link check |
| `release.bat` | Creates and validates a local release candidate under `.release\` |

Launcher checks:

```bat
cd launcher
npm.cmd ci
npm.cmd run check
cargo test --manifest-path src-tauri\Cargo.toml
npm.cmd run build:production
```

Game files, generated PPC sources, toolchains, builds, saves, logs, and test dumps are excluded from Git.

## Credits

**Alex "OAleex" Félix** — creator and lead developer

MojoRecomp uses work from the following projects and their contributors:

- [ReXGlue](https://github.com/rexglue/rexglue-sdk)
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp)
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp)
- [Tauri](https://github.com/tauri-apps/tauri), [Svelte](https://github.com/sveltejs/svelte), and Rust
- [SDL](https://github.com/libsdl-org/SDL)
- [FFmpeg](https://github.com/FFmpeg/FFmpeg)
- [LLVM](https://github.com/llvm/llvm-project) and [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler)
- [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers)
- [XboxDev/extract-xiso](https://github.com/XboxDev/extract-xiso)
- `{fmt}`, `toml++`, SIMDe, libmspack, TinySHA1, tiny-AES-c, xxHash, and launcher dependencies

Exact versions and license texts are listed in [Third-Party Notices](launcher/resources/THIRD_PARTY_NOTICES.md). Artwork sources are recorded in [Launcher Artwork Sources](launcher/public/art/SOURCES.md).

## AI usage

AI tools assisted with research, repetitive implementation work, debugging, tests, build automation, and early documentation drafts. Alex "OAleex" Félix directed the work, tested the game, reviewed the changes, and made the final decisions.

## Legal

MojoRecomp is an unofficial fan project for research and preservation. It is not affiliated with or endorsed by Activision, Microsoft, Radical Entertainment, Sierra Entertainment, or any other rights holder.

Original MojoRecomp code is available under the [ISC License](LICENSE) unless a file states otherwise. Third-party software keeps its own license. The ISC License does not cover original game code or data, generated guest code, ISOs, XEX files, videos, audio, textures, saves, names, characters, logos, or artwork.

Crash Bandicoot, Crash of the Titans, Crash: Mind over Mutant, and related material belong to their respective owners. Users must provide their own legally obtained supported game copy.
