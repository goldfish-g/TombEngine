#include "framework.h"
#include "Renderer/Native/WebGL2/WebGL2GraphicsDevice.h"

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>

#include "Renderer/Native/WebGL2/WebGL2PrimitiveBatch.h"
#include "Renderer/Native/WebGL2/WebGL2SpriteBatch.h"
#include "Renderer/Native/WebGL2/WebGL2SpriteFont.h"
#include "Specific/configuration.h"
#include "Specific/EngineMain.h"

namespace TEN::Renderer::Native::WebGL2
{
    constexpr GLenum MAX_TEXTURE_MAX_ANISOTROPY = 0x84FF;
    constexpr GLenum UNMASKED_VENDOR			= 0x9245;
    constexpr GLenum UNMASKED_RENDERER			= 0x9246;

    WebGL2GraphicsDevice* WebGL2GraphicsDevice::_instance = nullptr;

    struct GLFormat
    {
        GLenum Internal	 = GL_RGBA8;
        GLenum Format	 = GL_RGBA;
        GLenum Type		 = GL_UNSIGNED_BYTE;
        int	   PixelSize = 4;
        bool   Float32	 = false;
        bool   Bgra		 = false;
    };

    static GLFormat GetGLFormat(SurfaceFormat format)
    {
        switch (format)
        {
        case SurfaceFormat::SF_RGBA8_Unorm_Srgb: return { GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false, false };
        case SurfaceFormat::SF_RG8_Unorm: return { GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2, false, false };
        case SurfaceFormat::SF_R8_Unorm: return { GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, false, false };
        case SurfaceFormat::SF_R32_Float: return { GL_R32F, GL_RED, GL_FLOAT, 4, true, false };
        case SurfaceFormat::SF_RGBA32_Float: return { GL_RGBA32F, GL_RGBA, GL_FLOAT, 16, true, false };
        case SurfaceFormat::SF_BGRA8_Unorm: return { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false, true };
        case SurfaceFormat::SF_R11G11B10_Float: return { GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV, 4, false, false };
        case SurfaceFormat::SF_RGBA16_Float: return { GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8, false, false };
        case SurfaceFormat::SF_R16_Float: return { GL_R16F, GL_RED, GL_HALF_FLOAT, 2, false, false };
        default: return {};
        }
    }

    // WebGL has no BGRA formats: such pixels are reordered on the way in.
    static void SwizzleBgra(unsigned char* data, int pixelCount)
    {
        for (int i = 0; i < pixelCount; i++)
            std::swap(data[i * 4], data[i * 4 + 2]);
    }

    static NativeTexture* GetNative(ITextureBase* texture)
    {
        auto* webTexture = dynamic_cast<IWebGL2Texture*>(texture);
        return (webTexture != nullptr) ? webTexture->GetNativeTexture() : nullptr;
    }

    static NativeTexture* GetNative(IDepthTarget* depth)
    {
        auto* webDepth = static_cast<WebGL2DepthTarget*>(depth);
        return (webDepth != nullptr) ? webDepth->GetNativeTexture() : nullptr;
    }

    static int GetStageIndex(ShaderStage stage)
    {
        return (stage == ShaderStage::VertexShader) ? STAGE_VERTEX : STAGE_PIXEL;
    }

    static void BeginTextureUpload(GLenum target, GLuint texture)
    {
        glActiveTexture(GL_TEXTURE0 + UPLOAD_TEXTURE_UNIT);
        glBindTexture(target, texture);
    }

    static void UploadDecodedTexture(NativeTexture* native, const DecodedTexture& texture)
    {
        glGenTextures(1, &native->Texture);
        BeginTextureUpload(GL_TEXTURE_2D, native->Texture);
        glTexStorage2D(GL_TEXTURE_2D, native->Levels, native->InternalFormat, native->Width, native->Height);

        for (int level = 0; level < (int)texture.Levels.size(); level++)
        {
            const auto& data = texture.Levels[level];
            if (texture.Compressed)
            {
                glCompressedTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, data.Width, data.Height, texture.InternalFormat,
                                          (GLsizei)data.Data.size(), data.Data.data());
            }
            else
            {
                glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, data.Width, data.Height, texture.Format, texture.Type, data.Data.data());
            }
        }

        if (texture.GenerateMips)
            glGenerateMipmap(GL_TEXTURE_2D);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, native->Levels - 1);
    }

    // Engine-side objects.

    WebGL2Texture2D::WebGL2Texture2D(WebGL2GraphicsDevice* device, NativeTexture* native, int width, int height) :
        _device(device), _native(native), _width(width), _height(height)
    {
    }

    WebGL2Texture2D::~WebGL2Texture2D()
    {
        _device->ReleaseTexture(_native);
    }

    WebGL2RenderTarget2D::WebGL2RenderTarget2D(WebGL2GraphicsDevice* device, NativeTexture* native, int width, int height, int arraySize) :
        _device(device), _native(native), _width(width), _height(height), _arraySize(arraySize)
    {
    }

    WebGL2RenderTarget2D::~WebGL2RenderTarget2D()
    {
        _device->ReleaseTexture(_native);
    }

    WebGL2DepthTarget::WebGL2DepthTarget(WebGL2GraphicsDevice* device, NativeTexture* native, int arraySize) :
        _device(device), _native(native), _arraySize(arraySize)
    {
    }

    WebGL2DepthTarget::~WebGL2DepthTarget()
    {
        _device->ReleaseTexture(_native);
    }

    WebGL2Texture3D::WebGL2Texture3D(WebGL2GraphicsDevice* device, NativeTexture* native, int width, int height, int depth) :
        _device(device), _native(native), _width(width), _height(height), _depth(depth)
    {
    }

    WebGL2Texture3D::~WebGL2Texture3D()
    {
        _device->ReleaseTexture(_native);
    }

    WebGL2RenderTargetCube::WebGL2RenderTargetCube(WebGL2GraphicsDevice* device, NativeTexture* native, int size) :
        _device(device), _native(native), _size(size)
    {
    }

    WebGL2RenderTargetCube::~WebGL2RenderTargetCube()
    {
        _device->ReleaseTexture(_native);
    }

    WebGL2VertexBuffer::WebGL2VertexBuffer(WebGL2GraphicsDevice* device, NativeBuffer* native, int stride) :
        _device(device), _native(native), _stride(stride)
    {
    }

    WebGL2VertexBuffer::~WebGL2VertexBuffer()
    {
        _device->ReleaseBuffer(_native);
    }

    WebGL2IndexBuffer::WebGL2IndexBuffer(WebGL2GraphicsDevice* device, NativeBuffer* native) :
        _device(device), _native(native)
    {
    }

    WebGL2IndexBuffer::~WebGL2IndexBuffer()
    {
        _device->ReleaseBuffer(_native);
    }

    WebGL2ConstantBuffer::WebGL2ConstantBuffer(WebGL2GraphicsDevice* device, NativeConstantBuffer* native, int size) :
        _device(device), _native(native), _size(size)
    {
    }

    WebGL2ConstantBuffer::~WebGL2ConstantBuffer()
    {
        _device->ReleaseConstantBuffer(_native);
    }

    WebGL2StructuredBuffer::WebGL2StructuredBuffer(WebGL2GraphicsDevice* device, NativeTexture* native, int stride, int elementCount) :
        _device(device), _native(native), _stride(stride), _elementCount(elementCount)
    {
    }

    WebGL2StructuredBuffer::~WebGL2StructuredBuffer()
    {
        _device->ReleaseTexture(_native);
    }

    WebGL2InputLayout::WebGL2InputLayout(WebGL2GraphicsDevice* device, NativeInputLayout* native) :
        _device(device), _native(native)
    {
    }

    WebGL2InputLayout::~WebGL2InputLayout()
    {
        _device->ReleaseInputLayout(_native);
    }

    WebGL2GpuReadbackBuffer::WebGL2GpuReadbackBuffer(WebGL2GraphicsDevice* device, NativeReadback* native) :
        _device(device), _native(native)
    {
    }

    WebGL2GpuReadbackBuffer::~WebGL2GpuReadbackBuffer()
    {
        _device->ReleaseReadback(_native);
    }

    void WebGL2GpuReadbackBuffer::SubmitCopy(ITexture2D* src)
    {
        auto* source = GetNative(src);
        if (source == nullptr)
            return;

        auto* native = _native;
        auto* executor = _device->GetExecutor();
        _device->Record([native, source, executor]()
        {
            auto pixels = std::vector<unsigned char>{};
            executor->ReadPixels(source, pixels);

            auto lock = std::lock_guard<std::mutex>(native->Mutex);
            native->Completed = std::move(pixels);
            native->HasData = true;
        });
    }

    bool WebGL2GpuReadbackBuffer::TryRead(void* outBytes, int byteCount)
    {
        auto lock = std::lock_guard<std::mutex>(_native->Mutex);
        if (!_native->HasData || outBytes == nullptr || byteCount > (int)_native->Completed.size())
            return false;

        memcpy(outBytes, _native->Completed.data(), byteCount);
        return true;
    }

    // Device.

    WebGL2GraphicsDevice::WebGL2GraphicsDevice()
    {
        _instance = this;
    }

    WebGL2GraphicsDevice::~WebGL2GraphicsDevice()
    {
        _queue.Shutdown();
        if (_instance == this)
            _instance = nullptr;
    }

    void WebGL2GraphicsDevice::ExecuteFrame()
    {
        _queue.ExecutePendingFrame();
    }

    void ExecuteBrowserFrame()
    {
        auto* device = WebGL2GraphicsDevice::GetInstance();
        if (device != nullptr)
            device->ExecuteFrame();
    }

    unsigned int WebGL2GraphicsDevice::GetPresentedFrameCount()
    {
        return _queue.GetFrameCount();
    }

    void WebGL2GraphicsDevice::ReleaseTexture(NativeTexture* native)
    {
        if (native == nullptr)
            return;

        auto* executor = &_executor;
        Record([native, executor]()
        {
            executor->Forget(native);
            if (native->OwnsTexture && native->Texture != 0)
                glDeleteTextures(1, &native->Texture);

            delete native;
        });
    }

    void WebGL2GraphicsDevice::ReleaseBuffer(NativeBuffer* native)
    {
        auto* executor = &_executor;
        Record([native, executor]()
        {
            executor->Forget(native);
            glDeleteBuffers(1, &native->Buffer);
            delete native;
        });
    }

    void WebGL2GraphicsDevice::ReleaseConstantBuffer(NativeConstantBuffer* native)
    {
        auto* executor = &_executor;
        Record([native, executor]()
        {
            executor->Forget(native);
            glDeleteBuffers(1, &native->Buffer);
            delete native;
        });
    }

    void WebGL2GraphicsDevice::ReleaseInputLayout(NativeInputLayout* native)
    {
        auto* executor = &_executor;
        Record([native, executor]()
        {
            executor->Forget(native);
            delete native;
        });
    }

    void WebGL2GraphicsDevice::ReleaseReadback(NativeReadback* native)
    {
        Record([native]() { delete native; });
    }

    NativeTexture* WebGL2GraphicsDevice::CreateNativeTexture(GLenum target, SurfaceFormat format, int width, int height, int layers, int levels)
    {
        auto glFormat = GetGLFormat(format);

        auto* native = new NativeTexture();
        native->Target = target;
        native->InternalFormat = glFormat.Internal;
        native->Width = width;
        native->Height = height;
        native->Layers = layers;
        native->Levels = levels;
        native->IsFloat32 = glFormat.Float32;

        Record([native]()
        {
            glGenTextures(1, &native->Texture);
            BeginTextureUpload(native->Target, native->Texture);

            if (native->Target == GL_TEXTURE_2D || native->Target == GL_TEXTURE_CUBE_MAP)
                glTexStorage2D(native->Target, native->Levels, native->InternalFormat, native->Width, native->Height);
            else
                glTexStorage3D(native->Target, native->Levels, native->InternalFormat, native->Width, native->Height, native->Layers);
        });

        return native;
    }

    NativeTexture* WebGL2GraphicsDevice::CreateNativeDepth(DepthFormat format, int width, int height, int layers)
    {
        auto* native = new NativeTexture();
        native->Target = (layers > 1) ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
        native->InternalFormat = (format == DepthFormat::Depth24Stencil8) ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT32F;
        native->Width = width;
        native->Height = height;
        native->Layers = layers;

        Record([native]()
        {
            glGenTextures(1, &native->Texture);
            BeginTextureUpload(native->Target, native->Texture);

            if (native->Target == GL_TEXTURE_2D)
                glTexStorage2D(GL_TEXTURE_2D, 1, native->InternalFormat, native->Width, native->Height);
            else
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, native->InternalFormat, native->Width, native->Height, native->Layers);

            glTexParameteri(native->Target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(native->Target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        });

        return native;
    }

    std::unique_ptr<IVertexBuffer> WebGL2GraphicsDevice::CreateVertexBuffer(int numVertices, int vertexSize, void* data)
    {
        auto* native = new NativeBuffer();
        native->Size = std::max(numVertices, 1) * vertexSize;

        auto bytes = std::vector<unsigned char>{};
        if (data != nullptr && numVertices > 0)
            bytes.assign((unsigned char*)data, (unsigned char*)data + numVertices * vertexSize);

        Record([native, bytes = std::move(bytes)]()
        {
            glGenBuffers(1, &native->Buffer);
            glBindBuffer(GL_ARRAY_BUFFER, native->Buffer);
            glBufferData(GL_ARRAY_BUFFER, native->Size, bytes.empty() ? nullptr : bytes.data(), GL_DYNAMIC_DRAW);
        });

        return std::make_unique<WebGL2VertexBuffer>(this, native, vertexSize);
    }

    void WebGL2GraphicsDevice::UpdateVertexBuffer(IVertexBuffer* vertexBuffer, int startVertex, int count, void* data)
    {
        auto* buffer = static_cast<WebGL2VertexBuffer*>(vertexBuffer);
        if (buffer == nullptr || data == nullptr || count <= 0)
            return;

        // As in Direct3D 11, the vertices from startVertex go to the start of the
        // buffer, which is otherwise discarded.
        int stride = buffer->GetStride();
        auto* native = buffer->GetNativeBuffer();
        int size = std::min(count * stride, native->Size);
        auto bytes = std::vector<unsigned char>((unsigned char*)data + startVertex * stride, (unsigned char*)data + startVertex * stride + size);

        Record([native, bytes = std::move(bytes)]()
        {
            glBindBuffer(GL_ARRAY_BUFFER, native->Buffer);
            glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes.size(), bytes.data());
        });
    }

    void WebGL2GraphicsDevice::BindVertexBuffer(IVertexBuffer* vertexBuffer)
    {
        auto* buffer = static_cast<WebGL2VertexBuffer*>(vertexBuffer);
        auto* native = (buffer != nullptr) ? buffer->GetNativeBuffer() : nullptr;
        int stride = (buffer != nullptr) ? buffer->GetStride() : 0;
        auto* executor = &_executor;
        Record([executor, native, stride]() { executor->SetVertexBuffer(native, stride); });
    }

    std::unique_ptr<IIndexBuffer> WebGL2GraphicsDevice::CreateIndexBuffer(int numIndices, int* data)
    {
        auto* native = new NativeBuffer();
        native->Size = std::max(numIndices, 1) * (int)sizeof(int);

        auto bytes = std::vector<unsigned char>{};
        if (data != nullptr && numIndices > 0)
            bytes.assign((unsigned char*)data, (unsigned char*)data + numIndices * sizeof(int));

        auto* executor = &_executor;
        Record([native, executor, bytes = std::move(bytes)]()
        {
            glGenBuffers(1, &native->Buffer);
            executor->BindElementBufferForUpload(native->Buffer);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, native->Size, bytes.empty() ? nullptr : bytes.data(), GL_DYNAMIC_DRAW);
        });

        return std::make_unique<WebGL2IndexBuffer>(this, native);
    }

    void WebGL2GraphicsDevice::UpdateIndexBuffer(IIndexBuffer* indexBuffer, int numIndices, int startIndex, int* data)
    {
        auto* buffer = static_cast<WebGL2IndexBuffer*>(indexBuffer);
        if (buffer == nullptr || data == nullptr || numIndices <= 0)
            return;

        auto* native = buffer->GetNativeBuffer();
        int size = std::min(numIndices * (int)sizeof(int), native->Size);
        auto bytes = std::vector<unsigned char>((unsigned char*)&data[startIndex], (unsigned char*)&data[startIndex] + size);

        auto* executor = &_executor;
        Record([native, executor, bytes = std::move(bytes)]()
        {
            executor->BindElementBufferForUpload(native->Buffer);
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, (GLsizeiptr)bytes.size(), bytes.data());
        });
    }

    void WebGL2GraphicsDevice::BindIndexBuffer(IIndexBuffer* indexBuffer)
    {
        auto* buffer = static_cast<WebGL2IndexBuffer*>(indexBuffer);
        auto* native = (buffer != nullptr) ? buffer->GetNativeBuffer() : nullptr;
        auto* executor = &_executor;
        Record([executor, native]() { executor->SetIndexBuffer(native); });
    }

    std::unique_ptr<IRenderSurface2D> WebGL2GraphicsDevice::CreateRenderSurface2D(int width, int height, SurfaceFormat colorFormat, bool isTypeless, DepthFormat depthFormat)
    {
        auto glFormat = GetGLFormat(colorFormat);
        if (glFormat.Float32 && !_floatRenderable)
            TENLog("This browser can't draw to 32-bit float textures (EXT_color_buffer_float).", LogLevel::Warning);

        auto* color = CreateNativeTexture(GL_TEXTURE_2D, colorFormat, width, height, 1, 1);
        auto renderTarget = std::make_unique<WebGL2RenderTarget2D>(this, color, width, height, 1);

        auto depthTarget = std::unique_ptr<IDepthTarget>();
        if (depthFormat != DepthFormat::None)
            depthTarget = std::make_unique<WebGL2DepthTarget>(this, CreateNativeDepth(depthFormat, width, height, 1), 1);

        return std::make_unique<IRenderSurface2D>(std::move(renderTarget), std::move(depthTarget));
    }

    std::unique_ptr<IRenderSurface2D> WebGL2GraphicsDevice::CreateRenderSurface2D(int width, int height, int arraySize, SurfaceFormat colorFormat, DepthFormat depthFormat)
    {
        auto* color = CreateNativeTexture(GL_TEXTURE_2D_ARRAY, colorFormat, width, height, arraySize, 1);
        auto renderTarget = std::make_unique<WebGL2RenderTarget2D>(this, color, width, height, arraySize);

        auto depthTarget = std::unique_ptr<IDepthTarget>();
        if (depthFormat != DepthFormat::None)
        {
            auto* depth = CreateNativeDepth(depthFormat, width, height, arraySize);
            if (arraySize == 1)
            {
                // Array of one: still an array, as the colour target.
                depth->Target = GL_TEXTURE_2D_ARRAY;
            }

            depthTarget = std::make_unique<WebGL2DepthTarget>(this, depth, arraySize);
        }

        return std::make_unique<IRenderSurface2D>(std::move(renderTarget), std::move(depthTarget));
    }

    std::unique_ptr<IRenderSurface2D> WebGL2GraphicsDevice::CreateRenderSurface2D(IRenderSurface2D* parentRenderTarget, SurfaceFormat colorFormat)
    {
        auto* parent = GetNative(parentRenderTarget->GetRenderTarget());
        int width = parentRenderTarget->GetRenderTarget()->GetWidth();
        int height = parentRenderTarget->GetRenderTarget()->GetHeight();

        NativeTexture* view = nullptr;
        if (colorFormat == SurfaceFormat::SF_RGBA8_Unorm_Srgb)
        {
            // An sRGB view of a typeless texture: a texture of its own, see NativeTexture.
            view = CreateNativeTexture(GL_TEXTURE_2D, colorFormat, width, height, 1, 1);
            view->ViewOf = parent;
        }
        else
        {
            // A view in the parent's own format reads the parent itself.
            view = new NativeTexture(*parent);
            view->OwnsTexture = false;
            Record([view, parent]() { view->Texture = parent->Texture; });
        }

        auto renderTarget = std::make_unique<WebGL2RenderTarget2D>(this, view, width, height, 1);
        return std::make_unique<IRenderSurface2D>(std::move(renderTarget), std::unique_ptr<IDepthTarget>());
    }

    IRenderTargetCube* WebGL2GraphicsDevice::CreateRenderTargetCube(int size, SurfaceFormat colorFormat)
    {
        auto* native = CreateNativeTexture(GL_TEXTURE_CUBE_MAP, colorFormat, size, size, 1, 1);
        return new WebGL2RenderTargetCube(this, native, size);
    }

    std::unique_ptr<ITexture2D> WebGL2GraphicsDevice::CreateTexture2D(int width, int height, SurfaceFormat format, void* data, bool isDynamic)
    {
        auto glFormat = GetGLFormat(format);
        auto* native = CreateNativeTexture(GL_TEXTURE_2D, format, width, height, 1, 1);

        if (data != nullptr)
        {
            auto bytes = std::vector<unsigned char>((unsigned char*)data, (unsigned char*)data + width * height * glFormat.PixelSize);
            if (glFormat.Bgra)
                SwizzleBgra(bytes.data(), width * height);

            Record([native, glFormat, bytes = std::move(bytes)]()
            {
                BeginTextureUpload(GL_TEXTURE_2D, native->Texture);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, native->Width, native->Height, glFormat.Format, glFormat.Type, bytes.data());
            });
        }

        return std::make_unique<WebGL2Texture2D>(this, native, width, height);
    }

    std::unique_ptr<ITexture2D> WebGL2GraphicsDevice::CreateTexture2DFromFile(const std::string fileName)
    {
        auto file = std::ifstream(fileName, std::ios::binary);
        if (!file)
            throw std::runtime_error("Opening Texture file '" + fileName + "': file not found.");

        auto data = std::vector<unsigned char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return CreateTexture2DFromFileInMemory((int)data.size(), data.data());
    }

    std::unique_ptr<ITexture2D> WebGL2GraphicsDevice::CreateTexture2DFromFileInMemory(int dataSize, unsigned char* data)
    {
        // Decoded on the calling thread (often the level loading one), uploaded on the main one.
        auto decoded = DecodeTextureFile(data, dataSize, _textureCapabilities);
        if (!decoded.Valid)
            throw std::runtime_error("Texture data of " + std::to_string(dataSize) + " bytes could not be decoded.");

        return CreateTexture2DFromDecoded(std::move(decoded));
    }

    std::unique_ptr<ITexture2D> WebGL2GraphicsDevice::CreateTexture2DFromDecoded(DecodedTexture decoded)
    {
        auto* native = new NativeTexture();
        native->Target = GL_TEXTURE_2D;
        native->InternalFormat = decoded.InternalFormat;
        native->Width = decoded.Width;
        native->Height = decoded.Height;
        native->Levels = decoded.GenerateMips ? GetLevelCount(decoded.Width, decoded.Height) : (int)decoded.Levels.size();

        int width = decoded.Width;
        int height = decoded.Height;
        Record([native, decoded = std::move(decoded)]() { UploadDecodedTexture(native, decoded); });

        return std::make_unique<WebGL2Texture2D>(this, native, width, height);
    }

    void WebGL2GraphicsDevice::UpdateTexture2D(ITexture2D* texture, std::vector<char> data)
    {
        auto* native = GetNative(texture);
        if (native == nullptr)
            return;

        int pixelCount = native->Width * native->Height;
        if ((int)data.size() < pixelCount * 4)
            return;

        // Video frames come as BGRA, the only format the engine updates.
        auto bytes = std::vector<unsigned char>(data.begin(), data.begin() + pixelCount * 4);
        SwizzleBgra(bytes.data(), pixelCount);

        Record([native, bytes = std::move(bytes)]()
        {
            BeginTextureUpload(GL_TEXTURE_2D, native->Texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, native->Width, native->Height, GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
        });
    }

    void WebGL2GraphicsDevice::UpdateTexture2DFromVideo(ITexture2D* texture, int videoID)
    {
        auto* native = GetNative(texture);
        if (native == nullptr)
            return;

        Record([native, videoID]()
        {
            BeginTextureUpload(GL_TEXTURE_2D, native->Texture);
            EM_ASM({ Module['tenWebVideo']['upload']($0, GLctx, $1, $2); }, videoID, native->Width, native->Height);
        });
    }

    std::unique_ptr<ITexture3D> WebGL2GraphicsDevice::CreateTexture3D(int width, int height, int depth, SurfaceFormat format, const void* data)
    {
        auto glFormat = GetGLFormat(format);
        auto* native = CreateNativeTexture(GL_TEXTURE_3D, format, width, height, depth, 1);

        if (data != nullptr)
        {
            auto bytes = std::vector<unsigned char>((const unsigned char*)data, (const unsigned char*)data + width * height * depth * glFormat.PixelSize);
            if (glFormat.Bgra)
                SwizzleBgra(bytes.data(), width * height * depth);

            Record([native, glFormat, bytes = std::move(bytes)]()
            {
                BeginTextureUpload(GL_TEXTURE_3D, native->Texture);
                glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, native->Width, native->Height, native->Layers, glFormat.Format, glFormat.Type, bytes.data());
            });
        }

        return std::make_unique<WebGL2Texture3D>(this, native, width, height, depth);
    }

    void WebGL2GraphicsDevice::SetBlendMode(BlendMode blendMode)
    {
        // The engine's blend states and DirectXTK's CommonStates ones it uses.
        auto state = BlendState{};
        switch (blendMode)
        {
        case BlendMode::AlphaBlend:
            state = { true, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD };
            break;

        case BlendMode::Subtractive:
            state = { true, GL_SRC_ALPHA, GL_ONE, GL_FUNC_REVERSE_SUBTRACT, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD };
            break;

        case BlendMode::Additive:
            state = { true, GL_SRC_ALPHA, GL_ONE, GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE, GL_FUNC_ADD };
            break;

        case BlendMode::Distortion:
            state = { true, GL_ONE, GL_ONE, GL_FUNC_ADD, GL_ONE, GL_ONE, GL_FUNC_ADD };
            break;

        case BlendMode::Screen:
            state = { true, GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_FUNC_ADD, GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD };
            break;

        case BlendMode::Lighten:
            state = { true, GL_ONE, GL_ONE, GL_MAX, GL_SRC_ALPHA, GL_DST_ALPHA, GL_FUNC_ADD };
            break;

        case BlendMode::Exclude:
            state = { true, GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_FUNC_SUBTRACT, GL_ONE, GL_ZERO, GL_FUNC_ADD };
            break;

        case BlendMode::PremultipliedAlphaBlend:
            state = { true, GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD, GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD };
            break;

        default:
            break;
        }

        auto* executor = &_executor;
        Record([executor, state]() { executor->SetBlendState(state); });
    }

    void WebGL2GraphicsDevice::SetDepthState(DepthState depthState)
    {
        // DirectXTK's DepthRead, DepthDefault and DepthNone.
        auto state = DepthStencilState{ true, true, GL_LEQUAL };
        if (depthState == DepthState::Read)
            state.Write = false;
        else if (depthState == DepthState::None)
            state = DepthStencilState{ false, false, GL_LEQUAL };

        auto* executor = &_executor;
        Record([executor, state]() { executor->SetDepthState(state); });
    }

    void WebGL2GraphicsDevice::SetCullMode(CullMode cullMode)
    {
        // The engine's rasterizer states enable the scissor test; DirectXTK's
        // wireframe one, which WebGL can't draw, doesn't.
        auto state = RasterState{ GL_BACK, true };
        switch (cullMode)
        {
        case CullMode::None: state = RasterState{ 0, true }; break;
        case CullMode::Clockwise: state = RasterState{ GL_FRONT, true }; break;
        case CullMode::Wireframe: state = RasterState{ GL_BACK, false }; break;
        default: break;
        }

        auto* executor = &_executor;
        Record([executor, state]() { executor->SetRasterState(state); });
    }

    void WebGL2GraphicsDevice::SetScissor(RendererRectangle rectangle)
    {
        auto* executor = &_executor;
        Record([executor, rectangle]() { executor->SetScissor(rectangle.Left, rectangle.Top, rectangle.Right, rectangle.Bottom); });
    }

    void WebGL2GraphicsDevice::SetScissor(RendererViewport viewport)
    {
        // As the Direct3D 11 device: the width and height are the right and bottom edges.
        auto* executor = &_executor;
        Record([executor, viewport]() { executor->SetScissor(viewport.X, viewport.Y, viewport.Width, viewport.Height); });
    }

    void WebGL2GraphicsDevice::BindTexture(TextureRegister registerType, ITextureBase* texture)
    {
        auto* native = GetNative(texture);
        auto* executor = &_executor;
        int reg = (int)registerType;
        Record([executor, native, reg]() { executor->SetTexture(STAGE_PIXEL, reg, native); });
    }

    void WebGL2GraphicsDevice::BindSamplers(bool pointFilter)
    {
        auto* executor = &_executor;
        Record([executor, pointFilter]() { executor->SetSamplers(pointFilter); });
    }

    void WebGL2GraphicsDevice::UnbindTexture(ShaderStage stage, TextureRegister registerType)
    {
        auto* executor = &_executor;
        int reg = (int)registerType;
        int stageIndex = GetStageIndex(stage);
        Record([executor, stageIndex, reg]() { executor->SetTexture(stageIndex, reg, nullptr); });
    }

    std::unique_ptr<IConstantBuffer> WebGL2GraphicsDevice::CreateConstantBuffer(int size, std::string name)
    {
        auto* native = new NativeConstantBuffer();
        native->Data.resize(size);

        Record([native]()
        {
            glGenBuffers(1, &native->Buffer);
            glBindBuffer(GL_UNIFORM_BUFFER, native->Buffer);
            glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)native->Data.size(), nullptr, GL_DYNAMIC_DRAW);
            native->BufferSize = (int)native->Data.size();
        });

        return std::make_unique<WebGL2ConstantBuffer>(this, native, size);
    }

    void WebGL2GraphicsDevice::UpdateConstantBuffer(IConstantBuffer* constantBuffer, void* data, int size)
    {
        auto* buffer = static_cast<WebGL2ConstantBuffer*>(constantBuffer);
        if (buffer == nullptr || data == nullptr)
            return;

        // As in Direct3D 11: a size copies only that much from the start.
        int bytes = (size > 0 && size < buffer->GetSize()) ? size : buffer->GetSize();
        auto copy = std::vector<unsigned char>((unsigned char*)data, (unsigned char*)data + bytes);

        auto* native = buffer->GetNativeBuffer();
        auto* executor = &_executor;
        Record([executor, native, copy = std::move(copy)]() mutable { executor->UpdateConstantBuffer(native, copy); });
    }

    void WebGL2GraphicsDevice::BindConstantBuffer(ShaderStage shaderStage, ConstantBufferRegister constantBufferType, IConstantBuffer* buffer)
    {
        auto* webBuffer = static_cast<WebGL2ConstantBuffer*>(buffer);
        auto* native = (webBuffer != nullptr) ? webBuffer->GetNativeBuffer() : nullptr;
        auto* executor = &_executor;
        int stageIndex = GetStageIndex(shaderStage);
        int reg = (int)constantBufferType;
        Record([executor, stageIndex, reg, native]() { executor->SetConstantBuffer(stageIndex, reg, native); });
    }

    std::unique_ptr<IStructuredBuffer> WebGL2GraphicsDevice::CreateStructuredBuffer(int stride, int elementCount, std::wstring name)
    {
        // WebGL 2 has no storage buffers: elements go into a float texture the
        // shaders read texel by texel (see compile_shaders.py).
        int width = _shaders.GetDataTextureWidth();
        int texelsPerElement = (stride + 15) / 16;
        int texels = std::max(1, elementCount * texelsPerElement);
        int textureWidth = std::min(texels, width);
        int textureHeight = (texels + width - 1) / width;

        auto* native = CreateNativeTexture(GL_TEXTURE_2D, SurfaceFormat::SF_RGBA32_Float, textureWidth, textureHeight, 1, 1);
        return std::make_unique<WebGL2StructuredBuffer>(this, native, stride, elementCount);
    }

    void WebGL2GraphicsDevice::UpdateStructuredBuffer(IStructuredBuffer* buffer, const void* data, int elementCount)
    {
        auto* structured = static_cast<WebGL2StructuredBuffer*>(buffer);
        if (structured == nullptr || data == nullptr || elementCount <= 0)
            return;

        int stride = structured->GetStride();
        int texelsPerElement = (stride + 15) / 16;
        int count = std::min(elementCount, structured->GetElementCount());
        auto texels = std::vector<unsigned char>(count * texelsPerElement * 16);
        for (int i = 0; i < count; i++)
            memcpy(texels.data() + i * texelsPerElement * 16, (const unsigned char*)data + i * stride, stride);

        auto* native = structured->GetNativeTexture();
        int width = _shaders.GetDataTextureWidth();
        Record([native, width, texels = std::move(texels)]()
        {
            int texelCount = (int)texels.size() / 16;
            int fullRows = texelCount / width;
            int remainder = texelCount % width;

            BeginTextureUpload(GL_TEXTURE_2D, native->Texture);
            if (fullRows > 0)
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, fullRows, GL_RGBA, GL_FLOAT, texels.data());

            if (remainder > 0)
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, fullRows, remainder, 1, GL_RGBA, GL_FLOAT, texels.data() + fullRows * width * 16);
        });
    }

    void WebGL2GraphicsDevice::BindStructuredBuffer(ShaderStage shaderStage, TextureRegister registerType, IStructuredBuffer* buffer)
    {
        auto* structured = static_cast<WebGL2StructuredBuffer*>(buffer);
        auto* native = (structured != nullptr) ? structured->GetNativeTexture() : nullptr;
        auto* executor = &_executor;
        int stageIndex = GetStageIndex(shaderStage);
        int reg = (int)registerType;
        Record([executor, stageIndex, reg, native]() { executor->SetTexture(stageIndex, reg, native); });
    }

    void WebGL2GraphicsDevice::DrawIndexedTriangles(int count, int baseIndex, int baseVertex)
    {
        auto* executor = &_executor;
        Record([executor, count, baseIndex, baseVertex]() { executor->DrawIndexed(count, baseIndex, baseVertex, 0); });
    }

    void WebGL2GraphicsDevice::DrawIndexedInstancedTriangles(int count, int instances, int baseIndex, int baseVertex)
    {
        auto* executor = &_executor;
        Record([executor, count, instances, baseIndex, baseVertex]() { executor->DrawIndexed(count, baseIndex, baseVertex, std::max(instances, 1)); });
    }

    void WebGL2GraphicsDevice::DrawInstancedTriangles(int count, int instances, int baseVertex)
    {
        auto* executor = &_executor;
        Record([executor, count, instances, baseVertex]() { executor->DrawArrays(count, baseVertex, std::max(instances, 1)); });
    }

    void WebGL2GraphicsDevice::DrawTriangles(int count, int baseVertex)
    {
        auto* executor = &_executor;
        Record([executor, count, baseVertex]() { executor->DrawArrays(count, baseVertex, 0); });
    }

    void WebGL2GraphicsDevice::ClearRenderTarget2D(IRenderTarget2D* renderTarget, XMVECTORF32 clearColor)
    {
        ClearRenderTarget2D(renderTarget, 0, clearColor);
    }

    void WebGL2GraphicsDevice::ClearRenderTarget2D(IRenderTarget2D* renderTarget, int arrayIndex, XMVECTORF32 clearColor)
    {
        auto* native = GetNative(renderTarget);
        auto color = std::array<float, 4>{ clearColor.f[0], clearColor.f[1], clearColor.f[2], clearColor.f[3] };
        auto* executor = &_executor;
        Record([executor, native, arrayIndex, color]() { executor->ClearColor(native, arrayIndex, color.data()); });
    }

    void WebGL2GraphicsDevice::ClearDepthStencil(IDepthTarget* depthTarget, DepthStencilClearFlags clearFlags, float depth, unsigned char stencil)
    {
        ClearDepthStencil(depthTarget, 0, clearFlags, depth, stencil);
    }

    void WebGL2GraphicsDevice::ClearDepthStencil(IDepthTarget* depthTarget, int arrayIndex, DepthStencilClearFlags clearFlags, float depth, unsigned char stencil)
    {
        auto* native = GetNative(depthTarget);
        bool clearDepth = (clearFlags != DepthStencilClearFlags::Stencil);
        bool clearStencil = (clearFlags != DepthStencilClearFlags::Depth);
        auto* executor = &_executor;
        Record([executor, native, arrayIndex, clearDepth, clearStencil, depth, stencil]()
        {
            executor->ClearDepthStencil(native, arrayIndex, clearDepth, clearStencil, depth, stencil);
        });
    }

    void WebGL2GraphicsDevice::BindRenderTargetsInternal(std::vector<NativeTexture*> targets, std::vector<int> layers, NativeTexture* depth, int depthLayer)
    {
        auto* executor = &_executor;
        Record([executor, targets = std::move(targets), layers = std::move(layers), depth, depthLayer]()
        {
            executor->SetRenderTargets(targets.data(), layers.data(), (int)targets.size(), depth, depthLayer);
        });
    }

    void WebGL2GraphicsDevice::BindRenderTarget(IRenderTarget2D* renderTarget, IDepthTarget* depthTarget)
    {
        BindRenderTargetsInternal({ GetNative(renderTarget) }, { 0 }, GetNative(depthTarget), 0);
    }

    void WebGL2GraphicsDevice::BindRenderTarget(IRenderTargetBinding renderTarget, IDepthTargetBinding depthTarget)
    {
        BindRenderTargetsInternal({ GetNative(renderTarget.RenderTarget) }, { renderTarget.ArrayIndex }, GetNative(depthTarget.DepthTarget), depthTarget.ArrayIndex);
    }

    void WebGL2GraphicsDevice::BindRenderTargets(std::vector<IRenderTarget2D*> renderTargets, IDepthTarget* depthTarget)
    {
        auto targets = std::vector<NativeTexture*>{};
        auto layers = std::vector<int>{};
        for (auto* target : renderTargets)
        {
            targets.push_back(GetNative(target));
            layers.push_back(0);
        }

        BindRenderTargetsInternal(std::move(targets), std::move(layers), GetNative(depthTarget), 0);
    }

    void WebGL2GraphicsDevice::BindRenderTargets(std::vector<IRenderTargetBinding> renderTargets, IDepthTargetBinding depthTarget)
    {
        auto targets = std::vector<NativeTexture*>{};
        auto layers = std::vector<int>{};
        for (const auto& binding : renderTargets)
        {
            targets.push_back(GetNative(binding.RenderTarget));
            layers.push_back(binding.ArrayIndex);
        }

        BindRenderTargetsInternal(std::move(targets), std::move(layers), GetNative(depthTarget.DepthTarget), depthTarget.ArrayIndex);
    }

    void WebGL2GraphicsDevice::CopyTextureResource(ITexture2D* src, ITexture2D* dst)
    {
        auto* source = GetNative(src);
        auto* destination = GetNative(dst);
        auto* executor = &_executor;
        Record([executor, source, destination]() { executor->CopyTexture(source, destination); });
    }

    std::unique_ptr<IGpuReadbackBuffer> WebGL2GraphicsDevice::CreateGpuReadbackBuffer(int width, int height, SurfaceFormat format)
    {
        auto* native = new NativeReadback();
        native->Width = width;
        native->Height = height;
        native->ByteSize = width * height * GetGLFormat(format).PixelSize;
        return std::make_unique<WebGL2GpuReadbackBuffer>(this, native);
    }

    void WebGL2GraphicsDevice::SetPrimitiveType(PrimitiveType primitiveType)
    {
        GLenum topology = GL_TRIANGLES;
        if (primitiveType == PrimitiveType::TriangleStrip)
            topology = GL_TRIANGLE_STRIP;
        else if (primitiveType == PrimitiveType::LineList)
            topology = GL_LINES;

        auto* executor = &_executor;
        Record([executor, topology]() { executor->SetPrimitiveType(topology); });
    }

    void WebGL2GraphicsDevice::SetInputLayout(IInputLayout* inputLayout)
    {
        auto* layout = static_cast<WebGL2InputLayout*>(inputLayout);
        auto* native = (layout != nullptr) ? layout->GetNativeLayout() : nullptr;
        auto* executor = &_executor;
        Record([executor, native]() { executor->SetInputLayout(native); });
    }

    std::unique_ptr<IInputLayout> WebGL2GraphicsDevice::CreateInputLayout(std::vector<RendererInputLayoutField> fields, IShader* shader)
    {
        // Elements follow each other as with D3D11_APPEND_ALIGNED_ELEMENT.
        auto* native = new NativeInputLayout();
        int offset = 0;
        for (const auto& field : fields)
        {
            int size = 4;
            switch (field.Format)
            {
            case VertexInputFormat::VI_RG8_Unorm: size = 2; break;
            case VertexInputFormat::VI_RG32_Float: size = 8; break;
            case VertexInputFormat::VI_RGB32_Float: size = 12; break;
            case VertexInputFormat::VI_RGBA32_Float: size = 16; break;
            default: break;
            }

            int alignment = std::min(size, 4);
            offset = (offset + alignment - 1) / alignment * alignment;

            auto element = NativeInputElement{};
            element.Semantic = field.Semantic;
            element.Index = field.Slot;
            element.Format = field.Format;
            element.Offset = offset;
            native->Elements.push_back(element);

            offset += size;
        }

        return std::make_unique<WebGL2InputLayout>(this, native);
    }

    void WebGL2GraphicsDevice::CreateDevice()
    {
        TENLog("WebGL 2 renderer", LogLevel::Info);

        auto attributes = EmscriptenWebGLContextAttributes{};
        emscripten_webgl_init_context_attributes(&attributes);
        attributes.majorVersion = 2;
        attributes.minorVersion = 0;
        attributes.alpha = false;
        attributes.depth = false;
        attributes.stencil = false;
        attributes.antialias = false;
        attributes.premultipliedAlpha = false;
        attributes.preserveDrawingBuffer = false;
        attributes.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
        attributes.enableExtensionsByDefault = false;

        auto context = emscripten_webgl_create_context("#canvas", &attributes);
        if (context <= 0)
            throw std::runtime_error("This browser can't create a WebGL 2 context.");

        _context = (int)context;
        emscripten_webgl_make_context_current(context);

        _floatRenderable = emscripten_webgl_enable_extension(context, "EXT_color_buffer_float");
        _floatLinear = emscripten_webgl_enable_extension(context, "OES_texture_float_linear");
        // A page can ask for compressed textures to be decoded as on browsers without
        // them (most phones), to test that on a desktop.
        bool decodeTextures = EM_ASM_INT({ return Module['tenWebDecodeTextures'] ? 1 : 0; });
        if (!decodeTextures)
        {
            _textureCapabilities.S3tc = emscripten_webgl_enable_extension(context, "WEBGL_compressed_texture_s3tc");
            _textureCapabilities.S3tcSrgb = emscripten_webgl_enable_extension(context, "WEBGL_compressed_texture_s3tc_srgb");
            _textureCapabilities.Rgtc = emscripten_webgl_enable_extension(context, "EXT_texture_compression_rgtc");
            _textureCapabilities.Bptc = emscripten_webgl_enable_extension(context, "EXT_texture_compression_bptc");
        }

        // Phones take ETC2 instead (see CompressForPhone() in WebGL2TextureLoader.cpp).
        _textureCapabilities.Etc2 = emscripten_webgl_enable_extension(context, "WEBGL_compressed_texture_etc");

        if (emscripten_webgl_enable_extension(context, "EXT_texture_filter_anisotropic"))
            glGetFloatv(MAX_TEXTURE_MAX_ANISOTROPY, &_maxAnisotropy);

        _adapterInfo.Name = (const char*)glGetString(GL_RENDERER);
        if (emscripten_webgl_enable_extension(context, "WEBGL_debug_renderer_info"))
        {
            auto* renderer = (const char*)glGetString(UNMASKED_RENDERER);
            if (renderer != nullptr)
                _adapterInfo.Name = renderer;
        }

        GLint maxBlockSize = 0;
        glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &maxBlockSize);

        // WebGL 2 promises 16 KB. Larger constant buffers of the engine have their arrays
        // shortened to fit (see WebGL2ShaderLibrary.cpp) and are drawn in smaller batches.
        _maxBlockSize = std::max(maxBlockSize, 16384);
        _shaders.SetMaxBlockSize(_maxBlockSize);

        TENLog("WebGL renderer: " + _adapterInfo.Name, LogLevel::Info);
        TENLog(fmt::format("WebGL features: float targets {}, float filtering {}, S3TC {}, RGTC {}, BPTC {}, ETC2 {}, anisotropy {}, uniform block size {}",
            _floatRenderable, _floatLinear, _textureCapabilities.S3tc, _textureCapabilities.Rgtc, _textureCapabilities.Bptc, _textureCapabilities.Etc2,
            _maxAnisotropy, maxBlockSize), LogLevel::Info);

        _executor.Initialize(&_shaders, _floatLinear, _maxAnisotropy);
    }

    void WebGL2GraphicsDevice::Initialize()
    {
        // States are made by the executor when the context is.
    }

    std::unique_ptr<IRenderSurface2D> WebGL2GraphicsDevice::InitializeSwapChain(int width, int height)
    {
        // Frame rate as the Direct3D 11 device works it out.
        if (!g_Configuration.EnableHighFramerate)
        {
            _refreshRate = 30;
        }
        else
        {
            _refreshRate = GetCurrentScreenRefreshRate();
            if (_refreshRate == 0)
                _refreshRate = 60;
        }

        _viewportToolkit = Viewport(0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f);
        _screenWidth = width;
        _screenHeight = height;

        // The back buffer is a texture like any other; Present() copies it to the page.
        auto surface = CreateRenderSurface2D(width, height, SurfaceFormat::SF_RGBA8_Unorm, false, DepthFormat::Depth32);
        _backBuffer = GetNative(surface->GetRenderTarget());

        Record([width, height]() { emscripten_set_canvas_element_size("#canvas", width, height); });
        return surface;
    }

    std::string WebGL2GraphicsDevice::GetDefaultAdapterName()
    {
        return _adapterInfo.Name;
    }

    AdapterInfo WebGL2GraphicsDevice::GetAdapterInfo()
    {
        return _adapterInfo;
    }

    void WebGL2GraphicsDevice::ResizeSwapChain(int width, int height)
    {
        ClearState();
        _screenWidth = width;
        _screenHeight = height;
        _viewportToolkit = Viewport(0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f);
    }

    void WebGL2GraphicsDevice::LoadShaderLibrary(const std::string& sourceDirectory)
    {
        auto lock = std::lock_guard<std::mutex>(_shaderMutex);
        if (_shaders.IsLoaded())
            return;

        // The game's own shaders compiled for WebGL, else the engine's.
        if (!_shaders.Load(sourceDirectory + "WebGL.tenshaders"))
            _shaders.Load("/engine/Shaders.tenshaders");

        auto* executor = &_executor;
        Record([executor]() { executor->CreateEffectSamplers(); });
    }

    std::unique_ptr<IShader> WebGL2GraphicsDevice::CreateShader(ShaderCompileRequest& request)
    {
        LoadShaderLibrary(request.SourceDirectory);

        auto shader = std::make_unique<WebGL2Shader>();

        auto find = [&](const std::string& stage)
        {
            auto key = MakeShaderKey(request.FileName, stage, request.EntryPoint, request.Macros);
            auto* info = _shaders.Find(key);
            if (info == nullptr)
                TENLog("WebGL shader not found: " + key, LogLevel::Error);

            return info;
        };

        if (request.Type == ShaderType::Vertex || request.Type == ShaderType::PixelAndVertex)
            shader->VertexStage = find("VS");

        if (request.Type == ShaderType::Pixel || request.Type == ShaderType::PixelAndVertex)
            shader->PixelStage = find("PS");

        // SMAA gets the screen size as "float4(1.0 / W, 1.0 / H, W, H)".
        auto metrics = request.Macros.find("SMAA_RT_METRICS");
        if (metrics != request.Macros.end())
        {
            auto numbers = std::vector<float>{};
            auto pattern = std::regex("[0-9]+(\\.[0-9]+)?");
            for (auto it = std::sregex_iterator(metrics->second.begin(), metrics->second.end(), pattern); it != std::sregex_iterator(); ++it)
                numbers.push_back(std::stof(it->str()));

            if (numbers.size() >= 2)
            {
                float width = numbers[numbers.size() - 2];
                float height = numbers[numbers.size() - 1];
                shader->HasSmaaMetrics = true;
                shader->SmaaMetrics[0] = 1.0f / width;
                shader->SmaaMetrics[1] = 1.0f / height;
                shader->SmaaMetrics[2] = width;
                shader->SmaaMetrics[3] = height;
            }
        }

        return shader;
    }

    void WebGL2GraphicsDevice::BindShader(ShaderStage shaderStage, IShader* shader, bool forceNull)
    {
        // Same choices as the Direct3D 11 device: a shader without the stage leaves
        // the bound one alone unless forceNull, and a null shader with forceNull
        // also clears the vertex shader.
        auto* webShader = static_cast<WebGL2Shader*>(shader);
        auto* executor = &_executor;

        if (webShader == nullptr)
        {
            if (!forceNull)
                return;

            Record([executor, shaderStage]()
            {
                executor->SetShaderStage(STAGE_VERTEX, nullptr, nullptr);
                if (shaderStage == ShaderStage::PixelShader)
                    executor->SetShaderStage(STAGE_PIXEL, nullptr, nullptr);
            });
            return;
        }

        auto metrics = std::array<float, 4>{ webShader->SmaaMetrics[0], webShader->SmaaMetrics[1], webShader->SmaaMetrics[2], webShader->SmaaMetrics[3] };
        bool hasMetrics = webShader->HasSmaaMetrics;

        if (shaderStage == ShaderStage::VertexShader && (webShader->VertexStage != nullptr || forceNull))
        {
            auto* stage = webShader->VertexStage;
            Record([executor, stage, metrics, hasMetrics]() { executor->SetShaderStage(STAGE_VERTEX, stage, hasMetrics ? metrics.data() : nullptr); });
        }
        else if (shaderStage == ShaderStage::PixelShader && (webShader->PixelStage != nullptr || forceNull))
        {
            auto* stage = webShader->PixelStage;
            Record([executor, stage, metrics, hasMetrics]() { executor->SetShaderStage(STAGE_PIXEL, stage, hasMetrics ? metrics.data() : nullptr); });
        }
    }

    void WebGL2GraphicsDevice::Present()
    {
        auto* executor = &_executor;
        auto* backBuffer = _backBuffer;
        Record([executor, backBuffer]() { executor->Present(backBuffer); });
        _queue.SubmitFrame();
    }

    void WebGL2GraphicsDevice::ClearState()
    {
        auto* executor = &_executor;
        Record([executor]() { executor->ClearState(); });
    }

    std::unique_ptr<ISpriteFont> WebGL2GraphicsDevice::InitializeSpriteFont(std::string fontPath)
    {
        return std::make_unique<WebGL2SpriteFont>(this, fontPath);
    }

    std::unique_ptr<ISpriteBatch> WebGL2GraphicsDevice::InitializeSpriteBatch()
    {
        return std::make_unique<WebGL2SpriteBatch>(this);
    }

    std::unique_ptr<IPrimitiveBatch> WebGL2GraphicsDevice::InitializePrimitiveBatch()
    {
        return std::make_unique<WebGL2PrimitiveBatch>(this);
    }

    void WebGL2GraphicsDevice::SetViewport(RendererViewport viewport)
    {
        auto* executor = &_executor;
        Record([executor, viewport]() { executor->SetViewport(viewport); });
    }

    Vector3 WebGL2GraphicsDevice::Unproject(Vector3 position, Matrix projection, Matrix view, Matrix world)
    {
        return _viewportToolkit.Unproject(position, projection, view, world);
    }

    bool WebGL2GraphicsDevice::SaveScreenshot(IRenderTarget2D* renderTarget, std::string path)
    {
        // Not available yet in the browser build.
        TENLog("Screenshots aren't available in the browser yet.", LogLevel::Warning);
        return false;
    }

    void WebGL2GraphicsDevice::Flush()
    {
    }

    void WebGL2GraphicsDevice::UnbindAllRenderTargets()
    {
        BindRenderTargetsInternal({}, {}, nullptr, 0);
    }

    int WebGL2GraphicsDevice::GetRefreshRate()
    {
        return _refreshRate;
    }

    int WebGL2GraphicsDevice::GetScreenWidth()
    {
        return _screenWidth;
    }

    int WebGL2GraphicsDevice::GetScreenHeight()
    {
        return _screenHeight;
    }

    int WebGL2GraphicsDevice::GetMaxConstantBufferSize()
    {
        return _maxBlockSize;
    }
}


// Frames shown so far, which a page reads to show the frame rate.
extern "C" EMSCRIPTEN_KEEPALIVE unsigned int TENWebGetFrameCount()
{
    auto* device = TEN::Renderer::Native::WebGL2::WebGL2GraphicsDevice::GetInstance();
    return (device != nullptr) ? device->GetPresentedFrameCount() : 0;
}

#endif
