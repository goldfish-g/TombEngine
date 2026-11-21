#pragma once

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include "Renderer/Graphics/ISpriteBatch.h"
#include "Renderer/Native/WebGL2/WebGL2Executor.h"

namespace TEN::Renderer::Native::WebGL2
{
    class WebGL2GraphicsDevice;

    // DirectXTK's SpriteBatch, which the Windows build draws 2D pictures and text with.
    class WebGL2SpriteBatch : public ISpriteBatch
    {
    private:
        struct Sprite
        {
            NativeTexture* Texture	  = nullptr;
            float		   Corners[4][2] = {}; // Top left, top right, bottom left, bottom right.
            float		   UV[4]	  = {};	   // Left, top, right, bottom.
            Vector4		   Color	  = Vector4::One;
            int			   Order	  = 0;
        };

        WebGL2GraphicsDevice* _device	 = nullptr;
        SpriteSortingMode	  _sortMode	 = SpriteSortingMode::Deferred;
        BlendState			  _blend	 = {};
        std::vector<Sprite>	  _sprites	 = {};

        void Flush();

    public:
        WebGL2SpriteBatch(WebGL2GraphicsDevice* device);

        void Begin(SpriteSortingMode sortingMode, BlendMode blendMode) override;
        void End() override;
        void Draw(ITextureBase* texture, RendererRectangle area, Vector4 color) override;

        // A part of a texture: what SpriteFont draws glyphs with.
        void DrawSprite(NativeTexture* texture, const Vector2& position, const Vector2& size, const Vector4& sourceUV,
                        const Vector4& color, float rotation, const Vector2& origin, const Vector2& scale);
    };
}

#endif
