#include "framework.h"
#include "Renderer/Native/WebGL2/WebGL2Executor.h"

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include "Specific/trutils.h"

namespace TEN::Renderer::Native::WebGL2
{
    // Every shader here draws upside down: compile_shaders.py flips Y in vertex
    // shaders, so textures drawn to keep Direct3D's row order (top row first) and
    // read back with the coordinates the engine computed for Direct3D. Viewports
    // and scissor rectangles keep their Direct3D values for the same reason, and
    // Present() flips the picture the right way up for the page.

    constexpr GLenum TEXTURE_MAX_ANISOTROPY = 0x84FE;
    constexpr auto	 ZERO_UNIFORM_BUFFER_SIZE = 65536;

    static const char* SPRITE_VERTEX_SHADER = R"(#version 300 es
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;
uniform vec2 viewportSize;
out vec2 uv;
out vec4 color;
void main()
{
    vec2 ndc = vec2(inPosition.x * 2.0 / viewportSize.x - 1.0, 1.0 - inPosition.y * 2.0 / viewportSize.y);
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
    uv = inUV;
    color = inColor;
}
)";

    static const char* SPRITE_PIXEL_SHADER = R"(#version 300 es
precision highp float;
uniform highp sampler2D spriteTexture;
in vec2 uv;
in vec4 color;
out vec4 outColor;
void main()
{
    outColor = texture(spriteTexture, uv) * color;
}
)";

    static const char* FULLSCREEN_VERTEX_SHADER = R"(#version 300 es
void main()
{
    vec2 position = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
)";

    // Copies a texture into an sRGB one unchanged: decoding here and GL encoding
    // the result again gives back every 8-bit value.
    static const char* SRGB_COPY_PIXEL_SHADER = R"(#version 300 es
precision highp float;
uniform highp sampler2D source;
out vec4 outColor;
vec3 Decode(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
void main()
{
    vec4 c = texelFetch(source, ivec2(gl_FragCoord.xy), 0);
    outColor = vec4(Decode(c.rgb), c.a);
}
)";

    GLuint Executor::CreateSampler(GLint minFilter, GLint magFilter, GLint wrap, float maxLod, float anisotropy)
    {
        GLuint sampler = 0;
        glGenSamplers(1, &sampler);
        glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, minFilter);
        glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, magFilter);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, wrap);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, wrap);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, wrap);
        glSamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, maxLod);

        if (anisotropy > 1.0f)
            glSamplerParameterf(sampler, TEXTURE_MAX_ANISOTROPY, anisotropy);

        return sampler;
    }

    GLuint Executor::CreateInternalProgram(const char* vertexSource, const char* pixelSource)
    {
        auto compile = [](GLenum type, const char* source)
        {
            auto shader = glCreateShader(type);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);

            GLint status = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
            if (status != GL_TRUE)
            {
                char log[2048] = {};
                glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
                TENLog(std::string("WebGL internal shader failed to compile: ") + log, LogLevel::Error);
            }

            return shader;
        };

        auto vertex = compile(GL_VERTEX_SHADER, vertexSource);
        auto pixel = compile(GL_FRAGMENT_SHADER, pixelSource);
        auto program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, pixel);
        glLinkProgram(program);
        glDeleteShader(vertex);
        glDeleteShader(pixel);

        GLint status = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &status);
        if (status != GL_TRUE)
        {
            char log[2048] = {};
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            TENLog(std::string("WebGL internal program failed to link: ") + log, LogLevel::Error);
        }

        return program;
    }

    void Executor::Initialize(ShaderLibrary* shaders, bool floatLinear, float maxAnisotropy)
    {
        _shaders = shaders;
        _floatLinear = floatLinear;

        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);

        glGenVertexArrays(1, &_vertexArray);
        glBindVertexArray(_vertexArray);

        // Direct3D reads zero from what isn't bound, textures and constant buffers alike.
        GLenum targets[4] = { GL_TEXTURE_2D, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP };
        unsigned char zero[4] = {};
        glGenTextures(4, _zeroTextures);
        for (int i = 0; i < 4; i++)
        {
            glBindTexture(targets[i], _zeroTextures[i]);
            if (targets[i] == GL_TEXTURE_2D)
            {
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, zero);
            }
            else if (targets[i] == GL_TEXTURE_CUBE_MAP)
            {
                glTexStorage2D(GL_TEXTURE_CUBE_MAP, 1, GL_RGBA8, 1, 1);
                for (int face = 0; face < 6; face++)
                    glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, zero);
            }
            else
            {
                glTexStorage3D(targets[i], 1, GL_RGBA8, 1, 1, 1);
                glTexSubImage3D(targets[i], 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, zero);
            }
        }

        auto zeroData = std::vector<unsigned char>(ZERO_UNIFORM_BUFFER_SIZE);
        glGenBuffers(1, &_zeroUniformBuffer);
        glBindBuffer(GL_UNIFORM_BUFFER, _zeroUniformBuffer);
        glBufferData(GL_UNIFORM_BUFFER, ZERO_UNIFORM_BUFFER_SIZE, zeroData.data(), GL_STATIC_DRAW);

        // Direct3D's sampler states: the engine's point wrap one (base level only),
        // DirectXTK's common states, the default state of an empty slot, and one
        // for reading texels without filtering.
        float anisotropy = std::min(16.0f, maxAnisotropy);
        _pointWrapSampler = CreateSampler(GL_NEAREST, GL_NEAREST, GL_REPEAT, 0.0f, 1.0f);
        _pointClampSampler = CreateSampler(GL_NEAREST, GL_NEAREST, GL_CLAMP_TO_EDGE, 0.0f, 1.0f);
        _linearWrapSampler = CreateSampler(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, GL_REPEAT, 1000.0f, 1.0f);
        _linearClampSampler = CreateSampler(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, 1000.0f, 1.0f);
        _anisotropicWrapSampler = CreateSampler(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, GL_REPEAT, 1000.0f, anisotropy);
        _anisotropicClampSampler = CreateSampler(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, 1000.0f, anisotropy);
        _defaultSampler = _linearClampSampler;
        _fetchSampler = _pointClampSampler;

        _spriteProgram = CreateInternalProgram(SPRITE_VERTEX_SHADER, SPRITE_PIXEL_SHADER);
        glUseProgram(_spriteProgram);
        glUniform1i(glGetUniformLocation(_spriteProgram, "spriteTexture"), 0);
        _spriteViewportLocation = glGetUniformLocation(_spriteProgram, "viewportSize");

        _srgbCopyProgram = CreateInternalProgram(FULLSCREEN_VERTEX_SHADER, SRGB_COPY_PIXEL_SHADER);
        glUseProgram(_srgbCopyProgram);
        glUniform1i(glGetUniformLocation(_srgbCopyProgram, "source"), 0);
        glUseProgram(0);

        glGenBuffers(1, &_streamBuffer);
        _streamNative.Buffer = _streamBuffer;

        glGenBuffers(1, &_smaaBuffer.Buffer);
        glBindBuffer(GL_UNIFORM_BUFFER, _smaaBuffer.Buffer);
        glBufferData(GL_UNIFORM_BUFFER, 16, nullptr, GL_DYNAMIC_DRAW);
        _smaaBuffer.BufferSize = 16;
        _smaaBuffer.Data.resize(16);

        glFrontFace(GL_CCW);
        ClearState();
        InvalidateGLState();
    }

    void Executor::CreateEffectSamplers()
    {
        // Samplers the game's shaders declare with an effect state block.
        for (const auto& state : _shaders->GetSamplerStates())
        {
            if (state.Register < 0 || state.Register >= MAX_SAMPLER_REGISTERS)
                continue;

            GLint minFilter = state.Linear ?
                (state.MipLinear ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST) :
                (state.MipLinear ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
            auto sampler = CreateSampler(minFilter, state.Linear ? GL_LINEAR : GL_NEAREST, state.Address[0], 1000.0f, 1.0f);
            glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, state.Address[1]);
            glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, state.Address[2]);

            _samplers[state.Register] = sampler;
            _pointSamplers[state.Register] = (state.Address[0] == GL_REPEAT) ? _pointWrapSampler : _pointClampSampler;
        }
    }

    void Executor::Release()
    {
        for (auto& [key, framebuffer] : _framebuffers)
            glDeleteFramebuffers(1, &framebuffer);

        _framebuffers.clear();
    }

    void Executor::InvalidateGLState()
    {
        _framebufferDirty = true;
        _blendDirty = true;
        _depthDirty = true;
        _rasterDirty = true;
        _viewportDirty = true;
        _scissorDirty = true;
        _inputsDirty = true;
        _currentProgram = 0xFFFFFFFF;
        _currentFramebuffer = 0xFFFFFFFF;

        for (int i = 0; i < MAX_TEXTURE_UNITS; i++)
        {
            _boundTextures[i] = 0xFFFFFFFF;
            _boundSamplers[i] = 0xFFFFFFFF;
        }

        for (int i = 0; i < MAX_CONSTANT_BUFFER_REGISTERS; i++)
            _boundUniformBuffers[i] = 0xFFFFFFFF;

        _boundElementBuffer = 0xFFFFFFFF;
    }

    // State.

    void Executor::SetBlendState(const BlendState& state)
    {
        _blend = state;
        _blendDirty = true;
    }

    void Executor::SetDepthState(const DepthStencilState& state)
    {
        _depth = state;
        _depthDirty = true;
    }

    void Executor::SetRasterState(const RasterState& state)
    {
        _raster = state;
        _rasterDirty = true;
    }

    void Executor::SetViewport(const RendererViewport& viewport)
    {
        _viewport = viewport;
        _viewportDirty = true;
    }

    void Executor::SetScissor(int left, int top, int right, int bottom)
    {
        _scissor[0] = left;
        _scissor[1] = top;
        _scissor[2] = right;
        _scissor[3] = bottom;
        _scissorDirty = true;
    }

    void Executor::SetPrimitiveType(GLenum topology)
    {
        _topology = topology;
    }

    void Executor::SetInputLayout(NativeInputLayout* layout)
    {
        _inputLayout = layout;
        _inputsDirty = true;
    }

    void Executor::SetVertexBuffer(NativeBuffer* buffer, int stride)
    {
        _vertexBuffer = buffer;
        _vertexStride = stride;
        _inputsDirty = true;
    }

    void Executor::SetIndexBuffer(NativeBuffer* buffer)
    {
        _indexBuffer = buffer;
    }

    void Executor::SetShaderStage(int stage, const ShaderStageInfo* info, const float* smaaMetrics)
    {
        _stages[stage] = info;

        if (info != nullptr && info->UsesSmaaMetrics && smaaMetrics != nullptr &&
            memcmp(_smaaMetrics, smaaMetrics, sizeof(_smaaMetrics)) != 0)
        {
            memcpy(_smaaMetrics, smaaMetrics, sizeof(_smaaMetrics));
            memcpy(_smaaBuffer.Data.data(), _smaaMetrics, sizeof(_smaaMetrics));
            _smaaBuffer.Version++;
        }
    }

    void Executor::SetConstantBuffer(int stage, int reg, NativeConstantBuffer* buffer)
    {
        if (reg >= 0 && reg < MAX_CONSTANT_BUFFER_REGISTERS)
            _constantBuffers[stage][reg] = buffer;
    }

    void Executor::SetTexture(int stage, int reg, NativeTexture* texture)
    {
        if (reg < 0 || reg >= MAX_TEXTURE_REGISTERS)
            return;

        // Direct3D 11 refuses to read what is bound for drawing to.
        if (texture != nullptr && IsBoundAsOutput(texture))
            texture = nullptr;

        _textures[stage][reg] = texture;
    }

    void Executor::SetSamplers(bool pointFilter)
    {
        // The engine's slots s1-s6, as in Samplers.hlsli. s6, the shadow map
        // comparison sampler, isn't sampled with: compile_shaders.py turns the
        // comparison into texel reads.
        _samplers[1] = _pointWrapSampler;
        _samplers[2] = pointFilter ? _pointWrapSampler : _linearWrapSampler;
        _samplers[3] = pointFilter ? _pointWrapSampler : _linearClampSampler;
        _samplers[4] = pointFilter ? _pointWrapSampler : _anisotropicWrapSampler;
        _samplers[5] = pointFilter ? _pointWrapSampler : _anisotropicClampSampler;
        _samplers[6] = _linearClampSampler;

        _pointSamplers[1] = _pointWrapSampler;
        _pointSamplers[2] = _pointWrapSampler;
        _pointSamplers[3] = pointFilter ? _pointWrapSampler : _pointClampSampler;
        _pointSamplers[4] = _pointWrapSampler;
        _pointSamplers[5] = pointFilter ? _pointWrapSampler : _pointClampSampler;
        _pointSamplers[6] = _pointClampSampler;
    }

    void Executor::SetRenderTargets(NativeTexture* const* targets, const int* layers, int count, NativeTexture* depth, int depthLayer)
    {
        _renderTargetCount = std::min(count, MAX_RENDER_TARGETS);
        for (int i = 0; i < MAX_RENDER_TARGETS; i++)
        {
            _renderTargets[i] = (i < _renderTargetCount) ? targets[i] : nullptr;
            _renderTargetLayers[i] = (i < _renderTargetCount) ? layers[i] : 0;
        }

        _depthTarget = depth;
        _depthLayer = depthLayer;
        _framebufferDirty = true;

        // Direct3D 11 unbinds from reading what is now bound for drawing to.
        for (int stage = 0; stage < STAGE_COUNT; stage++)
        {
            for (int reg = 0; reg < MAX_TEXTURE_REGISTERS; reg++)
            {
                if (_textures[stage][reg] != nullptr && IsBoundAsOutput(_textures[stage][reg]))
                    _textures[stage][reg] = nullptr;
            }
        }
    }

    void Executor::ClearState()
    {
        // What ID3D11DeviceContext::ClearState() leaves: nothing bound, and the
        // default blend, depth and rasterizer states.
        for (int stage = 0; stage < STAGE_COUNT; stage++)
        {
            _stages[stage] = nullptr;
            for (int reg = 0; reg < MAX_CONSTANT_BUFFER_REGISTERS; reg++)
                _constantBuffers[stage][reg] = nullptr;

            for (int reg = 0; reg < MAX_TEXTURE_REGISTERS; reg++)
                _textures[stage][reg] = nullptr;
        }

        for (int reg = 0; reg < 10; reg++)
        {
            _samplers[reg] = 0;
            _pointSamplers[reg] = 0;
        }

        _inputLayout = nullptr;
        _vertexBuffer = nullptr;
        _indexBuffer = nullptr;
        _vertexStride = 0;
        _topology = GL_TRIANGLES;

        SetRenderTargets(nullptr, nullptr, 0, nullptr, 0);
        SetBlendState(BlendState{});
        SetDepthState(DepthStencilState{});
        SetRasterState(RasterState{});
        SetViewport(RendererViewport{ 0, 0, 0, 0, 0.0f, 1.0f });
        SetScissor(0, 0, 0, 0);
        _inputsDirty = true;
    }

    // GL state.

    GLuint Executor::GetFramebuffer(NativeTexture* const* targets, const int* layers, int count, NativeTexture* depth, int depthLayer)
    {
        auto key = std::vector<GLuint>{};
        key.reserve(count * 2 + 3);
        key.push_back(count);
        for (int i = 0; i < count; i++)
        {
            key.push_back(targets[i] != nullptr ? targets[i]->Texture : 0);
            key.push_back(layers[i]);
        }

        key.push_back(depth != nullptr ? depth->Texture : 0);
        key.push_back(depthLayer);

        auto it = _framebuffers.find(key);
        if (it != _framebuffers.end())
            return it->second;

        GLuint framebuffer = 0;
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        _currentFramebuffer = framebuffer;

        GLenum drawBuffers[MAX_RENDER_TARGETS] = {};
        for (int i = 0; i < count; i++)
        {
            auto* target = targets[i];
            drawBuffers[i] = GL_COLOR_ATTACHMENT0 + i;
            if (target == nullptr)
            {
                drawBuffers[i] = GL_NONE;
                continue;
            }

            if (target->Target == GL_TEXTURE_2D_ARRAY)
                glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, target->Texture, 0, layers[i]);
            else
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, target->Texture, 0);
        }

        if (count > 0)
        {
            glDrawBuffers(count, drawBuffers);
            _drawBufferMasks[framebuffer] = (1u << count) - 1;
        }
        else
        {
            GLenum none = GL_NONE;
            glDrawBuffers(1, &none);
            glReadBuffer(GL_NONE);
        }

        if (depth != nullptr)
        {
            auto attachment = (depth->InternalFormat == GL_DEPTH24_STENCIL8) ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
            if (depth->Target == GL_TEXTURE_2D_ARRAY)
                glFramebufferTextureLayer(GL_FRAMEBUFFER, attachment, depth->Texture, 0, depthLayer);
            else
                glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, depth->Texture, 0);
        }

        auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
            TENLog("WebGL framebuffer incomplete: " + std::to_string(status), LogLevel::Warning);

        _framebuffers[key] = framebuffer;
        return framebuffer;
    }

    void Executor::BindFramebuffer(GLuint framebuffer)
    {
        if (_currentFramebuffer == framebuffer)
            return;

        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        _currentFramebuffer = framebuffer;
    }

    void Executor::SetDrawBufferMask(GLuint framebuffer, int count, unsigned int mask)
    {
        auto& current = _drawBufferMasks[framebuffer];
        if (current == mask || count == 0)
            return;

        GLenum buffers[MAX_RENDER_TARGETS] = {};
        for (int i = 0; i < count; i++)
            buffers[i] = (mask & (1u << i)) ? (GL_COLOR_ATTACHMENT0 + i) : GL_NONE;

        glDrawBuffers(count, buffers);
        current = mask;
    }

    void Executor::ApplyFramebuffer(unsigned int outputMask)
    {
        if (_framebufferDirty)
        {
            auto framebuffer = GetFramebuffer(_renderTargets, _renderTargetLayers, _renderTargetCount, _depthTarget, _depthLayer);
            BindFramebuffer(framebuffer);
            _framebufferDirty = false;
        }

        // Direct3D leaves render targets the pixel shader doesn't write alone, where
        // WebGL refuses to draw: their draw buffers are switched off meanwhile.
        unsigned int mask = 0;
        for (int i = 0; i < _renderTargetCount; i++)
        {
            if (_renderTargets[i] != nullptr && (outputMask & (1u << i)))
                mask |= 1u << i;
        }

        SetDrawBufferMask(_currentFramebuffer, _renderTargetCount, mask);
    }

    void Executor::ApplyFixedFunction()
    {
        if (_blendDirty)
        {
            if (_blend.Enable)
            {
                glEnable(GL_BLEND);
                glBlendFuncSeparate(_blend.SrcColor, _blend.DstColor, _blend.SrcAlpha, _blend.DstAlpha);
                glBlendEquationSeparate(_blend.OpColor, _blend.OpAlpha);
            }
            else
            {
                glDisable(GL_BLEND);
            }

            _blendDirty = false;
        }

        if (_depthDirty)
        {
            if (_depth.Enable)
                glEnable(GL_DEPTH_TEST);
            else
                glDisable(GL_DEPTH_TEST);

            glDepthMask(_depth.Write ? GL_TRUE : GL_FALSE);
            glDepthFunc(_depth.Func);
            _depthDirty = false;
        }

        if (_rasterDirty)
        {
            if (_raster.CullFace != 0)
            {
                glEnable(GL_CULL_FACE);
                glCullFace(_raster.CullFace);
            }
            else
            {
                glDisable(GL_CULL_FACE);
            }

            if (_raster.Scissor)
                glEnable(GL_SCISSOR_TEST);
            else
                glDisable(GL_SCISSOR_TEST);

            _rasterDirty = false;
        }

        if (_viewportDirty)
        {
            glViewport(_viewport.X, _viewport.Y, std::max(0, _viewport.Width), std::max(0, _viewport.Height));
            glDepthRangef(_viewport.MinDepth, _viewport.MaxDepth);
            _viewportDirty = false;
        }

        if (_scissorDirty)
        {
            glScissor(_scissor[0], _scissor[1], std::max(0, _scissor[2] - _scissor[0]), std::max(0, _scissor[3] - _scissor[1]));
            _scissorDirty = false;
        }
    }

    void Executor::UploadConstantBuffer(NativeConstantBuffer& buffer, const BlockLayout& layout)
    {
        if (buffer.UploadedVersion == buffer.Version && buffer.UploadedLayout == &layout)
            return;

        glBindBuffer(GL_UNIFORM_BUFFER, buffer.Buffer);

        bool full = (buffer.UploadedLayout != &layout);
        if (buffer.BufferSize < layout.Size)
        {
            glBufferData(GL_UNIFORM_BUFFER, layout.Size, nullptr, GL_DYNAMIC_DRAW);
            buffer.BufferSize = layout.Size;
            full = true;

            for (int i = 0; i < MAX_CONSTANT_BUFFER_REGISTERS; i++)
            {
                if (_boundUniformBuffers[i] == buffer.Buffer)
                    _boundUniformBuffers[i] = 0xFFFFFFFF;
            }
        }

        int available = (int)buffer.Data.size();
        if (layout.IsIdentity)
        {
            // Only the part the engine wrote since the last upload, as Direct3D's
            // partial updates of large buffers (bones, objects) would do.
            int bytes = std::min(std::min(available, layout.Size), full ? available : buffer.DirtyBytes);
            if (bytes > 0)
                glBufferSubData(GL_UNIFORM_BUFFER, 0, bytes, buffer.Data.data());
        }
        else
        {
            _repackBuffer.assign(layout.Size, 0);
            for (const auto& copy : layout.Copies)
            {
                if (copy.Source + copy.Size <= available && copy.Destination + copy.Size <= layout.Size)
                    memcpy(_repackBuffer.data() + copy.Destination, buffer.Data.data() + copy.Source, copy.Size);
            }

            glBufferSubData(GL_UNIFORM_BUFFER, 0, layout.Size, _repackBuffer.data());
        }

        buffer.UploadedVersion = buffer.Version;
        buffer.UploadedLayout = &layout;
        buffer.DirtyBytes = 0;
    }

    void Executor::ApplyConstantBuffers(const Program& program)
    {
        for (const auto& block : program.Blocks)
        {
            NativeConstantBuffer* buffer = nullptr;
            if (block.Register == _shaders->GetSmaaMetricsRegister() && block.Layout->Name == "type_TENWebSmaa")
                buffer = &_smaaBuffer;
            else
                buffer = _constantBuffers[block.StageIndex][block.Register];

            GLuint id = _zeroUniformBuffer;
            if (buffer != nullptr && buffer->Buffer != 0)
            {
                UploadConstantBuffer(*buffer, *block.Layout);
                id = buffer->Buffer;
            }

            if (_boundUniformBuffers[block.Register] != id)
            {
                glBindBufferBase(GL_UNIFORM_BUFFER, block.Register, id);
                _boundUniformBuffers[block.Register] = id;
            }
        }
    }

    GLuint Executor::GetZeroTexture(GLenum target) const
    {
        switch (target)
        {
        case GL_TEXTURE_2D_ARRAY: return _zeroTextures[1];
        case GL_TEXTURE_3D: return _zeroTextures[2];
        case GL_TEXTURE_CUBE_MAP: return _zeroTextures[3];
        default: return _zeroTextures[0];
        }
    }

    void Executor::BindTextureUnit(int unit, GLenum target, GLuint texture, GLuint sampler)
    {
        if (unit >= MAX_TEXTURE_UNITS)
            return;

        if (_boundTextures[unit] != texture)
        {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(target, texture);
            _boundTextures[unit] = texture;
        }

        if (_boundSamplers[unit] != sampler)
        {
            glBindSampler(unit, sampler);
            _boundSamplers[unit] = sampler;
        }
    }

    bool Executor::IsBoundAsOutput(const NativeTexture* texture) const
    {
        const auto* resource = (texture->ViewOf != nullptr) ? texture->ViewOf : texture;

        for (int i = 0; i < _renderTargetCount; i++)
        {
            if (_renderTargets[i] == resource)
                return true;
        }

        return _depthTarget == resource;
    }

    void Executor::ApplyTextures(const Program& program)
    {
        for (const auto& unit : program.Units)
        {
            auto* texture = _textures[unit.StageIndex][unit.TextureRegister];

            GLuint id = GetZeroTexture(unit.Target);
            bool isFloat32 = false;
            if (texture != nullptr && texture->Target == unit.Target && !IsBoundAsOutput(texture))
            {
                id = texture->Texture;
                isFloat32 = texture->IsFloat32;
            }

            GLuint sampler = _fetchSampler;
            if (unit.SamplerRegister >= 0 && unit.SamplerRegister < MAX_SAMPLER_REGISTERS)
            {
                sampler = _samplers[unit.SamplerRegister] ? _samplers[unit.SamplerRegister] : _defaultSampler;

                // 32-bit float textures filter only with OES_texture_float_linear.
                if (isFloat32 && !_floatLinear)
                    sampler = _pointSamplers[unit.SamplerRegister] ? _pointSamplers[unit.SamplerRegister] : _pointClampSampler;
            }

            BindTextureUnit(unit.Unit, unit.Target, id, sampler);
        }
    }

    void Executor::ApplyVertexInputs(const Program& program, int baseVertex)
    {
        if (!_inputsDirty && _inputsProgram == &program && _inputsBaseVertex == baseVertex)
            return;

        GLuint vertexBuffer = (_vertexBuffer != nullptr) ? _vertexBuffer->Buffer : 0;
        glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);

        bool used[MAX_VERTEX_ATTRIBUTES] = {};
        if (_inputLayout != nullptr && vertexBuffer != 0)
        {
            for (const auto& input : program.Vertex->Inputs)
            {
                if (input.Location < 0 || input.Location >= MAX_VERTEX_ATTRIBUTES)
                    continue;

                const NativeInputElement* element = nullptr;
                for (const auto& candidate : _inputLayout->Elements)
                {
                    if (candidate.Index == input.Index && TEN::Utils::ToLower(candidate.Semantic) == TEN::Utils::ToLower(input.Semantic))
                    {
                        element = &candidate;
                        break;
                    }
                }

                if (element == nullptr)
                    continue;

                int location = input.Location;
                used[location] = true;
                if (!_enabledAttributes[location])
                {
                    glEnableVertexAttribArray(location);
                    _enabledAttributes[location] = true;
                }

                // WebGL 2 has no base vertex for indexed draws: the attributes start there instead.
                auto offset = (const void*)(intptr_t)(element->Offset + baseVertex * _vertexStride);
                switch (element->Format)
                {
                case VertexInputFormat::VI_RGBA8_Unorm: glVertexAttribPointer(location, 4, GL_UNSIGNED_BYTE, GL_TRUE, _vertexStride, offset); break;
                case VertexInputFormat::VI_RGBA8_Snorm: glVertexAttribPointer(location, 4, GL_BYTE, GL_TRUE, _vertexStride, offset); break;
                case VertexInputFormat::VI_RGBA8_Uint: glVertexAttribIPointer(location, 4, GL_UNSIGNED_BYTE, _vertexStride, offset); break;
                case VertexInputFormat::VI_RG8_Unorm: glVertexAttribPointer(location, 2, GL_UNSIGNED_BYTE, GL_TRUE, _vertexStride, offset); break;
                case VertexInputFormat::VI_R32_Float: glVertexAttribPointer(location, 1, GL_FLOAT, GL_FALSE, _vertexStride, offset); break;
                case VertexInputFormat::VI_R32_Uint: glVertexAttribIPointer(location, 1, GL_UNSIGNED_INT, _vertexStride, offset); break;
                case VertexInputFormat::VI_RG32_Float: glVertexAttribPointer(location, 2, GL_FLOAT, GL_FALSE, _vertexStride, offset); break;
                case VertexInputFormat::VI_RGB32_Float: glVertexAttribPointer(location, 3, GL_FLOAT, GL_FALSE, _vertexStride, offset); break;
                case VertexInputFormat::VI_RGBA32_Float: glVertexAttribPointer(location, 4, GL_FLOAT, GL_FALSE, _vertexStride, offset); break;
                }
            }
        }

        for (int location = 0; location < MAX_VERTEX_ATTRIBUTES; location++)
        {
            if (!used[location] && _enabledAttributes[location])
            {
                glDisableVertexAttribArray(location);
                _enabledAttributes[location] = false;
            }
        }

        _inputsDirty = false;
        _inputsProgram = &program;
        _inputsBaseVertex = baseVertex;
    }

    void Executor::RefreshView(NativeTexture* view)
    {
        auto* parent = view->ViewOf;
        if (parent == nullptr || view->ViewGeneration == parent->Generation)
            return;

        NativeTexture* targets[1] = { view };
        int layers[1] = { 0 };
        BindFramebuffer(GetFramebuffer(targets, layers, 1, nullptr, 0));
        SetDrawBufferMask(_currentFramebuffer, 1, 1u);

        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, view->Width, view->Height);
        glUseProgram(_srgbCopyProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, parent->Texture);
        glBindSampler(0, _fetchSampler);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        view->ViewGeneration = parent->Generation;
        InvalidateGLState();
    }

    void Executor::MarkOutputsWritten()
    {
        for (int i = 0; i < _renderTargetCount; i++)
        {
            if (_renderTargets[i] != nullptr)
                _renderTargets[i]->Generation++;
        }
    }

    bool Executor::PrepareDraw(int baseVertex)
    {
        if (_stages[STAGE_VERTEX] == nullptr || _stages[STAGE_PIXEL] == nullptr)
            return false;

        if (_renderTargetCount == 0 && _depthTarget == nullptr)
            return false;

        bool linked = false;
        auto* program = _shaders->GetProgram(_stages[STAGE_VERTEX], _stages[STAGE_PIXEL], linked);
        if (linked)
            _currentProgram = 0xFFFFFFFF;

        if (program == nullptr)
            return false;

        for (const auto& unit : program->Units)
        {
            auto* texture = _textures[unit.StageIndex][unit.TextureRegister];
            if (texture != nullptr && texture->ViewOf != nullptr)
                RefreshView(texture);
        }

        ApplyFramebuffer(program->Pixel->OutputMask);
        ApplyFixedFunction();

        if (_currentProgram != program->Id)
        {
            glUseProgram(program->Id);
            _currentProgram = program->Id;
            _inputsDirty = true;
        }

        ApplyConstantBuffers(*program);
        ApplyTextures(*program);
        ApplyVertexInputs(*program, baseVertex);
        MarkOutputsWritten();
        return true;
    }

    // Work.

    void Executor::DrawIndexed(int count, int baseIndex, int baseVertex, int instances)
    {
        if (_indexBuffer == nullptr || !PrepareDraw(baseVertex))
            return;

        if (_boundElementBuffer != _indexBuffer->Buffer)
            BindElementBufferForUpload(_indexBuffer->Buffer);

        auto offset = (const void*)(intptr_t)(baseIndex * sizeof(int));
        if (instances > 0)
            glDrawElementsInstanced(_topology, count, GL_UNSIGNED_INT, offset, instances);
        else
            glDrawElements(_topology, count, GL_UNSIGNED_INT, offset);
    }

    void Executor::DrawArrays(int count, int baseVertex, int instances)
    {
        if (!PrepareDraw(0))
            return;

        if (instances > 0)
            glDrawArraysInstanced(_topology, baseVertex, count, instances);
        else
            glDrawArrays(_topology, baseVertex, count);
    }

    void Executor::DrawPrimitives(const std::vector<Vertex>& vertices, GLenum topology)
    {
        if (vertices.empty())
            return;

        // DirectXTK's PrimitiveBatch: its own vertex buffer, everything else as bound.
        glBindBuffer(GL_ARRAY_BUFFER, _streamBuffer);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STREAM_DRAW);

        _vertexBuffer = &_streamNative;
        _vertexStride = (int)sizeof(Vertex);
        _topology = topology;
        _inputsDirty = true;

        if (PrepareDraw(0))
            glDrawArrays(topology, 0, (int)vertices.size());
    }

    void Executor::DrawSprites(const std::vector<SpriteDraw>& draws, const BlendState& blend)
    {
        // DirectXTK's SpriteBatch states: the blend state given, no depth, back
        // faces culled without scissor test, linear clamp sampling. It leaves them set.
        SetBlendState(blend);
        SetDepthState(DepthStencilState{ false, false, GL_LEQUAL });
        SetRasterState(RasterState{ GL_BACK, false });

        _stages[STAGE_VERTEX] = nullptr;
        _stages[STAGE_PIXEL] = nullptr;
        _inputLayout = nullptr;
        _vertexBuffer = nullptr;
        _topology = GL_TRIANGLES;

        if (_renderTargetCount == 0 || draws.empty())
            return;

        ApplyFramebuffer(1u);
        ApplyFixedFunction();

        auto vertices = std::vector<SpriteVertex2D>{};
        for (const auto& draw : draws)
            vertices.insert(vertices.end(), draw.Vertices.begin(), draw.Vertices.end());

        glUseProgram(_spriteProgram);
        _currentProgram = _spriteProgram;
        glUniform2f(_spriteViewportLocation, (float)std::max(1, _viewport.Width), (float)std::max(1, _viewport.Height));

        glBindBuffer(GL_ARRAY_BUFFER, _streamBuffer);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(SpriteVertex2D), vertices.data(), GL_STREAM_DRAW);

        for (int location = 0; location < MAX_VERTEX_ATTRIBUTES; location++)
        {
            bool use = (location < 3);
            if (use != _enabledAttributes[location])
            {
                if (use)
                    glEnableVertexAttribArray(location);
                else
                    glDisableVertexAttribArray(location);

                _enabledAttributes[location] = use;
            }
        }

        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVertex2D), (const void*)offsetof(SpriteVertex2D, Position));
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVertex2D), (const void*)offsetof(SpriteVertex2D, UV));
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(SpriteVertex2D), (const void*)offsetof(SpriteVertex2D, Color));
        _inputsDirty = true;

        int first = 0;
        for (const auto& draw : draws)
        {
            auto* texture = draw.Texture;
            if (texture != nullptr && texture->ViewOf != nullptr)
            {
                RefreshView(texture);
                ApplyFramebuffer(1u);
                ApplyFixedFunction();
                glUseProgram(_spriteProgram);
                _currentProgram = _spriteProgram;
            }

            GLuint id = (texture != nullptr && texture->Target == GL_TEXTURE_2D && !IsBoundAsOutput(texture)) ? texture->Texture : _zeroTextures[0];
            bool isFloat32 = (texture != nullptr && texture->IsFloat32);
            BindTextureUnit(0, GL_TEXTURE_2D, id, (isFloat32 && !_floatLinear) ? _pointClampSampler : _linearClampSampler);

            glDrawArrays(GL_TRIANGLES, first, (int)draw.Vertices.size());
            first += (int)draw.Vertices.size();
        }

        MarkOutputsWritten();
    }

    void Executor::ClearColor(NativeTexture* target, int layer, const float* color)
    {
        if (target == nullptr)
            return;

        NativeTexture* targets[1] = { target };
        int layers[1] = { layer };
        BindFramebuffer(GetFramebuffer(targets, layers, 1, nullptr, 0));
        SetDrawBufferMask(_currentFramebuffer, 1, 1u);

        // Direct3D clears ignore the scissor rectangle.
        glDisable(GL_SCISSOR_TEST);
        glClearBufferfv(GL_COLOR, 0, color);

        target->Generation++;
        _rasterDirty = true;
        _framebufferDirty = true;
    }

    void Executor::ClearDepthStencil(NativeTexture* target, int layer, bool depth, bool stencil, float depthValue, int stencilValue)
    {
        if (target == nullptr)
            return;

        BindFramebuffer(GetFramebuffer(nullptr, nullptr, 0, target, layer));

        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);

        bool hasStencil = (target->InternalFormat == GL_DEPTH24_STENCIL8);
        if (depth && stencil && hasStencil)
        {
            glClearBufferfi(GL_DEPTH_STENCIL, 0, depthValue, stencilValue);
        }
        else if (depth)
        {
            glClearBufferfv(GL_DEPTH, 0, &depthValue);
        }
        else if (stencil && hasStencil)
        {
            glClearBufferiv(GL_STENCIL, 0, &stencilValue);
        }

        _rasterDirty = true;
        _depthDirty = true;
        _framebufferDirty = true;
    }

    void Executor::CopyTexture(NativeTexture* source, NativeTexture* destination)
    {
        if (source == nullptr || destination == nullptr)
            return;

        int zero[1] = { 0 };
        NativeTexture* sources[1] = { source };
        NativeTexture* destinations[1] = { destination };
        auto readFramebuffer = GetFramebuffer(sources, zero, 1, nullptr, 0);
        auto drawFramebuffer = GetFramebuffer(destinations, zero, 1, nullptr, 0);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFramebuffer);
        _currentFramebuffer = drawFramebuffer;
        SetDrawBufferMask(drawFramebuffer, 1, 1u);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, source->Width, source->Height, 0, 0, destination->Width, destination->Height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        destination->Generation++;
        _currentFramebuffer = 0xFFFFFFFF;
        _framebufferDirty = true;
        _rasterDirty = true;
    }

    void Executor::Present(NativeTexture* backBuffer)
    {
        if (backBuffer == nullptr)
            return;

        int zero[1] = { 0 };
        NativeTexture* sources[1] = { backBuffer };
        auto readFramebuffer = GetFramebuffer(sources, zero, 1, nullptr, 0);

        // The picture is upside down (see the top of this file): flip it for the page.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, backBuffer->Width, backBuffer->Height, 0, backBuffer->Height, backBuffer->Width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        _currentFramebuffer = 0xFFFFFFFF;
        _framebufferDirty = true;
        _rasterDirty = true;
    }

    void Executor::UpdateConstantBuffer(NativeConstantBuffer* buffer, std::vector<unsigned char>& data)
    {
        if (buffer == nullptr)
            return;

        if (buffer->Data.size() < data.size())
            buffer->Data.resize(data.size());

        memcpy(buffer->Data.data(), data.data(), data.size());
        buffer->DirtyBytes = std::max(buffer->DirtyBytes, (int)data.size());
        buffer->Version++;
    }

    void Executor::ReadPixels(NativeTexture* source, std::vector<unsigned char>& pixels)
    {
        if (source == nullptr)
            return;

        int zero[1] = { 0 };
        NativeTexture* sources[1] = { source };
        BindFramebuffer(GetFramebuffer(sources, zero, 1, nullptr, 0));

        pixels.resize(source->Width * source->Height * 4);
        glReadPixels(0, 0, source->Width, source->Height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        _framebufferDirty = true;
    }

    void Executor::BindElementBufferForUpload(GLuint buffer)
    {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
        _boundElementBuffer = buffer;
    }

    void Executor::Forget(NativeTexture* texture)
    {
        for (int stage = 0; stage < STAGE_COUNT; stage++)
        {
            for (int reg = 0; reg < MAX_TEXTURE_REGISTERS; reg++)
            {
                if (_textures[stage][reg] == texture)
                    _textures[stage][reg] = nullptr;
            }
        }

        for (int i = 0; i < MAX_RENDER_TARGETS; i++)
        {
            if (_renderTargets[i] == texture)
            {
                _renderTargets[i] = nullptr;
                _framebufferDirty = true;
            }
        }

        if (_depthTarget == texture)
        {
            _depthTarget = nullptr;
            _framebufferDirty = true;
        }

        // Framebuffers that use the texture go too; GL may reuse its name.
        for (auto it = _framebuffers.begin(); it != _framebuffers.end();)
        {
            const auto& key = it->first;
            bool uses = false;
            int count = (int)key[0];
            for (int i = 0; i < count; i++)
                uses |= (key[1 + i * 2] == texture->Texture);

            uses |= (key[1 + count * 2] == texture->Texture);

            if (uses)
            {
                if (_currentFramebuffer == it->second)
                    _currentFramebuffer = 0xFFFFFFFF;

                _drawBufferMasks.erase(it->second);
                glDeleteFramebuffers(1, &it->second);
                it = _framebuffers.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (int i = 0; i < MAX_TEXTURE_UNITS; i++)
        {
            if (_boundTextures[i] == texture->Texture)
                _boundTextures[i] = 0xFFFFFFFF;
        }
    }

    void Executor::Forget(NativeConstantBuffer* buffer)
    {
        for (int stage = 0; stage < STAGE_COUNT; stage++)
        {
            for (int reg = 0; reg < MAX_CONSTANT_BUFFER_REGISTERS; reg++)
            {
                if (_constantBuffers[stage][reg] == buffer)
                    _constantBuffers[stage][reg] = nullptr;
            }
        }

        for (int i = 0; i < MAX_CONSTANT_BUFFER_REGISTERS; i++)
        {
            if (_boundUniformBuffers[i] == buffer->Buffer)
                _boundUniformBuffers[i] = 0xFFFFFFFF;
        }
    }

    void Executor::Forget(NativeBuffer* buffer)
    {
        if (_vertexBuffer == buffer)
            _vertexBuffer = nullptr;

        if (_indexBuffer == buffer)
            _indexBuffer = nullptr;

        if (_boundElementBuffer == buffer->Buffer)
            _boundElementBuffer = 0xFFFFFFFF;

        _inputsDirty = true;
    }

    void Executor::Forget(NativeInputLayout* layout)
    {
        if (_inputLayout == layout)
            _inputLayout = nullptr;

        _inputsDirty = true;
    }
}

#endif
