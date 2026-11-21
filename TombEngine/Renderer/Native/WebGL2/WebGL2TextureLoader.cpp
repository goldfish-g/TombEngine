#include "framework.h"
#include "Renderer/Native/WebGL2/WebGL2TextureLoader.h"

#ifdef SDL_PLATFORM_EMSCRIPTEN

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>

#define BCDEC_IMPLEMENTATION
#include <bcdec.h>

namespace TEN::Renderer::Native::WebGL2
{
    // Compressed formats of the WEBGL_compressed_texture_s3tc(_srgb),
    // EXT_texture_compression_rgtc and EXT_texture_compression_bptc extensions.
    constexpr GLenum COMPRESSED_RGBA_S3TC_DXT1		 = 0x83F1;
    constexpr GLenum COMPRESSED_RGBA_S3TC_DXT3		 = 0x83F2;
    constexpr GLenum COMPRESSED_RGBA_S3TC_DXT5		 = 0x83F3;
    constexpr GLenum COMPRESSED_SRGB_ALPHA_S3TC_DXT1 = 0x8C4D;
    constexpr GLenum COMPRESSED_SRGB_ALPHA_S3TC_DXT3 = 0x8C4E;
    constexpr GLenum COMPRESSED_SRGB_ALPHA_S3TC_DXT5 = 0x8C4F;
    constexpr GLenum COMPRESSED_RED_RGTC1			 = 0x8DBB;
    constexpr GLenum COMPRESSED_RED_GREEN_RGTC2		 = 0x8DBD;
    constexpr GLenum COMPRESSED_RGBA_BPTC_UNORM		 = 0x8E8C;
    constexpr GLenum COMPRESSED_SRGB_ALPHA_BPTC		 = 0x8E8D;
    constexpr GLenum COMPRESSED_RGB_BPTC_UNSIGNED	 = 0x8E8F;

    enum class BlockFormat
    {
        None,
        BC1,
        BC2,
        BC3,
        BC4,
        BC5,
        BC6H,
        BC7
    };

    struct PixelFormat
    {
        BlockFormat Block = BlockFormat::None;
        bool		Srgb  = false;
        bool		Bgra  = false; // 8-bit BGRA(X) pixels.
        bool		Bgra4 = false; // 4-bit BGRA pixels.
        bool		NoAlpha = false;
        bool		Valid = false;
    };

    int GetLevelCount(int width, int height)
    {
        int levels = 1;
        while (width > 1 || height > 1)
        {
            width = std::max(width / 2, 1);
            height = std::max(height / 2, 1);
            levels++;
        }

        return levels;
    }

    static int GetBlockSize(BlockFormat format)
    {
        switch (format)
        {
        case BlockFormat::BC1:
        case BlockFormat::BC4:
            return 8;

        default:
            return 16;
        }
    }

    static PixelFormat GetDxgiFormat(int dxgi)
    {
        auto format = PixelFormat{};
        format.Valid = true;

        switch (dxgi)
        {
        case 28: break; // R8G8B8A8_UNORM
        case 29: format.Srgb = true; break;
        case 87: format.Bgra = true; break;
        case 88: format.Bgra = true; format.NoAlpha = true; break;
        case 91: format.Bgra = true; format.Srgb = true; break;
        case 93: format.Bgra = true; format.NoAlpha = true; format.Srgb = true; break;
        case 115: format.Bgra4 = true; break;
        case 70: case 71: format.Block = BlockFormat::BC1; break;
        case 72: format.Block = BlockFormat::BC1; format.Srgb = true; break;
        case 73: case 74: format.Block = BlockFormat::BC2; break;
        case 75: format.Block = BlockFormat::BC2; format.Srgb = true; break;
        case 76: case 77: format.Block = BlockFormat::BC3; break;
        case 78: format.Block = BlockFormat::BC3; format.Srgb = true; break;
        case 79: case 80: format.Block = BlockFormat::BC4; break;
        case 82: case 83: format.Block = BlockFormat::BC5; break;
        case 94: case 95: format.Block = BlockFormat::BC6H; break;
        case 97: case 98: format.Block = BlockFormat::BC7; break;
        case 99: format.Block = BlockFormat::BC7; format.Srgb = true; break;
        default: format.Valid = false; break;
        }

        return format;
    }

    static bool GetCompressedFormat(const PixelFormat& format, const TextureCapabilities& capabilities, GLenum& internalFormat)
    {
        switch (format.Block)
        {
        case BlockFormat::BC1:
        case BlockFormat::BC2:
        case BlockFormat::BC3:
            if (format.Srgb && !capabilities.S3tcSrgb)
                return false;

            if (!format.Srgb && !capabilities.S3tc)
                return false;

            if (format.Block == BlockFormat::BC1)
                internalFormat = format.Srgb ? COMPRESSED_SRGB_ALPHA_S3TC_DXT1 : COMPRESSED_RGBA_S3TC_DXT1;
            else if (format.Block == BlockFormat::BC2)
                internalFormat = format.Srgb ? COMPRESSED_SRGB_ALPHA_S3TC_DXT3 : COMPRESSED_RGBA_S3TC_DXT3;
            else
                internalFormat = format.Srgb ? COMPRESSED_SRGB_ALPHA_S3TC_DXT5 : COMPRESSED_RGBA_S3TC_DXT5;

            return true;

        case BlockFormat::BC4:
            internalFormat = COMPRESSED_RED_RGTC1;
            return capabilities.Rgtc;

        case BlockFormat::BC5:
            internalFormat = COMPRESSED_RED_GREEN_RGTC2;
            return capabilities.Rgtc;

        case BlockFormat::BC6H:
            internalFormat = COMPRESSED_RGB_BPTC_UNSIGNED;
            return capabilities.Bptc;

        case BlockFormat::BC7:
            internalFormat = format.Srgb ? COMPRESSED_SRGB_ALPHA_BPTC : COMPRESSED_RGBA_BPTC_UNORM;
            return capabilities.Bptc;

        default:
            return false;
        }
    }

    // Decodes one mip level of blocks to pixels, for browsers without the format.
    static std::vector<unsigned char> DecodeBlocks(BlockFormat format, int width, int height, const unsigned char* data, int& pixelSize)
    {
        switch (format)
        {
        case BlockFormat::BC4: pixelSize = 1; break;
        case BlockFormat::BC5: pixelSize = 2; break;
        case BlockFormat::BC6H: pixelSize = 6; break;
        default: pixelSize = 4; break;
        }

        int blocksX = std::max(1, (width + 3) / 4);
        int blocksY = std::max(1, (height + 3) / 4);
        int paddedWidth = blocksX * 4;
        int pitch = paddedWidth * pixelSize;
        auto padded = std::vector<unsigned char>(pitch * blocksY * 4);
        int blockSize = GetBlockSize(format);

        for (int by = 0; by < blocksY; by++)
        {
            for (int bx = 0; bx < blocksX; bx++)
            {
                const auto* block = data + (by * blocksX + bx) * blockSize;
                auto* destination = padded.data() + (by * 4) * pitch + bx * 4 * pixelSize;

                switch (format)
                {
                case BlockFormat::BC1: bcdec_bc1(block, destination, pitch); break;
                case BlockFormat::BC2: bcdec_bc2(block, destination, pitch); break;
                case BlockFormat::BC3: bcdec_bc3(block, destination, pitch); break;
                case BlockFormat::BC4: bcdec_bc4(block, destination, pitch); break;
                case BlockFormat::BC5: bcdec_bc5(block, destination, pitch); break;
                case BlockFormat::BC6H: bcdec_bc6h_half(block, destination, pitch / 2, 0); break;
                case BlockFormat::BC7: bcdec_bc7(block, destination, pitch); break;
                default: break;
                }
            }
        }

        if (paddedWidth == width && blocksY * 4 == height)
            return padded;

        auto result = std::vector<unsigned char>(width * height * pixelSize);
        for (int y = 0; y < height; y++)
            memcpy(result.data() + y * width * pixelSize, padded.data() + y * pitch, width * pixelSize);

        return result;
    }

    static void SetDecodedFormat(DecodedTexture& texture, BlockFormat format, bool srgb)
    {
        switch (format)
        {
        case BlockFormat::BC4:
            texture.InternalFormat = GL_R8;
            texture.Format = GL_RED;
            break;

        case BlockFormat::BC5:
            texture.InternalFormat = GL_RG8;
            texture.Format = GL_RG;
            break;

        case BlockFormat::BC6H:
            texture.InternalFormat = GL_RGB16F;
            texture.Format = GL_RGB;
            texture.Type = GL_HALF_FLOAT;
            break;

        default:
            texture.InternalFormat = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
            texture.Format = GL_RGBA;
            break;
        }
    }

    // Fills a texture from the mip levels of a format, in blocks or pixels.
    static DecodedTexture DecodeLevels(const PixelFormat& format, int width, int height, int levelCount,
                                       const unsigned char* data, int size, const TextureCapabilities& capabilities)
    {
        auto texture = DecodedTexture{};
        texture.Width = width;
        texture.Height = height;

        GLenum compressedFormat = 0;
        bool keepBlocks = format.Block != BlockFormat::None &&
                          GetCompressedFormat(format, capabilities, compressedFormat) &&
                          (width % 4) == 0 && (height % 4) == 0;

        if (format.Block != BlockFormat::None)
        {
            if (keepBlocks)
            {
                texture.Compressed = true;
                texture.InternalFormat = compressedFormat;
            }
            else
            {
                SetDecodedFormat(texture, format.Block, format.Srgb);
            }
        }
        else
        {
            texture.InternalFormat = format.Srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
            texture.Format = GL_RGBA;
        }

        int offset = 0;
        int levelWidth = width;
        int levelHeight = height;
        for (int level = 0; level < levelCount; level++)
        {
            int levelSize = 0;
            if (format.Block != BlockFormat::None)
                levelSize = std::max(1, (levelWidth + 3) / 4) * std::max(1, (levelHeight + 3) / 4) * GetBlockSize(format.Block);
            else
                levelSize = levelWidth * levelHeight * (format.Bgra4 ? 2 : 4);

            if (offset + levelSize > size)
                break;

            auto result = TextureLevel{};
            result.Width = levelWidth;
            result.Height = levelHeight;

            if (keepBlocks)
            {
                result.Data.assign(data + offset, data + offset + levelSize);
            }
            else if (format.Block != BlockFormat::None)
            {
                int pixelSize = 0;
                result.Data = DecodeBlocks(format.Block, levelWidth, levelHeight, data + offset, pixelSize);
            }
            else
            {
                // WebGL has no BGRA formats, so those pixels are reordered here.
                int pixelCount = levelWidth * levelHeight;
                result.Data.resize(pixelCount * 4);
                const auto* source = data + offset;

                for (int i = 0; i < pixelCount; i++)
                {
                    auto* pixel = &result.Data[i * 4];
                    if (format.Bgra4)
                    {
                        unsigned short value = source[i * 2] | (source[i * 2 + 1] << 8);
                        pixel[0] = ((value >> 8) & 0xF) * 17;
                        pixel[1] = ((value >> 4) & 0xF) * 17;
                        pixel[2] = (value & 0xF) * 17;
                        pixel[3] = ((value >> 12) & 0xF) * 17;
                    }
                    else if (format.Bgra)
                    {
                        pixel[0] = source[i * 4 + 2];
                        pixel[1] = source[i * 4 + 1];
                        pixel[2] = source[i * 4 + 0];
                        pixel[3] = format.NoAlpha ? 255 : source[i * 4 + 3];
                    }
                    else
                    {
                        memcpy(pixel, source + i * 4, 4);
                    }
                }
            }

            texture.Levels.push_back(std::move(result));
            offset += levelSize;
            levelWidth = std::max(levelWidth / 2, 1);
            levelHeight = std::max(levelHeight / 2, 1);
        }

        texture.Valid = !texture.Levels.empty();
        return texture;
    }

    static unsigned int ReadUInt(const unsigned char* data)
    {
        return data[0] | (data[1] << 8) | (data[2] << 16) | ((unsigned int)data[3] << 24);
    }

    static DecodedTexture DecodeDds(const unsigned char* data, int size, const TextureCapabilities& capabilities)
    {
        constexpr int HEADER_SIZE		  = 128;
        constexpr int DX10_HEADER_SIZE	  = 20;
        constexpr unsigned int DDPF_FOURCC = 0x4;
        constexpr unsigned int DDPF_RGB	  = 0x40;
        constexpr unsigned int DDSD_MIPMAPCOUNT = 0x20000;

        if (size < HEADER_SIZE)
            return {};

        unsigned int flags = ReadUInt(data + 8);
        int height = (int)ReadUInt(data + 12);
        int width = (int)ReadUInt(data + 16);
        int mipCount = (flags & DDSD_MIPMAPCOUNT) ? std::max(1, (int)ReadUInt(data + 28)) : 1;
        unsigned int pixelFlags = ReadUInt(data + 80);
        unsigned int fourCC = ReadUInt(data + 84);
        unsigned int bitCount = ReadUInt(data + 88);
        unsigned int redMask = ReadUInt(data + 92);
        unsigned int alphaMask = ReadUInt(data + 104);

        auto makeFourCC = [](const char* code) { return (unsigned int)code[0] | (code[1] << 8) | (code[2] << 16) | ((unsigned int)code[3] << 24); };

        auto format = PixelFormat{};
        int offset = HEADER_SIZE;

        if (pixelFlags & DDPF_FOURCC)
        {
            format.Valid = true;
            if (fourCC == makeFourCC("DX10"))
            {
                if (size < HEADER_SIZE + DX10_HEADER_SIZE)
                    return {};

                format = GetDxgiFormat((int)ReadUInt(data + HEADER_SIZE));
                offset += DX10_HEADER_SIZE;
            }
            else if (fourCC == makeFourCC("DXT1"))
                format.Block = BlockFormat::BC1;
            else if (fourCC == makeFourCC("DXT2") || fourCC == makeFourCC("DXT3"))
                format.Block = BlockFormat::BC2;
            else if (fourCC == makeFourCC("DXT4") || fourCC == makeFourCC("DXT5"))
                format.Block = BlockFormat::BC3;
            else if (fourCC == makeFourCC("ATI1") || fourCC == makeFourCC("BC4U"))
                format.Block = BlockFormat::BC4;
            else if (fourCC == makeFourCC("ATI2") || fourCC == makeFourCC("BC5U"))
                format.Block = BlockFormat::BC5;
            else
                format.Valid = false;
        }
        else if ((pixelFlags & DDPF_RGB) && bitCount == 32)
        {
            format.Valid = true;
            format.Bgra = (redMask == 0x00FF0000);
            format.NoAlpha = (alphaMask == 0);
        }

        if (!format.Valid)
        {
            TENLog("Unsupported DDS texture format.", LogLevel::Warning);
            return {};
        }

        return DecodeLevels(format, width, height, mipCount, data + offset, size - offset, capabilities);
    }

    // WIC's sRGB rules, which DirectXTK's WICTextureLoader follows by default.
    static bool IsPngSrgb(const unsigned char* data, int size)
    {
        int offset = 8;
        while (offset + 12 <= size)
        {
            unsigned int length = (data[offset] << 24) | (data[offset + 1] << 16) | (data[offset + 2] << 8) | data[offset + 3];
            auto type = std::string((const char*)data + offset + 4, 4);

            if (type == "sRGB")
                return true;

            if (type == "gAMA" && length == 4 && offset + 12 <= size)
            {
                unsigned int gamma = (data[offset + 8] << 24) | (data[offset + 9] << 16) | (data[offset + 10] << 8) | data[offset + 11];
                return gamma == 45455;
            }

            if (type == "IDAT" || type == "IEND")
                break;

            offset += 12 + (int)length;
        }

        return false;
    }

    static bool IsJpegSrgb(const unsigned char* data, int size)
    {
        int offset = 2;
        while (offset + 4 <= size && data[offset] == 0xFF)
        {
            int marker = data[offset + 1];
            int length = (data[offset + 2] << 8) | data[offset + 3];
            if (marker == 0xDA)
                break;

            // APP1 holding EXIF: look for the ColorSpace tag (0xA001) in the EXIF IFD.
            if (marker == 0xE1 && length > 14 && offset + 2 + length <= size && !memcmp(data + offset + 4, "Exif\0\0", 6))
            {
                const auto* tiff = data + offset + 10;
                int tiffSize = length - 8;
                bool little = (tiff[0] == 'I');
                auto read16 = [&](int at) { return little ? (tiff[at] | (tiff[at + 1] << 8)) : ((tiff[at] << 8) | tiff[at + 1]); };
                auto read32 = [&](int at) { return little ? (unsigned int)(read16(at) | (read16(at + 2) << 16)) : (unsigned int)((read16(at) << 16) | read16(at + 2)); };

                auto findTag = [&](unsigned int ifd, int tag, unsigned int& value) -> bool
                {
                    if ((int)ifd + 2 > tiffSize)
                        return false;

                    int count = read16(ifd);
                    for (int i = 0; i < count; i++)
                    {
                        int entry = ifd + 2 + i * 12;
                        if (entry + 12 > tiffSize)
                            return false;

                        if (read16(entry) == tag)
                        {
                            value = (read16(entry + 2) == 3) ? read16(entry + 8) : read32(entry + 8);
                            return true;
                        }
                    }

                    return false;
                };

                unsigned int exifIfd = 0;
                unsigned int colorSpace = 0;
                if (tiffSize > 8 && findTag(read32(4), 0x8769, exifIfd) && findTag(exifIfd, 0xA001, colorSpace))
                    return colorSpace == 1;
            }

            offset += 2 + length;
        }

        return false;
    }

    static DecodedTexture DecodePicture(const unsigned char* data, int size)
    {
        bool isPng = size > 8 && !memcmp(data, "\x89PNG", 4);
        bool isJpeg = size > 3 && data[0] == 0xFF && data[1] == 0xD8;
        bool isBmp = size > 2 && data[0] == 'B' && data[1] == 'M';
        bool isGif = size > 4 && !memcmp(data, "GIF8", 4);

        // Windows decodes these with WIC. TGA, which WIC can't read, fails there too.
        if (!isPng && !isJpeg && !isBmp && !isGif)
        {
            TENLog("Unsupported picture format.", LogLevel::Warning);
            return {};
        }

        int width = 0;
        int height = 0;
        int channels = 0;
        if (!stbi_info_from_memory(data, size, &width, &height, &channels))
            return {};

        // WIC gives grey pictures without alpha one channel.
        int wanted = (channels == 1) ? 1 : 4;
        auto* pixels = stbi_load_from_memory(data, size, &width, &height, &channels, wanted);
        if (pixels == nullptr)
            return {};

        bool srgb = (isPng && IsPngSrgb(data, size)) || (isJpeg && IsJpegSrgb(data, size));

        auto texture = DecodedTexture{};
        texture.Valid = true;
        texture.Width = width;
        texture.Height = height;
        texture.GenerateMips = true;
        texture.InternalFormat = (wanted == 1) ? GL_R8 : (srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8);
        texture.Format = (wanted == 1) ? GL_RED : GL_RGBA;

        auto level = TextureLevel{};
        level.Width = width;
        level.Height = height;
        level.Data.assign(pixels, pixels + width * height * wanted);
        texture.Levels.push_back(std::move(level));

        stbi_image_free(pixels);
        return texture;
    }

    DecodedTexture DecodeTextureFile(const unsigned char* data, int size, const TextureCapabilities& capabilities)
    {
        if (data == nullptr || size < 4)
            return {};

        if (!memcmp(data, "DDS ", 4))
            return DecodeDds(data, size, capabilities);

        return DecodePicture(data, size);
    }

    DecodedTexture DecodeDxgiPixels(int dxgiFormat, int width, int height, int stride, int rows,
                                    const unsigned char* data, const TextureCapabilities& capabilities)
    {
        auto format = GetDxgiFormat(dxgiFormat);
        if (!format.Valid)
        {
            TENLog("Unsupported sprite font texture format " + std::to_string(dxgiFormat) + ".", LogLevel::Warning);
            return {};
        }

        return DecodeLevels(format, width, height, 1, data, stride * rows, capabilities);
    }
}

#endif
