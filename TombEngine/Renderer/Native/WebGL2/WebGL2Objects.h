#pragma once

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include <GLES3/gl3.h>

#include "Renderer/Graphics/IConstantBuffer.h"
#include "Renderer/Graphics/IDepthTarget.h"
#include "Renderer/Graphics/IGpuReadbackBuffer.h"
#include "Renderer/Graphics/IIndexBuffer.h"
#include "Renderer/Graphics/IInputLayout.h"
#include "Renderer/Graphics/IRenderTarget2D.h"
#include "Renderer/Graphics/IRenderTargetCube.h"
#include "Renderer/Graphics/IShader.h"
#include "Renderer/Graphics/IStructuredBuffer.h"
#include "Renderer/Graphics/ITexture2D.h"
#include "Renderer/Graphics/ITexture3D.h"
#include "Renderer/Graphics/IVertexBuffer.h"
#include "Renderer/RendererEnums.h"

namespace TEN::Renderer::Native::WebGL2
{
    using namespace TEN::Renderer::Graphics;

    class WebGL2GraphicsDevice;
    struct ShaderStageInfo;
    struct BlockLayout;

    // GL objects. They belong to the browser's main thread: the engine's objects
    // below only point at them, and commands recorded on any thread use them when
    // they run. Deleting one is a command too, so it happens after every command
    // recorded before it.

    struct NativeTexture
    {
        GLuint Texture		  = 0;
        GLenum Target		  = GL_TEXTURE_2D;
        GLenum InternalFormat = GL_RGBA8;
        int	   Width		  = 0;
        int	   Height		  = 0;
        int	   Layers		  = 1;
        int	   Levels		  = 1;
        bool   IsFloat32	  = false;
        bool   OwnsTexture	  = true;

        // Bumped whenever the texture is drawn to or cleared.
        unsigned int Generation = 0;

        // An sRGB view of another texture, which Direct3D 11 makes from typeless
        // textures. WebGL 2 has no views, so the view is a texture of its own,
        // refreshed from its parent before it is read.
        NativeTexture* ViewOf		  = nullptr;
        unsigned int   ViewGeneration = 0xFFFFFFFF;
    };

    struct NativeBuffer
    {
        GLuint Buffer = 0;
        int	   Size	  = 0;
    };

    struct NativeConstantBuffer
    {
        GLuint Buffer		= 0;
        int	   BufferSize	= 0;
        std::vector<unsigned char> Data = {};

        unsigned int	   Version		   = 0;
        unsigned int	   UploadedVersion = 0xFFFFFFFF;
        const BlockLayout* UploadedLayout  = nullptr;
        int				   DirtyBytes	   = 0;
    };

    struct NativeInputElement
    {
        std::string		  Semantic = {};
        int				  Index	   = 0;
        VertexInputFormat Format   = VertexInputFormat::VI_RGBA32_Float;
        int				  Offset   = 0;
    };

    struct NativeInputLayout
    {
        std::vector<NativeInputElement> Elements = {};
    };

    struct NativeReadback
    {
        GLuint PackBuffers[2] = {};
        GLsync Fences[2]	  = {};
        int	   ByteSize		  = 0;
        int	   Width		  = 0;
        int	   Height		  = 0;
        GLenum Format		  = GL_RGBA;
        GLenum Type			  = GL_UNSIGNED_BYTE;
        int	   WriteIndex	  = 0;
        int	   SubmitCount	  = 0;

        // Filled on the main thread once a copy has reached the CPU.
        std::mutex				   Mutex	 = {};
        std::vector<unsigned char> Completed = {};
        bool					   HasData	 = false;
    };

    // Engine-side objects.

    class IWebGL2Texture
    {
    public:
        virtual ~IWebGL2Texture() = default;
        virtual NativeTexture* GetNativeTexture() = 0;
    };

    class WebGL2Texture2D : public ITexture2D, public IWebGL2Texture
    {
    private:
        WebGL2GraphicsDevice* _device = nullptr;
        NativeTexture*		  _native = nullptr;
        int					  _width  = 0;
        int					  _height = 0;

    public:
        WebGL2Texture2D(WebGL2GraphicsDevice* device, NativeTexture* native, int width, int height);
        ~WebGL2Texture2D() override;

        int GetWidth() override { return _width; }
        int GetHeight() override { return _height; }
        bool IsValid() override { return _native != nullptr; }
        NativeTexture* GetNativeTexture() override { return _native; }
    };

    class WebGL2RenderTarget2D : public IRenderTarget2D, public IWebGL2Texture
    {
    private:
        WebGL2GraphicsDevice* _device	 = nullptr;
        NativeTexture*		  _native	 = nullptr;
        int					  _width	 = 0;
        int					  _height	 = 0;
        int					  _arraySize = 1;

    public:
        WebGL2RenderTarget2D(WebGL2GraphicsDevice* device, NativeTexture* native, int width, int height, int arraySize);
        ~WebGL2RenderTarget2D() override;

        int GetWidth() override { return _width; }
        int GetHeight() override { return _height; }
        int GetArraySize() override { return _arraySize; }
        bool IsValid() override { return _native != nullptr; }
        NativeTexture* GetNativeTexture() override { return _native; }
    };

    class WebGL2DepthTarget : public IDepthTarget
    {
    private:
        WebGL2GraphicsDevice* _device	 = nullptr;
        NativeTexture*		  _native	 = nullptr;
        int					  _arraySize = 1;

    public:
        WebGL2DepthTarget(WebGL2GraphicsDevice* device, NativeTexture* native, int arraySize);
        ~WebGL2DepthTarget() override;

        int GetArraySize() override { return _arraySize; }
        NativeTexture* GetNativeTexture() { return _native; }
    };

    class WebGL2Texture3D : public ITexture3D, public IWebGL2Texture
    {
    private:
        WebGL2GraphicsDevice* _device = nullptr;
        NativeTexture*		  _native = nullptr;
        int					  _width  = 0;
        int					  _height = 0;
        int					  _depth  = 0;

    public:
        WebGL2Texture3D(WebGL2GraphicsDevice* device, NativeTexture* native, int width, int height, int depth);
        ~WebGL2Texture3D() override;

        int GetWidth() override { return _width; }
        int GetHeight() override { return _height; }
        int GetDepth() override { return _depth; }
        bool IsValid() override { return _native != nullptr; }
        NativeTexture* GetNativeTexture() override { return _native; }
    };

    class WebGL2RenderTargetCube : public IRenderTargetCube, public IWebGL2Texture
    {
    private:
        WebGL2GraphicsDevice* _device = nullptr;
        NativeTexture*		  _native = nullptr;
        int					  _size	  = 0;

    public:
        WebGL2RenderTargetCube(WebGL2GraphicsDevice* device, NativeTexture* native, int size);
        ~WebGL2RenderTargetCube() override;

        int GetWidth() override { return _size; }
        int GetHeight() override { return _size; }
        NativeTexture* GetNativeTexture() override { return _native; }
    };

    class WebGL2VertexBuffer : public IVertexBuffer
    {
    private:
        WebGL2GraphicsDevice* _device = nullptr;
        NativeBuffer*		  _native = nullptr;
        int					  _stride = 0;

    public:
        WebGL2VertexBuffer(WebGL2GraphicsDevice* device, NativeBuffer* native, int stride);
        ~WebGL2VertexBuffer() override;

        NativeBuffer* GetNativeBuffer() { return _native; }
        int GetStride() const { return _stride; }
    };

    class WebGL2IndexBuffer : public IIndexBuffer
    {
    private:
        WebGL2GraphicsDevice* _device = nullptr;
        NativeBuffer*		  _native = nullptr;

    public:
        WebGL2IndexBuffer(WebGL2GraphicsDevice* device, NativeBuffer* native);
        ~WebGL2IndexBuffer() override;

        NativeBuffer* GetNativeBuffer() { return _native; }
    };

    class WebGL2ConstantBuffer : public IConstantBuffer
    {
    private:
        WebGL2GraphicsDevice* _device = nullptr;
        NativeConstantBuffer* _native = nullptr;
        int					  _size	  = 0;

    public:
        WebGL2ConstantBuffer(WebGL2GraphicsDevice* device, NativeConstantBuffer* native, int size);
        ~WebGL2ConstantBuffer() override;

        NativeConstantBuffer* GetNativeBuffer() { return _native; }
        int GetSize() const { return _size; }
    };

    class WebGL2StructuredBuffer : public IStructuredBuffer
    {
    private:
        WebGL2GraphicsDevice* _device		= nullptr;
        NativeTexture*		  _native		= nullptr;
        int					  _stride		= 0;
        int					  _elementCount = 0;

    public:
        WebGL2StructuredBuffer(WebGL2GraphicsDevice* device, NativeTexture* native, int stride, int elementCount);
        ~WebGL2StructuredBuffer() override;

        NativeTexture* GetNativeTexture() { return _native; }
        int GetStride() const { return _stride; }
        int GetElementCount() const { return _elementCount; }
    };

    class WebGL2InputLayout : public IInputLayout
    {
    private:
        WebGL2GraphicsDevice* _device = nullptr;
        NativeInputLayout*	  _native = nullptr;

    public:
        WebGL2InputLayout(WebGL2GraphicsDevice* device, NativeInputLayout* native);
        ~WebGL2InputLayout() override;

        NativeInputLayout* GetNativeLayout() { return _native; }
    };

    class WebGL2Shader : public IShader
    {
    public:
        const ShaderStageInfo* VertexStage	   = nullptr;
        const ShaderStageInfo* PixelStage	   = nullptr;
        bool				   HasSmaaMetrics  = false;
        float				   SmaaMetrics[4]  = {};
    };

    class WebGL2GpuReadbackBuffer : public IGpuReadbackBuffer
    {
    private:
        WebGL2GraphicsDevice* _device = nullptr;
        NativeReadback*		  _native = nullptr;

    public:
        WebGL2GpuReadbackBuffer(WebGL2GraphicsDevice* device, NativeReadback* native);
        ~WebGL2GpuReadbackBuffer() override;

        void SubmitCopy(ITexture2D* src) override;
        bool TryRead(void* outBytes, int byteCount) override;
    };
}

#endif
