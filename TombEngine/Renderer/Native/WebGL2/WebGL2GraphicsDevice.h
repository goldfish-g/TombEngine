#pragma once

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include "Renderer/Graphics/IGraphicsDevice.h"
#include "Renderer/Native/WebGL2/WebGL2CommandQueue.h"
#include "Renderer/Native/WebGL2/WebGL2Executor.h"
#include "Renderer/Native/WebGL2/WebGL2Objects.h"
#include "Renderer/Native/WebGL2/WebGL2ShaderLibrary.h"
#include "Renderer/Native/WebGL2/WebGL2TextureLoader.h"

namespace TEN::Renderer::Native::WebGL2
{
    using namespace TEN::Renderer::Graphics;

    // Texture unit textures are bound to while they are filled, which drawing never uses.
    constexpr auto UPLOAD_TEXTURE_UNIT = 31;

    // The graphics device of the browser build, on WebGL 2.
    //
    // WebGL can only be used from the browser's main thread, while the engine draws
    // from its game thread (and its level loading thread), as it does on Windows.
    // So every call here is recorded, and the main thread plays each presented
    // frame back with WebGL once per display refresh (see WebGL2CommandQueue.h).
    class WebGL2GraphicsDevice : public IGraphicsDevice
    {
    private:
        static WebGL2GraphicsDevice* _instance;

        CommandQueue		_queue				 = {};
        Executor			_executor			 = {};
        ShaderLibrary		_shaders			 = {};
        TextureCapabilities _textureCapabilities = {};
        bool				_floatRenderable	 = false;
        bool				_floatLinear		 = false;
        float				_maxAnisotropy		 = 1.0f;
        int					_context			 = 0;
        AdapterInfo			_adapterInfo		 = {};

        std::mutex	   _shaderMutex	  = {};
        int			   _screenWidth	  = 0;
        int			   _screenHeight  = 0;
        int			   _refreshRate	  = 60;
        Viewport	   _viewportToolkit = {};
        NativeTexture* _backBuffer	  = nullptr;

        NativeTexture* CreateNativeTexture(GLenum target, SurfaceFormat format, int width, int height, int layers, int levels);
        NativeTexture* CreateNativeDepth(DepthFormat format, int width, int height, int layers);
        void LoadShaderLibrary(const std::string& sourceDirectory);
        void BindRenderTargetsInternal(std::vector<NativeTexture*> targets, std::vector<int> layers, NativeTexture* depth, int depthLayer);

    public:
        WebGL2GraphicsDevice();
        ~WebGL2GraphicsDevice() override;

        static WebGL2GraphicsDevice* GetInstance() { return _instance; }

        // Browser main thread: plays back the frame presented last, if any.
        void ExecuteFrame();

        // Frames shown so far, for the page's frame rate display.
        unsigned int GetPresentedFrameCount();

        template <typename Function>
        void Record(Function&& function)
        {
            _queue.Record(std::forward<Function>(function));
        }

        Executor* GetExecutor() { return &_executor; }
        const TextureCapabilities& GetTextureCapabilities() const { return _textureCapabilities; }
        void ReleaseTexture(NativeTexture* native);
        void ReleaseBuffer(NativeBuffer* native);
        void ReleaseConstantBuffer(NativeConstantBuffer* native);
        void ReleaseInputLayout(NativeInputLayout* native);
        void ReleaseReadback(NativeReadback* native);

        // A texture from pixels or blocks decoded already (the sprite font's).
        std::unique_ptr<ITexture2D> CreateTexture2DFromDecoded(DecodedTexture decoded);

        // Copies the frame a video of the page shows into the texture, on the GPU
        // (see Specific/Platform/Web/VideoWeb.cpp).
        void UpdateTexture2DFromVideo(ITexture2D* texture, int videoID);

        // IGraphicsDevice.
        std::unique_ptr<IVertexBuffer> CreateVertexBuffer(int numVertices, int vertexSize, void* data) override;
        void UpdateVertexBuffer(IVertexBuffer* vertexBuffer, int startVertex, int count, void* data) override;
        void BindVertexBuffer(IVertexBuffer* vertexBuffer) override;

        std::unique_ptr<IIndexBuffer> CreateIndexBuffer(int numIndices, int* data) override;
        void UpdateIndexBuffer(IIndexBuffer* indexBuffer, int numIndices, int startIndex, int* data) override;
        void BindIndexBuffer(IIndexBuffer* indexBuffer) override;

        std::unique_ptr<IRenderSurface2D> CreateRenderSurface2D(int width, int height, SurfaceFormat colorFormat, bool isTypeless, DepthFormat depthFormat) override;
        std::unique_ptr<IRenderSurface2D> CreateRenderSurface2D(int width, int height, int arraySize, SurfaceFormat colorFormat, DepthFormat depthFormat) override;
        std::unique_ptr<IRenderSurface2D> CreateRenderSurface2D(IRenderSurface2D* parentRenderTarget, SurfaceFormat colorFormat) override;

        IRenderTargetCube* CreateRenderTargetCube(int size, SurfaceFormat colorFormat) override;

        std::unique_ptr<ITexture2D> CreateTexture2D(int width, int height, SurfaceFormat format, void* data, bool isDynamic = false) override;
        std::unique_ptr<ITexture2D> CreateTexture2DFromFile(const std::string fileName) override;
        std::unique_ptr<ITexture2D> CreateTexture2DFromFileInMemory(int dataSize, unsigned char* data) override;
        void UpdateTexture2D(ITexture2D* texture, std::vector<char> data) override;

        std::unique_ptr<ITexture3D> CreateTexture3D(int width, int height, int depth, SurfaceFormat format, const void* data) override;

        void SetBlendMode(BlendMode blendMode) override;
        void SetDepthState(DepthState depthState) override;
        void SetCullMode(CullMode cullMode) override;
        void SetScissor(RendererRectangle rectangle) override;
        void SetScissor(RendererViewport viewport) override;

        void BindTexture(TextureRegister registerType, ITextureBase* texture) override;
        void BindSamplers(bool pointFilter) override;
        void UnbindTexture(ShaderStage stage, TextureRegister registerType) override;

        std::unique_ptr<IConstantBuffer> CreateConstantBuffer(int size, std::string name) override;
        void UpdateConstantBuffer(IConstantBuffer* constantBuffer, void* data, int size = 0) override;
        void BindConstantBuffer(ShaderStage shaderStage, ConstantBufferRegister constantBufferType, IConstantBuffer* buffer) override;

        std::unique_ptr<IStructuredBuffer> CreateStructuredBuffer(int stride, int elementCount, std::wstring name) override;
        void UpdateStructuredBuffer(IStructuredBuffer* buffer, const void* data, int elementCount) override;
        void BindStructuredBuffer(ShaderStage shaderStage, TextureRegister registerType, IStructuredBuffer* buffer) override;

        void DrawIndexedTriangles(int count, int baseIndex, int baseVertex) override;
        void DrawIndexedInstancedTriangles(int count, int instances, int baseIndex, int baseVertex) override;
        void DrawInstancedTriangles(int count, int instances, int baseVertex) override;
        void DrawTriangles(int count, int baseVertex) override;

        void ClearRenderTarget2D(IRenderTarget2D* renderTarget, XMVECTORF32 clearColor) override;
        void ClearRenderTarget2D(IRenderTarget2D* renderTarget, int arrayIndex, XMVECTORF32 clearColor) override;
        void ClearDepthStencil(IDepthTarget* depthTarget, DepthStencilClearFlags clearFlags, float depth, unsigned char stencil) override;
        void ClearDepthStencil(IDepthTarget* depthTarget, int arrayIndex, DepthStencilClearFlags clearFlags, float depth, unsigned char stencil) override;

        void BindRenderTarget(IRenderTarget2D* renderTarget, IDepthTarget* depthTarget) override;
        void BindRenderTarget(IRenderTargetBinding renderTarget, IDepthTargetBinding depthTarget) override;
        void BindRenderTargets(std::vector<IRenderTarget2D*> renderTargets, IDepthTarget* depthTarget) override;
        void BindRenderTargets(std::vector<IRenderTargetBinding> renderTargets, IDepthTargetBinding depthTarget) override;

        void CopyTextureResource(ITexture2D* src, ITexture2D* dst) override;
        std::unique_ptr<IGpuReadbackBuffer> CreateGpuReadbackBuffer(int width, int height, SurfaceFormat format) override;

        void SetPrimitiveType(PrimitiveType primitiveType) override;

        void SetInputLayout(IInputLayout* inputLayout) override;
        std::unique_ptr<IInputLayout> CreateInputLayout(std::vector<RendererInputLayoutField> fields, IShader* shader) override;

        void CreateDevice() override;
        void Initialize() override;
        std::unique_ptr<IRenderSurface2D> InitializeSwapChain(int width, int height) override;
        std::string GetDefaultAdapterName() override;
        AdapterInfo GetAdapterInfo() override;
        void ResizeSwapChain(int width, int height) override;

        std::unique_ptr<IShader> CreateShader(ShaderCompileRequest& request) override;
        void BindShader(ShaderStage shaderStage, IShader* shader, bool forceNull) override;

        void Present() override;
        void ClearState() override;

        std::unique_ptr<ISpriteFont> InitializeSpriteFont(std::string fontPath) override;
        std::unique_ptr<ISpriteBatch> InitializeSpriteBatch() override;
        std::unique_ptr<IPrimitiveBatch> InitializePrimitiveBatch() override;

        void SetViewport(RendererViewport viewport) override;
        Vector3 Unproject(Vector3 position, Matrix projection, Matrix view, Matrix world) override;

        bool SaveScreenshot(IRenderTarget2D* renderTarget, std::string path) override;

        void Flush() override;
        void UnbindAllRenderTargets() override;

        int GetRefreshRate() override;

        int GetScreenWidth() override;
        int GetScreenHeight() override;
    };

    // Called by the page's main loop (see Specific/EngineMain.cpp).
    void ExecuteBrowserFrame();
}

#endif
