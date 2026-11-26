#include "framework.h"
#include "Renderer/Native/WebGL2/WebGL2TextureLoader.h"

#ifdef SDL_PLATFORM_EMSCRIPTEN

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>

#define BCDEC_IMPLEMENTATION
#include <bcdec.h>

#include <etcpak/ProcessRGB.hpp>
#include <functional>
#include <thread>

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

    // Phones sample neither the PNG pictures most levels hold nor their DXT5 blocks,
    // and as pixels the textures of a level fill hundreds of megabytes there. So
    // where the browser takes ETC2 but not S3TC (phones), textures are compressed to
    // ETC2 as they load: without alpha when they have none, EAC for one or two
    // channels, mipmaps included. Desktops keep them as the Windows build does.
    //
    // Small textures stay as they are: they take little memory.
    constexpr int ETC2_MIN_PIXELS = 128 * 128;

    // Runs work(begin, end) over [0, count) on up to 8 threads, each given at least minimum.
    static void ParallelFor(int count, int minimum, const std::function<void(int, int)>& work)
    {
        int threadCount = std::clamp((int)std::thread::hardware_concurrency() - 1, 1, 8);
        threadCount = std::min(threadCount, count / std::max(minimum, 1));
        if (threadCount <= 1)
        {
            work(0, count);
            return;
        }

        auto threads = std::vector<std::thread>{};
        int perThread = (count + threadCount - 1) / threadCount;
        for (int begin = 0; begin < count; begin += perThread)
            threads.emplace_back(work, begin, std::min(begin + perThread, count));

        for (auto& thread : threads)
            thread.join();
    }

    // sRGB colours to linear light (0-65535), and back from linear light / 16.
    static const std::array<uint16_t, 256>& GetSrgbToLinear()
    {
        static const auto table = []()
        {
            auto result = std::array<uint16_t, 256>{};
            for (int i = 0; i < 256; i++)
            {
                float value = i / 255.0f;
                value = (value <= 0.04045f) ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
                result[i] = (uint16_t)std::lround(value * 65535.0f);
            }

            return result;
        }();

        return table;
    }

    static const std::array<uint8_t, 4096>& GetLinearToSrgb()
    {
        static const auto table = []()
        {
            auto result = std::array<uint8_t, 4096>{};
            for (int i = 0; i < 4096; i++)
            {
                float value = i / 4095.0f;
                value = (value <= 0.0031308f) ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
                result[i] = (uint8_t)std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f);
            }

            return result;
        }();

        return table;
    }

    // The next mip level, as glGenerateMipmap makes it: each pixel the average of the
    // pixels it covers, in linear light for sRGB colours.
    static TextureLevel Downsample(const TextureLevel& level, int channels, bool srgb)
    {
        auto result = TextureLevel{};
        result.Width = std::max(level.Width / 2, 1);
        result.Height = std::max(level.Height / 2, 1);
        result.Data.resize((size_t)result.Width * result.Height * channels);

        const auto& toLinear = GetSrgbToLinear();
        const auto& toSrgb = GetLinearToSrgb();

        // A side of one pixel is averaged with itself.
        int stepX = (level.Width > 1) ? channels : 0;
        size_t stepY = (level.Height > 1) ? (size_t)level.Width * channels : 0;
        int spanX = (level.Width > 1) ? 2 : 1;
        int spanY = (level.Height > 1) ? 2 : 1;

        ParallelFor(result.Height, 64, [&](int begin, int end)
        {
            for (int y = begin; y < end; y++)
            {
                const auto* row = &level.Data[(size_t)y * spanY * level.Width * channels];
                auto* output = &result.Data[(size_t)y * result.Width * channels];

                for (int x = 0; x < result.Width; x++)
                {
                    const auto* p = row + (size_t)x * spanX * channels;
                    for (int c = 0; c < channels; c++, output++)
                    {
                        if (srgb && c < 3)
                        {
                            unsigned int sum = toLinear[p[c]] + toLinear[p[c + stepX]] + toLinear[p[c + stepY]] + toLinear[p[c + stepY + stepX]];
                            *output = toSrgb[(sum / 4) >> 4];
                        }
                        else
                        {
                            unsigned int sum = p[c] + p[c + stepX] + p[c + stepY] + p[c + stepY + stepX];
                            *output = (unsigned char)((sum + 2) / 4);
                        }
                    }
                }
            }
        });

        return result;
    }

    // Encodes a level in rows of 4x4 blocks, shared out between threads.
    static TextureLevel EncodeEtc2Level(const TextureLevel& level, int channels, GLenum format)
    {
        // etcpak reads whole blocks of BGRA pixels; edge blocks repeat the last pixels.
        int paddedWidth = (level.Width + 3) & ~3;
        int paddedHeight = (level.Height + 3) & ~3;
        auto pixels = std::vector<uint32_t>((size_t)paddedWidth * paddedHeight);
        ParallelFor(paddedHeight, 256, [&](int begin, int end)
        {
            for (int y = begin; y < end; y++)
            {
                int sourceY = std::min(y, level.Height - 1);
                for (int x = 0; x < paddedWidth; x++)
                {
                    const auto* source = &level.Data[((size_t)sourceY * level.Width + std::min(x, level.Width - 1)) * channels];
                    uint32_t r = source[0];
                    uint32_t g = (channels > 1) ? source[1] : 0;
                    uint32_t b = (channels > 2) ? source[2] : 0;
                    uint32_t a = (channels > 3) ? source[3] : 255;
                    pixels[(size_t)y * paddedWidth + x] = b | (g << 8) | (r << 16) | (a << 24);
                }
            }
        });

        int blocksX = paddedWidth / 4;
        int blocksY = paddedHeight / 4;
        int blockBytes = (format == GL_COMPRESSED_RGBA8_ETC2_EAC || format == GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC ||
                          format == GL_COMPRESSED_RG11_EAC) ? 16 : 8;

        auto result = TextureLevel{};
        result.Width = level.Width;
        result.Height = level.Height;
        result.Data.resize((size_t)blocksX * blocksY * blockBytes);

        auto encodeRows = [&](int firstRow, int rowCount)
        {
            const auto* source = pixels.data() + (size_t)firstRow * 4 * paddedWidth;
            auto* destination = (uint64_t*)(result.Data.data() + (size_t)firstRow * blocksX * blockBytes);
            auto blocks = (uint32_t)(rowCount * blocksX);

            switch (format)
            {
            case GL_COMPRESSED_R11_EAC:
                CompressEacR(source, destination, blocks, paddedWidth);
                break;

            case GL_COMPRESSED_RG11_EAC:
                CompressEacRg(source, destination, blocks, paddedWidth);
                break;

            case GL_COMPRESSED_RGB8_ETC2:
            case GL_COMPRESSED_SRGB8_ETC2:
                CompressEtc2Rgb(source, destination, blocks, paddedWidth, true);
                break;

            default:
                CompressEtc2Rgba(source, destination, blocks, paddedWidth, true);
                break;
            }
        };

        // Small levels aren't worth a thread.
        ParallelFor(blocksY, std::max(1, 4096 / blocksX), [&](int begin, int end) { encodeRows(begin, end - begin); });
        return result;
    }

    static void CompressForPhone(DecodedTexture& texture, const TextureCapabilities& capabilities)
    {
        if (!capabilities.Etc2 || capabilities.S3tc || !texture.Valid || texture.Compressed ||
            texture.Type != GL_UNSIGNED_BYTE || texture.Levels.empty())
        {
            return;
        }

        int channels = (texture.Format == GL_RGBA) ? 4 : (texture.Format == GL_RG) ? 2 : (texture.Format == GL_RED) ? 1 : 0;
        int baseWidth = texture.Levels[0].Width;
        int baseHeight = texture.Levels[0].Height;
        if (channels == 0 || baseWidth * baseHeight < ETC2_MIN_PIXELS)
            return;

        for (const auto& level : texture.Levels)
        {
            if (level.Data.size() < (size_t)level.Width * level.Height * channels)
                return;
        }

        bool opaque = true;
        if (channels == 4)
        {
            const auto& pixels = texture.Levels[0].Data;
            for (size_t i = 3; i < pixels.size() && opaque; i += 4)
                opaque = (pixels[i] == 255);
        }

        bool srgb = (texture.InternalFormat == GL_SRGB8_ALPHA8);
        GLenum format = GL_COMPRESSED_R11_EAC;
        if (channels == 2)
            format = GL_COMPRESSED_RG11_EAC;
        else if (channels == 4 && opaque)
            format = srgb ? GL_COMPRESSED_SRGB8_ETC2 : GL_COMPRESSED_RGB8_ETC2;
        else if (channels == 4)
            format = srgb ? GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC : GL_COMPRESSED_RGBA8_ETC2_EAC;

        auto start = std::chrono::steady_clock::now();

        if (texture.GenerateMips)
        {
            while (texture.Levels.back().Width > 1 || texture.Levels.back().Height > 1)
                texture.Levels.push_back(Downsample(texture.Levels.back(), channels, srgb));
        }

        auto mipmapped = std::chrono::steady_clock::now();

        for (auto& level : texture.Levels)
            level = EncodeEtc2Level(level, channels, format);

        texture.Compressed = true;
        texture.GenerateMips = false;
        texture.InternalFormat = format;

        if (baseWidth * baseHeight >= 1024 * 1024)
        {
            auto now = std::chrono::steady_clock::now();
            auto mipTime = std::chrono::duration_cast<std::chrono::milliseconds>(mipmapped - start).count();
            auto encodeTime = std::chrono::duration_cast<std::chrono::milliseconds>(now - mipmapped).count();
            TENLog(fmt::format("Compressed a {}x{} texture to {} in {} ms (mipmaps {} ms).", texture.Width, texture.Height,
                (channels < 4) ? "EAC" : (opaque ? "ETC2" : "ETC2 with alpha"), mipTime + encodeTime, mipTime), LogLevel::Info);
        }
    }

    DecodedTexture DecodeTextureFile(const unsigned char* data, int size, const TextureCapabilities& capabilities)
    {
        if (data == nullptr || size < 4)
            return {};

        auto texture = !memcmp(data, "DDS ", 4) ? DecodeDds(data, size, capabilities) : DecodePicture(data, size);
        CompressForPhone(texture, capabilities);
        return texture;
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
