# Build environment for the WebAssembly / WebGL 2 port of TombEngine.
#
# The libraries below are the versions the Windows build of TombEngine uses,
# built for the browser. Every one of them shares memory with the engine's
# threads and uses native WebAssembly exceptions, like the engine itself.

FROM emscripten/emsdk:6.0.13

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        glslang-tools \
        ccache \
        ninja-build \
        unzip \
        wget \
    && rm -rf /var/lib/apt/lists/*

ENV TEN_WEB_FLAGS="-pthread -msimd128 -fwasm-exceptions -sSUPPORT_LONGJMP=wasm"
ENV CFLAGS="-O2 ${TEN_WEB_FLAGS}"
ENV CXXFLAGS="-O2 ${TEN_WEB_FLAGS}"
ENV LDFLAGS="${TEN_WEB_FLAGS}"
RUN mkdir -p /ext/include /ext/lib /ext/src /src

# SDL 3.4.8, the version whose headers TombEngine ships in Libs/sdl3, with
# patches/ applied:
# - SDL-emscripten-joystick-state.patch: devices without trigger axes (such as
#   keyboards the browser lists as gamepads) reported their triggers half pressed
#   forever, and TombEngine binds crouch to the left trigger. Also sends a
#   gamepad's whole state when it is opened, and reads every gamepad on Android
#   in the standard layout: Chrome lays out the gamepads it has no mapping for
#   that way but calls them non-standard, so their d-pad did nothing and their
#   triggers acted as back and start.
COPY patches /patches
RUN git clone --depth 1 --branch release-3.4.8 https://github.com/libsdl-org/SDL /src/SDL \
    && git -C /src/SDL apply /patches/SDL-emscripten-joystick-state.patch \
    && emcmake cmake -S /src/SDL -B /src/SDL/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/ext \
        -DSDL_SHARED=OFF \
        -DSDL_STATIC=ON \
        -DSDL_PTHREADS=ON \
        -DSDL_TEST_LIBRARY=OFF \
        -DSDL_TESTS=OFF \
        -DSDL_EXAMPLES=OFF \
    && cmake --build /src/SDL/build \
    && cmake --install /src/SDL/build \
    && rm -rf /src/SDL

# Lua 5.3.5, the version of Libs/lua, built as C like the Windows DLL.
RUN wget -q https://www.lua.org/ftp/lua-5.3.5.tar.gz -O /tmp/lua.tgz \
    && mkdir /src/lua && tar xzf /tmp/lua.tgz -C /src/lua --strip-components=1 \
    && cd /src/lua/src \
    && emcc $CFLAGS -c $(ls *.c | grep -vxE 'luac?\.c') \
    && emar rcs /ext/lib/liblua.a *.o \
    && cd / && rm -rf /src/lua /tmp/lua.tgz

# LZ4 1.10.0, the version of Libs/lz4.
RUN git clone --depth 1 --branch v1.10.0 https://github.com/lz4/lz4 /src/lz4 \
    && cd /src/lz4/lib \
    && emcc $CFLAGS -c lz4.c \
    && emar rcs /ext/lib/liblz4.a lz4.o \
    && cd / && rm -rf /src/lz4

# DirectXMath and SimpleMath from DirectXTK December 2022, the release of the
# directxtk_desktop_2019 2022.12.16.1 package TombEngine builds with. sal.h is
# the annotation header DirectXMath expects outside Windows.
RUN git clone --depth 1 --branch dec2022 https://github.com/microsoft/DirectXMath /src/DirectXMath \
    && cp /src/DirectXMath/Inc/* /ext/include/ \
    && wget -q https://raw.githubusercontent.com/dotnet/runtime/v8.0.1/src/coreclr/pal/inc/rt/sal.h -O /ext/include/sal.h \
    && git clone --depth 1 --branch dec2022 https://github.com/microsoft/DirectXTK /src/DirectXTK \
    && cp /src/DirectXTK/Inc/SimpleMath.h /src/DirectXTK/Inc/SimpleMath.inl /ext/include/ \
    && cp /src/DirectXTK/Src/SimpleMath.cpp /ext/src/ \
    && rm -rf /src/DirectXMath /src/DirectXTK

# stb_image decodes the PNG, JPEG, BMP and TGA pictures Windows decodes with WIC.
RUN git clone --depth 1 https://github.com/nothings/stb /src/stb \
    && cp /src/stb/stb_image.h /src/stb/stb_image_write.h /ext/include/ \
    && git -C /src/stb log -1 --format='stb %H' > /ext/include/stb.version \
    && rm -rf /src/stb

# Decoders the browser build uses in place of Windows codecs: bcdec for the
# block-compressed textures of levels where the browser can't sample them,
# dr_wav, dr_mp3 and stb_vorbis for the sounds and music BASS plays on Windows.
RUN git clone -q --depth 1 https://github.com/iOrange/bcdec /src/bcdec \
    && cp /src/bcdec/bcdec.h /ext/include/ \
    && git -C /src/bcdec log -1 --format='bcdec %H' >> /ext/include/stb.version \
    && git clone -q --depth 1 https://github.com/mackron/dr_libs /src/dr_libs \
    && cp /src/dr_libs/dr_wav.h /src/dr_libs/dr_mp3.h /ext/include/ \
    && git -C /src/dr_libs log -1 --format='dr_libs %H' >> /ext/include/stb.version \
    && wget -q https://raw.githubusercontent.com/nothings/stb/9600522de53973336fa033a289858a4354cbb04c/stb_vorbis.c -O /ext/include/stb_vorbis.c \
    && rm -rf /src/bcdec /src/dr_libs

# etcpak's ETC2/EAC encoder. Phones sample neither the PNG pictures most levels
# hold nor their DXT5 blocks, and as pixels a level's textures fill hundreds of
# megabytes there, so the browser build compresses them to ETC2 as they load.
# Its SSE4.1 code runs on WebAssembly SIMD through Emscripten's <smmintrin.h>.
# Its one- and two-channel EAC encoders skip blocks of one value, as its ETC2 alpha
# encoder does: unused normal maps are one colour, and searching each of their
# blocks takes seconds.
RUN git clone -q https://github.com/wolfpld/etcpak /src/etcpak \
    && git -C /src/etcpak checkout -q 3d716e1550023dc5ff8b602636af8b76584e0b66 \
    && sed -i 's/ProcessAlpha_ETC2<false>( \(r\|rg\|&rg\[16\]\) )/ProcessAlpha_ETC2<true>( \1 )/' /src/etcpak/ProcessRGB.cpp \
    && mkdir -p /tmp/etcpak/shim /ext/include/etcpak \
    && printf '#include <smmintrin.h>\n' > /tmp/etcpak/shim/x86intrin.h \
    && for f in ProcessRGB Tables Dither; do \
        em++ -O3 ${TEN_WEB_FLAGS} -msse4.1 -DNDEBUG -w -I/tmp/etcpak/shim -c /src/etcpak/$f.cpp -o /tmp/etcpak/$f.o; \
    done \
    && emar rcs /ext/lib/libetcpak.a /tmp/etcpak/*.o \
    && cp /src/etcpak/ProcessRGB.hpp /src/etcpak/LICENSE.txt /ext/include/etcpak/ \
    && git -C /src/etcpak log -1 --format='etcpak %H' >> /ext/include/stb.version \
    && rm -rf /src/etcpak /tmp/etcpak

# Shader tools: DXC turns HLSL into SPIR-V, SPIRV-Cross turns that into GLSL ES 3.00.
# They run on the build machine, so they are built without the browser flags above.
RUN wget -q https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/linux_dxc_2026_09_28.x86_x64.tar.gz -O /tmp/dxc.tgz \
    && mkdir -p /opt/dxc && tar xzf /tmp/dxc.tgz -C /opt/dxc \
    && rm /tmp/dxc.tgz
RUN git clone --depth 1 --branch vulkan-sdk-1.4.363.0 https://github.com/KhronosGroup/SPIRV-Cross /src/spirv-cross \
    && env -u CFLAGS -u CXXFLAGS -u LDFLAGS cmake -S /src/spirv-cross -B /src/spirv-cross/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/spirv-cross \
        -DSPIRV_CROSS_ENABLE_TESTS=OFF \
        -DSPIRV_CROSS_SHARED=OFF \
    && cmake --build /src/spirv-cross/build \
    && cmake --install /src/spirv-cross/build \
    && rm -rf /src/spirv-cross
ENV PATH=/opt/dxc/bin:/opt/spirv-cross/bin:$PATH

ENV CCACHE_DIR=/ccache
WORKDIR /ten

# The build runs as the user who owns the source tree, and Emscripten builds its
# system libraries into its cache on first use.
RUN chmod -R a+rwX /emsdk/upstream/emscripten/cache
ENV HOME=/tmp
