#pragma once

#ifdef SDL_PLATFORM_EMSCRIPTEN

#include <GLES3/gl3.h>

namespace TEN::Renderer::Native::WebGL2
{
    // Shaders come precompiled from the game's HLSL by Tools/WebGL/compile_shaders.py,
    // which writes them with what they need from Direct3D's registers into one file.

    constexpr auto STAGE_VERTEX = 0;
    constexpr auto STAGE_PIXEL	= 1;
    constexpr auto STAGE_COUNT	= 2;

    struct BlockCopy
    {
        int Source		= 0;
        int Destination = 0;
        int Size		= 0;
    };

    // How a constant buffer moves from the Direct3D packing of the engine's C++
    // structures to the std140 packing of GLSL ES.
    struct BlockLayout
    {
        std::string			   Name		  = {};
        int					   Size		  = 0;
        std::vector<BlockCopy> Copies	  = {};
        bool				   IsIdentity = false;
    };

    struct StageBlock
    {
        const BlockLayout* Layout	= nullptr;
        int				   Register = 0;
    };

    struct StageTexture
    {
        std::string Uniform			= {};
        GLenum		Target			= GL_TEXTURE_2D;
        int			TextureRegister = 0;
        int			SamplerRegister = -1;
    };

    struct StageInput
    {
        std::string Semantic = {};
        int			Index	 = 0;
        int			Location = 0;
    };

    // The array a uniform block ends with, cut short to fit the device (see Load()).
    struct ShortenedArray
    {
        std::string Block  = {};
        std::string Member = {};
        int			Count  = 0;
        int			Length = 0;
    };

    struct ShaderStageInfo
    {
        std::string				  Key			  = {};
        int						  Stage			  = STAGE_VERTEX;
        bool					  UsesSmaaMetrics = false;
        std::string				  Glsl			  = {};
        std::vector<StageBlock>	  Blocks		  = {};
        std::vector<StageTexture> Textures		  = {};
        std::vector<StageInput>	  Inputs		  = {};
        std::vector<ShortenedArray> Shortened  = {};
        unsigned int			  OutputMask	  = 0; // Render targets a pixel stage writes.

        // Main thread only.
        GLuint Shader		 = 0;
        bool   CompileFailed = false;
    };

    struct SamplerStateInfo
    {
        int	   Register	  = 0;
        bool   Linear	  = true;
        bool   MipLinear  = true;
        GLenum Address[3] = { GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE };
    };

    struct ProgramBlock
    {
        const BlockLayout* Layout	  = nullptr;
        int				   Register	  = 0;
        int				   StageIndex = STAGE_PIXEL;
    };

    struct ProgramUnit
    {
        int	   Unit			   = 0;
        int	   StageIndex	   = STAGE_PIXEL;
        int	   TextureRegister = 0;
        int	   SamplerRegister = -1;
        GLenum Target		   = GL_TEXTURE_2D;
    };

    struct Program
    {
        GLuint					  Id	 = 0;
        const ShaderStageInfo*	  Vertex = nullptr;
        const ShaderStageInfo*	  Pixel	 = nullptr;
        std::vector<ProgramBlock> Blocks = {};
        std::vector<ProgramUnit>  Units	 = {};
        bool					  Failed = false;
    };

    class ShaderLibrary
    {
    private:
        std::map<std::string, std::unique_ptr<ShaderStageInfo>> _stages	 = {};
        std::map<std::string, std::unique_ptr<BlockLayout>>		_layouts = {};
        std::vector<SamplerStateInfo> _samplerStates		   = {};
        int							  _dataTextureWidth		   = 2048;
        int							  _smaaMetricsRegister	   = 12;
        int							  _maxBlockSize			   = 65536;
        bool						  _loaded				   = false;

        // Main thread only.
        std::map<std::pair<const ShaderStageInfo*, const ShaderStageInfo*>, std::unique_ptr<Program>> _programs = {};

        const BlockLayout* AddLayout(BlockLayout layout);
        bool CompileStage(ShaderStageInfo& stage);

    public:
        // Largest uniform block the device takes, in bytes. Set before Load().
        void SetMaxBlockSize(int size) { _maxBlockSize = size; }

        // Reads the manifest. Any thread; called once before shaders are created.
        bool Load(const std::string& path);
        bool IsLoaded() const { return _loaded; }

        const ShaderStageInfo* Find(const std::string& key) const;
        const std::vector<SamplerStateInfo>& GetSamplerStates() const { return _samplerStates; }
        int GetDataTextureWidth() const { return _dataTextureWidth; }
        int GetSmaaMetricsRegister() const { return _smaaMetricsRegister; }

        // Main thread: the linked program of a vertex and a pixel stage. Sets linked
        // when it linked one now, which leaves no program in use.
        Program* GetProgram(const ShaderStageInfo* vertex, const ShaderStageInfo* pixel, bool& linked);

        // Main thread: deletes the GL objects.
        void Release();
    };

    // Builds the key compile_shaders.py stores a shader under.
    std::string MakeShaderKey(const std::string& fileName, const std::string& stage, const std::string& entryPoint,
                              const std::map<std::string, std::string>& macros);
}

#endif
