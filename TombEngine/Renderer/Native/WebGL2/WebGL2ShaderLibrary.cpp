#include "framework.h"
#include "Renderer/Native/WebGL2/WebGL2ShaderLibrary.h"

#ifdef SDL_PLATFORM_EMSCRIPTEN

namespace TEN::Renderer::Native::WebGL2
{
    static GLenum GetSamplerTarget(const std::string& type)
    {
        if (type.find("2DArray") != std::string::npos)
            return GL_TEXTURE_2D_ARRAY;

        if (type.find("3D") != std::string::npos)
            return GL_TEXTURE_3D;

        if (type.find("Cube") != std::string::npos)
            return GL_TEXTURE_CUBE_MAP;

        return GL_TEXTURE_2D;
    }

    static GLenum GetAddressMode(const std::string& mode)
    {
        if (mode == "wrap")
            return GL_REPEAT;

        if (mode == "mirror")
            return GL_MIRRORED_REPEAT;

        return GL_CLAMP_TO_EDGE;
    }

    // Declares the array of a uniform block shorter in a stage's GLSL.
    static void ShortenArray(std::string& glsl, const ShortenedArray& array)
    {
        auto start = glsl.find("uniform " + array.Block);
        auto open = (start != std::string::npos) ? glsl.find('{', start) : std::string::npos;
        auto close = (open != std::string::npos) ? glsl.find('}', open) : std::string::npos;
        auto declaration = " " + array.Member + "[" + std::to_string(array.Count) + "];";
        auto at = (close != std::string::npos) ? glsl.find(declaration, open) : std::string::npos;
        if (at == std::string::npos || at > close)
        {
            TENLog("WebGL shader array " + array.Block + "." + array.Member + " not found to shorten.", LogLevel::Warning);
            return;
        }

        glsl.replace(at, declaration.size(), " " + array.Member + "[" + std::to_string(array.Length) + "];");
    }

    std::string MakeShaderKey(const std::string& fileName, const std::string& stage, const std::string& entryPoint,
                              const std::map<std::string, std::string>& macros)
    {
        auto key = fileName + "|" + stage + "|" + entryPoint + "|";

        bool first = true;
        for (const auto& [name, value] : macros)
        {
            // The SMAA screen size comes from a constant buffer instead.
            if (name == "SMAA_RT_METRICS")
                continue;

            if (!first)
                key += ";";

            key += name + "=" + value;
            first = false;
        }

        return key;
    }

    const BlockLayout* ShaderLibrary::AddLayout(BlockLayout layout)
    {
        auto id = layout.Name + "/" + std::to_string(layout.Size);
        for (const auto& copy : layout.Copies)
            id += "/" + std::to_string(copy.Source) + "," + std::to_string(copy.Destination) + "," + std::to_string(copy.Size);

        auto it = _layouts.find(id);
        if (it != _layouts.end())
            return it->second.get();

        layout.IsIdentity = (layout.Copies.size() == 1 && layout.Copies[0].Source == 0 && layout.Copies[0].Destination == 0);

        auto stored = std::make_unique<BlockLayout>(std::move(layout));
        auto* result = stored.get();
        _layouts[id] = std::move(stored);
        return result;
    }

    bool ShaderLibrary::Load(const std::string& path)
    {
        auto file = std::ifstream(path, std::ios::binary);
        if (!file)
        {
            TENLog("WebGL shader file not found: " + path, LogLevel::Error);
            return false;
        }

        auto content = std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        int position = 0;

        auto readLine = [&]()
        {
            int end = (int)content.find('\n', position);
            if (end == (int)std::string::npos)
                end = (int)content.size();

            auto line = content.substr(position, end - position);
            position = std::min(end + 1, (int)content.size());
            return line;
        };

        auto header = std::istringstream(readLine());
        auto magic = std::string();
        int format = 0;
        header >> magic >> format;
        if (magic != "TENWEBSHADERS" || format != 1)
        {
            TENLog("Unsupported WebGL shader file: " + path, LogLevel::Error);
            return false;
        }

        ShaderStageInfo* stage = nullptr;
        while (position < (int)content.size())
        {
            auto line = readLine();
            auto stream = std::istringstream(line);
            auto keyword = std::string();
            stream >> keyword;

            if (keyword == "datatexturewidth")
            {
                stream >> _dataTextureWidth;
            }
            else if (keyword == "smaametrics")
            {
                stream >> _smaaMetricsRegister;
            }
            else if (keyword == "samplerstate")
            {
                auto state = SamplerStateInfo{};
                auto filter = std::string();
                auto mip = std::string();
                auto address = std::array<std::string, 3>{};
                stream >> state.Register >> filter >> mip >> address[0] >> address[1] >> address[2];

                state.Linear = (filter == "linear");
                state.MipLinear = (mip == "linear");
                for (int i = 0; i < 3; i++)
                    state.Address[i] = GetAddressMode(address[i]);

                _samplerStates.push_back(state);
            }
            else if (keyword == "shader")
            {
                auto newStage = std::make_unique<ShaderStageInfo>();
                stream >> newStage->Key;
                newStage->Stage = (newStage->Key.find("|VS|") != std::string::npos) ? STAGE_VERTEX : STAGE_PIXEL;

                stage = newStage.get();
                _stages[stage->Key] = std::move(newStage);
            }
            else if (stage == nullptr)
            {
                continue;
            }
            else if (keyword == "smaa")
            {
                int value = 0;
                stream >> value;
                stage->UsesSmaaMetrics = (value != 0);
            }
            else if (keyword == "ubo")
            {
                auto layout = BlockLayout{};
                int reg = 0;
                int copyCount = 0;
                stream >> layout.Name >> reg >> layout.Size >> copyCount;

                for (int i = 0; i < copyCount; i++)
                {
                    auto copy = BlockCopy{};
                    stream >> copy.Source >> copy.Destination >> copy.Size;
                    layout.Copies.push_back(copy);
                }

                // The array the block ends with: name, offset, stride and length.
                auto array = ShortenedArray{ layout.Name };
                int arrayOffset = 0;
                int arrayStride = 0;
                bool hasArray = (bool)(stream >> array.Member >> arrayOffset >> arrayStride >> array.Count) && arrayStride > 0;

                if (layout.Size > _maxBlockSize)
                {
                    // Too large for the device: the block would not link. The array keeps
                    // the elements that fit, and the engine draws in batches that small
                    // (see IGraphicsDevice::GetMaxConstantBufferSize()).
                    array.Length = hasArray ? (_maxBlockSize - arrayOffset) / arrayStride : 0;
                    if (array.Length > 0 && array.Length < array.Count)
                    {
                        layout.Size = arrayOffset + array.Length * arrayStride;

                        auto copies = std::vector<BlockCopy>{};
                        for (auto copy : layout.Copies)
                        {
                            if (copy.Destination >= layout.Size)
                                continue;

                            copy.Size = std::min(copy.Size, layout.Size - copy.Destination);
                            copies.push_back(copy);
                        }

                        layout.Copies = std::move(copies);
                        stage->Shortened.push_back(array);
                    }
                    else
                    {
                        TENLog(fmt::format("WebGL constant buffer {} ({} bytes) is larger than this device takes ({} bytes).",
                            layout.Name, layout.Size, _maxBlockSize), LogLevel::Warning);
                    }
                }

                stage->Blocks.push_back(StageBlock{ AddLayout(std::move(layout)), reg });
            }
            else if (keyword == "texture")
            {
                auto texture = StageTexture{};
                auto type = std::string();
                stream >> texture.Uniform >> type >> texture.TextureRegister >> texture.SamplerRegister;
                texture.Target = GetSamplerTarget(type);
                stage->Textures.push_back(texture);
            }
            else if (keyword == "input")
            {
                auto input = StageInput{};
                stream >> input.Semantic >> input.Index >> input.Location;
                stage->Inputs.push_back(input);
            }
            else if (keyword == "glsl")
            {
                int size = 0;
                stream >> size;
                stage->Glsl = content.substr(position, size);
                position = std::min(position + size, (int)content.size());

                for (const auto& array : stage->Shortened)
                    ShortenArray(stage->Glsl, array);

                // Direct3D leaves render targets a shader doesn't write alone; WebGL
                // refuses to draw to them, so their draw buffers are switched off.
                auto outputs = std::regex("layout\\(location = ([0-9]+)\\) out ");
                for (auto it = std::sregex_iterator(stage->Glsl.begin(), stage->Glsl.end(), outputs); it != std::sregex_iterator(); ++it)
                    stage->OutputMask |= 1u << std::stoi((*it)[1].str());
            }
        }

        auto shortened = std::set<std::string>{};
        for (const auto& [key, loaded] : _stages)
        {
            for (const auto& array : loaded->Shortened)
                shortened.insert(fmt::format("{}.{} {} -> {}", array.Block, array.Member, array.Count, array.Length));
        }

        for (const auto& line : shortened)
            TENLog("WebGL uniform blocks up to " + std::to_string(_maxBlockSize) + " bytes: " + line, LogLevel::Info);

        TENLog("Loaded " + std::to_string(_stages.size()) + " WebGL shaders from " + path, LogLevel::Info);
        _loaded = true;
        return true;
    }

    const ShaderStageInfo* ShaderLibrary::Find(const std::string& key) const
    {
        auto it = _stages.find(key);
        return (it != _stages.end()) ? it->second.get() : nullptr;
    }

    bool ShaderLibrary::CompileStage(ShaderStageInfo& stage)
    {
        if (stage.Shader != 0)
            return true;

        if (stage.CompileFailed)
            return false;

        auto shader = glCreateShader(stage.Stage == STAGE_VERTEX ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
        const char* source = stage.Glsl.c_str();
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);

        GLint status = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
        if (status != GL_TRUE)
        {
            char log[4096] = {};
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            TENLog("WebGL shader " + stage.Key + " failed to compile: " + log, LogLevel::Error);
            glDeleteShader(shader);
            stage.CompileFailed = true;
            return false;
        }

        stage.Shader = shader;
        return true;
    }

    Program* ShaderLibrary::GetProgram(const ShaderStageInfo* vertex, const ShaderStageInfo* pixel, bool& linked)
    {
        linked = false;
        auto key = std::make_pair(vertex, pixel);
        auto it = _programs.find(key);
        if (it != _programs.end())
            return it->second->Failed ? nullptr : it->second.get();

        auto program = std::make_unique<Program>();
        program->Vertex = vertex;
        program->Pixel = pixel;

        auto* result = program.get();
        _programs[key] = std::move(program);
        linked = true;

        if (vertex == nullptr || pixel == nullptr ||
            !CompileStage(*(ShaderStageInfo*)vertex) || !CompileStage(*(ShaderStageInfo*)pixel))
        {
            result->Failed = true;
            return nullptr;
        }

        auto id = glCreateProgram();
        glAttachShader(id, vertex->Shader);
        glAttachShader(id, pixel->Shader);
        glLinkProgram(id);

        GLint status = GL_FALSE;
        glGetProgramiv(id, GL_LINK_STATUS, &status);
        if (status != GL_TRUE)
        {
            char log[4096] = {};
            glGetProgramInfoLog(id, sizeof(log), nullptr, log);
            TENLog("WebGL program " + vertex->Key + " + " + pixel->Key + " failed to link: " + log, LogLevel::Error);
            glDeleteProgram(id);
            result->Failed = true;
            return nullptr;
        }

        result->Id = id;
        glUseProgram(id);

        // Constant buffers keep their Direct3D register as binding point. A block
        // both stages read is one block in GL; the pixel stage's buffer is used.
        const ShaderStageInfo* stages[STAGE_COUNT] = { vertex, pixel };
        for (int stageIndex = STAGE_COUNT - 1; stageIndex >= 0; stageIndex--)
        {
            for (const auto& block : stages[stageIndex]->Blocks)
            {
                bool known = false;
                for (const auto& existing : result->Blocks)
                    known |= (existing.Register == block.Register);

                if (known)
                    continue;

                auto index = glGetUniformBlockIndex(id, block.Layout->Name.c_str());
                if (index == GL_INVALID_INDEX)
                    continue;

                GLint dataSize = 0;
                glGetActiveUniformBlockiv(id, index, GL_UNIFORM_BLOCK_DATA_SIZE, &dataSize);
                if (dataSize > _maxBlockSize)
                {
                    TENLog(fmt::format("WebGL program {} + {}: block {} is {} bytes, more than the device's {}.",
                        vertex->Key, pixel->Key, block.Layout->Name, dataSize, _maxBlockSize), LogLevel::Warning);
                }

                glUniformBlockBinding(id, index, block.Register);
                result->Blocks.push_back(ProgramBlock{ block.Layout, block.Register, stageIndex });
            }
        }

        // Every texture and sampler pair gets a texture unit of its own. A uniform
        // both stages declare is one uniform; the pixel stage's texture is used.
        auto usedLocations = std::set<GLint>{};
        for (int stageIndex = STAGE_COUNT - 1; stageIndex >= 0; stageIndex--)
        {
            for (const auto& texture : stages[stageIndex]->Textures)
            {
                auto location = glGetUniformLocation(id, texture.Uniform.c_str());
                if (location < 0 || usedLocations.count(location) != 0)
                    continue;

                usedLocations.insert(location);

                auto unit = ProgramUnit{};
                unit.Unit = (int)result->Units.size();
                unit.StageIndex = stageIndex;
                unit.TextureRegister = texture.TextureRegister;
                unit.SamplerRegister = texture.SamplerRegister;
                unit.Target = texture.Target;

                glUniform1i(location, unit.Unit);
                result->Units.push_back(unit);
            }
        }

        glUseProgram(0);
        return result;
    }

    void ShaderLibrary::Release()
    {
        for (auto& [key, program] : _programs)
        {
            if (program->Id != 0)
                glDeleteProgram(program->Id);
        }

        for (auto& [key, stage] : _stages)
        {
            if (stage->Shader != 0)
                glDeleteShader(stage->Shader);

            stage->Shader = 0;
        }

        _programs.clear();
    }
}

#endif
