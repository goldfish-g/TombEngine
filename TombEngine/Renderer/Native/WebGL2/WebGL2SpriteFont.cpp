#include "framework.h"
#include "Renderer/Native/WebGL2/WebGL2SpriteFont.h"

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include <cwctype>

#include "Renderer/Native/WebGL2/WebGL2GraphicsDevice.h"
#include "Renderer/Native/WebGL2/WebGL2SpriteBatch.h"

namespace TEN::Renderer::Native::WebGL2
{
    WebGL2SpriteFont::WebGL2SpriteFont(WebGL2GraphicsDevice* device, const std::string& fontPath) :
        _device(device)
    {
        auto file = std::ifstream(fontPath, std::ios::binary);
        if (!file)
            throw std::runtime_error("SpriteFont file not found: " + fontPath);

        auto data = std::vector<unsigned char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        int offset = 0;
        auto read = [&](void* destination, int size)
        {
            if (offset + size > (int)data.size())
                throw std::runtime_error("SpriteFont file is truncated: " + fontPath);

            memcpy(destination, data.data() + offset, size);
            offset += size;
        };

        // MakeSpriteFont's format: magic, glyphs, line spacing, default character, texture.
        char magic[8] = {};
        read(magic, 8);
        if (memcmp(magic, "DXTKfont", 8) != 0)
            throw std::runtime_error("Not a SpriteFont file: " + fontPath);

        unsigned int glyphCount = 0;
        read(&glyphCount, 4);
        _glyphs.resize(glyphCount);
        for (auto& glyph : _glyphs)
        {
            read(&glyph.Character, 4);
            read(&glyph.Left, 4);
            read(&glyph.Top, 4);
            read(&glyph.Right, 4);
            read(&glyph.Bottom, 4);
            read(&glyph.XOffset, 4);
            read(&glyph.YOffset, 4);
            read(&glyph.XAdvance, 4);
        }

        unsigned int defaultCharacter = 0;
        read(&_lineSpacing, 4);
        read(&defaultCharacter, 4);

        unsigned int textureWidth = 0;
        unsigned int textureHeight = 0;
        unsigned int textureFormat = 0;
        unsigned int textureStride = 0;
        unsigned int textureRows = 0;
        read(&textureWidth, 4);
        read(&textureHeight, 4);
        read(&textureFormat, 4);
        read(&textureStride, 4);
        read(&textureRows, 4);

        if (offset + (int)(textureStride * textureRows) > (int)data.size())
            throw std::runtime_error("SpriteFont texture is truncated: " + fontPath);

        auto decoded = DecodeDxgiPixels((int)textureFormat, (int)textureWidth, (int)textureHeight, (int)textureStride, (int)textureRows,
                                        data.data() + offset, device->GetTextureCapabilities());
        if (!decoded.Valid)
            throw std::runtime_error("SpriteFont texture can't be decoded: " + fontPath);

        _textureWidth = (int)textureWidth;
        _textureHeight = (int)textureHeight;
        _texture = device->CreateTexture2DFromDecoded(std::move(decoded));

        std::sort(_glyphs.begin(), _glyphs.end(), [](const FontGlyph& a, const FontGlyph& b) { return a.Character < b.Character; });

        // The engine sets a space as default character on Windows.
        _defaultGlyph = Find(L' ');
    }

    const WebGL2SpriteFont::FontGlyph* WebGL2SpriteFont::Find(unsigned int character) const
    {
        auto it = std::lower_bound(_glyphs.begin(), _glyphs.end(), character,
            [](const FontGlyph& glyph, unsigned int value) { return glyph.Character < value; });

        if (it != _glyphs.end() && it->Character == character)
            return &*it;

        return _defaultGlyph;
    }

    std::vector<unsigned short> WebGL2SpriteFont::ToUtf16(const std::string& text) const
    {
        // Windows text is UTF-16, which the engine converts its UTF-8 strings to.
        auto result = std::vector<unsigned short>{};
        result.reserve(text.size());

        for (int i = 0; i < (int)text.size();)
        {
            unsigned char byte = (unsigned char)text[i];
            unsigned int codePoint = 0xFFFD;
            int length = 1;

            if (byte < 0x80)
            {
                codePoint = byte;
            }
            else if ((byte & 0xE0) == 0xC0 && i + 1 < (int)text.size())
            {
                codePoint = ((byte & 0x1F) << 6) | (text[i + 1] & 0x3F);
                length = 2;
            }
            else if ((byte & 0xF0) == 0xE0 && i + 2 < (int)text.size())
            {
                codePoint = ((byte & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F);
                length = 3;
            }
            else if ((byte & 0xF8) == 0xF0 && i + 3 < (int)text.size())
            {
                codePoint = ((byte & 0x07) << 18) | ((text[i + 1] & 0x3F) << 12) | ((text[i + 2] & 0x3F) << 6) | (text[i + 3] & 0x3F);
                length = 4;
            }

            if (codePoint > 0xFFFF)
            {
                codePoint -= 0x10000;
                result.push_back((unsigned short)(0xD800 + (codePoint >> 10)));
                result.push_back((unsigned short)(0xDC00 + (codePoint & 0x3FF)));
            }
            else
            {
                result.push_back((unsigned short)codePoint);
            }

            i += length;
        }

        return result;
    }

    template <typename Action>
    void WebGL2SpriteFont::ForEachGlyph(const std::vector<unsigned short>& text, Action action) const
    {
        // SpriteFont::Impl::ForEachGlyph, with whitespace that draws nothing skipped.
        float x = 0.0f;
        float y = 0.0f;

        for (auto character : text)
        {
            if (character == L'\r')
                continue;

            if (character == L'\n')
            {
                x = 0.0f;
                y += _lineSpacing;
                continue;
            }

            const auto* glyph = Find(character);
            if (glyph == nullptr)
                continue;

            x += glyph->XOffset;
            if (x < 0.0f)
                x = 0.0f;

            float advance = (float)(glyph->Right - glyph->Left) + glyph->XAdvance;
            if (!std::iswspace((wint_t)character) || (glyph->Right - glyph->Left) > 1 || (glyph->Bottom - glyph->Top) > 1)
                action(*glyph, x, y);

            x += advance;
        }
    }

    float WebGL2SpriteFont::GetLineSpacing()
    {
        return _lineSpacing;
    }

    Vector2 WebGL2SpriteFont::MeasureString(const std::string& str)
    {
        auto result = Vector2::Zero;
        ForEachGlyph(ToUtf16(str), [&](const FontGlyph& glyph, float x, float y)
        {
            float width = (float)(glyph.Right - glyph.Left);
            float height = (float)(glyph.Bottom - glyph.Top) + glyph.YOffset;
            height = std::iswspace((wint_t)glyph.Character) ? _lineSpacing : std::max(height, _lineSpacing);

            result.x = std::max(result.x, x + width);
            result.y = std::max(result.y, y + height);
        });

        return result;
    }

    Glyph WebGL2SpriteFont::FindGlyph(char c)
    {
        // A char becomes a wchar_t on Windows, so characters above 127 turn into
        // values no font has, and the default glyph is found.
        auto character = (unsigned int)(unsigned short)(short)c;
        const auto* found = Find(character);

        auto glyph = Glyph{};
        if (found != nullptr)
        {
            glyph.Character = found->Character;
            glyph.XOffset = found->XOffset;
            glyph.YOffset = found->YOffset;
            glyph.XAdvance = found->XAdvance;
        }

        return glyph;
    }

    void WebGL2SpriteFont::DrawString(ISpriteBatch* spriteBatch, const std::string& text, Vector2 position, Vector4 color, float rotation, Vector2 origin, float scale)
    {
        auto* batch = static_cast<WebGL2SpriteBatch*>(spriteBatch);
        auto* webTexture = dynamic_cast<IWebGL2Texture*>(_texture.get());
        if (batch == nullptr || webTexture == nullptr)
            return;

        auto* texture = webTexture->GetNativeTexture();
        ForEachGlyph(ToUtf16(text), [&](const FontGlyph& glyph, float x, float y)
        {
            auto size = Vector2((float)(glyph.Right - glyph.Left), (float)(glyph.Bottom - glyph.Top));
            auto sourceUV = Vector4(
                (float)glyph.Left / _textureWidth, (float)glyph.Top / _textureHeight,
                (float)glyph.Right / _textureWidth, (float)glyph.Bottom / _textureHeight);
            auto glyphOrigin = origin - Vector2(x, y + glyph.YOffset);

            batch->DrawSprite(texture, position, size, sourceUV, color, rotation, glyphOrigin, Vector2(scale));
        });
    }
}

#endif
