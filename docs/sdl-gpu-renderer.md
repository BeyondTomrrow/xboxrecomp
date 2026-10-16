# SDL3 GPU renderer (Linux hardware rendering)

Status: milestone 0 done (device, surfaces, colour clears, downloads;
off unless `RECOMP_SDLGPU=1`). Test: `tests/sdlgpu_posix`.

## Why

On Windows, NV2A draws go to Direct3D 11 (`kernel/nv2a_gpu_d3d11.inc.c`). On
Linux they fall back to the software rasterizer in `nv2a_pb_exec.c`, which is
fine for menus and far too slow for a match. This renderer gives Linux the
same hardware path through SDL3's GPU API (Vulkan on Linux, Metal on macOS).

## The contract

`nv2a_pb_exec.c` talks to a hardware backend through five functions. The
SDL3 renderer implements exactly these and nothing else is visible to the
executor:

| Function | Called | Must |
|---|---|---|
| `gpu_enabled()` | everywhere | say whether hardware rendering is on |
| `gpu_draw_batch(flat2d)` | END of a primitive batch | draw it and return 1, or return 0 to let the software rasterizer draw it |
| `gpu_clear(param)` | NV097_CLEAR_SURFACE | clear and return 1, or return 0 for the software path |
| `gpu_flip()` | FLIP_STALL | finish the frame (bring it back to guest RAM) |
| `gpu_cpu_access()` | before the software path touches a surface | make guest RAM current |

Returning 0 is always allowed. That is what lets the renderer grow one
feature at a time: anything it does not handle yet is drawn exactly as it is
today.

## Ownership of a surface

A render target lives in guest RAM (the title reads and writes it) and, once
the renderer touches it, in a GPU texture. Each surface has one owner at a
time:

- **GUEST**: guest RAM is current. Before a GPU draw, upload it.
- **GPU**: the texture is newer. Before the CPU (software rasterizer, flip,
  title) reads it, download it.

`gpu_cpu_access()` and `gpu_flip()` download every GPU-owned surface. This
one rule replaces all the special cases.

## Presenting

Version 1 does not present from the GPU. `gpu_flip()` downloads the frame
into guest RAM, and the existing presenter (`video/fb_present.c`, snapshot at
flip) shows it. That avoids a second owner for the window (the Wayland
"surface already exists" failure) and keeps one presentation path on Linux.
Presenting straight from the GPU texture is a later optimisation.

## Shaders

The D3D11 backend uses one fixed shader (`nv2a_gpu_d3d11.hlsl`); everything
per draw, register combiners included, arrives in a constant buffer. The SDL3
renderer ports it once to GLSL (`kernel/sdlgpu/nv2a_sdlgpu.vert/.frag`),
compiled to SPIR-V at build time with `glslangValidator` and embedded. No
shader compiler at run time.

The D3D11 geometry shader (per-triangle depth reconstruction) has no SDL3
equivalent. Vertices are already transformed on the CPU, so the same three
values (projected corners, depth slope, clipped flag) are computed per
triangle on the CPU and passed as flat vertex attributes.

## Files

All under `src/kernel/sdlgpu/`, included into `nv2a_pb_exec.c` through
`nv2a_gpu_sdl3.inc.c` like the D3D11 backend (they need the executor's
state). One responsibility per file:

| File | Owns |
|---|---|
| `sdlgpu.inc.c` | device, lock, the five entry points |
| `sdlgpu_surface.inc.c` | render targets, upload/download, ownership |
| `sdlgpu_texture.inc.c` | NV2A texture decode and cache |
| `sdlgpu_pipeline.inc.c` | blend/depth/stencil/raster state to pipeline cache |
| `sdlgpu_draw.inc.c` | vertex conversion, per-triangle data, draw |
| `nv2a_sdlgpu.vert/.frag` | the ported shader |

Conventions: `sg_` prefix for everything in the renderer; all state in one
`s_sg` struct; no function longer than a screen where it can be split;
every fallback to software is a single `return 0` with a comment saying why.

## Milestones

Each one is testable on its own, on real hardware or on Mesa's lavapipe
(software Vulkan) in CI.

0. **Device and surfaces.** Headless device, surface cache with ownership,
   color clears on the GPU, download on flip/CPU access. Test: GPU clear then
   download equals the software clear, byte for byte.
1. **Flat 2D draws.** Shader pipeline in the build, untextured and textured
   pre-transformed triangles (`flat2d`), alpha blending. Menus and HUD go to
   the GPU. Test: rendered frames match the software rasterizer within a
   small tolerance.
2. **3D draws.** Depth/stencil, the per-triangle depth data, all blend and
   compare modes, the four texture stages, register combiners. A match goes
   to the GPU.
3. **Formats and caching.** Every NV2A texture format, swizzle/tiling,
   texture cache with invalidation on guest writes.
4. **Present from the GPU.** Skip the download when nothing on the CPU needs
   the frame.
