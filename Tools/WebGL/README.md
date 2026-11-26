# TombEngine in the browser

The browser build of TombEngine: WebAssembly with Emscripten, drawing with WebGL 2.
It builds from the same sources as the Windows build; what only exists on Windows
has a browser counterpart.

## Building

Everything builds in the image of this folder's `Dockerfile` (Emscripten, SDL 3,
Lua, LZ4, DirectXMath and the shader tools, at the versions TombEngine uses):

    docker build -t ten-webgl-dev Tools/WebGL
    docker run --rm -v <repo>:/ten -w /ten ten-webgl-dev Tools/WebGL/build.sh /ten/build-web

`build.sh` configures `CMakeLists.txt` and builds `TombEngine.js`, `TombEngine.wasm`
and `TombEngine.data` (the engine's own shaders, compiled for WebGL, and the dummy
title level). The sources are read from `TombEngine.vcxproj`.

## How it maps to the Windows build

- **Renderer** - `Renderer/Native/WebGL2` implements the graphics device interface
  the Direct3D 11 renderer implements. WebGL can only be used from the page's main
  thread while the engine draws from its own threads, so the device records every
  call and the main thread plays each presented frame back once per display
  refresh. It keeps Direct3D 11's state and rules (what an empty slot reads, what
  ClearState resets, render targets that can't be read while drawn to).
- **Shaders** - `compile_shaders.py` compiles a game's HLSL shaders ahead of time
  (DXC, SPIRV-Cross) into GLSL ES 3.00, as the Windows build compiles them at
  start-up. It writes `Shaders/WebGL.tenshaders` for a game; games without one
  use the engine's. A few Direct3D features GLSL ES lacks are rewritten on the
  way: structured buffers, comparison sampling of float textures, effect-file
  samplers, Direct3D constant buffer packing.
- **Textures** - `WebGL2TextureLoader.cpp` reads DDS files and pictures as DirectXTK
  does on Windows, and decodes block formats the browser can't sample. Phones
  sample neither the PNG pictures most levels hold nor their DXT5 blocks, and as
  pixels a level's textures fill hundreds of megabytes there, so where the browser
  takes ETC2 but not S3TC the loader compresses them to ETC2/EAC (etcpak) as they
  load, mipmaps included. Desktops keep them as the Windows build does.
- **Constant buffer size** - Direct3D 11 takes constant buffers of 64 KB, WebGL 2
  devices as little as 16 KB (iPhones and most Macs). On those, the arrays the
  largest buffers end with (objects, sprites) are declared shorter when the
  shaders load, and the renderer draws instanced statics and sprites in batches
  that fit (`IGraphicsDevice::GetMaxConstantBufferSize()`).
- **Platform** - `Specific/Platform/Web` holds the platform subsystem, a log that
  can't deadlock the page, and what replaces the libraries with no browser build: a
  mixer for BASS (sound), the browser's own `<video>` for libVLC (`VideoWeb.cpp`), and
  nothing for Discord (nothing to talk to in a page).
- **Videos** - play in `<video>` elements of the page (`Module.tenWebVideo` in
  `pre.js`): the browser decodes them and plays their sound, and the engine copies
  each frame into its video texture on the GPU, scaled down to 2048 pixels at most.
  The page can tell the engine where a video is with `Module.tenWebVideoUrl(path)`,
  so videos stream instead of loading into memory. Browsers play H.264 or VP9 video
  in MP4 or WebM; Chrome also plays Matroska (`.mkv`), other browsers don't.
- **Main loop** - `Specific/EngineMain.cpp` runs the window's events and the frames
  from the page's loop instead of its own.
- **Files** - `pre.js` makes file names case-insensitive and accepts backslashes,
  as Windows does, since games rely on both.

## Running

The page has to be cross-origin isolated (COOP and COEP headers) and secure, for
the engine's threads. Before calling `callMain(['-gamedir', '/games/<id>'])` it writes
the game's files into the engine's file system. See `ten-web` in the super repo
for a test page.
