#include "framework.h"
#include "Renderer/Native/WebGL2/WebGL2SpriteBatch.h"

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include "Renderer/Native/WebGL2/WebGL2GraphicsDevice.h"

namespace TEN::Renderer::Native::WebGL2
{
    WebGL2SpriteBatch::WebGL2SpriteBatch(WebGL2GraphicsDevice* device) :
        _device(device)
    {
    }

    void WebGL2SpriteBatch::Begin(SpriteSortingMode sortingMode, BlendMode blendMode)
    {
        _sortMode = sortingMode;
        _sprites.clear();

        // The CommonStates blend states the Direct3D 11 sprite batch picks.
        switch (blendMode)
        {
        case BlendMode::Additive:
        case BlendMode::Distortion:
            _blend = { true, GL_SRC_ALPHA, GL_ONE, GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE, GL_FUNC_ADD };
            break;

        case BlendMode::AlphaBlend:
            _blend = { true, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD };
            break;

        case BlendMode::PremultipliedAlphaBlend:
            _blend = { true, GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD, GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD };
            break;

        default:
            _blend = BlendState{};
            break;
        }
    }

    void WebGL2SpriteBatch::End()
    {
        Flush();
    }

    void WebGL2SpriteBatch::Draw(ITextureBase* texture, RendererRectangle area, Vector4 color)
    {
        auto* webTexture = dynamic_cast<IWebGL2Texture*>(texture);
        auto* native = (webTexture != nullptr) ? webTexture->GetNativeTexture() : nullptr;

        auto sprite = Sprite{};
        sprite.Texture = native;
        sprite.Corners[0][0] = (float)area.Left;
        sprite.Corners[0][1] = (float)area.Top;
        sprite.Corners[1][0] = (float)area.Right;
        sprite.Corners[1][1] = (float)area.Top;
        sprite.Corners[2][0] = (float)area.Left;
        sprite.Corners[2][1] = (float)area.Bottom;
        sprite.Corners[3][0] = (float)area.Right;
        sprite.Corners[3][1] = (float)area.Bottom;
        sprite.UV[0] = 0.0f;
        sprite.UV[1] = 0.0f;
        sprite.UV[2] = 1.0f;
        sprite.UV[3] = 1.0f;
        sprite.Color = color;
        sprite.Order = (int)_sprites.size();
        _sprites.push_back(sprite);

        if (_sortMode == SpriteSortingMode::Immediate)
            Flush();
    }

    void WebGL2SpriteBatch::DrawSprite(NativeTexture* texture, const Vector2& position, const Vector2& size, const Vector4& sourceUV,
                                       const Vector4& color, float rotation, const Vector2& origin, const Vector2& scale)
    {
        // As DirectXTK places a sprite: corners at position + (corner * size - origin) * scale, turned by rotation.
        float sine = sinf(rotation);
        float cosine = cosf(rotation);
        const float cornerOffsets[4][2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f } };

        auto sprite = Sprite{};
        sprite.Texture = texture;
        for (int i = 0; i < 4; i++)
        {
            float x = (cornerOffsets[i][0] * size.x - origin.x) * scale.x;
            float y = (cornerOffsets[i][1] * size.y - origin.y) * scale.y;
            sprite.Corners[i][0] = position.x + x * cosine - y * sine;
            sprite.Corners[i][1] = position.y + x * sine + y * cosine;
        }

        sprite.UV[0] = sourceUV.x;
        sprite.UV[1] = sourceUV.y;
        sprite.UV[2] = sourceUV.z;
        sprite.UV[3] = sourceUV.w;
        sprite.Color = color;
        sprite.Order = (int)_sprites.size();
        _sprites.push_back(sprite);

        if (_sortMode == SpriteSortingMode::Immediate)
            Flush();
    }

    void WebGL2SpriteBatch::Flush()
    {
        if (_sprites.empty())
            return;

        // Every sprite here has the same depth, so depth sorting keeps the order.
        if (_sortMode == SpriteSortingMode::Texture)
        {
            std::stable_sort(_sprites.begin(), _sprites.end(),
                [](const Sprite& a, const Sprite& b) { return a.Texture < b.Texture; });
        }

        auto draws = std::vector<SpriteDraw>{};
        for (const auto& sprite : _sprites)
        {
            if (draws.empty() || draws.back().Texture != sprite.Texture)
                draws.push_back(SpriteDraw{ sprite.Texture, {} });

            // Two triangles, the winding DirectXTK uses: top left, top right,
            // bottom left; top right, bottom right, bottom left.
            const int order[6] = { 0, 1, 2, 1, 3, 2 };
            const float uvs[4][2] = { { sprite.UV[0], sprite.UV[1] }, { sprite.UV[2], sprite.UV[1] }, { sprite.UV[0], sprite.UV[3] }, { sprite.UV[2], sprite.UV[3] } };
            for (int corner : order)
            {
                auto vertex = SpriteVertex2D{};
                vertex.Position[0] = sprite.Corners[corner][0];
                vertex.Position[1] = sprite.Corners[corner][1];
                vertex.UV[0] = uvs[corner][0];
                vertex.UV[1] = uvs[corner][1];
                vertex.Color[0] = sprite.Color.x;
                vertex.Color[1] = sprite.Color.y;
                vertex.Color[2] = sprite.Color.z;
                vertex.Color[3] = sprite.Color.w;
                draws.back().Vertices.push_back(vertex);
            }
        }

        _sprites.clear();

        auto* executor = _device->GetExecutor();
        auto blend = _blend;
        _device->Record([executor, blend, draws = std::move(draws)]() { executor->DrawSprites(draws, blend); });
    }
}

#endif
