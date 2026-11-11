# Snapmaker GPU toolpath pipeline (ported)

This folder is a port of the **Snapmaker OrcaSlicer** G-code preview render
pipeline (`Snapmaker/OrcaSlicer`, *Render engine build - v2.4.1*, commit
`6e07486c42`, GUI/`PathData.*`, GUI/`PathRenderer.*`, GUI/`BufferTexture.*`
and `resources/shaders/140/gpu_path*`).

The original pipeline replaces the classic "one vertex buffer per toolpath"
preview with a **de-geometrized** renderer:

* the CPU stores only path points and small per-move attribute tables,
* one instanced draw per layer expands every path step into a 10-vertex prism
  in the vertex shader (`gl_VertexID` + TBO fetches),
* colors are sampled per fragment from a gradient/palette LUT,
* option markers (seam / retract / tool change / ...) are instanced diamonds
  placed entirely in the vertex shader.

That removes the huge VBOs and the CPU-side geometry rebuilds that made the
old preview slow when zooming into dense models.

The original Snapmaker code is licensed under the AGPLv3, the same license as
this project; the port keeps the Snapmaker attribution in every file header.

## Files

| File | Role |
| --- | --- |
| `SnapmakerBufferTexture.*` | TBO wrapper (raw float tables) |
| `SnapmakerPathData.*` | layer stack, nodes/groups/steps, attribute tables |
| `SnapmakerPathRenderer.*` | instanced prism/marker meshes, LUTs, draw calls |
| `SnapmakerPathView.*` | Wonderprint adapter: maps the libvgcode viewer onto the pipeline |
| `resources/shaders/140/snapmaker_gpu_path.*` | toolpath shaders |
| `resources/shaders/140/snapmaker_gpu_path_marker.*` | option marker shaders |

## Integration in Wonderprint-Orca

Wonderprint-Orca drives its preview with **libvgcode** (data model, layer/move
sliders, legend, export). The port therefore keeps libvgcode untouched and
only replaces the *toolpath drawing*:

* `GCodeViewer::load_as_gcode()` builds the Snapmaker stack from the same
  `GCodeProcessorResult` right after `m_viewer.load()`.
* `GCodeViewer::render_toolpaths()` asks `SnapmakerPathView::SyncFromViewer()`
  to mirror the current view type, layer window, playback position and
  role/option visibility, then draws with `SnapmakerPathRenderer`.
* If the shaders are missing, the GL context is older than 3.1, the layer
  count of libvgcode and the pipeline do not match, or the data was loaded as
  a print preview (no `GCodeProcessorResult`), rendering automatically falls
  back to libvgcode.

The color attribute table stores an explicit source per move so the GPU
colors match `libvgcode::ViewerImpl::get_vertex_color()` (role palette, tool
palette, color-print palette, option palette, gradient ranges and the gray
"layer contains pause/custom G-code" case). This keeps the legend and the
toolpaths consistent.

## Disabling

The renderer is selectable at runtime in
**Preferences > General > Features > Preview render type**:

* `libvgcode` - the original libvgcode renderer;
* `New pipe` - the Snapmaker GPU pipeline.

The change is applied immediately (no restart); when the GPU pipeline is
unavailable (OpenGL < 3.1, shader compile failure, print preview without a
`GCodeProcessorResult`, or a layer-count mismatch), the preview falls back to
libvgcode automatically.

The same choice can be written directly to the app config:

```ini
[app]
preview_render_type = new_pipe   ; or libvgcode
```

The legacy Snapmaker key is still honored when `preview_render_type` is not
present:

```ini
[app]
gcode_preview_gpu_accel = false
```

## Notes / differences from Snapmaker's own viewer

* Layers are grouped by `GCodeProcessorResult::MoveVertex::layer_id` instead
  of a Z scan, so the stack is in 1:1 lockstep with libvgcode's layer table.
* The bottom-slider endpoints and all slider UI stay owned by libvgcode; the
  Snapmaker legacy render-path records used only for those endpoints were not
  ported.
* Arc interpolation points are not emitted by this fork's GCodeProcessor, so
  a move contributes a single node.
* Instance meshes own a VAO, which is required by the core-profile contexts
  this fork requests.
