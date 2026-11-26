#!/usr/bin/env python3
"""Compiles the HLSL shaders of a TombEngine game into GLSL ES 3.00 for WebGL 2.

The Windows build compiles the game's own Shaders folder at start-up with
D3DCompile. Browsers have no HLSL compiler, so this script does the same work
ahead of time: DXC turns each shader into SPIR-V with the flags the Windows
build uses, and SPIRV-Cross turns the SPIR-V into GLSL ES 3.00. The result is a
single JSON manifest that the WebGL 2 graphics device loads instead of the
HLSL files.

Because it reads the game's own HLSL, shaders that a builder changed come out
changed too.

Usage: compile_shaders.py <Shaders dir> <output manifest> [--keep <dir>]
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

# The variants ShaderManager.cpp loads, as (file, entry point, stages, defines).
# Stages are "VS", "PS" or "VSPS"; the entry functions are named <stage><entry>.
SMAA_DEFINES = {"SMAA_HLSL_4_1": "1"}
VARIANTS = [
    # LoadAAShaders()
    ("SMAA", "EdgeDetection", "VS", "smaa"),
    ("SMAA", "LumaEdgeDetection", "PS", "smaa"),
    ("SMAA", "ColorEdgeDetection", "PS", "smaa"),
    ("SMAA", "DepthEdgeDetection", "PS", "smaa"),
    ("SMAA", "BlendingWeightCalculation", "VSPS", "smaa"),
    ("SMAA", "NeighborhoodBlending", "VSPS", "smaa"),
    ("FXAA", "", "PS", {}),
    # LoadCommonShaders()
    ("Rooms", "", "VSPS", {}),
    ("Rooms", "", "PS", {"TRANSPARENT": ""}),
    ("RoomAmbient", "", "VSPS", {}),
    ("RoomAmbient", "Sky", "VSPS", {}),
    ("Objects", "", "VSPS", {}),
    ("Sky", "", "VSPS", {}),
    ("Solid", "", "VSPS", {}),
    ("Inventory", "", "VSPS", {}),
    ("FullScreenQuad", "", "VSPS", {}),
    ("ShadowMap", "", "VSPS", {"SHADOW_MAP": ""}),
    ("HUD", "", "VS", {}),
    ("HUD", "ColoredHUD", "PS", {}),
    ("HUD", "TexturedHUD", "PS", {}),
    ("HUD", "TexturedHUDBar", "PS", {}),
    ("InstancedSprites", "", "VSPS", {}),
    ("GBuffer", "", "PS", {}),
    ("GBuffer", "Rooms", "VS", {}),
    ("GBuffer", "Objects", "VS", {}),
    # LoadPostprocessShaders()
    ("PostProcess", "", "VSPS", {}),
    ("PostProcess", "Monochrome", "PS", {}),
    ("PostProcess", "Negative", "PS", {}),
    ("PostProcess", "Exclusion", "PS", {}),
    ("PostProcess", "Distortion", "PS", {}),
    ("PostProcess", "DOFDownsample", "PS", {}),
    ("PostProcess", "DOFFarBlur", "PS", {}),
    ("PostProcess", "DOFNearDilate", "PS", {}),
    ("PostProcess", "DOFNearBlur", "PS", {}),
    ("PostProcess", "DOFComposite", "PS", {}),
    ("PostProcess", "FinalPass", "PS", {}),
    ("PostProcess", "LensFlare", "PS", {}),
    ("PostProcess", "Downscale", "PS", {}),
    ("PostProcess", "Blur", "PS", {}),
    ("PostProcess", "GlowCombine", "PS", {}),
    ("SSAO", "", "PS", {}),
    ("SSAO", "Blur", "PS", {}),
]

# The SMAA shaders get the size of the screen as a macro on Windows. Here they
# read it from a constant buffer in a slot the engine leaves free, which the
# graphics device fills from the macro it is given.
SMAA_METRICS_REGISTER = 12
SMAA_METRICS_NAME = "TENWebSmaaRtMetrics"

# Register spaces kept apart in SPIR-V, so each binding tells its HLSL register.
B_SHIFT, T_SHIFT, S_SHIFT, U_SHIFT = 0, 100, 200, 300

# Structured buffers become textures of float4 texels this wide (WebGL 2 has no
# storage buffers). The graphics device lays the elements out the same way.
DATA_TEXTURE_WIDTH = 2048

# Samplers declared with an effect state block get registers from this one up.
# TombEngine binds its own samplers to s1-s6.
EFFECT_SAMPLER_FIRST_REGISTER = 10

SCALAR_SIZES = {"float": 1, "int": 1, "uint": 1, "dword": 1}


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def run(args, cwd=None):
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


def parse_struct_fields(source, name):
    match = re.search(r"struct\s+" + re.escape(name) + r"\s*\{(.*?)\};", source, re.S)
    if not match:
        return None
    fields = []
    for line in match.group(1).split(";"):
        line = re.sub(r"//.*", "", line).strip()
        if not line:
            continue
        m = re.match(r"(float|int|uint|dword)([1-4]?)\s+(\w+)$", line)
        if not m:
            return None
        fields.append((m.group(1), int(m.group(2) or 1), m.group(3)))
    return fields


def rewrite_structured_buffers(text, all_sources):
    """Turns StructuredBuffer<T> declarations into float4 data textures."""

    def replace(match):
        type_name, var_name, register = match.group(1), match.group(2), match.group(3)
        fields = None
        for source in all_sources:
            fields = parse_struct_fields(source, type_name)
            if fields:
                break
        if not fields:
            fail("Can't turn StructuredBuffer<%s> into a texture: unsupported struct." % type_name)

        size = sum(count for _, count, _ in fields)
        texels = (size + 3) // 4
        lines = ["Texture2D<float4> %s : register(%s);" % (var_name, register)]
        lines.append("%s TENWebLoad%s(uint index)" % (type_name, var_name))
        lines.append("{")
        lines.append("    %s result;" % type_name)
        for i in range(texels):
            lines.append(
                "    float4 texel%d = %s.Load(int3(int((index * %du + %du) %% %du), int((index * %du + %du) / %du), 0));"
                % (i, var_name, texels, i, DATA_TEXTURE_WIDTH, texels, i, DATA_TEXTURE_WIDTH))
        offset = 0
        for scalar, count, field in fields:
            parts = []
            for c in range(count):
                component = "texel%d.%s" % ((offset + c) // 4, "xyzw"[(offset + c) % 4])
                parts.append(component if scalar == "float" else "as%s(%s)" % ("uint" if scalar in ("uint", "dword") else "int", component))
            value = parts[0] if count == 1 else "%s%d(%s)" % (scalar, count, ", ".join(parts))
            lines.append("    result.%s = %s;" % (field, value))
            offset += count
        lines.append("    return result;")
        lines.append("}")
        return "\n".join(lines) + "\n#define TENWEB_SB_" + var_name + "\n"

    pattern = re.compile(r"StructuredBuffer\s*<\s*(\w+)\s*>\s*(\w+)\s*:\s*register\s*\(\s*(t\d+)\s*\)\s*;")
    names = [m.group(2) for m in pattern.finditer(text)]
    text = pattern.sub(replace, text)
    return text, names


def rewrite_effect_samplers(text, sampler_states):
    """Gives samplers declared with an effect state block a register of their own.

    D3DCompile ignores the state block and leaves such samplers unbound, which
    DXC refuses. They get spare registers here, and the manifest keeps the
    filter and addressing the block asks for.
    """

    def replace(match):
        name, block = match.group(1), match.group(2)
        state = {"filter": "linear", "mip": "linear", "address": ["clamp", "clamp", "clamp"]}
        filter_match = re.search(r"Filter\s*=\s*(\w+)", block)
        if filter_match:
            value = filter_match.group(1).upper()
            state["filter"] = "point" if value.startswith("MIN_MAG_MIP_POINT") or value.startswith("MIN_MAG_POINT") else "linear"
            state["mip"] = "point" if value.endswith("MIP_POINT") else "linear"
        for i, axis in enumerate("UVW"):
            address = re.search(r"Address%s\s*=\s*(\w+)" % axis, block)
            if address:
                state["address"][i] = address.group(1).lower()
        if name not in sampler_states:
            sampler_states[name] = dict(state, register=EFFECT_SAMPLER_FIRST_REGISTER + len(sampler_states))
        return "SamplerState %s : register(s%d);" % (name, sampler_states[name]["register"])

    return re.sub(r"SamplerState\s+(\w+)\s*\{([^}]*)\}\s*;", replace, text)


def split_padding_vectors(text):
    """Splits float2/float3 padding members into scalars.

    Direct3D packs a float3 right after a float, std140 aligns it to 16 bytes.
    As scalars the members keep their Direct3D offsets and fit std140 too.
    """

    def replace(match):
        scalar, count, name = match.group(1), int(match.group(2)), match.group(3)
        return " ".join("%s %s_%s;" % (scalar, name, "xyz"[i]) for i in range(count))

    return re.sub(r"\b(float|int|uint)([23])\s+(\w*[Pp]adding\w*)\s*;", replace, text)


def strip_effect_blocks(text):
    """Removes effect-file state objects and techniques, which D3DCompile ignores."""
    pattern = re.compile(r"\b(DepthStencilState|BlendState|RasterizerState|technique10|technique11|technique)\s+\w+\s*\{")
    while True:
        match = pattern.search(text)
        if not match:
            return text
        depth, i = 1, match.end()
        while depth and i < len(text):
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        if text[i:i + 1] == ";":
            i += 1
        text = text[:match.start()] + text[i:]


def rewrite_comparison_sampling(text):
    """Replaces SampleCmp on float textures with the filtering Direct3D does.

    The engine's shadow map is an R32_FLOAT colour texture read with a linear
    comparison sampler (LESS_EQUAL). GLSL ES only compares depth textures, so
    the four texels are compared and filtered bilinearly here, as Direct3D does.
    """
    pattern = re.compile(r"(\w+)\.SampleCmp(?:LevelZero)?\s*\(\s*\w+\s*,")
    if not pattern.search(text):
        return text
    text = pattern.sub(r"TENWebSampleCmp(\1,", text)
    helper = """
#ifndef TENWEB_SAMPLE_CMP
#define TENWEB_SAMPLE_CMP

float TENWebCompareTexel(float reference, float texel)
{
    return reference <= texel ? 1.0f : 0.0f;
}

float TENWebSampleCmp(Texture2DArray tex, float3 location, float reference)
{
    uint width, height, layers;
    tex.GetDimensions(width, height, layers);
    float2 position = location.xy * float2(width, height) - 0.5f;
    int2 base = int2(floor(position));
    float2 weight = position - floor(position);
    int layer = clamp(int(round(location.z)), 0, int(layers) - 1);
    int2 limit = int2(width, height) - 1;
    float s00 = TENWebCompareTexel(reference, tex.Load(int4(clamp(base, 0, limit), layer, 0)).r);
    float s10 = TENWebCompareTexel(reference, tex.Load(int4(clamp(base + int2(1, 0), 0, limit), layer, 0)).r);
    float s01 = TENWebCompareTexel(reference, tex.Load(int4(clamp(base + int2(0, 1), 0, limit), layer, 0)).r);
    float s11 = TENWebCompareTexel(reference, tex.Load(int4(clamp(base + int2(1, 1), 0, limit), layer, 0)).r);
    return lerp(lerp(s00, s10, weight.x), lerp(s01, s11, weight.x), weight.y);
}

float TENWebSampleCmp(Texture2D tex, float2 location, float reference)
{
    uint width, height;
    tex.GetDimensions(width, height);
    float2 position = location * float2(width, height) - 0.5f;
    int2 base = int2(floor(position));
    float2 weight = position - floor(position);
    int2 limit = int2(width, height) - 1;
    float s00 = TENWebCompareTexel(reference, tex.Load(int3(clamp(base, 0, limit), 0)).r);
    float s10 = TENWebCompareTexel(reference, tex.Load(int3(clamp(base + int2(1, 0), 0, limit), 0)).r);
    float s01 = TENWebCompareTexel(reference, tex.Load(int3(clamp(base + int2(0, 1), 0, limit), 0)).r);
    float s11 = TENWebCompareTexel(reference, tex.Load(int3(clamp(base + int2(1, 1), 0, limit), 0)).r);
    return lerp(lerp(s00, s10, weight.x), lerp(s01, s11, weight.x), weight.y);
}

#endif
"""
    # The helpers go in front of the first function, after the declarations
    # the file starts with, so they don't depend on anything in it.
    return helper + text


def prepare_sources(source_dir, work_dir):
    files = {}
    for name in os.listdir(source_dir):
        if os.path.splitext(name)[1].lower() in (".hlsl", ".hlsli", ".fx", ".h"):
            with open(os.path.join(source_dir, name), "r", encoding="utf-8", errors="replace") as f:
                files[name] = f.read()

    buffer_names = []
    sampler_states = {}
    for name, text in files.items():
        text, names = rewrite_structured_buffers(text, files.values())
        text = rewrite_effect_samplers(text, sampler_states)
        text = strip_effect_blocks(text)
        text = split_padding_vectors(text)
        text = rewrite_comparison_sampling(text)
        files[name] = text
        buffer_names += names

    for name, text in files.items():
        for var_name in buffer_names:
            text = re.sub(r"\b%s\s*\[([^\[\]]+)\]" % re.escape(var_name), r"TENWebLoad%s(\1)" % var_name, text)
        with open(os.path.join(work_dir, name), "w", encoding="utf-8") as f:
            f.write(text)
    return buffer_names, sampler_states


def source_file(work_dir, base):
    # Windows finds files whatever their case, and the engine relies on it
    # (it loads "FullScreenQuad" from FullscreenQuad.hlsl).
    names = {name.lower(): name for name in os.listdir(work_dir)}
    for ext in (".hlsl", ".fx"):
        if (base + ext).lower() in names:
            return names[(base + ext).lower()]
    fail("Shader source %s.hlsl not found." % base)


def rename_interface(glsl, stage):
    # Vertex outputs and pixel inputs carry their HLSL semantic in their names
    # (out_var_TEXCOORD0, in_var_TEXCOORD0). GLSL ES links stages by name, so
    # both sides get the same name. Vertex inputs keep theirs.
    if stage == "VS":
        return re.sub(r"\bout_var_(\w+)", r"tenweb_var_\1", glsl)
    return re.sub(r"\bin_var_(\w+)", r"tenweb_var_\1", glsl)


# Components of the vector and scalar types SPIRV-Cross reflects.
COMPONENTS = {}
for prefix in ("", "i", "u", "b", "d"):
    for n in (2, 3, 4):
        COMPONENTS["%svec%d" % (prefix, n)] = n
for scalar in ("float", "int", "uint", "bool", "double"):
    COMPONENTS[scalar] = 1


def layout_leaves(types, type_name, base):
    """Lists (offset, bytes) of every scalar or vector in a block, in member order."""
    leaves = []
    for member in types[type_name]["members"]:
        dims = member.get("array", [])
        count = 1
        for dim in dims:
            count *= dim
        stride = member.get("array_stride", 0)
        for i in range(count):
            leaves += member_leaves(types, member, base + member["offset"] + i * stride)
    return leaves


def member_leaves(types, member, offset):
    kind = member["type"]
    if kind in types:
        return layout_leaves(types, kind, offset)
    match = re.match(r"d?mat(\d)(?:x(\d))?$", kind)
    if match:
        columns = int(match.group(1))
        rows = int(match.group(2) or columns)
        stride = member.get("matrix_stride", 16)
        vectors, size = (rows, columns) if member.get("row_major") else (columns, rows)
        return [(offset + i * stride, size * 4) for i in range(vectors)]
    if kind not in COMPONENTS:
        fail("Unknown constant buffer member type %s." % kind)
    return [(offset, COMPONENTS[kind] * 4)]


def layout_copies(dx_types, dx_type, gl_types, gl_type):
    """Copies that move a constant buffer from Direct3D packing to std140."""
    dx_leaves = layout_leaves(dx_types, dx_type, 0)
    gl_leaves = layout_leaves(gl_types, gl_type, 0)
    if len(dx_leaves) != len(gl_leaves):
        fail("Constant buffer layouts don't match member for member.")
    copies = []
    for (src, size), (dst, gl_size) in zip(dx_leaves, gl_leaves):
        if size != gl_size:
            fail("Constant buffer member sizes differ between layouts.")
        if copies and copies[-1][0] + copies[-1][2] == src and copies[-1][1] + copies[-1][2] == dst:
            copies[-1][2] += size
        else:
            copies.append([src, dst, size])
    return copies


def last_array(types, type_name):
    """The array a block ends with, as (name, offset, stride, length), or None.

    WebGL 2 devices may take uniform blocks of only 16 KB. The engine cuts this
    array short on them, and draws in batches that fit.
    """
    members = types[type_name]["members"]
    if not members:
        return None
    last = max(members, key=lambda m: m["offset"])
    dims = last.get("array", [])
    if len(dims) != 1 or not last.get("array_size_is_literal", [True])[0] or not last.get("array_stride"):
        return None
    return (last["name"], last["offset"], last["array_stride"], dims[0])


def split_semantic(name):
    m = re.match(r"(.*?)(\d*)$", name)
    return m.group(1), int(m.group(2) or 0)


def compile_stage(work_dir, temp_dir, file_name, stage, entry, defines, smaa):
    src = source_file(work_dir, file_name)
    target = ("vs_6_0" if stage == "VS" else "ps_6_0")
    spv = os.path.join(temp_dir, "%s.%s%s.spv" % (file_name, stage, entry))

    # -Zpr matches D3DCOMPILE_PACK_MATRIX_ROW_MAJOR. The IEEE strictness flag of the
    # Windows build (-Gis) is left out: it stops DXC from legalizing SPIR-V, and the
    # browser compiles the GLSL with its own floating-point rules anyway.
    args = ["dxc", "-T", target, "-E", stage + entry, "-spirv", "-Zpr", "-HV", "2018",
            "-fvk-b-shift", str(B_SHIFT), "0", "-fvk-t-shift", str(T_SHIFT), "0",
            "-fvk-s-shift", str(S_SHIFT), "0", "-fvk-u-shift", str(U_SHIFT), "0",
            "-Wno-conversion", "-Wno-for-redefinition", "-Wno-effects-syntax"]
    dxc_defines = dict(defines)
    if smaa:
        # SMAA_HLSL_4_1 only adds Gather() to the depth edge detection, which GLSL ES 3.00
        # lacks. Without it SMAA reads the same texels with point samples.
        dxc_defines.pop("SMAA_HLSL_4_1", None)
    for key, value in sorted(dxc_defines.items()):
        args += ["-D", "%s=%s" % (key, value) if value != "" else key]
    if smaa:
        # Prepend the metrics constant buffer through a wrapper file.
        wrapper = os.path.join(work_dir, "TENWeb_%s_%s%s.hlsl" % (file_name, stage, entry))
        with open(wrapper, "w") as f:
            f.write("cbuffer TENWebSmaa : register(b%d) { float4 %s; };\n" % (SMAA_METRICS_REGISTER, SMAA_METRICS_NAME))
            f.write("#define SMAA_RT_METRICS %s\n" % SMAA_METRICS_NAME)
            f.write('#include "%s"\n' % src)
        src = os.path.basename(wrapper)

    # Constant buffers are compiled twice: with the packing of Direct3D, which is
    # how the engine's C++ structures lay them out, and with std140, the only
    # packing GLSL ES 3.00 has. The GLSL comes from the std140 build, and the
    # two layouts give the copies that turn one into the other.
    reflections = {}
    for layout in ("dx", "gl"):
        layout_spv = spv + "." + layout
        code, output = run(args + ["-fvk-use-%s-layout" % layout, src, "-Fo", layout_spv], cwd=work_dir)
        if code != 0:
            fail("DXC failed for %s %s%s:\n%s" % (file_name, stage, entry, output))
        code, reflection = run(["spirv-cross", layout_spv, "--reflect"])
        if code != 0:
            fail("SPIRV-Cross reflection failed for %s %s%s:\n%s" % (file_name, stage, entry, reflection))
        reflections[layout] = json.loads(reflection)
    spv += ".gl"
    reflection = reflections["gl"]

    cross = ["spirv-cross", spv, "--es", "--version", "300", "--remove-unused-variables"]
    if stage == "VS":
        cross += ["--fixup-clipspace", "--flip-vert-y"]
    code, glsl = run(cross)
    if code != 0:
        fail("SPIRV-Cross failed for %s %s%s:\n%s" % (file_name, stage, entry, glsl))

    glsl = rename_interface(glsl, stage)

    # Direct3D computes in 32-bit floats. SPIRV-Cross gives GLSL ES pixel shaders
    # mediump as default precision, which phones run in 16 bits: world positions
    # overflow and lighting breaks. (Its command line can't set the default.)
    glsl = glsl.replace("precision mediump float;", "precision highp float;")
    glsl = glsl.replace("precision mediump int;", "precision highp int;")

    # Validate the result against GLSL ES 3.00, the language of WebGL 2.
    check = os.path.join(temp_dir, "check." + ("vert" if stage == "VS" else "frag"))
    with open(check, "w") as f:
        f.write(glsl)
    code, output = run(["glslangValidator", check])
    if code != 0:
        fail("Generated GLSL is not valid GLSL ES 3.00 for %s %s%s:\n%s" % (file_name, stage, entry, output))

    ubos = []
    dx_ubos = {u["binding"]: u for u in reflections["dx"].get("ubos", [])}
    for ubo in reflection.get("ubos", []):
        dx_ubo = dx_ubos.get(ubo["binding"])
        if not dx_ubo:
            fail("Constant buffer b%d differs between layouts in %s %s%s." % (ubo["binding"], file_name, stage, entry))
        ubos.append({
            "block": ubo["name"].replace(".", "_"),
            "register": ubo["binding"] - B_SHIFT,
            "size": ubo["block_size"],
            "copies": layout_copies(reflections["dx"]["types"], dx_ubo["type"], reflection["types"], ubo["type"]),
            "array": last_array(reflection["types"], ubo["type"]),
        })

    images = reflection.get("separate_images", [])
    samplers = reflection.get("separate_samplers", [])
    combined = []
    for sampler_type, uniform in re.findall(r"uniform\s+(?:\w+\s+)?(\w+)\s+(SPIRV_Cross_Combined\w+)\s*;", glsl):
        found = None
        for image in images:
            for sampler in samplers:
                if uniform == "SPIRV_Cross_Combined" + image["name"] + sampler["name"]:
                    found = (image, sampler)
        if not found:
            # A texture read with Load() gets a dummy sampler.
            for image in images:
                if uniform == "SPIRV_Cross_Combined" + image["name"] + "SPIRV_Cross_DummySampler":
                    found = (image, None)
        if not found:
            fail("Can't map %s to its registers in %s %s%s." % (uniform, file_name, stage, entry))
        image, sampler = found
        combined.append({
            "uniform": uniform,
            "type": sampler_type,
            "t": image["binding"] - T_SHIFT,
            "s": (sampler["binding"] - S_SHIFT) if sampler else -1,
        })

    if reflection.get("ssbos"):
        fail("Storage buffer left in %s %s%s." % (file_name, stage, entry))

    inputs = []
    if stage == "VS":
        for attribute in reflection.get("inputs", []):
            name = attribute["name"].replace("in.var.", "")
            semantic, index = split_semantic(name)
            inputs.append({"semantic": semantic, "index": index, "location": attribute["location"]})

    return {"glsl": glsl, "ubos": ubos, "textures": combined, "inputs": inputs}


def main():
    if len(sys.argv) < 3:
        fail(__doc__)
    source_dir, output = sys.argv[1], sys.argv[2]
    keep = sys.argv[sys.argv.index("--keep") + 1] if "--keep" in sys.argv else None

    with tempfile.TemporaryDirectory() as temp_dir:
        work_dir = os.path.join(temp_dir, "src")
        os.makedirs(work_dir)
        buffers, sampler_states = prepare_sources(source_dir, work_dir)

        manifest = {
            "format": 1,
            "dataTextureWidth": DATA_TEXTURE_WIDTH,
            "smaaMetricsRegister": SMAA_METRICS_REGISTER,
            "structuredBuffers": buffers,
            "samplerStates": sorted(sampler_states.values(), key=lambda s: s["register"]),
            "shaders": {},
        }
        for file_name, entry, stages, defines in VARIANTS:
            smaa_presets = [None]
            if defines == "smaa":
                smaa_presets = ["SMAA_PRESET_MEDIUM", "SMAA_PRESET_ULTRA"]
            for preset in smaa_presets:
                variant_defines = dict(SMAA_DEFINES) if preset else dict(defines)
                if preset:
                    variant_defines[preset] = ""
                for stage in ("VS", "PS"):
                    if stage not in stages:
                        continue
                    key = variant_key(file_name, stage, entry, variant_defines)
                    print("Compiling", key)
                    shader = compile_stage(work_dir, temp_dir, file_name, stage, entry, variant_defines, preset is not None)
                    shader["smaaMetrics"] = preset is not None
                    manifest["shaders"][key] = shader

        if keep:
            os.makedirs(keep, exist_ok=True)
            for key, shader in manifest["shaders"].items():
                safe = re.sub(r"[^\w.=-]", "_", key)
                with open(os.path.join(keep, safe + (".vert" if "|VS|" in key else ".frag")), "w") as f:
                    f.write(shader["glsl"])

    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    write_manifest(manifest, output)
    print("Wrote %d shaders to %s" % (len(manifest["shaders"]), output))


def write_manifest(manifest, path):
    """Writes the manifest in the line format WebGL2ShaderLibrary.cpp reads.

    Each line is a keyword and space-separated values; a shader's GLSL follows
    its "glsl <bytes>" line verbatim.
    """
    out = ["TENWEBSHADERS %d" % manifest["format"],
           "datatexturewidth %d" % manifest["dataTextureWidth"],
           "smaametrics %d" % manifest["smaaMetricsRegister"]]
    for state in manifest["samplerStates"]:
        out.append("samplerstate %d %s %s %s" % (state["register"], state["filter"], state["mip"], " ".join(state["address"])))
    text = "\n".join(out) + "\n"
    for key, shader in manifest["shaders"].items():
        lines = ["shader %s" % key, "smaa %d" % (1 if shader["smaaMetrics"] else 0)]
        for ubo in shader["ubos"]:
            copies = " ".join("%d %d %d" % tuple(c) for c in ubo["copies"])
            line = "ubo %s %d %d %d %s" % (ubo["block"], ubo["register"], ubo["size"], len(ubo["copies"]), copies)
            if ubo["array"]:
                # After the copies, where readers that don't know it stop reading.
                line += " %s %d %d %d" % ubo["array"]
            lines.append(line)
        for texture in shader["textures"]:
            lines.append("texture %s %s %d %d" % (texture["uniform"], texture["type"], texture["t"], texture["s"]))
        for attribute in shader["inputs"]:
            lines.append("input %s %d %d" % (attribute["semantic"], attribute["index"], attribute["location"]))
        glsl = shader["glsl"].encode("utf-8")
        lines.append("glsl %d" % len(glsl))
        text += "\n".join(lines) + "\n" + glsl.decode("utf-8") + "\nend\n"
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def variant_key(file_name, stage, entry, defines):
    # Same key the graphics device builds from a ShaderCompileRequest. The SMAA
    # metrics macro is left out: its value moves to a constant buffer.
    parts = ["%s=%s" % (k, v) for k, v in sorted(defines.items()) if k != "SMAA_RT_METRICS"]
    return "%s|%s|%s|%s" % (file_name, stage, entry, ";".join(parts))


if __name__ == "__main__":
    main()
