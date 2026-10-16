/*
 * `small_string_append_*` translation.  ImGui/Fade/GaussianBlur generators
 * dropped (post-FX/UI out of scope).  Vulkan/Metal/D3D
 * conditional branches preserved structurally; only OpenGL paths exercised in
 * the build.
 */

#include "shadergen.h"

#include "common/assert.h"
#include "common/log.h"

#include "glad/gl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(ShaderGen);

static void ss_appendf(small_string_t* ss, const char* fmt, ...) PRINTFLIKE(2, 3);
static void ss_appendf(small_string_t* ss, const char* fmt, ...)
{
  va_list ap; va_start(ap, fmt);
  small_string_append_vsprintf(ss, fmt, ap);
  va_end(ap);
}

#define SS_LIT(ss, lit) small_string_append_view((ss), (lit), (u32)(sizeof(lit) - 1))

void shadergen_init(shadergen_t* sg, gpu_render_api_t api, gpu_shader_language_t lang,
                    bool supports_dual_source_blend, bool supports_framebuffer_fetch)
{
  memset(sg, 0, sizeof(*sg));
  sg->render_api      = api;
  sg->shader_language = lang;
  sg->glsl            = (lang == GPU_SHADER_LANGUAGE_GLSL || lang == GPU_SHADER_LANGUAGE_GLSL_ES ||
                         lang == GPU_SHADER_LANGUAGE_GLSL_VK);
  sg->spirv           = (lang == GPU_SHADER_LANGUAGE_GLSL_VK);
  sg->supports_dual_source_blend = supports_dual_source_blend;
  sg->supports_framebuffer_fetch = supports_framebuffer_fetch;

  if (sg->glsl)
  {
    if (api == GPU_RENDER_API_OPENGL || api == GPU_RENDER_API_OPENGL_ES)
    {
      sg->glsl_version = shadergen_get_glsl_version(api);
      shadergen_get_glsl_version_string(api, sg->glsl_version, sg->glsl_version_string);
      sg->use_glsl_interface_blocks = shadergen_use_glsl_interface_blocks();
      sg->use_glsl_binding_layout   = shadergen_use_glsl_binding_layout();
    }
    else
    {
      sg->use_glsl_interface_blocks = (lang == GPU_SHADER_LANGUAGE_GLSL_VK);
      sg->use_glsl_binding_layout   = (lang == GPU_SHADER_LANGUAGE_GLSL_VK);
    }
  }
}

void shadergen_destroy(shadergen_t* sg) { (void)sg; }

gpu_shader_language_t shadergen_get_shader_language_for_api(gpu_render_api_t api)
{
  switch (api)
  {
    case GPU_RENDER_API_D3D11:
    case GPU_RENDER_API_D3D12:      return GPU_SHADER_LANGUAGE_HLSL;
    case GPU_RENDER_API_VULKAN:
    case GPU_RENDER_API_METAL:      return GPU_SHADER_LANGUAGE_GLSL_VK;
    case GPU_RENDER_API_OPENGL:     return GPU_SHADER_LANGUAGE_GLSL;
    case GPU_RENDER_API_OPENGL_ES:  return GPU_SHADER_LANGUAGE_GLSL_ES;
    case GPU_RENDER_API_NONE:
    default:                        return GPU_SHADER_LANGUAGE_NONE;
  }
}

bool shadergen_use_glsl_interface_blocks(void)
{
  return GLAD_GL_ES_VERSION_3_2 || GLAD_GL_VERSION_3_2;
}

bool shadergen_use_glsl_binding_layout(void)
{
  return GLAD_GL_ES_VERSION_3_1 || GLAD_GL_VERSION_4_3 ||
         (GLAD_GL_ARB_explicit_attrib_location && GLAD_GL_ARB_explicit_uniform_location &&
          GLAD_GL_ARB_shading_language_420pack);
}

u32 shadergen_get_glsl_version(gpu_render_api_t api)
{
  const char* glsl_version = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
  const bool glsl_es = (api == GPU_RENDER_API_OPENGL_ES);
  if (!glsl_version) return glsl_es ? 300u : 460u;

  /* skip leading non-digits */
  const char* p = glsl_version;
  while (*p && (*p < '0' || *p > '9')) ++p;

  int major = 0, minor = 0;
  if (sscanf(p, "%d.%d", &major, &minor) == 2)
  {
    if (!glsl_es && (major > 4 || (major == 4 && minor > 30)))
    { major = 4; minor = 30; }
    else if (glsl_es && (major > 3 || (major == 3 && minor > 20)))
    { major = 3; minor = 20; }
  }
  else
  {
    ERROR_LOG("Invalid GLSL version string: '%s' ('%s')", glsl_version, p);
    if (glsl_es) { major = 3; minor = 0; }
  }

  return (u32)major * 100u + (u32)minor;
}

void shadergen_get_glsl_version_string(gpu_render_api_t api, u32 version, char out[32])
{
  const bool glsl_es = (api == GPU_RENDER_API_OPENGL_ES);
  const u32 major = version / 100;
  const u32 minor = version % 100;
  snprintf(out, 32, "#version %u%02u%s", major, minor, (glsl_es && major >= 3) ? " es" : "");
}

void shadergen_define_macro_bool(const shadergen_t* sg, small_string_t* ss, const char* name, bool enabled)
{
  (void)sg;
  ss_appendf(ss, "#define %s %u\n", name, enabled ? 1u : 0u);
}

void shadergen_define_macro_int(const shadergen_t* sg, small_string_t* ss, const char* name, s32 value)
{
  (void)sg;
  ss_appendf(ss, "#define %s %d\n", name, value);
}

void shadergen_write_header(shadergen_t* sg, small_string_t* ss, bool enable_rov,
                            bool enable_framebuffer_fetch, bool enable_dual_source_blend)
{
  if (sg->shader_language == GPU_SHADER_LANGUAGE_GLSL || sg->shader_language == GPU_SHADER_LANGUAGE_GLSL_ES)
    ss_appendf(ss, "%s\n\n", sg->glsl_version_string);
  else if (sg->spirv)
    SS_LIT(ss, "#version 450 core\n\n");

  /* Extension enabling for OpenGL. */
  if (enable_framebuffer_fetch &&
      (sg->shader_language == GPU_SHADER_LANGUAGE_GLSL || sg->shader_language == GPU_SHADER_LANGUAGE_GLSL_ES))
  {
    if (GLAD_GL_EXT_shader_framebuffer_fetch)      SS_LIT(ss, "#extension GL_EXT_shader_framebuffer_fetch : require\n");
    else if (GLAD_GL_ARM_shader_framebuffer_fetch) SS_LIT(ss, "#extension GL_ARM_shader_framebuffer_fetch : require\n");
  }

  if (sg->shader_language == GPU_SHADER_LANGUAGE_GLSL_ES)
  {
    if (enable_dual_source_blend)
    {
      if (GLAD_GL_EXT_blend_func_extended) SS_LIT(ss, "#extension GL_EXT_blend_func_extended : require\n");
      if (GLAD_GL_ARB_blend_func_extended) SS_LIT(ss, "#extension GL_ARB_blend_func_extended : require\n");
    }
  }
  else if (sg->shader_language == GPU_SHADER_LANGUAGE_GLSL)
  {
    if (sg->use_glsl_binding_layout && !GLAD_GL_VERSION_4_3)
    {
      SS_LIT(ss, "#extension GL_ARB_explicit_attrib_location : require\n");
      SS_LIT(ss, "#extension GL_ARB_explicit_uniform_location : require\n");
      SS_LIT(ss, "#extension GL_ARB_shading_language_420pack : require\n");
    }
    if (!GLAD_GL_VERSION_3_2)
      SS_LIT(ss, "#extension GL_ARB_uniform_buffer_object : require\n");
    if (!GLAD_GL_VERSION_4_3 && !GLAD_GL_ES_VERSION_3_1 && GLAD_GL_ARB_shader_storage_buffer_object)
      SS_LIT(ss, "#extension GL_ARB_shader_storage_buffer_object : require\n");
  }
  else if (sg->shader_language == GPU_SHADER_LANGUAGE_GLSL_VK)
  {
    if (enable_rov)
      SS_LIT(ss, "#extension GL_ARB_fragment_shader_interlock : require\n");
  }

  shadergen_define_macro_bool(sg, ss, "API_OPENGL",     sg->render_api == GPU_RENDER_API_OPENGL);
  shadergen_define_macro_bool(sg, ss, "API_OPENGL_ES",  sg->render_api == GPU_RENDER_API_OPENGL_ES);
  shadergen_define_macro_bool(sg, ss, "API_D3D11",      sg->render_api == GPU_RENDER_API_D3D11);
  shadergen_define_macro_bool(sg, ss, "API_D3D12",      sg->render_api == GPU_RENDER_API_D3D12);
  shadergen_define_macro_bool(sg, ss, "API_VULKAN",     sg->render_api == GPU_RENDER_API_VULKAN);
  shadergen_define_macro_bool(sg, ss, "API_METAL",      sg->render_api == GPU_RENDER_API_METAL);

  if (sg->shader_language == GPU_SHADER_LANGUAGE_GLSL_ES)
  {
    SS_LIT(ss, "precision highp float;\n");
    SS_LIT(ss, "precision highp int;\n");
    SS_LIT(ss, "precision highp sampler2D;\n");
    SS_LIT(ss, "precision highp isampler2D;\n");
    SS_LIT(ss, "precision highp usampler2D;\n");
    if (GLAD_GL_ES_VERSION_3_1) SS_LIT(ss, "precision highp sampler2DMS;\n");
    if (GLAD_GL_ES_VERSION_3_2) SS_LIT(ss, "precision highp usamplerBuffer;\n");
    SS_LIT(ss, "\n");
  }

  if (sg->glsl)
  {
    SS_LIT(ss,
      "#define GLSL 1\n"
      "#define float2 vec2\n#define float3 vec3\n#define float4 vec4\n"
      "#define int2 ivec2\n#define int3 ivec3\n#define int4 ivec4\n"
      "#define uint2 uvec2\n#define uint3 uvec3\n#define uint4 uvec4\n"
      "#define bool2 bvec2\n#define bool3 bvec3\n#define bool4 bvec4\n"
      "#define float2x2 mat2\n#define float3x3 mat3\n#define float4x4 mat4\n"
      "#define mul(x, y) ((x) * (y))\n"
      "#define nointerpolation flat\n"
      "#define frac fract\n"
      "#define lerp mix\n"
      "#define CONSTANT const\n"
      "#define GLOBAL\n"
      "#define FOR_UNROLL for\n#define FOR_LOOP for\n#define IF_BRANCH if\n#define IF_FLATTEN if\n"
      "#define VECTOR_EQ(a, b) ((a) == (b))\n"
      "#define VECTOR_NEQ(a, b) ((a) != (b))\n"
      "#define VECTOR_COMP_EQ(a, b) equal((a), (b))\n"
      "#define VECTOR_COMP_NEQ(a, b) notEqual((a), (b))\n"
      "#define SAMPLE_TEXTURE(name, coords) texture(name, coords)\n"
      "#define SAMPLE_TEXTURE_OFFSET(name, coords, offset) textureOffset(name, coords, offset)\n"
      "#define SAMPLE_TEXTURE_LEVEL(name, coords, level) textureLod(name, coords, level)\n"
      "#define SAMPLE_TEXTURE_LEVEL_OFFSET(name, coords, level, offset) textureLodOffset(name, coords, level, offset)\n"
      "#define LOAD_TEXTURE(name, coords, mip) texelFetch(name, coords, mip)\n"
      "#define LOAD_TEXTURE_MS(name, coords, sample) texelFetch(name, coords, int(sample))\n"
      "#define LOAD_TEXTURE_OFFSET(name, coords, mip, offset) texelFetchOffset(name, coords, mip, offset)\n"
      "#define LOAD_TEXTURE_BUFFER(name, index) texelFetch(name, index)\n"
      "#define BEGIN_ARRAY(type, size) type[size](\n"
      "#define END_ARRAY )\n"
      "#define VECTOR_BROADCAST(type, value) (type(value))\n"
      "float saturate(float value) { return clamp(value, 0.0, 1.0); }\n"
      "float2 saturate(float2 value) { return clamp(value, float2(0.0, 0.0), float2(1.0, 1.0)); }\n"
      "float3 saturate(float3 value) { return clamp(value, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0)); }\n"
      "float4 saturate(float4 value) { return clamp(value, float4(0.0, 0.0, 0.0, 0.0), float4(1.0, 1.0, 1.0, 1.0)); }\n");
  }
  else
  {
    SS_LIT(ss,
      "#define HLSL 1\n"
      "#define roundEven round\n#define mix lerp\n#define fract frac\n"
      "#define vec2 float2\n#define vec3 float3\n#define vec4 float4\n"
      "#define ivec2 int2\n#define ivec3 int3\n#define ivec4 int4\n"
      "#define uivec2 uint2\n#define uivec3 uint3\n#define uivec4 uint4\n"
      "#define bvec2 bool2\n#define bvec3 bool3\n#define bvec4 bool4\n"
      "#define mat2 float2x2\n#define mat3 float3x3\n#define mat4 float4x4\n"
      "#define CONSTANT static const\n#define GLOBAL static\n"
      "#define FOR_UNROLL [unroll] for\n#define FOR_LOOP [loop] for\n#define IF_BRANCH [branch] if\n#define IF_FLATTEN [flatten] if\n"
      "#define VECTOR_EQ(a, b) (all((a) == (b)))\n#define VECTOR_NEQ(a, b) (any((a) != (b)))\n"
      "#define VECTOR_COMP_EQ(a, b) ((a) == (b))\n#define VECTOR_COMP_NEQ(a, b) ((a) != (b))\n"
      "#define SAMPLE_TEXTURE(name, coords) name.Sample(name##_ss, coords)\n"
      "#define SAMPLE_TEXTURE_OFFSET(name, coords, offset) name.Sample(name##_ss, coords, offset)\n"
      "#define SAMPLE_TEXTURE_LEVEL(name, coords, level) name.SampleLevel(name##_ss, coords, level)\n"
      "#define SAMPLE_TEXTURE_LEVEL_OFFSET(name, coords, level, offset) name.SampleLevel(name##_ss, coords, level, offset)\n"
      "#define LOAD_TEXTURE(name, coords, mip) name.Load(int3(coords, mip))\n"
      "#define LOAD_TEXTURE_MS(name, coords, sample) name.Load(coords, sample)\n"
      "#define LOAD_TEXTURE_OFFSET(name, coords, mip, offset) name.Load(int3(coords, mip), offset)\n"
      "#define LOAD_TEXTURE_BUFFER(name, index) name.Load(index)\n"
      "#define BEGIN_ARRAY(type, size) {\n#define END_ARRAY }\n"
      "#define VECTOR_BROADCAST(type, value) ((type)(value))\n");
  }

  if (!sg->glsl ||
      (sg->shader_language == GPU_SHADER_LANGUAGE_GLSL && sg->glsl_version < 400) ||
      (sg->shader_language == GPU_SHADER_LANGUAGE_GLSL_ES && sg->glsl_version < 310)) 
  {
    SS_LIT(ss,
      "uint packUnorm4x8(float4 value) {\n"
      "  uint4 ret = uint4(round(saturate(value) * 255.0));\n"
      "  return ret.x | (ret.y << 8) | (ret.z << 16) | (ret.w << 24);\n"
      "}\n"
      "\n"
      "float4 unpackUnorm4x8(uint value) {\n"
      "  uint4 ret = uint4(value & 0xffu, (value >> 8) & 0xffu, (value >> 16) & 0xffu, value >> 24);\n"
      "  return float4(ret) / 255.0;\n" 
      "}\n");
  }

  SS_LIT(ss, "\n");
  sg->has_uniform_buffer = false;
}

void shadergen_write_uniform_buffer_decl(const shadergen_t* sg, small_string_t* ss, bool push_constant)
{
  const u32 binding = push_constant ? 1u : 0u;
  const char* name  = push_constant ? "PushConstants" : "UBOBlock";
  shadergen_t* mut  = (shadergen_t*)sg;   /* mutable: m_has_uniform_buffer is set lazily */

  if (sg->shader_language == GPU_SHADER_LANGUAGE_GLSL_VK)
  {
    if (push_constant && (sg->render_api == GPU_RENDER_API_VULKAN || sg->render_api == GPU_RENDER_API_METAL))
    {
      ss_appendf(ss, "layout(push_constant, row_major) uniform %s\n", name);
    }
    else
    {
      ss_appendf(ss, "layout(std140, row_major, set = 0, binding = %u) uniform %s\n", binding, name);
      mut->has_uniform_buffer = true;
    }
  }
  else if (sg->glsl)
  {
    if (sg->use_glsl_binding_layout)
      ss_appendf(ss, "layout(std140, row_major, binding = %u) uniform %s\n", binding, name);
    else
      ss_appendf(ss, "layout(std140, row_major) uniform %s\n", name);
    mut->has_uniform_buffer = true;
  }
  else
  {
    ss_appendf(ss, "cbuffer %s : register(b%u)\n", name, binding);
    mut->has_uniform_buffer = true;
  }
}

void shadergen_declare_uniform_buffer(shadergen_t* sg, small_string_t* ss,
                                      const char* const* members, u32 member_count, bool push_constant)
{
  shadergen_write_uniform_buffer_decl(sg, ss, push_constant);
  SS_LIT(ss, "{\n");
  for (u32 i = 0; i < member_count; ++i)
    ss_appendf(ss, "%s;\n", members[i]);
  SS_LIT(ss, "};\n\n");
}

void shadergen_declare_texture(const shadergen_t* sg, small_string_t* ss, const char* name, u32 index,
                               bool multisampled, bool is_int, bool is_unsigned)
{
  if (sg->glsl)
  {
    if (sg->spirv)
    {
      const u32 set = (sg->has_uniform_buffer || shadergen_is_metal(sg)) ? 1u : 0u;
      ss_appendf(ss, "layout(set = %u, binding = %u) ", set, index);
    }
    else if (sg->use_glsl_binding_layout)
    {
      ss_appendf(ss, "layout(binding = %u) ", index);
    }
    ss_appendf(ss, "uniform %s%s %s;\n",
               is_int ? (is_unsigned ? "u" : "i") : "",
               multisampled ? "sampler2DMS" : "sampler2D", 
               name);
  }
  else
  {
    ss_appendf(ss, "%s%s> %s : register(t%u);\n",
               multisampled ? "Texture2DMS<" : "Texture2D<",
               is_int ? (is_unsigned ? "uint4" : "int4") : "float4", 
               name, index);
    ss_appendf(ss, "SamplerState %s_ss : register(s%u);\n", name, index);
  }
}

void shadergen_declare_texture_buffer(const shadergen_t* sg, small_string_t* ss, const char* name, u32 index,
                                      bool is_int, bool is_unsigned)
{
  if (sg->glsl)
  {
    if (sg->spirv)
    {
      const u32 set = (sg->has_uniform_buffer || shadergen_is_metal(sg)) ? 1u : 0u;
      ss_appendf(ss, "layout(set = %u, binding = %u) ", set, index);
    }
    else if (sg->use_glsl_binding_layout)
    {
      ss_appendf(ss, "layout(binding = %u) ", index);
    }
    ss_appendf(ss, "uniform %ssamplerBuffer %s;\n",
               is_int ? (is_unsigned ? "u" : "i") : "", name);
  }
  else
  {
    ss_appendf(ss, "Buffer<%s> %s : register(t%u);\n",
               is_int ? (is_unsigned ? "uint4" : "int4") : "float4",
               name, index);
  }
}

void shadergen_declare_image(const shadergen_t* sg, small_string_t* ss, const char* name, u32 index,
                             bool is_float, bool is_int, bool is_unsigned)
{
  if (sg->glsl)
  {
    if (sg->spirv)
    {
      const u32 set = sg->has_uniform_buffer ? 2u : 1u;
      ss_appendf(ss, "layout(set = %u, binding = %u, %s) uniform restrict coherent image2D %s;\n",
                 set, index, is_int ? (is_unsigned ? "rgba8ui" : "rgba8i") : "rgba8", name);
    }
    else
    {
      ss_appendf(ss, "layout(binding = %u, %s) uniform restrict coherent image2D %s;\n",
                 index, is_int ? (is_unsigned ? "rgba8ui" : "rgba8i") : "rgba8", name);
    }
  }
  else
  {
    ss_appendf(ss, "RasterizerOrderedTexture2D<%s> %s : register(u%u);\n",
               is_int ? (is_unsigned ? "uint4" : "int4")
                      : (is_float ? "float4" : "unorm float4"), 
               name, index);
  }
}

const char* shadergen_get_interpolation_qualifier(const shadergen_t* sg, bool interface_block,
                                                  bool centroid_interpolation, bool sample_interpolation,
                                                  bool is_out)
{
  const bool shading_language_420pack = GLAD_GL_ARB_shading_language_420pack;
  if (sg->glsl && interface_block && (!sg->spirv && !shading_language_420pack))
  {
    return sample_interpolation ? (is_out ? "sample out " : "sample in ")
         : (centroid_interpolation ? (is_out ? "centroid out " : "centroid in ") : "");
  }
  return sample_interpolation ? "sample "
       : (centroid_interpolation ? "centroid " : "");
}

void shadergen_declare_vertex_entry_point(const shadergen_t* sg, small_string_t* ss,
                                          const char* const* attributes, u32 attribute_count,
                                          u32 num_color_outputs, u32 num_texcoord_outputs,
                                          const shadergen_io_pair_t* additional_outputs,
                                          u32 additional_output_count,
                                          bool declare_vertex_id, const char* output_block_suffix,
                                          bool msaa, bool ssaa, bool noperspective_color)
{
  if (sg->glsl)
  {
    if (sg->use_glsl_binding_layout)
    {
      for (u32 i = 0; i < attribute_count; ++i)
        ss_appendf(ss, "layout(location = %u) in %s;\n", i, attributes[i]);
    }
    else
    {
      for (u32 i = 0; i < attribute_count; ++i)
        ss_appendf(ss, "in %s;\n", attributes[i]);
    }

    if (sg->use_glsl_interface_blocks)
    {
      const char* qualifier = shadergen_get_interpolation_qualifier(sg, true, msaa, ssaa, true);
      if (sg->spirv) SS_LIT(ss, "layout(location = 0) ");
      ss_appendf(ss, "out VertexData%s {\n", output_block_suffix);
      for (u32 i = 0; i < num_color_outputs; ++i)
        ss_appendf(ss, "  %s%sfloat4 v_col%u;\n", noperspective_color ? "noperspective " : "", qualifier, i);
      for (u32 i = 0; i < num_texcoord_outputs; ++i)
        ss_appendf(ss, "  %sfloat2 v_tex%u;\n", qualifier, i);
      for (u32 i = 0; i < additional_output_count; ++i)
      {
        const shadergen_io_pair_t* p = &additional_outputs[i];
        const char* q = (p->qualifier && p->qualifier[0]) ? p->qualifier : qualifier;
        ss_appendf(ss, "  %s %s;\n", q, p->name);
      }
      SS_LIT(ss, "};\n");
    }
    else
    {
      const char* qualifier = shadergen_get_interpolation_qualifier(sg, false, msaa, ssaa, true);
      u32 location = 0;
      for (u32 i = 0; i < num_color_outputs; ++i)
      {
        if (sg->spirv) ss_appendf(ss, "layout(location = %u) ", location++);
        ss_appendf(ss, "%s%sout float4 v_col%u;\n", qualifier, noperspective_color ? "noperspective " : "", i);
      }
      for (u32 i = 0; i < num_texcoord_outputs; ++i)
      {
        if (sg->spirv) ss_appendf(ss, "layout(location = %u) ", location++);
        ss_appendf(ss, "%sout float2 v_tex%u;\n", qualifier, i);
      }
      for (u32 i = 0; i < additional_output_count; ++i)
      {
        const shadergen_io_pair_t* p = &additional_outputs[i];
        const char* q = (p->qualifier && p->qualifier[0]) ? p->qualifier : qualifier;
        if (sg->spirv) ss_appendf(ss, "layout(location = %u) ", location++);
        ss_appendf(ss, "%s out %s;\n", q, p->name);
      }
    }

    SS_LIT(ss, "#define v_pos gl_Position\n\n");
    if (declare_vertex_id)
    {
      if (sg->spirv) SS_LIT(ss, "#define v_id uint(gl_VertexIndex)\n");
      else           SS_LIT(ss, "#define v_id uint(gl_VertexID)\n");
    }
    SS_LIT(ss, "\n");
    SS_LIT(ss, "void main()\n");
  }
  else
  {
    const char* qualifier = shadergen_get_interpolation_qualifier(sg, false, msaa, ssaa, true);
    SS_LIT(ss, "void main(\n");
    if (declare_vertex_id) SS_LIT(ss, "  in uint v_id : SV_VertexID,\n");

    for (u32 i = 0; i < attribute_count; ++i)
      ss_appendf(ss, "  in %s : ATTR%u,\n", attributes[i], i);
    for (u32 i = 0; i < num_color_outputs; ++i)
      ss_appendf(ss, "  %s%sout float4 v_col%u : COLOR%u,\n",
                 qualifier, noperspective_color ? "noperspective " : "", i, i);
    for (u32 i = 0; i < num_texcoord_outputs; ++i)
      ss_appendf(ss, "  %sout float2 v_tex%u : TEXCOORD%u,\n", qualifier, i, i);

    u32 add_idx = num_texcoord_outputs;
    for (u32 i = 0; i < additional_output_count; ++i)
    {
      const shadergen_io_pair_t* p = &additional_outputs[i];
      const char* q = (p->qualifier && p->qualifier[0]) ? p->qualifier : qualifier;
      ss_appendf(ss, "  %s out %s : TEXCOORD%u,\n", q, p->name, add_idx);
      ++add_idx;
    }
    SS_LIT(ss, "  out float4 v_pos : SV_Position)\n");
  }
}

void shadergen_declare_fragment_entry_point(const shadergen_t* sg, small_string_t* ss,
                                            u32 num_color_inputs, u32 num_texcoord_inputs,
                                            const shadergen_io_pair_t* additional_inputs,
                                            u32 additional_input_count,
                                            bool declare_fragcoord, u32 num_color_outputs,
                                            bool dual_source_output, bool depth_output,
                                            bool msaa, bool ssaa, bool declare_sample_id,
                                            bool noperspective_color, bool feedback_loop, bool rov)
{
  if (sg->glsl)
  {
    if (num_color_inputs > 0 || num_texcoord_inputs > 0 || additional_input_count > 0)
    {
      if (sg->use_glsl_interface_blocks)
      {
        const char* qualifier = shadergen_get_interpolation_qualifier(sg, true, msaa, ssaa, false);
        if (sg->spirv) SS_LIT(ss, "layout(location = 0) ");
        SS_LIT(ss, "in VertexData {\n");
        for (u32 i = 0; i < num_color_inputs; ++i)
          ss_appendf(ss, "  %s%sfloat4 v_col%u;\n", qualifier, noperspective_color ? "noperspective " : "", i);
        for (u32 i = 0; i < num_texcoord_inputs; ++i)
          ss_appendf(ss, "  %sfloat2 v_tex%u;\n", qualifier, i);
        for (u32 i = 0; i < additional_input_count; ++i)
        {
          const shadergen_io_pair_t* p = &additional_inputs[i];
          const char* q = (p->qualifier && p->qualifier[0]) ? p->qualifier : qualifier;
          ss_appendf(ss, "  %s %s;\n", q, p->name);
        }
        SS_LIT(ss, "};\n");
      }
      else
      {
        const char* qualifier = shadergen_get_interpolation_qualifier(sg, false, msaa, ssaa, false);
        u32 location = 0;
        for (u32 i = 0; i < num_color_inputs; ++i)
        {
          if (sg->spirv) ss_appendf(ss, "layout(location = %u) ", location++);
          ss_appendf(ss, "%s%sin float4 v_col%u;\n", qualifier, noperspective_color ? "noperspective " : "", i);
        }
        for (u32 i = 0; i < num_texcoord_inputs; ++i)
        {
          if (sg->spirv) ss_appendf(ss, "layout(location = %u) ", location++);
          ss_appendf(ss, "%sin float2 v_tex%u;\n", qualifier, i);
        }
        for (u32 i = 0; i < additional_input_count; ++i)
        {
          const shadergen_io_pair_t* p = &additional_inputs[i];
          const char* q = (p->qualifier && p->qualifier[0]) ? p->qualifier : qualifier;
          if (sg->spirv) ss_appendf(ss, "layout(location = %u) ", location++);
          ss_appendf(ss, "%s in %s;\n", q, p->name);
        }
      }
    }

    if (declare_fragcoord)   SS_LIT(ss, "#define v_pos gl_FragCoord\n");
    if (declare_sample_id)   SS_LIT(ss, "#define f_sample_index uint(gl_SampleID)\n");
    if (depth_output)        SS_LIT(ss, "#define o_depth gl_FragDepth\n");

    const char* target_0_qualifier = "out";
    if (feedback_loop)
    {
      if (sg->render_api == GPU_RENDER_API_OPENGL || sg->render_api == GPU_RENDER_API_OPENGL_ES)
      {
        if (GLAD_GL_EXT_shader_framebuffer_fetch)
        {
          target_0_qualifier = "inout";
          SS_LIT(ss, "#define LAST_FRAG_COLOR o_col0\n");
        }
        else if (GLAD_GL_ARM_shader_framebuffer_fetch)
        {
          SS_LIT(ss, "#define LAST_FRAG_COLOR gl_LastFragColorARM\n");
        }
      }
    }
    else if (rov)
    {
      SS_LIT(ss, "layout(pixel_interlock_ordered) in;\n");
      SS_LIT(ss, "#define ROV_LOAD(name, coords) imageLoad(name, ivec2(coords))\n");
      SS_LIT(ss, "#define ROV_STORE(name, coords, value) imageStore(name, ivec2(coords), value)\n");
      SS_LIT(ss, "#define BEGIN_ROV_REGION beginInvocationInterlockARB()\n");
      SS_LIT(ss, "#define END_ROV_REGION endInvocationInterlockARB()\n");
    }

    if (sg->use_glsl_binding_layout)
    {
      if (dual_source_output && sg->supports_dual_source_blend && num_color_outputs > 1)
      {
        for (u32 i = 0; i < num_color_outputs; ++i)
          ss_appendf(ss, "layout(location = 0, index = %u) %s float4 o_col%u;\n",
                     i, (i == 0) ? target_0_qualifier : "out", i);
      }
      else
      {
        for (u32 i = 0; i < num_color_outputs; ++i)
          ss_appendf(ss, "layout(location = %u) %s float4 o_col%u;\n",
                     i, (i == 0) ? target_0_qualifier : "out", i);
      }
    }
    else
    {
      for (u32 i = 0; i < num_color_outputs; ++i)
        ss_appendf(ss, "%s float4 o_col%u;\n", (i == 0) ? target_0_qualifier : "out", i);
    }

    SS_LIT(ss, "\n");
    SS_LIT(ss, "void main()\n");
  }
  else
  {
    if (rov)
    {
      SS_LIT(ss, "#define ROV_LOAD(name, coords) name[uint2(coords)]\n");
      SS_LIT(ss, "#define ROV_STORE(name, coords, value) name[uint2(coords)] = value\n");
      SS_LIT(ss, "#define BEGIN_ROV_REGION\n#define END_ROV_REGION\n");
    }

    const char* qualifier = shadergen_get_interpolation_qualifier(sg, false, msaa, ssaa, false);
    SS_LIT(ss, "void main(\n");
    bool first = true;
    for (u32 i = 0; i < num_color_inputs; ++i)
    {
      ss_appendf(ss, "%s  %s%sin float4 v_col%u : COLOR%u",
                 first ? "" : ",\n", qualifier, noperspective_color ? "noperspective " : "", i, i);
      first = false;
    }
    for (u32 i = 0; i < num_texcoord_inputs; ++i)
    {
      ss_appendf(ss, "%s  %sin float2 v_tex%u : TEXCOORD%u",
                 first ? "" : ",\n", qualifier, i, i);
      first = false;
    }
    u32 add_idx = num_texcoord_inputs;
    for (u32 i = 0; i < additional_input_count; ++i)
    {
      const shadergen_io_pair_t* p = &additional_inputs[i];
      const char* q = (p->qualifier && p->qualifier[0]) ? p->qualifier : qualifier;
      ss_appendf(ss, "%s  %s in %s : TEXCOORD%u", first ? "" : ",\n", q, p->name, add_idx);
      ++add_idx; first = false;
    }
    if (declare_fragcoord) { ss_appendf(ss, "%s  in float4 v_pos : SV_Position", first ? "" : ",\n"); first = false; }
    if (declare_sample_id) { ss_appendf(ss, "%s  in uint f_sample_index : SV_SampleIndex", first ? "" : ",\n"); first = false; }
    if (depth_output)      { ss_appendf(ss, "%s  out float o_depth : SV_Depth", first ? "" : ",\n"); first = false; }
    for (u32 i = 0; i < num_color_outputs; ++i)
    {
      ss_appendf(ss, "%s  out float4 o_col%u : SV_Target%u", first ? "" : ",\n", i, i);
      first = false;
    }
    SS_LIT(ss, ")");
  }
  (void)dual_source_output;   /* used in GLSL branch above */
}

static char* take_string(small_string_t* ss)
{
  /* small_string_t buffer is owned heap when on_heap == true; otherwise stack-
   * backed.  Either way, strdup the contents into a freshly-malloc'd copy so
   * the caller's free() pairs with our malloc(). */
  const u32 n = ss->length;
  char* out = (char*)malloc(n + 1);
  if (out)
  {
    if (n > 0) memcpy(out, ss->buffer, n);
    out[n] = '\0';
  }
  small_string_destroy(ss);
  return out;
}

char* shadergen_generate_passthrough_vertex_shader(shadergen_t* sg)
{
  small_string_t ss; small_string_init(&ss);
  shadergen_write_header(sg, &ss, false, false, false);
  static const char* attrs[] = { "float2 a_pos", "float2 a_tex0" };
  shadergen_declare_vertex_entry_point(sg, &ss, attrs, 2, 0, 1, NULL, 0, false, "", false, false, false);
  SS_LIT(&ss,
    "\n"
    "{\n"
    "  v_pos = float4(a_pos, 0.0f, 1.0f);\n"
    "  v_tex0 = a_tex0;\n"
    "  #if API_VULKAN\n"
    "    v_pos.y = -v_pos.y;\n"
    "  #endif\n" 
    "}\n");
  return take_string(&ss);
}

char* shadergen_generate_screen_quad_vertex_shader(shadergen_t* sg, float z)
{
  small_string_t ss; small_string_init(&ss);
  shadergen_write_header(sg, &ss, false, false, false);
  shadergen_declare_vertex_entry_point(sg, &ss, NULL, 0, 0, 1, NULL, 0, true, "", false, false, false);
  SS_LIT(&ss, "{\n");
  SS_LIT(&ss, "  v_tex0 = float2(float((v_id << 1) & 2u), float(v_id & 2u));\n");
  ss_appendf(&ss,
    "  v_pos = float4(v_tex0 * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), %ff, 1.0f);\n", z);
  SS_LIT(&ss,
    "  #if API_OPENGL || API_OPENGL_ES || API_VULKAN\n"
    "    v_pos.y = -v_pos.y;\n"
    "  #endif\n" 
    "}\n");
  return take_string(&ss);
}

char* shadergen_generate_fill_fragment_shader(shadergen_t* sg)
{
  small_string_t ss; small_string_init(&ss);
  shadergen_write_header(sg, &ss, false, false, false);
  static const char* members[] = { "float4 u_fill_color" };
  shadergen_declare_uniform_buffer(sg, &ss, members, 1, true);
  shadergen_declare_fragment_entry_point(sg, &ss, 0, 1, NULL, 0, false, 1, false, false, false, false, false, false, false, false);
  SS_LIT(&ss,
    "\n"
    "{\n"
    "  o_col0 = u_fill_color;\n" 
    "}\n");
  return take_string(&ss);
}

char* shadergen_generate_fill_fragment_shader_fixed(shadergen_t* sg, float r, float g, float b, float a)
{
  small_string_t ss; small_string_init(&ss);
  shadergen_write_header(sg, &ss, false, false, false);
  shadergen_declare_fragment_entry_point(sg, &ss, 0, 0, NULL, 0, false, 1, false, false, false, false, false, false, false, false);
  SS_LIT(&ss, "{\n");
  ss_appendf(&ss, "  o_col0 = float4(%f, %f, %f, %f);\n", r, g, b, a);
  SS_LIT(&ss, "}\n");
  return take_string(&ss);
}

char* shadergen_generate_copy_fragment_shader(shadergen_t* sg, bool offset)
{
  small_string_t ss; small_string_init(&ss);
  shadergen_write_header(sg, &ss, false, false, false);
  if (offset)
  {
    static const char* members[] = { "float4 u_src_rect" };
    shadergen_declare_uniform_buffer(sg, &ss, members, 1, true);
  }
  shadergen_declare_texture(sg, &ss, "samp0", 0, false, false, false);
  shadergen_declare_fragment_entry_point(sg, &ss, 0, 1, NULL, 0, false, 1, false, false, false, false, false, false, false, false);

  if (offset)
  {
    SS_LIT(&ss,
      "\n"
      "{\n"
      "  float2 coords = u_src_rect.xy + v_tex0 * u_src_rect.zw;\n"
      "  o_col0 = SAMPLE_TEXTURE(samp0, coords);\n" 
      "}\n");
  }
  else
  {
    SS_LIT(&ss,
      "\n"
      "{\n"
      "  o_col0 = SAMPLE_TEXTURE(samp0, v_tex0);\n" 
      "}\n");
  }
  return take_string(&ss);
}
