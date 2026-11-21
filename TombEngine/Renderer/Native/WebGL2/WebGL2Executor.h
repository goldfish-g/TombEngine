#pragma once

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include <GLES3/gl3.h>

#include "Renderer/Graphics/Vertices/Vertex.h"
#include "Renderer/Native/WebGL2/WebGL2Objects.h"
#include "Renderer/Native/WebGL2/WebGL2ShaderLibrary.h"
#include "Renderer/Structures/RendererViewport.h"

namespace TEN::Renderer::Native::WebGL2
{
    using namespace TEN::Renderer::Graphics::Vertices;
    using namespace TEN::Renderer::Structures;

    constexpr auto MAX_CONSTANT_BUFFER_REGISTERS = 16;
    constexpr auto MAX_TEXTURE_REGISTERS		 = 32;
    constexpr auto MAX_SAMPLER_REGISTERS		 = 16;
    constexpr auto MAX_RENDER_TARGETS			 = 8;
    constexpr auto MAX_TEXTURE_UNITS			 = 16;
    constexpr auto MAX_VERTEX_ATTRIBUTES		 = 16;

    // Rasterizer states the engine uses: Direct3D's default one (after ClearState),
    // the engine's own three, which all enable the scissor test, and DirectXTK's
    // sprite state.
    struct RasterState
    {
        GLenum CullFace = GL_BACK; // 0 for none.
        bool   Scissor	= false;
    };

    struct DepthStencilState
    {
        bool   Enable = true;
        bool   Write  = true;
        GLenum Func	  = GL_LESS;
    };

    struct BlendState
    {
        bool   Enable	 = false;
        GLenum SrcColor	 = GL_ONE;
        GLenum DstColor	 = GL_ZERO;
        GLenum OpColor	 = GL_FUNC_ADD;
        GLenum SrcAlpha	 = GL_ONE;
        GLenum DstAlpha	 = GL_ZERO;
        GLenum OpAlpha	 = GL_FUNC_ADD;
    };

    struct SpriteVertex2D
    {
        float Position[2];
        float UV[2];
        float Color[4];
    };

    struct SpriteDraw
    {
        NativeTexture*				Texture	 = nullptr;
        std::vector<SpriteVertex2D> Vertices = {}; // Six per sprite.
    };

    // Runs recorded commands with WebGL 2 on the browser's main thread, keeping the
    // state Direct3D 11 would have and applying it to GL when something is drawn.
    class Executor
    {
    private:
        ShaderLibrary* _shaders = nullptr;
        bool		   _floatLinear = false;
        int			   _dataTextureWidth = 2048;

        // Direct3D state.
        const ShaderStageInfo* _stages[STAGE_COUNT] = {};
        float				   _smaaMetrics[4]		= {};
        NativeInputLayout*	   _inputLayout			= nullptr;
        NativeBuffer*		   _vertexBuffer		= nullptr;
        int					   _vertexStride		= 0;
        NativeBuffer*		   _indexBuffer			= nullptr;
        GLenum				   _topology			= GL_TRIANGLES;
        NativeConstantBuffer*  _constantBuffers[STAGE_COUNT][MAX_CONSTANT_BUFFER_REGISTERS] = {};
        NativeTexture*		   _textures[STAGE_COUNT][MAX_TEXTURE_REGISTERS] = {};
        GLuint				   _samplers[MAX_SAMPLER_REGISTERS] = {};
        GLuint				   _pointSamplers[MAX_SAMPLER_REGISTERS] = {};

        NativeTexture* _renderTargets[MAX_RENDER_TARGETS]	   = {};
        int			   _renderTargetLayers[MAX_RENDER_TARGETS] = {};
        int			   _renderTargetCount					   = 0;
        NativeTexture* _depthTarget							   = nullptr;
        int			   _depthLayer							   = 0;

        BlendState		  _blend	= {};
        DepthStencilState _depth	= {};
        RasterState		  _raster	= {};
        RendererViewport  _viewport = {};
        int				  _scissor[4] = {}; // Left, top, right, bottom.

        // What GL has, to skip calls that change nothing.
        bool   _framebufferDirty = true;
        bool   _blendDirty		 = true;
        bool   _depthDirty		 = true;
        bool   _rasterDirty		 = true;
        bool   _viewportDirty	 = true;
        bool   _scissorDirty	 = true;
        bool   _inputsDirty		 = true;
        GLuint _currentProgram	 = 0;
        GLuint _currentFramebuffer = 0;
        GLuint _boundTextures[MAX_TEXTURE_UNITS] = {};
        GLuint _boundSamplers[MAX_TEXTURE_UNITS] = {};
        GLuint _boundUniformBuffers[MAX_CONSTANT_BUFFER_REGISTERS] = {};
        GLuint _boundElementBuffer = 0;
        bool   _enabledAttributes[MAX_VERTEX_ATTRIBUTES] = {};
        const Program* _inputsProgram = nullptr;
        int			   _inputsBaseVertex = 0;

        std::map<std::vector<GLuint>, GLuint> _framebuffers = {};
        std::map<GLuint, unsigned int>		  _drawBufferMasks = {};

        // Objects of the executor itself.
        GLuint _vertexArray		 = 0;
        GLuint _zeroTextures[4]	 = {}; // 2D, 2D array, 3D, cube.
        GLuint _zeroUniformBuffer = 0;
        GLuint _fetchSampler	 = 0;
        GLuint _defaultSampler	 = 0;
        GLuint _pointWrapSampler = 0;
        GLuint _linearWrapSampler  = 0;
        GLuint _linearClampSampler = 0;
        GLuint _anisotropicWrapSampler	= 0;
        GLuint _anisotropicClampSampler = 0;
        GLuint _pointClampSampler = 0;
        GLuint _spriteProgram	 = 0;
        GLint  _spriteViewportLocation = -1;
        GLuint _srgbCopyProgram	 = 0;
        GLuint _streamBuffer	 = 0;
        NativeBuffer		 _streamNative = {};
        NativeConstantBuffer _smaaBuffer   = {};
        std::vector<unsigned char> _repackBuffer = {};

        GLuint GetFramebuffer(NativeTexture* const* targets, const int* layers, int count, NativeTexture* depth, int depthLayer);
        void   BindFramebuffer(GLuint framebuffer);
        void   ApplyFramebuffer(unsigned int outputMask);
        void   SetDrawBufferMask(GLuint framebuffer, int count, unsigned int mask);
        void   ApplyFixedFunction();
        void   ApplyConstantBuffers(const Program& program);
        void   ApplyTextures(const Program& program);
        void   ApplyVertexInputs(const Program& program, int baseVertex);
        void   UploadConstantBuffer(NativeConstantBuffer& buffer, const BlockLayout& layout);
        void   RefreshView(NativeTexture* view);
        bool   IsBoundAsOutput(const NativeTexture* texture) const;
        void   MarkOutputsWritten();
        bool   PrepareDraw(int baseVertex);
        void   InvalidateGLState();
        void   BindTextureUnit(int unit, GLenum target, GLuint texture, GLuint sampler);
        GLuint GetZeroTexture(GLenum target) const;
        GLuint CreateSampler(GLint minFilter, GLint magFilter, GLint wrap, float maxLod, float anisotropy);
        GLuint CreateInternalProgram(const char* vertexSource, const char* pixelSource);

    public:
        void Initialize(ShaderLibrary* shaders, bool floatLinear, float maxAnisotropy);

        // Once the game's shaders are loaded: the samplers they declare themselves.
        void CreateEffectSamplers();
        void Release();

        // State, as the device methods of the same names set it in Direct3D 11.
        void SetBlendState(const BlendState& state);
        void SetDepthState(const DepthStencilState& state);
        void SetRasterState(const RasterState& state);
        void SetViewport(const RendererViewport& viewport);
        void SetScissor(int left, int top, int right, int bottom);
        void SetPrimitiveType(GLenum topology);
        void SetInputLayout(NativeInputLayout* layout);
        void SetVertexBuffer(NativeBuffer* buffer, int stride);
        void SetIndexBuffer(NativeBuffer* buffer);
        void SetShaderStage(int stage, const ShaderStageInfo* info, const float* smaaMetrics);
        void SetConstantBuffer(int stage, int reg, NativeConstantBuffer* buffer);
        void SetTexture(int stage, int reg, NativeTexture* texture);
        void SetSamplers(bool pointFilter);
        void SetRenderTargets(NativeTexture* const* targets, const int* layers, int count, NativeTexture* depth, int depthLayer);
        void ClearState();

        // Work.
        void DrawIndexed(int count, int baseIndex, int baseVertex, int instances);
        void DrawArrays(int count, int baseVertex, int instances);
        void DrawPrimitives(const std::vector<Vertex>& vertices, GLenum topology);
        void DrawSprites(const std::vector<SpriteDraw>& draws, const BlendState& blend);
        void ClearColor(NativeTexture* target, int layer, const float* color);
        void ClearDepthStencil(NativeTexture* target, int layer, bool depth, bool stencil, float depthValue, int stencilValue);
        void CopyTexture(NativeTexture* source, NativeTexture* destination);
        void Present(NativeTexture* backBuffer);
        void UpdateConstantBuffer(NativeConstantBuffer* buffer, std::vector<unsigned char>& data);
        void ReadPixels(NativeTexture* source, std::vector<unsigned char>& pixels);

        // Binds an index buffer to fill it. That changes the vertex array's binding, which is tracked here.
        void BindElementBufferForUpload(GLuint buffer);

        // Drops every reference to a texture about to be deleted.
        void Forget(NativeTexture* texture);
        void Forget(NativeConstantBuffer* buffer);
        void Forget(NativeBuffer* buffer);
        void Forget(NativeInputLayout* layout);
    };
}

#endif
