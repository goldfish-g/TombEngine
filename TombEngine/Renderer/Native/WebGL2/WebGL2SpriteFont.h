#pragma once

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include "Renderer/Graphics/ISpriteFont.h"
#include "Renderer/Graphics/ITexture2D.h"

namespace TEN::Renderer::Native::WebGL2
{
    class WebGL2GraphicsDevice;

    // DirectXTK's SpriteFont: reads its .spritefont files and lays text out as it does.
    class WebGL2SpriteFont : public ISpriteFont
    {
    private:
        struct FontGlyph
        {
            unsigned int Character = 0;
            int			 Left	   = 0;
            int			 Top	   = 0;
            int			 Right	   = 0;
            int			 Bottom	   = 0;
            float		 XOffset   = 0.0f;
            float		 YOffset   = 0.0f;
            float		 XAdvance  = 0.0f;
        };

        WebGL2GraphicsDevice*		_device			  = nullptr;
        std::vector<FontGlyph>		_glyphs			  = {};
        float						_lineSpacing	  = 0.0f;
        const FontGlyph*			_defaultGlyph	  = nullptr;
        std::unique_ptr<ITexture2D> _texture		  = nullptr;
        int							_textureWidth	  = 1;
        int							_textureHeight	  = 1;

        const FontGlyph* Find(unsigned int character) const;
        std::vector<unsigned short> ToUtf16(const std::string& text) const;

        template <typename Action>
        void ForEachGlyph(const std::vector<unsigned short>& text, Action action) const;

    public:
        WebGL2SpriteFont(WebGL2GraphicsDevice* device, const std::string& fontPath);

        float GetLineSpacing() override;
        Vector2 MeasureString(const std::string& str) override;
        Glyph FindGlyph(char c) override;
        void DrawString(ISpriteBatch* spriteBatch, const std::string& text, Vector2 position, Vector4 color, float rotation, Vector2 origin, float scale = 1) override;
    };
}

#endif
