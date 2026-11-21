#pragma once

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include <GLES3/gl3.h>

#include "Renderer/Graphics/IPrimitiveBatch.h"

namespace TEN::Renderer::Native::WebGL2
{
    using namespace TEN::Renderer::Graphics;
    using namespace TEN::Renderer::Graphics::Vertices;

    class WebGL2GraphicsDevice;

    // DirectXTK's PrimitiveBatch: lines, triangles and quads in a buffer of its
    // own, drawn with whatever shaders and states are bound.
    class WebGL2PrimitiveBatch : public IPrimitiveBatch
    {
    private:
        WebGL2GraphicsDevice* _device	= nullptr;
        std::vector<Vertex>	  _vertices = {};
        GLenum				  _topology = GL_TRIANGLES;

        void SetTopology(GLenum topology);
        void Flush();

    public:
        WebGL2PrimitiveBatch(WebGL2GraphicsDevice* device);

        void Begin() override;
        void End() override;
        void DrawLine(Vertex const& v1, Vertex const& v2) override;
        void DrawTriangle(Vertex const& v1, Vertex const& v2, Vertex const& v3) override;
        void DrawQuad(Vertex const& v1, Vertex const& v2, Vertex const& v3, Vertex const& v4) override;
    };
}

#endif
