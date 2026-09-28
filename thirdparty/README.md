# Third-Party Sources

MojoRecomp keeps upstream source dependencies as Git submodules pinned to exact
revisions. Project-specific changes remain under `patches/` and are applied only
to generated copies under `thirdparty/work/`, leaving the submodules pristine.

| Dependency | Pinned revision |
| --- | --- |
| XenonRecomp | `ddd128bcca99fe8bfbb99bea583c972351fa6ace` |
| XenosRecomp | `990d03b28a27b50277ee5d8d942e1c5f873869d1` |
| FFmpeg | `0604b464c7cb4ebc94940cf1f324a3b26b87717c` |
| SDL | `8bf3b7215ad9fc3deb583c6a3a37c6c67f2e24e4` |
| Vulkan-Headers | `e3b1eec08173d6b825cd3ac88c885a63b621504a` (`v1.4.357`) |
| extract-xiso | `1766b46fb0638c70e13a3a429093c2c9376ce8fe` (`v2.7.1`) |

Initialize the source tree with:

```bat
git submodule update --init --recursive
```

`thirdparty/build/` contains generated build output. `thirdparty/work/` contains
temporary copies of XenonRecomp, XenosRecomp, and extract-xiso with the
MojoRecomp patches applied. Both directories are ignored, so the source
submodules remain pristine.

The local LLVM/Clang toolchain remains under the ignored `thirdparty/llvm22/`
directory because it is a prebuilt development toolchain rather than a source
dependency of the shipped runtime.
