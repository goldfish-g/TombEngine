#pragma once

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include <GLES3/gl3.h>

namespace TEN::Renderer::Native::WebGL2
{
    // Compressed formats the browser can sample, found when the context is made.
    struct TextureCapabilities
    {
        bool S3tc	  = false;
        bool S3tcSrgb = false;
        bool Rgtc	  = false;
        bool Bptc	  = false;
        bool Etc2	  = false;
    };

    struct TextureLevel
    {
        int Width  = 0;
        int Height = 0;
        std::vector<unsigned char> Data = {};
    };

    // A picture decoded for upload: either the blocks of a compressed format the
    // browser takes as they are, or pixels.
    struct DecodedTexture
    {
        bool   Valid		  = false;
        int	   Width		  = 0;
        int	   Height		  = 0;
        GLenum InternalFormat = GL_RGBA8;
        GLenum Format		  = GL_RGBA;
        GLenum Type			  = GL_UNSIGNED_BYTE;
        bool   Compressed	  = false;
        bool   GenerateMips	  = false;
        std::vector<TextureLevel> Levels = {};
    };

    // Decodes a DDS file the way DirectXTK's DDSTextureLoader does, or another
    // picture the way its WICTextureLoader does on Windows, mips included.
    DecodedTexture DecodeTextureFile(const unsigned char* data, int size, const TextureCapabilities& capabilities);

    // Decodes the texture of a DirectXTK sprite font, given as a DXGI format.
    DecodedTexture DecodeDxgiPixels(int dxgiFormat, int width, int height, int stride, int rows,
                                    const unsigned char* data, const TextureCapabilities& capabilities);

    int GetLevelCount(int width, int height);
}

#endif
