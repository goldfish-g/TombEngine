#include "framework.h"
#include "Renderer/Native/WebGL2/WebGL2PrimitiveBatch.h"

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include "Renderer/Native/WebGL2/WebGL2GraphicsDevice.h"

namespace TEN::Renderer::Native::WebGL2
{
    WebGL2PrimitiveBatch::WebGL2PrimitiveBatch(WebGL2GraphicsDevice* device) :
        _device(device)
    {
    }

    void WebGL2PrimitiveBatch::Begin()
    {
        _vertices.clear();
    }

    void WebGL2PrimitiveBatch::End()
    {
        Flush();
    }

    void WebGL2PrimitiveBatch::SetTopology(GLenum topology)
    {
        if (_topology != topology)
            Flush();

        _topology = topology;
    }

    void WebGL2PrimitiveBatch::DrawLine(Vertex const& v1, Vertex const& v2)
    {
        SetTopology(GL_LINES);
        _vertices.push_back(v1);
        _vertices.push_back(v2);
    }

    void WebGL2PrimitiveBatch::DrawTriangle(Vertex const& v1, Vertex const& v2, Vertex const& v3)
    {
        SetTopology(GL_TRIANGLES);
        _vertices.push_back(v1);
        _vertices.push_back(v2);
        _vertices.push_back(v3);
    }

    void WebGL2PrimitiveBatch::DrawQuad(Vertex const& v1, Vertex const& v2, Vertex const& v3, Vertex const& v4)
    {
        // DirectXTK draws a quad as the triangles 0, 1, 2 and 0, 2, 3.
        SetTopology(GL_TRIANGLES);
        _vertices.push_back(v1);
        _vertices.push_back(v2);
        _vertices.push_back(v3);
        _vertices.push_back(v1);
        _vertices.push_back(v3);
        _vertices.push_back(v4);
    }

    void WebGL2PrimitiveBatch::Flush()
    {
        if (_vertices.empty())
            return;

        auto* executor = _device->GetExecutor();
        auto topology = _topology;
        _device->Record([executor, topology, vertices = std::move(_vertices)]() { executor->DrawPrimitives(vertices, topology); });
        _vertices = {};
    }
}

#endif
