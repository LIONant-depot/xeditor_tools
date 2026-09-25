# xeditor_tools

Shared 3D editor-viewport scaffolding: the orbit camera (with WASD/QE fly) and the ground
grid/shadow-map every resource-editor preview panel needs, factored out of xskeleton.plugin (where it
first lived) so any editor can use it without depending on a skeleton plugin.

Sits on top of `xeditor` (the editor framework) and `xGPU` (the renderer) as dependencies - not
purist about it, just the two things this actually needs.

## Layout

Same shape as `xbmp_tools`: one umbrella `.cpp` a consumer adds to its own build, `#include`-ing the
`Details/*.cpp` implementation files; no static library is produced.

- `src/xeditor_tools_camera.h` / `src/Details/xeditor_tools_camera.cpp` — the orbit+fly camera
- `src/xeditor_tools_grid.h` / `src/Details/xeditor_tools_grid.cpp` — the ground grid + shadow map
- `src/xeditor_tools.cpp` — the single file a consuming target adds to its own source list
- `src/shaders/` — the grid shader source (compiled by the host project's own shader pipeline, same as
  every other plugin's shaders)

## Camera

`xeditor_tools::camera` — right-drag rotates, middle-drag pans, the wheel zooms (the same orbit
behavior every editor already had), extended with WASD (forward/back/strafe) and Q/E (down/up) flying,
always active while the viewport is hovered (no held key needed to enable it). Flying shifts the
camera's target by a world-space vector, the same trick middle-mouse-pan already used, so rotate/pan/
zoom keep working identically afterward.

## Grid

`xeditor_tools::grid` — the ground plane and the shared shadow-map render target. It does not cast
shadows itself: a consumer draws its own geometry (a mesh, a skinned mesh, bone wedges - all resource-
type specific) into `m_ShadowPass` with its own shadow-caster pipeline, then calls `Draw()` for the
ground plane to receive it.
