/*
 * NOTE: Some parts of the original file have more permissive licenses (xBR,
 * MMPX, JINC2 shader sources; see SPDX headers in the source).  All
 * of those bodies live in `WriteBatchTextureFilter` which is dropped under
 * #if 0 / TODO here, so we don't actually emit them in this build.
 *
 * `class GPU_HW_ShaderGen final : public ShaderGen` single-impl into free
 * functions taking `gpu_hw_shadergen_t* sg` (which embeds `shadergen_t base`
 * at offset 0).  `std::stringstream ss; ss << ...` becomes `small_string_t ss`
 * + `SS_LIT`/`ss_appendf`.  Result is a malloc'd C string the caller frees.
 *
 * Drops:
 *   - WriteBatchTextureFilter body (sprite-mode Bilinear/JINC2/xBR/MMPX/Scale*)
 *   - GenerateAdaptiveDownsample{Vertex,Mip,Blur,Composite}FragmentShader
 *   - GenerateReplacementMergeFragmentShader
 *   - GenerateVRAMReplacementBlitFragmentShader
 * Each drop is wrapped in `#if 0 ... #endif // TODO` so it's grep-able.
 *
 * The fragment-shader generator's USE_ROV/per_sample_shading conditionals are
 * structurally preserved; we still pass the flags through, the GLSL #if
 * branches still emit; but the consumer (gpu_hw.c) passes false, so the
 * dropped paths simply never execute.
 */

#include "gpu_hw_shadergen.h"

#include "common/assert.h"
#include "common/log.h"

#include "core/gpu_types.h"
#include "core/types.h"

#include <stdarg.h>
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

static char* take_string(small_string_t* ss)
{
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

/* Convenience: forward base shadergen_t API so the call sites read like
 * `WriteHeader(ss)` etc.  These don't add complexity; they're plain
 * forwards that let me preserve the C++ shape. */

static ALWAYS_INLINE void wh(gpu_hw_shadergen_t* sg, small_string_t* ss)
{
  shadergen_write_header(&sg->base, ss, false, false, false);
}
static ALWAYS_INLINE void wh3(gpu_hw_shadergen_t* sg, small_string_t* ss,
                              bool enable_rov, bool enable_fbf, bool enable_dsb)
{
  shadergen_write_header(&sg->base, ss, enable_rov, enable_fbf, enable_dsb);
}
static ALWAYS_INLINE void dm_b(gpu_hw_shadergen_t* sg, small_string_t* ss, const char* n, bool e)
{
  shadergen_define_macro_bool(&sg->base, ss, n, e);
}
static ALWAYS_INLINE void dm_i(gpu_hw_shadergen_t* sg, small_string_t* ss, const char* n, s32 v)
{
  shadergen_define_macro_int(&sg->base, ss, n, v);
}
static ALWAYS_INLINE void dt(gpu_hw_shadergen_t* sg, small_string_t* ss,
                              const char* name, u32 index, bool ms)
{
  shadergen_declare_texture(&sg->base, ss, name, index, ms, false, false);
}
static ALWAYS_INLINE void dt_int(gpu_hw_shadergen_t* sg, small_string_t* ss,
                                  const char* name, u32 index, bool ms, bool is_int, bool is_unsigned)
{
  shadergen_declare_texture(&sg->base, ss, name, index, ms, is_int, is_unsigned);
}
static ALWAYS_INLINE void dtb(gpu_hw_shadergen_t* sg, small_string_t* ss,
                              const char* name, u32 index, bool is_int, bool is_unsigned)
{
  shadergen_declare_texture_buffer(&sg->base, ss, name, index, is_int, is_unsigned);
}
static ALWAYS_INLINE void di(gpu_hw_shadergen_t* sg, small_string_t* ss,
                              const char* name, u32 index, bool is_float)
{
  shadergen_declare_image(&sg->base, ss, name, index, is_float, false, false);
}

static ALWAYS_INLINE void dub(gpu_hw_shadergen_t* sg, small_string_t* ss,
                               const char* const* members, u32 n, bool push_constant)
{
  shadergen_declare_uniform_buffer(&sg->base, ss, members, n, push_constant);
}

/* Wrapper around shadergen_declare_vertex_entry_point/fragment with the most
 * common defaults. */

static void dvep(gpu_hw_shadergen_t* sg, small_string_t* ss,
                 const char* const* attrs, u32 nattrs,
                 u32 num_color_outputs, u32 num_texcoord_outputs,
                 const shadergen_io_pair_t* additional_outputs, u32 additional_output_count,
                 bool declare_vertex_id, const char* output_block_suffix,
                 bool msaa, bool ssaa, bool noperspective_color)
{
  shadergen_declare_vertex_entry_point(&sg->base, ss, attrs, nattrs,
                                        num_color_outputs, num_texcoord_outputs,
                                       additional_outputs, additional_output_count,
                                       declare_vertex_id, output_block_suffix, 
                                       msaa, ssaa, noperspective_color);
}

static void dfep(gpu_hw_shadergen_t* sg, small_string_t* ss,
                 u32 num_color_inputs, u32 num_texcoord_inputs,
                 const shadergen_io_pair_t* additional_inputs, u32 additional_input_count,
                 bool declare_fragcoord, u32 num_color_outputs, bool dual_source_output,
                 bool depth_output, bool msaa, bool ssaa, bool declare_sample_id,
                 bool noperspective_color, bool feedback_loop, bool rov)
{
  shadergen_declare_fragment_entry_point(&sg->base, ss,
                                          num_color_inputs, num_texcoord_inputs,
                                         additional_inputs, additional_input_count,
                                         declare_fragcoord, num_color_outputs,
                                         dual_source_output, depth_output,
                                         msaa, ssaa, declare_sample_id, 
                                         noperspective_color, feedback_loop, rov);
}

void gpu_hw_shadergen_init(gpu_hw_shadergen_t* sg, gpu_render_api_t render_api,
                           bool supports_dual_source_blend, bool supports_framebuffer_fetch)
{
  shadergen_init(&sg->base, render_api,
                 shadergen_get_shader_language_for_api(render_api),
                 supports_dual_source_blend, supports_framebuffer_fetch);
}

void gpu_hw_shadergen_destroy(gpu_hw_shadergen_t* sg)
{
  shadergen_destroy(&sg->base);
}

static void write_color_conversion_functions(gpu_hw_shadergen_t* sg, small_string_t* ss)
{
  (void)sg;
  SS_LIT(ss,
    "\n"
    "uint RGBA8ToRGBA5551(float4 v)\n"
    "{\n"
    "  uint r = uint(roundEven(v.r * 31.0));\n"
    "  uint g = uint(roundEven(v.g * 31.0));\n"
    "  uint b = uint(roundEven(v.b * 31.0));\n"
    "  uint a = (v.a != 0.0) ? 1u : 0u;\n"
    "  return (r) | (g << 5) | (b << 10) | (a << 15);\n"
    "}\n"
    "\n"
    "float4 RGBA5551ToRGBA8(uint v)\n"
    "{\n"
    "  uint r = (v & 31u);\n"
    "  uint g = ((v >> 5) & 31u);\n"
    "  uint b = ((v >> 10) & 31u);\n"
    "  uint a = ((v >> 15) & 1u);\n"
    "\n"
    "  return float4(float(r) / 31.0, float(g) / 31.0, float(b) / 31.0, float(a));\n" 
    "}\n");
}

static void write_batch_uniform_buffer(gpu_hw_shadergen_t* sg, small_string_t* ss)
{
  static const char* members[] = {
    "uint2 u_texture_window_and",
    "uint2 u_texture_window_or",
    "float u_src_alpha_factor",
    "float u_dst_alpha_factor",
    "uint u_interlaced_displayed_field",
    "bool u_set_mask_while_drawing",
    "float u_resolution_scale",
    "float u_rcp_resolution_scale",
    "float u_resolution_scale_minus_one", 
  };
  shadergen_declare_uniform_buffer(&sg->base, ss, members, (u32)(sizeof(members) / sizeof(members[0])), false);
}

static void write_batch_texture_filter(gpu_hw_shadergen_t* sg, small_string_t* ss,
                                       gpu_texture_filter_t texture_filter)
{
  (void)sg;
  /* JINC2 and xBRZ shaders originally from beetle-psx, modified to support
   * filtering mask channel. */
  if (texture_filter == GPU_TEXTURE_FILTER_BILINEAR ||
      texture_filter == GPU_TEXTURE_FILTER_BILINEAR_BIN_ALPHA)
  {
    SS_LIT(ss,
      "\n"
      "void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits,\n"
      "                            out float4 texcol, out float ialpha)\n"
      "{\n"
      "  // Compute the coordinates of the four texels we will be interpolating between.\n"
      "  // Clamp this to the triangle texture coordinates.\n"
      "  float2 texel_top_left = frac(coords) - float2(0.5, 0.5);\n"
      "  float2 texel_offset = sign(texel_top_left);\n"
      "  float4 fcoords = max(coords.xyxy + float4(0.0, 0.0, texel_offset.x, texel_offset.y),\n"
      "                        float4(0.0, 0.0, 0.0, 0.0));\n"
      "\n"
      "  // Load four texels.\n"
      "  float4 s00 = SampleFromVRAM(texpage, fcoords.xy, uv_limits);\n"
      "  float4 s10 = SampleFromVRAM(texpage, fcoords.zy, uv_limits);\n"
      "  float4 s01 = SampleFromVRAM(texpage, fcoords.xw, uv_limits);\n"
      "  float4 s11 = SampleFromVRAM(texpage, fcoords.zw, uv_limits);\n"
      "\n"
      "  // Compute alpha from how many texels aren't pixel color 0000h.\n"
      "  float a00 = float(VECTOR_NEQ(s00, TRANSPARENT_PIXEL_COLOR));\n"
      "  float a10 = float(VECTOR_NEQ(s10, TRANSPARENT_PIXEL_COLOR));\n"
      "  float a01 = float(VECTOR_NEQ(s01, TRANSPARENT_PIXEL_COLOR));\n"
      "  float a11 = float(VECTOR_NEQ(s11, TRANSPARENT_PIXEL_COLOR));\n"
      "\n"
      "  // Bilinearly interpolate.\n"
      "  float2 weights = abs(texel_top_left);\n"
      "  texcol = lerp(lerp(s00, s10, weights.x), lerp(s01, s11, weights.x), weights.y);\n"
      "  ialpha = lerp(lerp(a00, a10, weights.x), lerp(a01, a11, weights.x), weights.y);\n"
      "\n"
      "  // Compensate for partially transparent sampling.\n"
      "  if (ialpha > 0.0)\n"
      "    texcol.rgb /= float3(ialpha, ialpha, ialpha);\n"
      "\n"
      "#if !TEXTURE_ALPHA_BLENDING\n"
      "  ialpha = (ialpha >= 0.5) ? 1.0 : 0.0;\n"
      "#endif\n"
      "}\n");
  }
  else if (texture_filter == GPU_TEXTURE_FILTER_JINC2 ||
           texture_filter == GPU_TEXTURE_FILTER_JINC2_BIN_ALPHA)
  {
    SS_LIT(ss,
      "CONSTANT float JINC2_WINDOW_SINC = 0.44;\n"
      "CONSTANT float JINC2_SINC = 0.82;\n"
      "CONSTANT float JINC2_AR_STRENGTH = 0.8;\n"
      "\n"
      "CONSTANT   float halfpi            = 1.5707963267948966192313216916398;\n"
      "CONSTANT   float pi                = 3.1415926535897932384626433832795;\n"
      "CONSTANT   float wa                = 1.382300768;\n"
      "CONSTANT   float wb                = 2.576105976;\n"
      "\n"
      "// Calculates the distance between two points\n"
      "float d(float2 pt1, float2 pt2)\n"
      "{\n"
      "  float2 v = pt2 - pt1;\n"
      "  return sqrt(dot(v,v));\n"
      "}\n"
      "\n"
      "float min4(float a, float b, float c, float d)\n"
      "{\n"
      "    return min(a, min(b, min(c, d)));\n"
      "}\n"
      "\n"
      "float4 min4(float4 a, float4 b, float4 c, float4 d)\n"
      "{\n"
      "    return min(a, min(b, min(c, d)));\n"
      "}\n"
      "\n"
      "float max4(float a, float b, float c, float d)\n"
      "{\n"
      "  return max(a, max(b, max(c, d)));\n"
      "}\n"
      "\n"
      "float4 max4(float4 a, float4 b, float4 c, float4 d)\n"
      "{\n"
      "    return max(a, max(b, max(c, d)));\n"
      "}\n"
      "\n"
      "float4 resampler(float4 x)\n"
      "{\n"
      "   float4 res;\n"
      "\n"
      "   // res = (x==float4(0.0, 0.0, 0.0, 0.0)) ?  float4(wa*wb)  :  sin(x*wa)*sin(x*wb)/(x*x);\n"
      "   // Need to use mix(.., equal(..)) since we want zero check to be component wise\n"
      "   float4 a = sin(x * wa) * sin(x * wb) / (x * x);\n"
      "   float4 b = float4(wa*wb, wa*wb, wa*wb, wa*wb);\n"
      "   bool4 s = VECTOR_COMP_EQ(x, float4(0.0, 0.0, 0.0, 0.0));\n"
      "   return float4(s.x ? b.x : a.x, s.y ? b.y : a.y, s.z ? b.z : a.z, s.w ? b.w : a.w);\n"
      "}\n"
      "\n"
      "void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits,\n"
      "                            out float4 texcol, out float ialpha)\n"
      "{\n"
      "    float4 weights[4];\n"
      "\n"
      "    float2 dx = float2(1.0, 0.0);\n"
      "    float2 dy = float2(0.0, 1.0);\n"
      "\n"
      "    float2 pc = coords.xy;\n"
      "\n"
      "    float2 tc = (floor(pc-float2(0.5,0.5))+float2(0.5,0.5));\n"
      "\n"
      "    weights[0] = resampler(float4(d(pc, tc    -dx    -dy), d(pc, tc           -dy), d(pc, tc    +dx    -dy), d(pc, tc+2.0*dx    -dy)));\n"
      "    weights[1] = resampler(float4(d(pc, tc    -dx       ), d(pc, tc              ), d(pc, tc    +dx       ), d(pc, tc+2.0*dx       )));\n"
      "    weights[2] = resampler(float4(d(pc, tc    -dx    +dy), d(pc, tc           +dy), d(pc, tc    +dx    +dy), d(pc, tc+2.0*dx    +dy)));\n"
      "    weights[3] = resampler(float4(d(pc, tc    -dx+2.0*dy), d(pc, tc       +2.0*dy), d(pc, tc    +dx+2.0*dy), d(pc, tc+2.0*dx+2.0*dy)));\n"
      "\n"
      "    dx = dx;\n"
      "    dy = dy;\n"
      "    tc = tc;\n"
      "\n"
      "#define sample_texel(coords) SampleFromVRAM(texpage, (coords), uv_limits)\n"
      "\n"
      "    float4 c00 = sample_texel(tc    -dx    -dy);\n"
      "    float a00 = float(VECTOR_NEQ(c00, TRANSPARENT_PIXEL_COLOR));\n"
      "    float4 c10 = sample_texel(tc           -dy);\n"
      "    float a10 = float(VECTOR_NEQ(c10, TRANSPARENT_PIXEL_COLOR));\n"
      "    float4 c20 = sample_texel(tc    +dx    -dy);\n"
      "    float a20 = float(VECTOR_NEQ(c20, TRANSPARENT_PIXEL_COLOR));\n"
      "    float4 c30 = sample_texel(tc+2.0*dx    -dy);\n"
      "    float a30 = float(VECTOR_NEQ(c30, TRANSPARENT_PIXEL_COLOR));\n"
      "    float4 c01 = sample_texel(tc    -dx       );\n"
      "    float a01 = float(VECTOR_NEQ(c01, TRANSPARENT_PIXEL_COLOR));\n"
      "    float4 c11 = sample_texel(tc              );\n"
      "    float a11 = float(VECTOR_NEQ(c11, TRANSPARENT_PIXEL_COLOR));\n"
      "    float4 c21 = sample_texel(tc    +dx       );\n"
      "    float a21 = float(VECTOR_NEQ(c21, TRANSPARENT_PIXEL_COLOR));\n"
      "    float4 c31 = sample_texel(tc+2.0*dx       );\n"
      "    float a31 = float(VECTOR_NEQ(c31, TRANSPARENT_PIXEL_COLOR));\n"
      "    float4 c02 = sample_texel(tc    -dx    +dy);\n"
      "    float a02 = float(VECTOR_NEQ(c02, TRANSPARENT_PIXEL_COLOR));\n"
      "    float4 c12 = sample_texel(tc           +dy);\n"
      "    float a12 = float(VECTOR_NEQ(c12, TRANSPARENT_PIXEL_COLOR));\n"
      "    float4 c22 = sample_texel(tc    +dx    +dy);\n"
      "    float a22 = float(VECTOR_NEQ(c22, TRANSPARENT_PIXEL_COLOR));\n"
      "    float4 c32 = sample_texel(tc+2.0*dx    +dy);\n"
      "    float a32 = float(VECTOR_NEQ(c32, TRANSPARENT_PIXEL_COLOR));\n"
      "    float4 c03 = sample_texel(tc    -dx+2.0*dy);\n"
      "    float a03 = float(VECTOR_NEQ(c03, TRANSPARENT_PIXEL_COLOR));\n"
      "    float4 c13 = sample_texel(tc       +2.0*dy);\n"
      "    float a13 = float(VECTOR_NEQ(c13, TRANSPARENT_PIXEL_COLOR));\n"
      "    float4 c23 = sample_texel(tc    +dx+2.0*dy);\n"
      "    float a23 = float(VECTOR_NEQ(c23, TRANSPARENT_PIXEL_COLOR));\n"
      "    float4 c33 = sample_texel(tc+2.0*dx+2.0*dy);\n"
      "    float a33 = float(VECTOR_NEQ(c33, TRANSPARENT_PIXEL_COLOR));\n"
      "\n"
      "#undef sample_texel\n"
      "\n"
      "    //  Get min/max samples\n"
      "    float4 min_sample = min4(c11, c21, c12, c22);\n"
      "    float min_sample_alpha = min4(a11, a21, a12, a22);\n"
      "    float4 max_sample = max4(c11, c21, c12, c22);\n"
      "    float max_sample_alpha = max4(a11, a21, a12, a22);\n"
      "\n"
      "    float4 color;\n"
      "    color = float4(dot(weights[0], float4(c00.x, c10.x, c20.x, c30.x)), dot(weights[0], float4(c00.y, c10.y, c20.y, c30.y)), dot(weights[0], float4(c00.z, c10.z, c20.z, c30.z)), dot(weights[0], float4(c00.w, c10.w, c20.w, c30.w)));\n"
      "    color+= float4(dot(weights[1], float4(c01.x, c11.x, c21.x, c31.x)), dot(weights[1], float4(c01.y, c11.y, c21.y, c31.y)), dot(weights[1], float4(c01.z, c11.z, c21.z, c31.z)), dot(weights[1], float4(c01.w, c11.w, c21.w, c31.w)));\n"
      "    color+= float4(dot(weights[2], float4(c02.x, c12.x, c22.x, c32.x)), dot(weights[2], float4(c02.y, c12.y, c22.y, c32.y)), dot(weights[2], float4(c02.z, c12.z, c22.z, c32.z)), dot(weights[2], float4(c02.w, c12.w, c22.w, c32.w)));\n"
      "    color+= float4(dot(weights[3], float4(c03.x, c13.x, c23.x, c33.x)), dot(weights[3], float4(c03.y, c13.y, c23.y, c33.y)), dot(weights[3], float4(c03.z, c13.z, c23.z, c33.z)), dot(weights[3], float4(c03.w, c13.w, c23.w, c33.w)));\n"
      "    color = color/(dot(weights[0], float4(1,1,1,1)) + dot(weights[1], float4(1,1,1,1)) + dot(weights[2], float4(1,1,1,1)) + dot(weights[3], float4(1,1,1,1)));\n"
      "\n"
      "    float alpha;\n"
      "    alpha = dot(weights[0], float4(a00, a10, a20, a30));\n"
      "    alpha+= dot(weights[1], float4(a01, a11, a21, a31));\n"
      "    alpha+= dot(weights[2], float4(a02, a12, a22, a32));\n"
      "    alpha+= dot(weights[3], float4(a03, a13, a23, a33));\n"
      "    //alpha = alpha/(weights[0].w + weights[1].w + weights[2].w + weights[3].w);\n"
      "    alpha = alpha/(dot(weights[0], float4(1,1,1,1)) + dot(weights[1], float4(1,1,1,1)) + dot(weights[2], float4(1,1,1,1)) + dot(weights[3], float4(1,1,1,1)));\n"
      "\n"
      "    // Anti-ringing\n"
      "    float4 aux = color;\n"
      "    float aux_alpha = alpha;\n"
      "    color = clamp(color, min_sample, max_sample);\n"
      "    alpha = clamp(alpha, min_sample_alpha, max_sample_alpha);\n"
      "    color = lerp(aux, color, JINC2_AR_STRENGTH);\n"
      "    alpha = lerp(aux_alpha, alpha, JINC2_AR_STRENGTH);\n"
      "\n"
      "    // final sum and weight normalization\n"
      "    ialpha = alpha;\n"
      "    texcol = color;\n"
      "\n"
      "    // Compensate for partially transparent sampling.\n"
      "    if (ialpha > 0.0)\n"
      "      texcol.rgb /= float3(ialpha, ialpha, ialpha);\n"
      "\n"
      "#if !TEXTURE_ALPHA_BLENDING\n"
      "  ialpha = (ialpha >= 0.5) ? 1.0 : 0.0;\n"
      "#endif\n"
      "}\n"
    );
  }
  else if (texture_filter == GPU_TEXTURE_FILTER_XBR ||
           texture_filter == GPU_TEXTURE_FILTER_XBR_BIN_ALPHA)
  {
    SS_LIT(ss,
      "CONSTANT int BLEND_NONE = 0;\n"
      "CONSTANT int BLEND_NORMAL = 1;\n"
      "CONSTANT int BLEND_DOMINANT = 2;\n"
      "CONSTANT float LUMINANCE_WEIGHT = 1.0;\n"
      "CONSTANT float EQUAL_COLOR_TOLERANCE = 0.1176470588235294;\n"
      "CONSTANT float STEEP_DIRECTION_THRESHOLD = 2.2;\n"
      "CONSTANT float DOMINANT_DIRECTION_THRESHOLD = 3.6;\n"
      "CONSTANT float4 w = float4(0.2627, 0.6780, 0.0593, 0.5);\n"
      "\n"
      "float DistYCbCr(float4 pixA, float4 pixB)\n"
      "{\n"
      "  const float scaleB = 0.5 / (1.0 - w.b);\n"
      "  const float scaleR = 0.5 / (1.0 - w.r);\n"
      "  float4 diff = pixA - pixB;\n"
      "  float Y = dot(diff, w);\n"
      "  float Cb = scaleB * (diff.b - Y);\n"
      "  float Cr = scaleR * (diff.r - Y);\n"
      "\n"
      "  return sqrt(((LUMINANCE_WEIGHT * Y) * (LUMINANCE_WEIGHT * Y)) + (Cb * Cb) + (Cr * Cr));\n"
      "}\n"
      "\n"
      "bool IsPixEqual(const float4 pixA, const float4 pixB)\n"
      "{\n"
      "  return (DistYCbCr(pixA, pixB) < EQUAL_COLOR_TOLERANCE);\n"
      "}\n"
      "\n"
      "float get_left_ratio(float2 center, float2 origin, float2 direction, float2 scale)\n"
      "{\n"
      "  float2 P0 = center - origin;\n"
      "  float2 proj = direction * (dot(P0, direction) / dot(direction, direction));\n"
      "  float2 distv = P0 - proj;\n"
      "  float2 orth = float2(-direction.y, direction.x);\n"
      "  float side = sign(dot(P0, orth));\n"
      "  float v = side * length(distv * scale);\n"
      "\n"
      "//  return step(0, v);\n"
      "  return smoothstep(-sqrt(2.0)/2.0, sqrt(2.0)/2.0, v);\n"
      "}\n"
      "\n"
      "#define P(coord, xoffs, yoffs) SampleFromVRAM(texpage, coords + float2((xoffs), (yoffs)), uv_limits)\n"
      "\n"
      "void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits,\n"
      "                            out float4 texcol, out float ialpha)\n"
      "{\n"
      "  //---------------------------------------\n"
      "  // Input Pixel Mapping:  -|x|x|x|-\n"
      "  //                       x|A|B|C|x\n"
      "  //                       x|D|E|F|x\n"
      "  //                       x|G|H|I|x\n"
      "  //                       -|x|x|x|-\n"
      "\n"
      "  float2 scale = float2(8.0, 8.0);\n"
      "  float2 pos = frac(coords.xy) - float2(0.5, 0.5);\n"
      "  float2 coord = coords.xy - pos;\n"
      "\n"
      "  float4 A = P(coord, -1,-1);\n"
      "  float Aw = A.w;\n"
      "  A.w = float(VECTOR_NEQ(A, TRANSPARENT_PIXEL_COLOR));\n"
      "  float4 B = P(coord,  0,-1);\n"
      "  float Bw = B.w;\n"
      "  B.w = float(VECTOR_NEQ(B, TRANSPARENT_PIXEL_COLOR));\n"
      "  float4 C = P(coord,  1,-1);\n"
      "  float Cw = C.w;\n"
      "  C.w = float(VECTOR_NEQ(C, TRANSPARENT_PIXEL_COLOR));\n"
      "  float4 D = P(coord, -1, 0);\n"
      "  float Dw = D.w;\n"
      "  D.w = float(VECTOR_NEQ(D, TRANSPARENT_PIXEL_COLOR));\n"
      "  float4 E = P(coord, 0, 0);\n"
      "  float Ew = E.w;\n"
      "  E.w = float(VECTOR_NEQ(E, TRANSPARENT_PIXEL_COLOR));\n"
      "  float4 F = P(coord,  1, 0);\n"
      "  float Fw = F.w;\n"
      "  F.w = float(VECTOR_NEQ(F, TRANSPARENT_PIXEL_COLOR));\n"
      "  float4 G = P(coord, -1, 1);\n"
      "  float Gw = G.w;\n"
      "  G.w = float(VECTOR_NEQ(G, TRANSPARENT_PIXEL_COLOR));\n"
      "  float4 H = P(coord,  0, 1);\n"
      "  float Hw = H.w;\n"
      "  H.w = float(VECTOR_NEQ(H, TRANSPARENT_PIXEL_COLOR));\n"
      "  float4 I = P(coord,  1, 1);\n"
      "  float Iw = I.w;\n"
      "  I.w = float(VECTOR_NEQ(H, TRANSPARENT_PIXEL_COLOR));\n"
      "\n"
      "  // blendResult Mapping: x|y|\n"
      "  //                      w|z|\n"
      "  int4 blendResult = int4(BLEND_NONE,BLEND_NONE,BLEND_NONE,BLEND_NONE);\n"
      "\n"
      "  // Preprocess corners\n"
      "  // Pixel Tap Mapping: -|-|-|-|-\n"
      "  //                    -|-|B|C|-\n"
      "  //                    -|D|E|F|x\n"
      "  //                    -|G|H|I|x\n"
      "  //                    -|-|x|x|-\n"
      "  if (!((VECTOR_EQ(E,F) && VECTOR_EQ(H,I)) || (VECTOR_EQ(E,H) && VECTOR_EQ(F,I))))\n"
      "  {\n"
      "    float dist_H_F = DistYCbCr(G, E) + DistYCbCr(E, C) + DistYCbCr(P(coord, 0,2), I) + DistYCbCr(I, P(coord, 2,0)) + (4.0 * DistYCbCr(H, F));\n"
      "    float dist_E_I = DistYCbCr(D, H) + DistYCbCr(H, P(coord, 1,2)) + DistYCbCr(B, F) + DistYCbCr(F, P(coord, 2,1)) + (4.0 * DistYCbCr(E, I));\n"
      "    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_H_F) < dist_E_I;\n"
      "    blendResult.z = ((dist_H_F < dist_E_I) && VECTOR_NEQ(E,F) && VECTOR_NEQ(E,H)) ? ((dominantGradient) ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;\n"
      "  }\n"
      "\n"
      "\n"
      "  // Pixel Tap Mapping: -|-|-|-|-\n"
      "  //                    -|A|B|-|-\n"
      "  //                    x|D|E|F|-\n"
      "  //                    x|G|H|I|-\n"
      "  //                    -|x|x|-|-\n"
      "  if (!((VECTOR_EQ(D,E) && VECTOR_EQ(G,H)) || (VECTOR_EQ(D,G) && VECTOR_EQ(E,H))))\n"
      "  {\n"
      "    float dist_G_E = DistYCbCr(P(coord, -2,1)  , D) + DistYCbCr(D, B) + DistYCbCr(P(coord, -1,2), H) + DistYCbCr(H, F) + (4.0 * DistYCbCr(G, E));\n"
      "    float dist_D_H = DistYCbCr(P(coord, -2,0)  , G) + DistYCbCr(G, P(coord, 0,2)) + DistYCbCr(A, E) + DistYCbCr(E, I) + (4.0 * DistYCbCr(D, H));\n"
      "    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_D_H) < dist_G_E;\n"
      "    blendResult.w = ((dist_G_E > dist_D_H) && VECTOR_NEQ(E,D) && VECTOR_NEQ(E,H)) ? ((dominantGradient) ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;\n"
      "  }\n"
      "\n"
      "  // Pixel Tap Mapping: -|-|x|x|-\n"
      "  //                    -|A|B|C|x\n"
      "  //                    -|D|E|F|x\n"
      "  //                    -|-|H|I|-\n"
      "  //                    -|-|-|-|-\n"
      "  if (!((VECTOR_EQ(B,C) && VECTOR_EQ(E,F)) || (VECTOR_EQ(B,E) && VECTOR_EQ(C,F))))\n"
      "  {\n"
      "    float dist_E_C = DistYCbCr(D, B) + DistYCbCr(B, P(coord, 1,-2)) + DistYCbCr(H, F) + DistYCbCr(F, P(coord, 2,-1)) + (4.0 * DistYCbCr(E, C));\n"
      "    float dist_B_F = DistYCbCr(A, E) + DistYCbCr(E, I) + DistYCbCr(P(coord, 0,-2), C) + DistYCbCr(C, P(coord, 2,0)) + (4.0 * DistYCbCr(B, F));\n"
      "    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_B_F) < dist_E_C;\n"
      "    blendResult.y = ((dist_E_C > dist_B_F) && VECTOR_NEQ(E,B) && VECTOR_NEQ(E,F)) ? ((dominantGradient) ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;\n"
      "  }\n"
      "\n"
      "  // Pixel Tap Mapping: -|x|x|-|-\n"
      "  //                    x|A|B|C|-\n"
      "  //                    x|D|E|F|-\n"
      "  //                    -|G|H|-|-\n"
      "  //                    -|-|-|-|-\n"
      "  if (!((VECTOR_EQ(A,B) && VECTOR_EQ(D,E)) || (VECTOR_EQ(A,D) && VECTOR_EQ(B,E))))\n"
      "  {\n"
      "    float dist_D_B = DistYCbCr(P(coord, -2,0), A) + DistYCbCr(A, P(coord, 0,-2)) + DistYCbCr(G, E) + DistYCbCr(E, C) + (4.0 * DistYCbCr(D, B));\n"
      "    float dist_A_E = DistYCbCr(P(coord, -2,-1), D) + DistYCbCr(D, H) + DistYCbCr(P(coord, -1,-2), B) + DistYCbCr(B, F) + (4.0 * DistYCbCr(A, E));\n"
      "    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_D_B) < dist_A_E;\n"
      "    blendResult.x = ((dist_D_B < dist_A_E) && VECTOR_NEQ(E,D) && VECTOR_NEQ(E,B)) ? ((dominantGradient) ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;\n"
      "  }\n"
      "\n"
      "  float4 res = E;\n"
      "  float resW = Ew;\n"
      "\n"
      "  // Pixel Tap Mapping: -|-|-|-|-\n"
      "  //                    -|-|B|C|-\n"
      "  //                    -|D|E|F|x\n"
      "  //                    -|G|H|I|x\n"
      "  //                    -|-|x|x|-\n"
      "  if(blendResult.z != BLEND_NONE)\n"
      "  {\n"
      "    float dist_F_G = DistYCbCr(F, G);\n"
      "    float dist_H_C = DistYCbCr(H, C);\n"
      "    bool doLineBlend = (blendResult.z == BLEND_DOMINANT ||\n"
      "                !((blendResult.y != BLEND_NONE && !IsPixEqual(E, G)) || (blendResult.w != BLEND_NONE && !IsPixEqual(E, C)) ||\n"
      "                  (IsPixEqual(G, H) && IsPixEqual(H, I) && IsPixEqual(I, F) && IsPixEqual(F, C) && !IsPixEqual(E, I))));\n"
      "\n"
      "    float2 origin = float2(0.0, 1.0 / sqrt(2.0));\n"
      "    float2 direction = float2(1.0, -1.0);\n"
      "    if(doLineBlend)\n"
      "    {\n"
      "      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_F_G <= dist_H_C) && VECTOR_NEQ(E,G) && VECTOR_NEQ(D,G);\n"
      "      bool haveSteepLine = (STEEP_DIRECTION_THRESHOLD * dist_H_C <= dist_F_G) && VECTOR_NEQ(E,C) && VECTOR_NEQ(B,C);\n"
      "      origin = haveShallowLine? float2(0.0, 0.25) : float2(0.0, 0.5);\n"
      "      direction.x += haveShallowLine? 1.0: 0.0;\n"
      "      direction.y -= haveSteepLine? 1.0: 0.0;\n"
      "    }\n"
      "\n"
      "    float4 blendPix = lerp(H,F, step(DistYCbCr(E, F), DistYCbCr(E, H)));\n"
      "    float blendW = lerp(Hw,Fw, step(DistYCbCr(E, F), DistYCbCr(E, H)));\n"
      "    res = lerp(res, blendPix, get_left_ratio(pos, origin, direction, scale));\n"
      "    resW = lerp(resW, blendW, get_left_ratio(pos, origin, direction, scale));\n"
      "  }\n"
      "\n"
      "  // Pixel Tap Mapping: -|-|-|-|-\n"
      "  //                    -|A|B|-|-\n"
      "  //                    x|D|E|F|-\n"
      "  //                    x|G|H|I|-\n"
      "  //                    -|x|x|-|-\n"
      "  if(blendResult.w != BLEND_NONE)\n"
      "  {\n"
      "    float dist_H_A = DistYCbCr(H, A);\n"
      "    float dist_D_I = DistYCbCr(D, I);\n"
      "    bool doLineBlend = (blendResult.w == BLEND_DOMINANT ||\n"
      "                !((blendResult.z != BLEND_NONE && !IsPixEqual(E, A)) || (blendResult.x != BLEND_NONE && !IsPixEqual(E, I)) ||\n"
      "                  (IsPixEqual(A, D) && IsPixEqual(D, G) && IsPixEqual(G, H) && IsPixEqual(H, I) && !IsPixEqual(E, G))));\n"
      "\n"
      "    float2 origin = float2(-1.0 / sqrt(2.0), 0.0);\n"
      "    float2 direction = float2(1.0, 1.0);\n"
      "    if(doLineBlend)\n"
      "    {\n"
      "      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_H_A <= dist_D_I) && VECTOR_NEQ(E,A) && VECTOR_NEQ(B,A);\n"
      "      bool haveSteepLine  = (STEEP_DIRECTION_THRESHOLD * dist_D_I <= dist_H_A) && VECTOR_NEQ(E,I) && VECTOR_NEQ(F,I);\n"
      "      origin = haveShallowLine? float2(-0.25, 0.0) : float2(-0.5, 0.0);\n"
      "      direction.y += haveShallowLine? 1.0: 0.0;\n"
      "      direction.x += haveSteepLine? 1.0: 0.0;\n"
      "    }\n"
      "    origin = origin;\n"
      "    direction = direction;\n"
      "\n"
      "    float4 blendPix = lerp(H,D, step(DistYCbCr(E, D), DistYCbCr(E, H)));\n"
      "    float blendW = lerp(Hw,Dw, step(DistYCbCr(E, D), DistYCbCr(E, H)));\n"
      "    res = lerp(res, blendPix, get_left_ratio(pos, origin, direction, scale));\n"
      "    resW = lerp(resW, blendW, get_left_ratio(pos, origin, direction, scale));\n"
      "  }\n"
      "\n"
      "  // Pixel Tap Mapping: -|-|x|x|-\n"
      "  //                    -|A|B|C|x\n"
      "  //                    -|D|E|F|x\n"
      "  //                    -|-|H|I|-\n"
      "  //                    -|-|-|-|-\n"
      "  if(blendResult.y != BLEND_NONE)\n"
      "  {\n"
      "    float dist_B_I = DistYCbCr(B, I);\n"
      "    float dist_F_A = DistYCbCr(F, A);\n"
      "    bool doLineBlend = (blendResult.y == BLEND_DOMINANT ||\n"
      "                !((blendResult.x != BLEND_NONE && !IsPixEqual(E, I)) || (blendResult.z != BLEND_NONE && !IsPixEqual(E, A)) ||\n"
      "                  (IsPixEqual(I, F) && IsPixEqual(F, C) && IsPixEqual(C, B) && IsPixEqual(B, A) && !IsPixEqual(E, C))));\n"
      "\n"
      "    float2 origin = float2(1.0 / sqrt(2.0), 0.0);\n"
      "    float2 direction = float2(-1.0, -1.0);\n"
      "\n"
      "    if(doLineBlend)\n"
      "    {\n"
      "      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_B_I <= dist_F_A) && VECTOR_NEQ(E,I) && VECTOR_NEQ(H,I);\n"
      "      bool haveSteepLine  = (STEEP_DIRECTION_THRESHOLD * dist_F_A <= dist_B_I) && VECTOR_NEQ(E,A) && VECTOR_NEQ(D,A);\n"
      "      origin = haveShallowLine? float2(0.25, 0.0) : float2(0.5, 0.0);\n"
      "      direction.y -= haveShallowLine? 1.0: 0.0;\n"
      "      direction.x -= haveSteepLine? 1.0: 0.0;\n"
      "    }\n"
      "\n"
      "    float4 blendPix = lerp(F,B, step(DistYCbCr(E, B), DistYCbCr(E, F)));\n"
      "    float blendW = lerp(Fw,Bw, step(DistYCbCr(E, B), DistYCbCr(E, F)));\n"
      "    res = lerp(res, blendPix, get_left_ratio(pos, origin, direction, scale));\n"
      "    resW = lerp(resW, blendW, get_left_ratio(pos, origin, direction, scale));\n"
      "  }\n"
      "\n"
      "  // Pixel Tap Mapping: -|x|x|-|-\n"
      "  //                    x|A|B|C|-\n"
      "  //                    x|D|E|F|-\n"
      "  //                    -|G|H|-|-\n"
      "  //                    -|-|-|-|-\n"
      "  if(blendResult.x != BLEND_NONE)\n"
      "  {\n"
      "    float dist_D_C = DistYCbCr(D, C);\n"
      "    float dist_B_G = DistYCbCr(B, G);\n"
      "    bool doLineBlend = (blendResult.x == BLEND_DOMINANT ||\n"
      "                !((blendResult.w != BLEND_NONE && !IsPixEqual(E, C)) || (blendResult.y != BLEND_NONE && !IsPixEqual(E, G)) ||\n"
      "                  (IsPixEqual(C, B) && IsPixEqual(B, A) && IsPixEqual(A, D) && IsPixEqual(D, G) && !IsPixEqual(E, A))));\n"
      "\n"
      "    float2 origin = float2(0.0, -1.0 / sqrt(2.0));\n"
      "    float2 direction = float2(-1.0, 1.0);\n"
      "    if(doLineBlend)\n"
      "    {\n"
      "      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_D_C <= dist_B_G) && VECTOR_NEQ(E,C) && VECTOR_NEQ(F,C);\n"
      "      bool haveSteepLine  = (STEEP_DIRECTION_THRESHOLD * dist_B_G <= dist_D_C) && VECTOR_NEQ(E,G) && VECTOR_NEQ(H,G);\n"
      "      origin = haveShallowLine? float2(0.0, -0.25) : float2(0.0, -0.5);\n"
      "      direction.x -= haveShallowLine? 1.0: 0.0;\n"
      "      direction.y += haveSteepLine? 1.0: 0.0;\n"
      "    }\n"
      "\n"
      "    float4 blendPix = lerp(D,B, step(DistYCbCr(E, B), DistYCbCr(E, D)));\n"
      "    float blendW = lerp(Dw,Bw, step(DistYCbCr(E, B), DistYCbCr(E, D)));\n"
      "    res = lerp(res, blendPix, get_left_ratio(pos, origin, direction, scale));\n"
      "    resW = lerp(resW, blendW, get_left_ratio(pos, origin, direction, scale));\n"
      "  }\n"
      "\n"
      "  ialpha = res.w;\n"
      "  texcol = float4(res.xyz, resW);\n"
      "\n"
      "  // Compensate for partially transparent sampling.\n"
      "  if (ialpha > 0.0)\n"
      "    texcol.rgb /= float3(ialpha, ialpha, ialpha);\n"
      "\n"
      "#if !TEXTURE_ALPHA_BLENDING\n"
      "  ialpha = (ialpha >= 0.5) ? 1.0 : 0.0;\n"
      "#endif\n"
      "}\n"
      "\n"
      "#undef P\n"
      "\n"
    );
  }
  else if (texture_filter == GPU_TEXTURE_FILTER_MMPX)
  {
    SS_LIT(ss,
      "#define src(xoffs, yoffs) packUnorm4x8(SampleFromVRAM(texpage, bcoords + float2((xoffs), (yoffs)), uv_limits))\n"
    );
    SS_LIT(ss,
      "uint luma(uint C) {\n"
      "    uint alpha = (C & 0xFF000000u) >> 24;\n"
      "    return (((C & 0x00FF0000u) >> 16) + ((C & 0x0000FF00u) >> 8) + (C & 0x000000FFu) + 1u) * (256u - alpha);\n"
      "}\n"
      "\n"
      "bool all_eq2(uint B, uint A0, uint A1) {\n"
      "    return ((B ^ A0) | (B ^ A1)) == 0u;\n"
      "}\n"
      "\n"
      "bool all_eq3(uint B, uint A0, uint A1, uint A2) {\n"
      "    return ((B ^ A0) | (B ^ A1) | (B ^ A2)) == 0u;\n"
      "}\n"
      "\n"
      "bool all_eq4(uint B, uint A0, uint A1, uint A2, uint A3) {\n"
      "    return ((B ^ A0) | (B ^ A1) | (B ^ A2) | (B ^ A3)) == 0u;\n"
      "}\n"
      "\n"
      "bool any_eq3(uint B, uint A0, uint A1, uint A2) {\n"
      "    return B == A0 || B == A1 || B == A2;\n"
      "}\n"
      "\n"
      "bool none_eq2(uint B, uint A0, uint A1) {\n"
      "    return (B != A0) && (B != A1);\n"
      "}\n"
      "\n"
      "bool none_eq4(uint B, uint A0, uint A1, uint A2, uint A3) {\n"
      "    return B != A0 && B != A1 && B != A2 && B != A3;\n"
      "}\n"
      "\n"
      "void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits, out float4 texcol, out float ialpha)\n"
      "{\n"
      "  float2 bcoords = floor(coords);\n"
      "\n"
      "  uint A = src(-1, -1), B = src(+0, -1), C = src(+1, -1);\n"
      "  uint D = src(-1, +0), E = src(+0, +0), F = src(+1, +0);\n"
      "  uint G = src(-1, +1), H = src(+0, +1), I = src(+1, +1);\n"
      "\n"
      "  uint J = E, K = E, L = E, M = E;\n"
      "\n"
      "  if (((A ^ E) | (B ^ E) | (C ^ E) | (D ^ E) | (F ^ E) | (G ^ E) | (H ^ E) | (I ^ E)) != 0u) {\n"
      "    uint P = src(+0, -2), S = src(+0, +2);\n"
      "    uint Q = src(-2, +0), R = src(+2, +0);\n"
      "    uint Bl = luma(B), Dl = luma(D), El = luma(E), Fl = luma(F), Hl = luma(H);\n"
      "\n"
      "    // 1:1 slope rules\n"
      "    if ((D == B && D != H && D != F) && (El >= Dl || E == A) && any_eq3(E, A, C, G) && ((El < Dl) || A != D || E != P || E != Q)) J = D;\n"
      "    if ((B == F && B != D && B != H) && (El >= Bl || E == C) && any_eq3(E, A, C, I) && ((El < Bl) || C != B || E != P || E != R)) K = B;\n"
      "    if ((H == D && H != F && H != B) && (El >= Hl || E == G) && any_eq3(E, A, G, I) && ((El < Hl) || G != H || E != S || E != Q)) L = H;\n"
      "    if ((F == H && F != B && F != D) && (El >= Fl || E == I) && any_eq3(E, C, G, I) && ((El < Fl) || I != H || E != R || E != S)) M = F;\n"
      "\n"
      "    // Intersection rules\n"
      "    if ((E != F && all_eq4(E, C, I, D, Q) && all_eq2(F, B, H)) && (F != src(+3, +0))) K = M = F;\n"
      "    if ((E != D && all_eq4(E, A, G, F, R) && all_eq2(D, B, H)) && (D != src(-3, +0))) J = L = D;\n"
      "    if ((E != H && all_eq4(E, G, I, B, P) && all_eq2(H, D, F)) && (H != src(+0, +3))) L = M = H;\n"
      "    if ((E != B && all_eq4(E, A, C, H, S) && all_eq2(B, D, F)) && (B != src(+0, -3))) J = K = B;\n"
      "    if (Bl < El && all_eq4(E, G, H, I, S) && none_eq4(E, A, D, C, F)) J = K = B;\n"
      "    if (Hl < El && all_eq4(E, A, B, C, P) && none_eq4(E, D, G, I, F)) L = M = H;\n"
      "    if (Fl < El && all_eq4(E, A, D, G, Q) && none_eq4(E, B, C, I, H)) K = M = F;\n"
      "    if (Dl < El && all_eq4(E, C, F, I, R) && none_eq4(E, B, A, G, H)) J = L = D;\n"
      "\n"
      "    // 2:1 slope rules\n"
      "    if (H != B) {\n"
      "      if (H != A && H != E && H != C) {\n"
      "        if (all_eq3(H, G, F, R) && none_eq2(H, D, src(+2, -1))) L = M;\n"
      "        if (all_eq3(H, I, D, Q) && none_eq2(H, F, src(-2, -1))) M = L;\n"
      "      }\n"
      "\n"
      "      if (B != I && B != G && B != E) {\n"
      "        if (all_eq3(B, A, F, R) && none_eq2(B, D, src(+2, +1))) J = K;\n"
      "        if (all_eq3(B, C, D, Q) && none_eq2(B, F, src(-2, +1))) K = J;\n"
      "      }\n"
      "    } // H !== B\n"
      "\n"
      "    if (F != D) {\n"
      "      if (D != I && D != E && D != C) {\n"
      "        if (all_eq3(D, A, H, S) && none_eq2(D, B, src(+1, +2))) J = L;\n"
      "        if (all_eq3(D, G, B, P) && none_eq2(D, H, src(+1, -2))) L = J;\n"
      "      }\n"
      "\n"
      "      if (F != E && F != A && F != G) {\n"
      "        if (all_eq3(F, C, H, S) && none_eq2(F, B, src(-1, +2))) K = M;\n"
      "        if (all_eq3(F, I, B, P) && none_eq2(F, H, src(-1, -2))) M = K;\n"
      "      }\n"
      "    } // F !== D\n"
      "  } // not constant\n"
      "\n"
      "  // select quadrant based on fractional part of texture coordinates\n"
      "  float2 fpart = frac(coords);\n"
      "  uint res = (fpart.x < 0.5f) ? ((fpart.y < 0.5f) ? J : L) : ((fpart.y < 0.5f) ? K : M);\n"
      "\n"
      "  ialpha = float(res != 0u);\n"
      "  texcol = unpackUnorm4x8(res);\n"
      "}\n"
      "\n"
      "#undef src\n"
    );
  }
  else if (texture_filter == GPU_TEXTURE_FILTER_MMPX_ENHANCED)
  {
    SS_LIT(ss,
      "	#define srcf(xoffs,yoffs) SampleFromVRAM(texpage, bcoords + float2((xoffs), (yoffs)), uv_limits)\n"
      "	#define src(xoffs,yoffs) packUnorm4x8(srcf(xoffs,yoffs))\n"
      "	\n"
      "\n"
      "float luma(float4 col) {\n"
      "\n"
      "	//Use CRT-era BT.601 standard.\n"
      "    float rgbsum =dot(col.rgb, float3(0.299, 0.587, 0.114));\n"
      "\n"
      "    float alphafactor = \n"
      "        (col.a > 0.998) ? 0.0 :\n"
      "        (col.a > 0.5) ? 2.0 :\n"
      "        (col.a > 0.002) ? 4.0 : 6.0;\n"
      "\n"
      "    return rgbsum + alphafactor;\n"
      "}\n"
      "\n"
      "float mixGate(float4 col1, float4 col2) {\n"
      "\n"
      "	float4 diff = col1 - col2;\n"
      "\n"
      "	float delta_range = max(diff.r, max(diff.g, diff.b)) - min(diff.r, min(diff.g, diff.b));\n"
      "\n"
      "	float dot_diff = dot(diff, diff);\n"
      "\n"
      "	float factor = (delta_range * delta_range) * 2.618034;\n"
      "\n"
      "	return step(dot_diff, mix(0.75, 0.0, factor));\n"
      "}\n"
      "\n"
      "#define all_eq2(a, b1, b2) (a == b1 && a == b2)\n"
      "#define all_eq4(a, b1, b2, b3, b4) (a == b1 && a == b2 && a == b3 && a == b4)\n"
      "#define any_eq2(a, b1, b2) (a == b1 || a == b2)\n"
      "#define none_eq2(a, b1, b2) !any_eq2(a, b1, b2)\n"
      "#define none_eq4(a, b1, b2, b3, b4) (a!=b1 && a!=b2 && a!=b3 && a!=b4)\n"
      "\n"
      "float4 admixC(float4 vX, float4 vE) {\n"
      "\n"
      "	float mixFactor = mixGate(vX, vE) * (-0.381966) + 1.0;\n"
      "\n"
      "	return mix(vX,vE,mixFactor);\n"
      "}\n"
      "\n"
      "float4 admixK(float4 vX, float4 vE) {\n"
      "\n"
      "    float4 diff = vX - vE;\n"
      "\n"
      "	float mixFactor = dot(diff.rgb, diff.rgb) * 0.16666 + 0.5;\n"
      "\n"
      "	return mix(vX,vE,mixFactor);\n"
      "}\n"
      "\n"
      "////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////\n"
      "\n"
      "void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits, out float4 texcol, out float ialpha)\n"
      "{\n"
      "\n"
      "	float2 bcoords = floor(coords);\n"
      "\n"
      "	float4 vE = SampleFromVRAM(texpage, bcoords, uv_limits);\n"
      "\n"
      "	float4 vB = srcf(0.0, -1.0);\n"
      "	float4 vD = srcf(-1.0, 0.0);\n"
      "	float4 vF = srcf(+1.0, 0.0);\n"
      "	float4 vH = srcf(0.0, +1.0);\n"
      "\n"
      "    uint E = packUnorm4x8(vE);\n"
      "    uint B = packUnorm4x8(vB);\n"
      "    uint D = packUnorm4x8(vD);\n"
      "    uint F = packUnorm4x8(vF);\n"
      "    uint H = packUnorm4x8(vH);\n"
      "\n"
      "	// default pixel\n"
      "	ialpha = float(E != 0u);\n"
      "	texcol = vE;\n"
      "\n"
      "bool skiprest = (E == D && E == F) || (E == B && E == H) || (B == H && D == F);\n"
      "if (!skiprest) {\n"
      "\n"
      "    // 5x5\n"
      "    uint A = src(-1.0, -1.0);\n"
      "    uint C = src(+1.0, -1.0);\n"
      "    uint G = src(-1.0, +1.0);\n"
      "    uint I = src(+1.0, +1.0);\n"
      "\n"
      "	uint P  = src( 0.0, -2.0);\n"
      "	uint Q  = src(-2.0,  0.0);\n"
      "	uint R  = src(+2.0,  0.0);\n"
      "	uint S  = src( 0.0, +2.0);\n"
      "\n"
      "	uint PA = src(-1.0, -2.0);\n"
      "	uint PC = src(+1.0, -2.0);\n"
      "	uint QA = src(-2.0, -1.0);\n"
      "	uint QG = src(-2.0, +1.0);\n"
      "	uint RC = src(+2.0, -1.0);\n"
      "	uint RI = src(+2.0, +1.0);\n"
      "	uint SG = src(-1.0, +2.0);\n"
      "	uint SI = src(+1.0, +2.0);\n"
      "\n"
      "\n"
      "    float4 J = vE;    float4 K = vE;    float4 L = vE;    float4 M = vE;\n"
      "\n"
      "\n"
      "    float Bl = luma(vB) + float(B==0u) *2.0;\n"
      "    float Dl = luma(vD) + float(D==0u) *2.0;\n"
      "    float El = luma(vE) + float(E==0u) *2.0;\n"
      "    float Fl = luma(vF) + float(F==0u) *2.0;\n"
      "    float Hl = luma(vH) + float(H==0u) *2.0;\n"
      "\n"
      "    bool slope1 = false;    bool slope2 = false;    bool slope3 = false;    bool slope4 = false;\n"
      "\n"
      "// B - D\n"
      "    if ( E!=B && (D == B && D != H && D != F) && (El >= Dl || E == A && B !=PA && D !=QA) && any_eq2(E, C, G) && ((El < Dl) || A != D || E != P || E != Q)\n"
      "		) {\n"
      "		J=vB;\n"
      "		slope1 = true;\n"
      "	}\n"
      "\n"
      "// B - F\n"
      "    if ( E!=B && (B == F && B != D && B != H) && (El >= Bl || E == C && B !=PC && F !=RC) && any_eq2(E, A, I) && ((El < Bl) || C != B || E != P || E != R)\n"
      "	 ) {\n"
      "		K=vB;\n"
      "		slope2 = true;\n"
      "	}\n"
      "\n"
      "// D - H\n"
      "    if ( E!=H && (H == D && H != F && H != B) && (El >= Hl || E == G && D !=QG && H !=SG) && any_eq2(E, A, I) && ((El < Hl) || G != H || E != S || E != Q)\n"
      "	 ) {\n"
      "		L=vH;\n"
      "		slope3 = true;\n"
      "	}\n"
      "// F - H\n"
      "    if ( E!=H && (F == H && F != B && F != D) && (El >= Fl || E == I && F !=RI && H !=SI) && any_eq2(E, C, G) && ((El < Fl) || I != H || E != R || E != S)\n"
      "	  ) {\n"
      "		M=vH;\n"
      "		slope4 = true;\n"
      "	}\n"
      "\n"
      "//  long gentle 2:1 slope\n"
      "\n"
      "if (slope4) { //zone4 long slope\n"
      "	if (all_eq2(R,F,G) && R != RC && Q != G) L=M;\n"
      "	// vertical\n"
      "	if (all_eq2(S,H,C) && S != SG && P != C) K=M;\n"
      "}\n"
      "\n"
      "if (slope3) { //zone3 long slope\n"
      "	// horizontal\n"
      "	if (all_eq2(Q,D,I) && Q != QA && R != I) M=L;\n"
      "	// vertical\n"
      "	if (all_eq2(S,H,A) && S != SI && A != P) J=L;\n"
      "}\n"
      "\n"
      "if (slope2) { //zone2 long slope\n"
      "	// horizontal\n"
      "	if (all_eq2(R,F,A) && R != RI && A != Q) J=K;\n"
      "	// vertical\n"
      "	if (all_eq2(P,B,I) && P != PA && I != S) M=K;\n"
      "}\n"
      "\n"
      "if (slope1) { //zone1 long slope\n"
      "	// horizontal\n"
      "	if (all_eq2(Q,D,C) && Q != QG && C != R) K=J;\n"
      "	// vertical\n"
      "	if (all_eq2(P,B,G) && P != PC && G != S) L=J;\n"
      "}\n"
      "\n"
      "skiprest = skiprest||slope1||slope2||slope3||slope4||E==0u||B==0u||D==0u||F==0u||H==0u;\n"
      "\n"
      "/* Concave + Cross type */\n"
      "\n"
      "    if (!skiprest && Bl < El && all_eq4(E, G, H, I, S) && none_eq4(E, A, D, C, F)) { J=admixC(vB,J);	K=J;    skiprest = true;}\n"
      "    if (!skiprest && Hl < El && all_eq4(E, A, B, C, P) && none_eq4(E, D, G, I, F)) { L=admixC(vH,L);	M=L;    skiprest = true;}\n"
      "    if (!skiprest && Fl < El && all_eq4(E, A, D, G, Q) && none_eq4(E, B, C, I, H)) { K=admixC(vF,K);	M=K;    skiprest = true;}\n"
      "    if (!skiprest && Dl < El && all_eq4(E, C, F, I, R) && none_eq4(E, B, A, G, H)) { J=admixC(vD,J);	L=J;    skiprest = true;}\n"
      "\n" 
      "/* K type */\n"
      "\n"
      "    if (!skiprest && (E != F && all_eq4(E, C, I, D, Q) && all_eq2(F, B, H)) && (F != src(+3.0, +0.0))) {K=admixK(vF,K); M=K;skiprest=true;}	// RIGHT\n"
      "    if (!skiprest && (E != D && all_eq4(E, A, G, F, R) && all_eq2(D, B, H)) && (D != src(-3.0, +0.0))) {J=admixK(vD,J); L=J;skiprest=true;}	// LEFT\n"
      "    if (!skiprest && (E != H && all_eq4(E, G, I, B, P) && all_eq2(H, D, F)) && (H != src(+0.0, +3.0))) {L=admixK(vH,L); M=L;skiprest=true;}	// BOTTOM\n"
      "    if (!skiprest && (E != B && all_eq4(E, A, C, H, S) && all_eq2(B, D, F)) && (B != src(+0.0, -3.0))) {J=admixK(vB,J); K=J;}				// TOP\n"
      "\n"
      "	//final write\n"
      "	float2 fpart = frac(coords);\n"
      "\n"
      "	float4 res = (fpart.x < 0.5) ? ((fpart.y < 0.5) ? J : L) : ((fpart.y < 0.5) ? K : M);\n"
      "\n"
      "	ialpha = step(0.002, res.r+res.g+res.b+res.a);\n"
      "	texcol = res;\n"
      "}\n"
      "	\n"
      "}\n"
      "\n"
      "#undef src\n"
      "#undef srcf\n"
      "#undef all_eq2\n"
      "#undef all_eq4\n"
      "#undef any_eq2\n"
      "#undef none_eq2\n"
      "#undef none_eq4\n"
      "\n" 
    );
  }
  else if (texture_filter == GPU_TEXTURE_FILTER_MMPX_QUALITY)
  {
    SS_LIT(ss,
      "	#define srcf(xoffs,yoffs) SampleFromVRAM(texpage, bcoords + float2((xoffs), (yoffs)), uv_limits)\n"
      "	#define src(xoffs,yoffs) packUnorm4x8(srcf(xoffs,yoffs))\n"
      "	\n"
      "\n"
      "//RGB visual weight + alpha segmentation\n"
      "float luma(float4 col) {\n"
      "\n"
      "	//Use CRT-era BT.601 standard. Clamp range to [0.0 - 0.999]\n"
      "    float rgbsum =min(dot(col.rgb, float3(0.299, 0.587, 0.114)), 0.9999);\n"
      "\n"
      "	// Alpha weighting can be removed for subsequent fractional bit extraction\n"
      "    float alphafactor = \n"
      "        (col.a > 0.998) ? 0.0 :		// Opaque\n"
      "        (col.a > 0.5) ? 2.0 :\n"
      "        (col.a > 0.002) ? 4.0 : 6.0;	// Fully transparent\n"
      "\n"
      "    return rgbsum + alphafactor;\n"
      "}\n"
      "\n"
     " Constant explanations:\n"
      "0.145898	:			Double short golden ratio of 1.0\n"
      "0.0638587	:		Squared double short golden ratio of RGB Euclidean distance\n"
      "0.4377		:		Squared single short golden ratio of RGB Euclidean distance\n"
      "0.75			:		Squared half of RGB Euclidean distance\n"
      "\n"
      /* "\n"
      "// duck calculation: dot(diff,diff) directly incorporates alpha channel  //duck.alpha\n"
      "// Note: Transparent duck pixels are determined outside sim and mixFactor functions\n"
      "bool simb(float4 col1, float4 col2) {\n"
      "\n"
      "	float4 diff = col1 - col2;\n"
      "\n"
      "	float maxdiff = max(diff.r, max(diff.g, diff.b));\n"
      "	float mindiff = min(diff.r, min(diff.g, diff.b));\n"
      "\n"
      "	// Luminance baseline weight: both colors must be > 0.078 (0.234/3)\n"
      "	float weight = step(0.234, min(col1.r+col1.g+col1.b, col2.r+col2.g+col2.b));\n"
      "\n"
      "	// Find the most opposite channel: if one positive and one negative, take the smallest absolute value; 0 for same direction\n"
      "	// Use max(0.0, ...) to filter same-sign cases\n"
      "	// Skip team_rebel if either pixel luminance < 0.078\n"
      "	float team_rebel = min(max(0.0, maxdiff), max(0.0, -mindiff)) * weight;\n"
      "	float finaldist = (maxdiff - mindiff) + team_rebel;\n"
      "\n"
      "	float dot_diff = dot(diff, diff);\n"
      "\n"
      "	// Equivalent to (finaldist / 0.145898 )^2\n"
      "	float factor = (finaldist * finaldist) * 46.9787;\n"
      "\n"
      "	return dot_diff < mix(0.0638587, 0.0, factor);\n"
      "}\n"
      "\n"
      "bool sim(float4 col1, float4 col2) {\n"
      "\n"
      "	float4 diff = col1 - col2;\n"
      "\n"
      "	// RGB color difference range (max_diff - min_diff)\n"
      "	float delta_range = max(diff.r, max(diff.g, diff.b)) - min(diff.r, min(diff.g, diff.b));\n"
      "\n"
      "	float dot_diff = dot(diff, diff);\n"
      "\n"
      "	// Equivalent to (delta_range / 0.382 )^2\n"
      "	float factor = (delta_range * delta_range) * 6.8541;\n"
      "\n"
      "	return dot_diff < mix(0.0638587, 0.0, factor);\n"
      "}\n"
      "\n"
      "bool vi_sim(float4 col1, uint uC1, uint uC2) {\n"
      "    if (uC1==uC2) return true;\n"
      "    float4 col2 = unpackUnorm4x8(uC2);	// duck.alpha\n"
      "    return sim(col1, col2);\n"
      "}\n"
      "\n"
      "float mixGate(float4 col1, float4 col2) {\n"
      "\n"
      "	float4 diff = col1 - col2;\n"
      "\n"
      "	// RGB color difference range (max_diff - min_diff)\n"
      "	float delta_range = max(diff.r, max(diff.g, diff.b)) - min(diff.r, min(diff.g, diff.b));\n"
      "\n"
      "	float dot_diff = dot(diff, diff);\n"
      "\n"
      "	// Equivalent to (delta_range / 0.618 )^2\n"
      "	float factor = (delta_range * delta_range) * 2.618034;\n"
      "\n"
      "	return step(dot_diff, mix(0.75, 0.0, factor));\n"
      "}\n"
      "\n"
      "\n"
      "#define eq(a,b) (a==b)\n"
      "\n"
      "#define neq(a,b) (a!=b)\n"
      "\n"
      "#define all_eq2(a, b1, b2) \\\n"
      "	( eq(a,b1) && eq(a,b2))\n"
      "\n"
      "#define all_eq3(a, b1, b2, b3) \\\n"
      "	( eq(a,b1) && eq(a,b2) && eq(a,b3))\n"
      "\n"
      "#define all_eq4(a, b1, b2, b3, b4) \\\n"
      "	( eq(a,b1) && eq(a,b2) && eq(a,b3) && eq(a,b4))\n"
      "\n"
      "#define any_eq2(a, b1, b2) (eq(a,b1)||eq(a,b2))\n"
      "#define any_eq3(a, b1, b2, b3) (eq(a,b1)||eq(a,b2)||eq(a,b3))\n"
      "// Better than a!=b1 && a!=b2\n"
      "#define none_eq2(a, b1, b2) !any_eq2(a, b1, b2)\n"
      "\n"
      "\n"
      "// Pre-define\n"
      "//#define testcolor float4(1.0, 0.0, 1.0, 1.0)  // Magenta\n"
      "//#define testcolor2 float4(0.0, 1.0, 1.0, 1.0)  // Cyan\n"
      "//#define testcolor3 float4(1.0, 1.0, 0.0, 1.0)  // Yellow\n"
      "//#define testcolor4 float4(1.0, 1.0, 1.0, 1.0)  // White\n"
      "#define slopOFF float4(2.0, 2.0, 2.0, 2.0)\n"
      "#define slopeBAD float4(4.0, 4.0, 4.0, 4.0)\n"
      "#define theEXIT float4(8.0, 8.0, 8.0, 8.0)\n"
      "\n"
      "#define mixXE mix(vX,vE,mixFactor)\n"
      "#define mixXEoff mixXE+slopOFF\n"
      "#define Xoff vX+slopOFF\n"
      "//#define checkblack(col) ((col).g < 0.078 && (col).r < 0.1 && (col).b < 0.1)\n"
      "\n"
      "#if API_OPENGL || API_OPENGL_ES || API_VULKAN\n"
      "\n"
      "    #define checkblack(col) all(lessThan((col).rgb, float3(0.1, 0.078, 0.1)))\n"
      "    #define checkwhite(col) all(greaterThan((col).rgb, float3(0.92, 0.92, 0.92)))\n"
      "    #define vec_neq(a, b)   any(greaterThan(abs((a)-(b)), float4(0.01)))\n"
      "\n"
      "#else\n"
      "\n"
      "    #define checkblack(col) all((col).rgb < float3(0.1, 0.078, 0.1))\n"
      "    #define checkwhite(col) all((col).rgb > float3(0.92, 0.92, 0.92))\n"
      "    #define vec_neq(a, b)   any(abs((a)-(b)) > 0.01)\n"
      "\n"
      "#endif\n"
      "//pin zz\n"
      "// \"Concave + Cross\" type weak blending (weak blend / none)\n"
      "float4 admixC(float4 vX, float4 vE) {\n"
      "	// Weak blending. Blend enabled? 0.618 else 1.0\n"
      "	float mixFactor = mixGate(vX, vE) * (-0.381966) + 1.0;\n"
      "\n"
      "	return mixXE;\n"
      "}\n"
      "\n"
      "// K-type forced weak blending\n"
      "float4 admixK(float4 vX, float4 vE) {\n"
      "    float4 diff = vX - vE;\n"
      "	// mixFactor slides from 0.5-1.0 based on point set distance, quadratic curve, steeper closer to 1.0\n"
      "	float mixFactor = dot(diff.rgb, diff.rgb) * 0.16666 + 0.5;	// xxx.alpha\n"
      "	// mixFactor slides linearly from 0.5-1.0 based on Euclidean distance\n"
      "	//float mixFactor = distance(vX, vE) * 0.28867 + 0.5;\n"
      "	return mixXE;\n"
      "}\n"
      "\n"
      "// L-type 2:1 slope  Main corner extension\n"
      "// Practice: This rule requires 4 pixels on strict slope to be identical. Otherwise various artifacts will appear!\n"
      "float4 admixL(float4 vX, float4 vE, float4 vS) {\n"
      "\n"
      "    // Check eqX,E: Originally captured many duplicate pixels, now main thread passes slopeok filter.\n"
      "\n"
      "	// If target X and reference S(sample) differ, blending has occurred once; direct copy, no re-blending\n"
      "	if (vec_neq(vX, vS)) return vX;\n"
      "\n"
      "	float mixFactor = 0.381966 * mixGate(vX,vE) * step(0.002,vE.r+vE.g+vE.b+vE.a); // xxx.alpha.duck\n"
      "\n"
      "    return mixXE;\n"
      "}\n"
      "float4 admixX( uint A, uint B, uint C, uint D, uint E, uint F, uint G, uint H, uint I\n"
      "		  , uint P, uint PA, uint PC, uint Q, uint QA, uint QG, uint R, uint RC, uint RI, uint S, uint SG, uint SI, uint AA, uint CC, uint GG\n"
      "		  , float El, float Bl, float Dl, float Fl, float Hl\n"
      "		  , float4 vE, float4 vB, float4 vD, float4 vC, float4 vG\n"
      "		  ) {\n"
      "\n"
      "\n"
      "	bool eq_B_C = eq(B,C);\n"
      "	bool eq_D_G = eq(D,G);\n"
      "\n"
      "    if (eq_B_C && eq_D_G) return slopeBAD;\n"
      "\n"
      "\n"
      "	//Pre-declare\n"
      "	bool eq_B_P;		bool eq_B_PA;	bool eq_B_PC;\n"
      "	bool eq_D_Q;		bool eq_D_QA;	bool eq_D_QG;\n"
      "	bool eq_E_F;		bool eq_E_H;		bool eq_A_AA;\n"
      "\n"
      "	float4 vX;\n"
      "	float mixFactor;\n"
      "\n"
      "	bool eq_E_C = eq(E,C);\n"
      "	bool eq_E_G = eq(E,G);\n"
      "    bool eq_A_P = eq(A,P);\n"
      "    bool eq_A_Q = eq(A,Q);\n"
      "    bool comboE3 = eq_E_C && eq_E_G;\n"
      "    bool comboA3 = eq_A_P && eq_A_Q;\n"
      "\n"
      "if (neq(B,D)){\n"
      "\n"
      "	if (eq(E,A)) return slopeBAD;\n"
      "\n"
      "	float diffBD = abs(Bl-Dl);\n"
      "	if (diffBD > El-Bl || diffBD > El-Dl) return slopeBAD;\n"
      "\n"
      "\n"
      "	vX = mix(vB, vD, 0.5);\n"
      "	vX.a = min(vB.a, vD.a);\n"
      "\n"
      "	mixFactor = 0.381966 * mixGate(vX,vE) * float(E!=0u);\n"
      "\n"
      "	eq_B_PC = eq(B,PC);\n"
      "	eq_D_QG = eq(D,QG);\n"
      " \n"
      "    if (none_eq2(A,B,D)){\n"
      "		if (comboA3) return mixXEoff;\n"
      "		if ( eq_A_P && eq_B_PC && !eq_B_C ) return mixXEoff;\n"
      "		if ( eq_A_Q && eq_D_QG && !eq_D_G ) return mixXEoff;\n"
      "\n"
      "		if ( eq_A_P && eq_E_G ) return mixXEoff;\n"
      "		if ( eq_A_Q && eq_E_C ) return mixXEoff;\n"
      "\n"
      "		if ( eq_E_C && eq_D_G ) return mixXEoff;\n"
      "		if ( eq_E_G && eq_B_C ) return mixXEoff;\n"
      "\n"
      "}\n"
      "    if ( comboE3 ) return mixXEoff;\n"
      "\n"
      "    if ( eq_E_C && eq_B_PC && neq(B,P)) return mixXEoff;\n"
      "	if ( eq_E_G && eq_D_QG && neq(D,Q)) return mixXEoff;\n"
      "\n"
      "	eq_E_F = eq(E,F);\n"
      "\n"
      "	if (eq(F,H)) {\n"
      "\n"
      "		if ( eq_E_C && !eq_D_G && (!eq_E_F||neq(E,P)) ) return mixXEoff;\n"
      "		if ( eq_E_G && !eq_B_C && (!eq_E_F||neq(E,Q)) ) return mixXEoff;\n"
      "\n"
      "		if ( !eq_E_F && eq_B_PC && eq(F,RC) ) return mixXEoff;\n"
      "		if ( !eq_E_F && eq_D_QG && eq(H,SG) ) return mixXEoff;\n"
      "	}\n"
      "\n"
      "    return slopeBAD;\n"
      "}\n"
      "\n"
      "	Bl = fract(Bl);\n"
      "	Dl = fract(Dl);\n"
      "	El = fract(El);\n"
      "	Fl = fract(Fl);\n"
      "	Hl = fract(Hl);\n"
      "\n"
      "	bool Xisblack = checkblack(vB);\n"
      "	if ( Xisblack && El >0.5 && (Fl<0.078 || Hl<0.078) ) return theEXIT;\n"
      "\n"
      "	vX = vB;\n"
      "\n"
      "	mixFactor = 0.381966 * mixGate(vX,vE) * float(E!=0u);\n"
      "\n"
      "	bool B_slope;	bool B_tower;	bool B_wall;\n"
      "    bool D_slope;	bool D_tower;	bool D_wall;\n"
      "	bool En3;\n"
      "    #define En4square En3&&eq(E,I)\n"
      "\n"
      "if (eq(E,A)) {\n"
      "\n"
      "	eq_E_F = eq(E,F);\n"
      "	eq_E_H = eq(E,H);\n"
      "\n"
      "	bool Eisblack = checkblack(vE);\n"
      "\n"
      "    if ( comboE3 && !eq_E_F && !eq_E_H && eq(E,I) ) {\n"
      "\n"
      "		if (Eisblack) return theEXIT;\n"
      "		mixFactor = 0.618034 * (1.0 - mixFactor);\n"
      "		return mixXEoff;\n"
      "	}\n"
      "\n"
      "	eq_A_AA = eq(A,AA);\n"
      "\n"
      "    if ( comboA3 && eq_A_AA && none_eq2(A,PA,QA) )  {	\n"
      "		if (Eisblack) return theEXIT;\n"
      "		mixFactor = 0.618034 * (1.0 - mixFactor);\n"
      "		if ( neq(B,PA) && eq(PA,QA) ) return mixXEoff;\n"
      "		mixFactor += 0.236068;\n"
      "		return mixXEoff;\n"
      "	}\n"
      "\n"
      "	if (E==0u && !Xisblack) return vX;\n"
      "\n"
      "    eq_B_PC = eq(B,PC);\n"
      "    eq_B_PA = eq(B,PA);\n"
      "    eq_D_QG = eq(D,QG);\n"
      "    eq_D_QA = eq(D,QA);\n"
      "\n"
      "	if ( comboE3 && comboA3 &&\n"
      "		(eq_B_PC || eq_D_QG) && eq_D_QA && eq_B_PA) {\n"
      "		mixFactor = mixFactor * (-0.618034) + 0.8541;\n"
      "        return mixXEoff;\n"
      "	}\n"
      "\n"
      "	if ( comboE3 && eq_A_P\n"
      "		 && eq_B_PA && eq_D_QA && eq_D_QG\n"
      "		 && eq_E_H\n"
      "		) {\n"
      "		mixFactor = mixFactor * (-0.618034) + 0.8541;\n"
      "        return mixXEoff;\n"
      "		}\n"
      "\n"
      "	if ( comboE3 && eq_A_Q\n"
      "		 && eq_B_PA && eq_D_QA && eq_B_PC\n"
      "		 && eq_E_F\n"
      "		) {\n"
      "		mixFactor = mixFactor * (-0.618034) + 0.8541;\n"
      "        return mixXEoff;\n"
      "		}\n"
      "\n"
      "	if (comboA3) return Xoff;\n"
      "\n"
      "    if (comboE3) return mixXEoff;\n"
      "\n"
      "	eq_B_P = eq(B, P);\n"
      "	eq_D_Q = eq(D, Q);\n"
      "\n"
      "	B_slope = eq_B_PC && !eq_B_P && !eq_B_C && !eq_B_PA;\n"
      "	D_slope = eq_D_QG && !eq_D_Q && !eq_D_G && !eq_D_QA;\n"
      "\n"
      "	B_wall = eq_B_C && !eq_B_PC && !eq_B_P;\n"
      "	D_wall = eq_D_G && !eq_D_QG && !eq_D_Q;\n"
      "	\n"
      "	B_tower = eq_B_P && !eq_B_PC && !eq_B_C && !eq_B_PA;\n"
      "	D_tower = eq_D_Q && !eq_D_QG && !eq_D_G && !eq_D_QA;\n"
      "\n"
      "	if ( B_slope && eq_E_G ) return mixXEoff;\n"
      "	if ( D_slope && eq_E_C ) return mixXEoff;\n"
      "\n"
      "    float scoreE = 0.0; \n"
      "	float scoreB = 0.0; \n"
      "	float scoreD = 0.0; \n"
      "	float scoreZ = 0.0;\n"
      "\n"
      "    if (eq_E_C) {\n"
      "		scoreE += 1.0 +float(eq(F,H)) +float(B_slope);\n"
      "		scoreE -= float(all_eq2(E,P,PC)&&!D_wall);\n"
      "	}\n"
      "\n"
      "    if (eq_E_G) {\n"
      "        scoreE += 1.0 +float(eq(F,H)) +float(D_slope);\n"
      "		scoreE -= float(all_eq2(E,Q,QG)&&!B_wall);\n"
      "    }\n"
      "\n"
      "	scoreE += float(B_slope && eq_A_Q || D_slope && eq_A_P);\n"
      "\n"
      "    En3 = eq_E_F && eq_E_H;\n"
      "\n"
      "	if ( scoreE<0.1 && mixFactor<0.1 && En4square && eq(E,S)==eq(E,SI) && eq(E,R)==eq(E,RI) ) return theEXIT;\n"
      "\n"
      "	if ( scoreE<0.1 && !En3 && neq(E,I) ) {\n"
      "		if ( B_wall && eq_E_F ) return theEXIT;\n"
      "		if ( D_wall && eq_E_H ) return theEXIT;\n"
      "    }\n"
      "\n"
      "	scoreE += float(B_slope && eq_A_P || D_slope && eq_A_Q);\n"
      "\n"
      "    if ( !En3 && eq(F,H) ) {\n"
      "		if (Eisblack) return slopeBAD;\n"
      "		bool condZ1 = B_wall && (eq(F,R) || eq(F,RC) || eq(G,H) || eq(F,I));\n"
      "		bool condZ2 = D_wall && (eq(C,F) || eq(H,SG) || eq(H,S) || eq(F,I));\n"
      "		scoreZ = float(condZ1 || condZ2);\n"
      "    }\n"
      "\n"
      "    if (eq_B_PA) {\n"
      "		scoreB -= 1.0 +float(eq(P,C)) +float(eq_A_AA);\n"
      "	}\n"
      "\n"
      "	if (eq(P,C)){\n"
      "		scoreB -= float(eq_A_AA);\n"
      "		scoreZ *= float(scoreE < 0.1);\n"
      "	}\n"
      "\n"
      "    if (eq_D_QA) {\n"
      "		scoreD -= 1.0 +float(eq(G,Q)) +float(eq_A_AA);\n"
      "	}\n"
      "\n"
      "	if (eq(G,Q)){\n"
      "		scoreD -= float(eq_A_AA);\n"
      "		scoreZ *= float(scoreE < 0.1);\n"
      "	}\n"
      "\n"
      "    float scoreFinal = scoreE + scoreB + scoreD + scoreZ ;\n"
      "\n"
      "	scoreFinal += float(min(scoreB,scoreD) > -0.1 && (B_wall && D_tower || B_tower && D_wall)) *2.0;\n"
      "\n"
      "	mixFactor *= (1.0 - step(1.9, scoreFinal));\n"
      "	return mixXE + slopeBAD*(1.0 - step(0.9, scoreFinal));\n"
      "\n"
      "}\n"
      "\n"
      "    if (eq_E_C ) {\n"
      "		if (comboA3) return vX;\n"
      "		if (comboE3) return mixXE;\n"
      "		if (all_eq2(B,A,PA) && all_eq3(E,F,P,PC)) return theEXIT;\n"
      "		return mixXE;\n"
      "	}\n"
      "\n"
      "	if (eq_E_G) {\n"
      "		if (comboA3) return vX;\n"
      "		if (comboE3) return mixXE;\n"
      "		if (all_eq2(D,A,QA) && all_eq3(E,H,Q,QG)) return theEXIT;\n"
      "		return mixXE;\n"
      "	}\n"
      "\n"
      "	if (E==0u) return theEXIT;		// xxx.alpha\n"
      "\n"
      "    bool eq_A_B = eq(A,B);\n"
      "    bool eq_F_H = eq(F,H);\n"
      "\n"
      "	eq_B_P  = eq(B,P);\n"
      "	eq_B_PC = eq(B,PC);\n"
      "	eq_B_PA = eq(B,PA);\n"
      "	eq_D_Q  = eq(D,Q);\n"
      "	eq_D_QG = eq(D,QG);\n"
      "	eq_D_QA = eq(D,QA);\n"
      "\n"
      "	B_slope = eq_B_PC && !eq_B_P && !eq_B_C;\n"
      "	D_slope = eq_D_QG && !eq_D_Q && !eq_D_G;\n"
      "	B_tower = eq_B_P && !eq_B_PC && !eq_B_C && !eq_B_PA;\n"
      "	D_tower = eq_D_Q && !eq_D_QG && !eq_D_G && !eq_D_QA;\n"
      "	B_wall = eq_B_C && !eq_B_PC && !eq_B_P;\n"
      "	D_wall = eq_D_G && !eq_D_QG && !eq_D_Q;\n"
      "\n"
      "    if (!eq_A_B) {\n"
      "\n"
      "        if (comboA3) return Xoff;\n"
      "\n"
      "        if ( (B_slope||B_tower) && (D_slope||D_tower) ) return Xoff;\n"
      "\n"
      "        if ( B_slope && eq_A_P ) return mixXEoff;\n"
      "        if ( D_slope && eq_A_Q ) return mixXEoff;\n"
      "\n"
      "        if ( (B_slope || D_slope) && eq_F_H ) return mixXEoff;\n"
      "\n"
      "        if ( B_slope && eq(H,SG) ) return mixXEoff;\n"
      "        if ( D_slope && eq(F,RC) ) return mixXEoff;\n"
      "\n"
      "        if ( B_slope && eq_A_Q && eq(Q,QG) ) return mixXEoff;\n"
      "        if ( D_slope && eq_A_P && eq(P,PC) ) return mixXEoff;\n"
      "\n"
      "    }\n"
      "\n"
      "	bool sim_EC = E!=0u && C!=0u && sim(vE, vC);\n"
      "	bool sim_EG = E!=0u && G!=0u && sim(vE, vG);\n"
      "\n"
      "	float E_lumDiff = mix(0.381966, 0.145898, max((El - 0.8541),0.0) * 6.8541);\n"
      "\n"
      "    if ( mixFactor<0.1 && !sim_EC && !sim_EG && neq(E,I) && abs(El-Fl)>E_lumDiff && abs(El-Hl)>E_lumDiff ) return slopeBAD;\n"
      "\n"
      "	eq_E_F = eq(E,F);\n"
      "	eq_E_H = eq(E,H);\n"
      "\n"
      "	if ( eq_B_C && eq_D_Q ) {\n"
      "		if ( eq(P,PC) && eq(A,QA) && !eq_D_QG && eq_E_F && !eq_E_H && eq(H,I)) return theEXIT;\n"
      "		if ( eq_A_B ) return slopeBAD;\n"
      "		if ( B_wall && D_tower && eq_E_F) return vX;\n"
      "		return mixXEoff;\n"
      "	}\n"
      "\n"
      "	if ( eq(D,G) && eq(B,P)) {\n"
      "		if ( eq(Q,QG) && eq(A,PA) && !eq_B_PC && eq_E_H && !eq_E_F && eq(F,I)) return theEXIT;\n"
      "		if ( eq_A_B ) return slopeBAD;\n"
      "		if ( B_tower && D_wall && eq_E_H) return vX;\n"
      "		return mixXEoff;\n"
      "	}\n"
      "\n"
      "    En3 = eq_E_F && eq_E_H;\n"
      "\n"
      "	if ( En4square ) {\n"
      "        if ( ( eq_B_C || eq_D_G) && eq_A_B) return theEXIT;\n"
      "        if ( ( eq_B_C || eq_D_G || mixFactor<0.1) && (eq(E,S) == eq(E, SI) && eq(E,R) == eq(E, RI)) ) return theEXIT;\n"
      "        return mixXEoff;\n"
      "    }\n"
      "\n"
      "	if (!eq_B_C && !eq_D_G ) {\n"
      "		if ( comboA3 && eq_F_H ) return Xoff;\n"
      "\n"
      "		if ( comboA3&&eq_B_PC&&eq(C,CC) ) return Xoff;\n"
      "		if ( comboA3&&eq_D_QG&&eq(G,GG) ) return Xoff;\n"
      "\n"
      "		if ( !eq_B_P && !eq_B_PC && !eq_D_Q && !eq_D_QG && !En3 ) return slopeBAD;\n"
      "\n"
      "		if (eq_A_Q&&sim_EC) return mixXEoff;\n"
      "		if (eq_A_P&&sim_EG) return mixXEoff;\n"
      "		if (sim_EC&&sim_EG ) return mixXEoff;\n"
      "	}\n"
      "\n"
      " 	if ( En3 && eq_A_B) return theEXIT;\n"
      "\n"
      "	if (eq_F_H) {\n"
      "\n"
      "		if ( eq_B_PC&&eq(F,RC) || eq_D_QG&&eq(H,SG) ) return mixXEoff;\n"
      "\n"
      "		if (eq_A_B) return slopeBAD;\n"
      "\n"
      "		if ( eq_B_C || eq_D_G) return mixXEoff;\n"
      "		if ( eq_B_PC || eq_D_QG) return mixXEoff;\n"
      "\n"
      "	}\n"
      "\n"
      "	return slopeBAD;\n"
      "\n"
      "}\n"
      "\n"
      "float4 admixS( uint A, uint B, uint C, uint D, uint E, uint F, uint G, uint H, uint I\n"
      "		   , uint R, uint RC, uint RI, uint S, uint SG, uint SI, uint II, uint CC\n"
      "		   , float4 vE, float4 vF, float4 vC\n"
      "		   ) {\n"
      "\n"
      "    if (any_eq2(F,C,I)) return vE;\n"
      "\n"
      "	if ( (eq(F,RI) || eq(G,S) || eq(R, RI)) && neq(R,I) ) return vE;\n"
      "\n"
      "    if (eq(H, S) && none_eq2(H,I,SG)) return vE;\n"
      "\n"
      "    if ( eq(R, RC) || eq(G,SG) ) return vE;\n"
      "\n"
      "	if ( checkwhite(vE) && all_eq2(E,C,D) && none_eq2(E,RC,CC)) return vE;\n"
      "\n"
      "	#define vX vF\n"
      "	float mixFactor = 0.381966 * mixGate(vX,vE) * float(E!=0);\n"
      "\n"
      "	if ( eq(E,C) && (eq(E,D)||eq(B,D)) ) return mixXE;\n"
      "\n"
      "	bool sim_E_C = E!=0u && E!=0u && sim(vE,vC);\n"
      "\n"
      "	if ( sim_E_C && eq(E,D) && eq(B,C) ) return mixXE;\n"
      "\n"
      "	if ( (sim_E_C || mixFactor>0.1) && all_eq2(B,C,D) ) return mixXE;\n"
      "\n"
      "    return vE;\n"
      "}\n"
      "void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits, out float4 texcol, out float ialpha)\n"
      "{\n"
      "\n"
      "  float2 bcoords = floor(coords);\n"
      "\n"
      "	float4 vE = SampleFromVRAM(texpage, bcoords, uv_limits);\n"
      "\n"
      "	float4 vB = srcf(0.0, -1.0);\n"
      "	float4 vD = srcf(-1.0, 0.0);\n"
      "	float4 vF = srcf(+1.0, 0.0);\n"
      "	float4 vH = srcf(0.0, +1.0);\n"
      "\n"
      "    uint E = packUnorm4x8(vE);\n"
      "    uint B = packUnorm4x8(vB);\n"
      "    uint D = packUnorm4x8(vD);\n"
      "    uint F = packUnorm4x8(vF);\n"
      "    uint H = packUnorm4x8(vH);\n"
      "\n"
      "	// default pixel\n"
      "	ialpha = float(E != 0u);\n"
      "	texcol = vE;\n"
      "\n"
      "    bool eq_E_D = eq(E,D);\n"
      "    bool eq_E_F = eq(E,F);\n"
      "    bool eq_E_B = eq(E,B);\n"
      "    bool eq_E_H = eq(E,H);\n"
      "    bool eq_B_H = eq(B,H);\n"
      "    bool eq_D_F = eq(D,F);\n"
      "\n"
      "\n"
      "bool skiprest = (eq_E_D && eq_E_F) || (eq_E_B && eq_E_H) || (eq_B_H && eq_D_F);\n"
      "if (!skiprest) {\n"
      "\n"
      "\n"
      "\n"
      "    // 5x5\n"
      "	float4 vA = srcf(-1.0, -1.0);\n"
      "	float4 vC = srcf(+1.0, -1.0);\n"
      "	float4 vG = srcf(-1.0, +1.0);\n"
      "	float4 vI = srcf(+1.0, +1.0);\n"
      "\n"
      "    uint A = packUnorm4x8(vA);\n"
      "    uint C = packUnorm4x8(vC);\n"
      "    uint G = packUnorm4x8(vG);\n"
      "    uint I = packUnorm4x8(vI);\n"
      "\n"
      "	uint P  = src( 0.0, -2.0);\n"
      "	uint Q  = src(-2.0,  0.0);\n"
      "	uint R  = src(+2.0,  0.0);\n"
      "	uint S  = src( 0.0, +2.0);\n"
      "\n"
      "	uint PA = src(-1.0, -2.0);\n"
      "	uint PC = src(+1.0, -2.0);\n"
      "	uint QA = src(-2.0, -1.0);\n"
      "	uint QG = src(-2.0, +1.0); //             AA    PA    [P]   PC    CC\n"
      "	uint RC = src(+2.0, -1.0); //                |--------------|\n"
      "	uint RI = src(+2.0, +1.0); //             QA |  A |  B | C  | RC\n"
      "	uint SG = src(-1.0, +2.0); //                |----|----|----|\n"
      "	uint SI = src(+1.0, +2.0); //            [Q] |  D |  E | F  | [R]\n"
      "	uint AA = src(-2.0, -2.0); //                |----|----|----|\n"
      "	uint CC = src(+2.0, -2.0); //             QG |  G |  H | I  | RI\n"
      "	uint GG = src(-2.0, +2.0); //                |----|----|----|\n"
      "	uint II = src(+2.0, +2.0); //             GG    SG    [S]   SI    II\n"
      "\n"
      "\n"
      "    float4 J = vE;    float4 K = vE;    float4 L = vE;    float4 M = vE;\n"
      "\n"
      "\n"
      "    float Bl = luma(vB) + float(B==0u);\n"
      "    float Dl = luma(vD) + float(D==0u);\n"
      "    float El = luma(vE) + float(E==0u);\n"
      "    float Fl = luma(vF) + float(F==0u);\n"
      "    float Hl = luma(vH) + float(H==0u);\n"
      "\n"
      "// 	pre-cal\n"
      "    bool eq_B_D = eq(B,D);\n"
      "    bool eq_B_F = eq(B,F);\n"
      "    bool eq_D_H = eq(D,H);\n"
      "    bool eq_F_H = eq(F,H);\n"
      "\n"
      "\n"
      "    bool oppoPix =  eq_B_H || eq_D_F;\n"
      "\n"
      "    bool slope1 = false;    bool slope2 = false;    bool slope3 = false;    bool slope4 = false;\n"
      "\n"
      "    bool slope1ok = false;  bool slope2ok = false;  bool slope3ok = false;  bool slope4ok = false;\n"
      "    bool slope1end = false;  bool slope2end = false;  bool slope3end = false;  bool slope4end = false;\n"
      "\n"
      "\n"
      "// B - D\n"
      "	if ( (B!=0u && D!=0u) && // xxx.alpha\n"
      "		(!eq_E_B && !eq_E_D && !oppoPix) && (!eq_D_H && !eq_B_F)\n"
      "	 && (eq(E,A) || El>=Dl&&El>=Bl) && ( (El<Dl&&El<Bl) || none_eq2(A,B,D) || neq(E,P) || neq(E,Q) )\n"
      "	 && ( eq_B_D &&(eq(E,A)||eq(B,PC)||eq(D,QG)||sim(vE,vC)||sim(vE,vG)) || simb(vB,vD)&&(eq_F_H||eq(E,C)||eq(E,G)) )\n"
      "		) {\n"
      "		J=admixX(A,B,C,D,E,F,G,H,I\n"
      "				,P,PA,PC,Q,QA,QG,R,RC,RI,S,SG,SI,AA,CC,GG\n"
      "				,El, Bl, Dl, Fl, Hl\n"
      "				,vE, vB, vD, vC, vG\n"
      "				);\n"
      "		slope1 = true;\n"
      "		slope1ok = (J.b < 1.1);\n"
      "		slope1end = (J.b < 3.1);\n"
      "		skiprest = (J.b > 7.1);\n"
      "		J = (J.b > 3.1) ? vE :	\n"
      "			(J.b > 1.1) ? (J - 2.0) :\n"
      "			J;\n"
      "	}\n"
      "// B - F\n"
      "	if ( !slope1 && (B!=0u && F!=0u)\n"
      "	 && (!eq_E_B && !eq_E_F && !oppoPix) && (!eq_B_D && !eq_F_H)\n"
      "	 && (eq(E,C) || El>=Bl&&El>=Fl) && ( (El<Bl&&El<Fl) || none_eq2(C,B,F) || neq(E,P) || neq(E,R) )\n"
      "	 && ( eq_B_F &&(eq(E,C)||eq(B,PA)||eq(F,RI)||sim(vE,vA)||sim(vE,vI)) || simb(vB,vF)&&(eq_D_H||eq(E,A)||eq(E,I)) ) \n"
      "	 ) {\n"
      "		K=admixX(C,F,I,B,E,H,A,D,G\n"
      "				,R,RC,RI,P,PC,PA,S,SI,SG,Q,QA,QG,CC,II,AA\n"
      "				,El,Fl,Bl,Hl,Dl\n"
      "				,vE,vF,vB,vI,vA\n"
      "				);\n"
      "		slope2 = true;\n"
      "		slope2ok = (K.b < 1.1);\n"
      "		slope2end = (K.b < 3.1);\n"
      "		skiprest = (K.b > 7.1);\n"
      "		K = (K.b > 3.1) ? vE :	\n"
      "			(K.b > 1.1) ? (K - 2.0) :\n"
      "			K;\n"
      "	}\n"
      "// D - H\n"
      "	if ( !slope1 && !skiprest && (D!=0u && H!=0u)\n"
      "	 && (!eq_E_D && !eq_E_H && !oppoPix) && (!eq_F_H && !eq_B_D)\n"
      "	 && (eq(E,G) || El>=Hl&&El>=Dl)  &&  ((El<Hl&&El<Dl) || none_eq2(G,D,H) || neq(E,S) || neq(E,Q))\n"
      "	 &&	( eq_D_H &&(eq(E,G)||eq(D,QA)||eq(H,SI)||sim(vE,vA)||sim(vE,vI)) || simb(vD,vH)&&(eq_B_F||eq(E,A)||eq(E,I)) )\n"
      "	 ) {\n"
      "		L=admixX(G,D,A,H,E,B,I,F,C\n"
      "				,Q,QG,QA,S,SG,SI,P,PA,PC,R,RI,RC,GG,AA,II\n"
      "				,El,Dl,Hl,Bl,Fl\n"
      "				,vE,vD,vH,vA,vI\n"
      "				);\n"
      "		slope3 = true;\n"
      "		slope3ok = (L.b < 1.1);\n"
      "		slope3end = (L.b < 3.1);\n"
      "		skiprest = (L.b > 7.1);\n"
      "		L = (L.b > 3.1) ? vE :	\n"
      "			(L.b > 1.1) ? (L - 2.0) :\n"
      "			L;\n"
      "	}\n"
      "// F - H\n"
      "	if ( !slope2 && !slope3 && !skiprest && (F!=0u && H!=0u)\n"
      "	 && (!eq_E_F && !eq_E_H && !oppoPix) && (!eq_B_F && !eq_D_H)\n"
      "	 && (eq(E,I) || El>=Fl&&El>=Hl)  &&  ((El<Fl&&El<Hl) || none_eq2(I,F,H) || neq(E,R) || neq(E,S))\n"
      "	 && ( eq_F_H &&(eq(E,I)||eq(F,RC)||eq(H,SG)||sim(vE,vC)||sim(vE,vG)) || simb(vF,vH)&&(eq_B_D||eq(E,C)||eq(E,G)) )\n"
      "	  ) {\n"
      "		M=admixX(I,H,G,F,E,D,C,B,A\n"
      "				,S,SI,SG,R,RI,RC,Q,QG,QA,P,PC,PA,II,GG,CC\n"
      "				,El,Hl,Fl,Dl,Bl\n"
      "				,vE,vH,vF,vG,vC\n"
      "				);\n"
      "		slope4 = true;\n"
      "		slope4ok = (M.b < 1.1);\n"
      "		slope4end = (M.b < 3.1);\n"
      "		skiprest = (M.b > 7.1);\n"
      "		M = (M.b > 3.1) ? vE :	\n"
      "			(M.b > 1.1) ? (M - 2.0) :\n"
      "			M;\n"
      "	}\n"
      "\n"
      "\n"
      "//  long gentle 2:1 slope  (P100)\n"
      "\n"
      "	if (slope4ok) { //zone4 long slope\n"
      "\n"
      "		if (all_eq2(R,F,G) && neq(R, RC) && (neq(Q,G)||eq(Q, QA))) {L=admixL(M,L,vH); skiprest = true;}\n"
      "		// vertical\n"
      "		if (all_eq2(S,H,C) && neq(S, SG) && (neq(P,C)||eq(P, PA))) {K=admixL(M,K,vF); skiprest = true;}\n"
      "	}\n"
      "\n"
      "	if (slope3ok) { //zone3 long slope\n"
      "		// horizontal\n"
      "		if (all_eq2(Q,D,I) && neq(Q, QA) && (neq(R,I)||eq(R, RC))) {M=admixL(L,M,vH); skiprest = true;}\n"
      "		// vertical\n"
      "		if (all_eq2(S,H,A) && neq(S, SI) && (neq(A,P)||eq(P, PC))) {J=admixL(L,J,vD); skiprest = true;}\n"
      "	}\n"
      "\n"
      "	if (slope2ok) { //zone2 long slope\n"
      "		// horizontal\n"
      "		if (all_eq2(R,F,A) && neq(R, RI) && (neq(A,Q)||eq(Q, QG))) {J=admixL(K,J,vB); skiprest = true;}\n"
      "		// vertical\n"
      "		if (all_eq2(P,B,I) && neq(P, PA) && (neq(I,S)||eq(S, SG))) {M=admixL(K,M,vF); skiprest = true;}\n"
      "	}\n"
      "\n"
      "	if (slope1ok) { //zone1 long slope\n"
      "		// horizontal\n"
      "		if (all_eq2(Q,D,C) && neq(Q, QG) && (neq(C,R)||eq(R, RI))) {K=admixL(J,K,vB); skiprest = true;}\n"
      "		// vertical\n"
      "		if (all_eq2(P,B,G) && neq(P, PC) && (neq(G,S)||eq(S, SI))) {L=admixL(J,L,vD); skiprest = true;}\n"
      "	}\n"
      "\n"
      "if (!skiprest && !oppoPix) {\n"
      "\n"
      "\n"
      "        // horizontal bottom\n"
      "    if (!eq_E_H && none_eq2(H,A,C)) {\n"
      "\n"
      "        //                                    A B C .\n"
      "        //                                  Q D e f r       Zone 4\n"
      "        //					                g h I\n"
      "        //					                  S\n"
      "        if ( (!slope2 && !eq_B_F) && (!slope3 && !eq_D_H) && (!slope4end && !eq_F_H) && F!=0u &&\n"
      "            !eq_E_F && eq(R,H) && eq(F,G) ) {\n"
      "            M = admixS( A, B, C, D, E, F, G, H, I\n"
      "                      , R, RC, RI, S, SG, SI, II, CC\n"
      "                      , vE, vF, vC\n"
      "                      );\n"
      "            skiprest = true;}\n"
      "\n"
      "        //                                  .  A B C\n"
      "        //                                  q d e F R       Zone 3\n"
      "        //                                     G h i\n"
      "        //					                   S\n"
      "        if ( !skiprest && (!slope1 && !eq_B_D) && (!slope4 && !eq_F_H) && (!slope3end && !eq_D_H) && D!=0u &&\n"
      "             !eq_E_D && eq(Q,H) && eq(D,I) ) {\n"
      "            L = admixS( C, B, A, F, E, D, I, H, G\n"
      "                      , Q, QA, QG, S, SI, SG, GG, AA\n"
      "                      , vE, vD, vA\n"
      "                      );\n"
      "            skiprest = true;}\n"
      "    }\n"
      "\n"
      "    // horizontal up\n"
      "    if ( !skiprest && !eq_E_B && none_eq2(B,G,I)) {\n"
      "\n"
      "        //					                   P\n"
      "        //                                    a b C\n"
      "        //                                  Q D e f r       Zone 2\n"
      "        //                                    G H  I  .\n"
      "        if ( (!slope1 && !eq_B_D)  && (!slope4 && !eq_F_H) && (!slope2end && !eq_B_F) && F!=0u &&\n"
      "              !eq_E_F && eq(B,R) && eq(A,F) ) {\n"
      "            K = admixS( G, H, I, D, E, F, A, B, C\n"
      "                      , R, RI, RC, P, PA, PC, CC, II\n"
      "                      , vE, vF, vI\n"
      "                      );\n"
      "            skiprest = true;}\n"
      "\n"
      "        //					                  P\n"
      "        //                                    A B C\n"
      "        //                                 Q D E F R        Zone 1\n"
      "        //                                  . G H I\n"
      "        if ( !skiprest && (!slope2 && !eq_B_F) && (!slope3 && !eq_D_H) && (!slope1end && !eq_B_D) && D!=0u &&\n"
      "             !eq_E_D && eq(B,Q) && eq(C,D) ) {\n"
      "            J = admixS( I, H, G, F, E, D, C, B, A\n"
      "                      , Q, QG, QA, P, PC, PA, AA, GG\n"
      "                      , vE, vD, vG\n"
      "                      );\n"
      "            skiprest = true;}\n"
      "\n"
      "    }\n"
      "\n"
      "    // vertical left\n"
      "    if ( !skiprest && !eq_E_D && none_eq2(D,C,I) ) {\n"
      "\n"
      "        //                                    A B C\n"
      "        //                                  Q D E F R\n"
      "        //                                    G H I        Zone 3\n"
      "        //                                       S .\n"
      "        if ( (!slope1 && !eq_B_D) && (!slope4 && !eq_F_H) && (!slope3end && !eq_D_H) && H!=0u &&\n"
      "              !eq_E_H && eq(D,S) && eq(A,H) ) {\n"
      "            L = admixS( C, F, I, B, E, H, A, D, G\n"
      "                      , S, SI, SG, Q, QA, QG, GG, II\n"
      "                      , vE, vH, vI\n"
      "                      );\n"
      "            skiprest = true;}\n"
      "\n"
      "        //                                      P .\n"
      "        //                                    A B C\n"
      "        //                                  Q D E F R       Zone 1\n"
      "        //                                    G H I\n"
      "        if ( !skiprest && (!slope3 && !eq_D_H) && (!slope2 && !eq_B_F) && (!slope1end && !eq_B_D) && B!=0u &&\n"
      "              !eq_E_B && eq(P,D) && eq(B,G) ) {\n"
      "            J = admixS( I, F, C, H, E, B, G, D, A\n"
      "                      , P, PC, PA, Q, QG, QA, AA, CC\n"
      "                      , vE, vB, vC\n"
      "                      );\n"
      "            skiprest = true;}\n"
      "\n"
      "    }\n"
      "\n"
      "    // vertical right\n"
      "    if ( !skiprest && !eq_E_F && none_eq2(F,A,G) ) { // right\n"
      "\n"
      "        //                                    A B C\n"
      "        //                                  Q D E F R\n"
      "        //                                    G H I        Zone 4\n"
      "        //                                    . S\n"
      "        if ( (!slope2 && !eq_B_F) && (!slope3 && !eq_D_H) && (!slope4end && !eq_F_H) && H!=0u &&\n"
      "              !eq_E_H && eq(S,F) && eq(H,C) ) {\n"
      "            M = admixS( A, D, G, B, E, H, C, F\n"
      "                      , I, S, SG, SI, R, RC, RI, II, GG\n"
      "                      , vE, vH, vG\n"
      "                      );\n"
      "            skiprest = true;}\n"
      "\n"
      "        //                                    . P\n"
      "        //                                    A B C\n"
      "        //                                  Q D E F R        Zone 2\n"
      "        //                                    G H I\n"
      "        if ( !skiprest && (!slope1 && !eq_B_D) && (!slope4 && !eq_F_H) && (!slope2end && !eq_B_F) && B!=0u &&\n"
      "             !eq_E_B && eq(P,F) && eq(B,I) ) {\n"
      "            K = admixS( G, D, A, H, E, B, I, F, C\n"
      "                      , P, PA, PC, R, RI, RC, CC, AA\n"
      "                      , vE, vB, vA\n"
      "                      );\n"
      "            skiprest = true;}\n"
      "\n"
      "    } // vertical right\n"
      "} // sawslope\n"
      "\n"
      "\n"
      "skiprest = skiprest||slope1||slope2||slope3||slope4||E==0u||B==0u||D==0u||F==0u||H==0u;\n"
      "\n" */
     "*************************************************\n"
      "        Concave + Cross	(P100)\n"
      " ************************************************\n"
      /* "\n"
      "float4 vT;\n"
      "\n"
      "if (!skiprest &&\n"
      "    Bl<El && !eq_E_D && !eq_E_F && eq_E_H && none_eq2(E,A,C) && all_eq2(G,H,I) && vi_sim(vE,E,S) ) { // TOP\n"
      "\n"
      "    if (eq_B_D||eq_B_F) { J=admixC(vB,J);    K=J;\n"
      "        if (eq_D_F) { L=mix(J,L, 0.61804);   M=L; }\n"
      "    } else { vT = El-Bl < abs(El-Dl) ? vB : vD;  J=admixC(vT,J);\n"
      "            if (eq_D_F) { K=J;  L=mix(J,L, 0.61804);    M=L; }\n"
      "            else {vT = El-Bl < abs(El-Fl) ? vB : vF; 		K=admixC(vT,K); }\n"
      "           }\n"
      "\n"
      "   skiprest = true;\n"
      "}\n"
      "\n"
      "if (!skiprest &&\n"
      "    Hl<El && !eq_E_D && !eq_E_F && eq_E_B && none_eq2(E,G,I) && all_eq2(A,B,C) && vi_sim(vE,E,P) ) { // BOTTOM\n"
      "\n"
      "    if (eq_D_H||eq_F_H) { L=admixC(vH,L);    M=L;\n"
      "        if (eq_D_F) { J=mix(L,J, 0.61804);   K=J; }\n"
      "    } else { vT = El-Hl < abs(El-Dl) ? vH : vD;  L=admixC(vT,L);\n"
      "            if (eq_D_F) { M=L;  J=mix(L,J, 0.61804);    K=J; }\n"
      "            else { vT = El-Hl < abs(El-Fl) ? vH : vF;    M=admixC(vT,M); }\n"
      "           }\n"
      "\n"
      "   skiprest = true;\n"
      "}\n"
      "\n"
      "if (!skiprest &&\n"
      "    Fl<El && !eq_E_B && !eq_E_H && eq_E_D && none_eq2(E,C,I) && all_eq2(A,D,G) && vi_sim(vE,E,Q) ) { // RIGHT\n"
      "\n"
      "    if (eq_B_F||eq_F_H) { K=admixC(vF,K);    M=K;\n"
      "        if (eq_B_H) { J=mix(K,J, 0.61804);   L=J; }\n"
      "    } else { vT = El-Fl < abs(El-Bl) ? vF : vB;  K=admixC(vT,K);\n"
      "            if (eq_B_H) { M=K;  J=mix(K,J, 0.61804);    L=J; }\n"
      "            else { vT = El-Fl < abs(El-Hl) ? vF : vH;    M=admixC(vT,M); }\n"
      "           }\n"
      "\n"
      "   skiprest = true;\n"
      "}\n"
      "\n"
      "if (!skiprest &&\n"
      "    Dl<El && !eq_E_B && !eq_E_H && eq_E_F && none_eq2(E,A,G) && all_eq2(C,F,I) && vi_sim(vE,E,R) ) { // LEFT\n"
      "\n"
      "    if (eq_B_D||eq_D_H) { J=admixC(vD,J);    L=J;\n"
      "        if (eq_B_H) { K=mix(J,K, 0.61804);   M=K; }\n"
      "    } else { vT = El-Dl < abs(El-Bl) ? vD : vB;  J=admixC(vT,J);\n"
      "            if (eq_B_H) { L=J;   K=mix(J,K, 0.61804);    M=K; }\n"
      "            else { vT = El-Dl < abs(El-Hl) ? vD : vH;    L=admixC(vT,L); }\n"
      "           }\n"
      "\n"
      "   skiprest = true;\n"
      "}\n"
      "\n" */
     "\n"
      "     XO\n"
      "  OOOX\n"
      "     XO\n"
      "\n"
      "\n"
      "\n"
      "if (!skiprest && !eq_E_F&&eq_E_D&&eq_B_F&&eq_F_H && all_eq2(E,C,I) && (eq(E,Q)||El>Fl) && neq(F,src(+3.0, 0.0)) ) {K=admixK(vF,K); M=K;skiprest=true;}	// RIGHT\n"
      "if (!skiprest && !eq_E_D&&eq_E_F&&eq_B_D&&eq_D_H && all_eq2(E,A,G) && (eq(E,R)||El>Dl) && neq(D,src(-3.0, 0.0)) ) {J=admixK(vD,J); L=J;skiprest=true;}	// LEFT\n"
      "if (!skiprest && !eq_E_H&&eq_E_B&&eq_D_H&&eq_F_H && all_eq2(E,G,I) && (eq(E,P)||El>Hl) && neq(H,src(0.0, +3.0)) ) {L=admixK(vH,L); M=L;skiprest=true;}	// BOTTOM\n"
      "if (!skiprest && !eq_E_B&&eq_E_H&&eq_B_D&&eq_B_F && all_eq2(E,A,C) && (eq(E,S)||El>Bl) && neq(B,src(0.0, -3.0)) ) {J=admixK(vB,J); K=J;}				// TOP\n"
      "\n"
      "\n"
      "	//final write\n"
      "	float2 fpart = frac(coords);\n"
      "\n"
      "	float4 res = (fpart.x < 0.5) ? ((fpart.y < 0.5) ? J : L) : ((fpart.y < 0.5) ? K : M);\n"
      "\n"
      "	ialpha = step(0.002, res.r+res.g+res.b+res.a);\n"
      "	texcol = res;\n"
      "}\n"
      "	\n"
      "}\n"
      "\n"
      "#undef src\n" 
    );
  }
  else if (texture_filter == GPU_TEXTURE_FILTER_SCALE2X)
  {
    SS_LIT(ss,
      "#define src(xoffs, yoffs) packUnorm4x8(SampleFromVRAM(texpage, bcoords + float2((xoffs), (yoffs)), uv_limits))\n"
      "\n"
      "void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits, out float4 texcol, out float ialpha)\n"
      "{\n"
      "	float2 bcoords = floor(coords);\n"
      "\n"
      "	uint E = src(+0, +0);\n"
      "	uint B = src(+0, - 1);\n"
      "	uint D = src(-1, +0);\n"
      "	uint F = src(+1, +0);\n"
      "	uint H = src(+0, +1);\n"
      "\n"
      "	uint J = (D == B && B != F && D != H) ? D : E;\n"
      "	uint K = (B == F && D != F && H != F) ? F : E;\n"
      "	uint L = (H == D && F != D && B != D) ? D : E;\n"
      "	uint M = (H == F && D != H && B != F) ? F : E;\n"
      "\n"
      "	// select quadrant based on fractional part of texture coordinates\n"
      "	float2 fpart = frac(coords);\n"
      "	uint res = (fpart.x < 0.5f) ? ((fpart.y < 0.5f) ? J : L) : ((fpart.y < 0.5f) ? K : M);\n"
      "\n"
      "	ialpha = float(res != 0u);\n"
      "	texcol = unpackUnorm4x8(res);\n"
      "}\n"
      "\n"
      "#undef src\n" 
    );
  }
  else if (texture_filter == GPU_TEXTURE_FILTER_SCALE3X)
  {
    SS_LIT(ss,
      "#define src(xoffs, yoffs) packUnorm4x8(SampleFromVRAM(texpage, bcoords + float2((xoffs), (yoffs)), uv_limits))\n"
      "\n"
      "void FilteredSampleFromVRAM(TEXPAGE_VALUE texpage, float2 coords, float4 uv_limits, out float4 texcol, out float ialpha)\n"
      "{\n"
      "	float2 bcoords = floor(coords);\n"
      "\n"
      "	uint E = src(+0, +0);\n"
      "	uint B = src(+0, -1);\n"
      "	uint D = src(-1, +0);\n"
      "	uint F = src(+1, +0);\n"
      "	uint H = src(+0, +1);\n"
      "\n"
      "	uint res = E;\n"
      "	if (B != H && D != F) {\n"
      "		uint A = src(-1, -1);\n"
      "		uint C = src(+1, -1);\n"
      "		uint G = src(-1, +1);\n"
      "		uint I = src(+1, +1);\n"
      "\n"
      "		uint E0 = (D == B) ? D : E;\n"
      "		uint E1 = (D == B && E != C) || (B == F && E != A) ? B : E;\n"
      "		uint E2 = (B == F) ? F : E;\n"
      "		uint E3 = (D == B && E != G) || (D == H && E != A) ? D : E;\n"
      "		uint E4 = E;\n"
      "		uint E5 = (B == F && E != I) || (H == F && E != C) ? F : E;\n"
      "		uint E6 = (D == H) ? D : E;\n"
      "		uint E7 = (D == H && E != I) || (H == F && E != G) ? H : E;\n"
      "		uint E8 = (H == F) ? F : E;\n"
      "\n"
      "		// select quadrant based on fractional part of texture coordinates\n"
      "		float2 fpart = frac(coords);\n"
      "		uint R0, R1, R2;\n"
      "		if (fpart.y < 0.34f) {\n"
      "			R0 = E0;\n"
      "			R1 = E1;\n"
      "			R2 = E2;\n"
      "		} else if (fpart.y < 0.67f) {\n"
      "			R0 = E3;\n"
      "			R1 = E4;\n"
      "			R2 = E5;\n"
      "		} else {\n"
      "			R0 = E6;\n"
      "			R1 = E7;\n"
      "			R2 = E8;\n"
      "		}\n"
      "\n"
      "		res = (fpart.x < 0.34f) ? R0 : ((fpart.x < 0.67f) ? R1 : R2);\n"
      "	}\n"
      "\n"
      "	ialpha = float(res != 0u);\n"
      "	texcol = unpackUnorm4x8(res);\n"
      "}\n"
      "\n"
      "#undef src\n"
    );
  }
}

char* gpu_hw_shadergen_generate_screen_vertex_shader(gpu_hw_shadergen_t* sg)
{
  small_string_t ss; small_string_init(&ss);
  wh(sg, &ss);
  static const char* attrs[] = { "float2 a_pos", "float2 a_tex0" };
  dvep(sg, &ss, attrs, 2, 0, 1, NULL, 0, false, "", false, false, false);
  SS_LIT(&ss,
    "\n"
    "{\n"
    "  // Depth set to 1 for PGXP depth buffer.\n"
    "  v_pos = float4(a_pos, 1.0f, 1.0f);\n"
    "  v_tex0 = a_tex0;\n"
    "\n"
    "  // NDC space Y flip in Vulkan.\n"
    "  #if API_OPENGL || API_OPENGL_ES || API_VULKAN\n"
    "    v_pos.y = -v_pos.y;\n"
    "  #endif\n" 
    "}\n");
  return take_string(&ss);
}

char* gpu_hw_shadergen_generate_batch_vertex_shader(gpu_hw_shadergen_t* sg,
                                                    bool upscaled, bool msaa, bool per_sample_shading,
                                                    bool textured, bool palette, bool page_texture,
                                                    bool uv_limits, bool force_round_texcoords,
                                                    bool pgxp_depth, bool disable_color_perspective)
{
  small_string_t ss; small_string_init(&ss);
  wh(sg, &ss);
  dm_b(sg, &ss, "TEXTURED", textured);
  dm_b(sg, &ss, "PALETTE", palette);
  dm_b(sg, &ss, "PAGE_TEXTURE", page_texture);
  dm_b(sg, &ss, "UV_LIMITS", uv_limits);
  dm_b(sg, &ss, "FORCE_ROUND_TEXCOORDS", force_round_texcoords);
  dm_b(sg, &ss, "PGXP_DEPTH", pgxp_depth);
  dm_b(sg, &ss, "UPSCALED", upscaled);

  write_batch_uniform_buffer(sg, &ss);

  if (textured && page_texture)
  {
    if (uv_limits)
    {
      static const char* attrs[] = {
        "float4 a_pos", "float4 a_col0", "uint a_texcoord", "uint a_texpage", "float4 a_uv_limits"
      };
      static const shadergen_io_pair_t addl[] = { { "nointerpolation", "float4 v_uv_limits" } };
      dvep(sg, &ss, attrs, 5, 1, 1, addl, 1, false, "", msaa, per_sample_shading, disable_color_perspective);
    }
    else
    {
      static const char* attrs[] = {
        "float4 a_pos", "float4 a_col0", "uint a_texcoord", "uint a_texpage"
      };
      dvep(sg, &ss, attrs, 4, 1, 1, NULL, 0, false, "", msaa, per_sample_shading, disable_color_perspective);
    }
  }
  else if (textured)
  {
    if (uv_limits)
    {
      static const char* attrs[] = {
        "float4 a_pos", "float4 a_col0", "uint a_texcoord", "uint a_texpage", "float4 a_uv_limits"
      };
      const shadergen_io_pair_t addl[] = {
        { "nointerpolation", palette ? "uint4 v_texpage" : "uint2 v_texpage" },
        { "nointerpolation", "float4 v_uv_limits" },
      };
      dvep(sg, &ss, attrs, 5, 1, 1, addl, 2, false, "", msaa, per_sample_shading, disable_color_perspective);
    }
    else
    {
      static const char* attrs[] = {
        "float4 a_pos", "float4 a_col0", "uint a_texcoord", "uint a_texpage"
      };
      const shadergen_io_pair_t addl[] = {
        { "nointerpolation", palette ? "uint4 v_texpage" : "uint2 v_texpage" },
      };
      dvep(sg, &ss, attrs, 4, 1, 1, addl, 1, false, "", msaa, per_sample_shading, disable_color_perspective);
    }
  }
  else
  {
    static const char* attrs[] = { "float4 a_pos", "float4 a_col0" };
    dvep(sg, &ss, attrs, 2, 1, 0, NULL, 0, false, "", msaa, per_sample_shading, disable_color_perspective);
  }

  SS_LIT(&ss,
    "\n"
    "{\n"
    "  // Offset the vertex position by 0.5 to ensure correct interpolation of texture coordinates\n"
    "  // at 1x resolution scale. This doesn't work at >1x, we adjust the texture coordinates before\n"
    "  // uploading there instead.\n"
    "  float vertex_offset = (UPSCALED == 0) ? 0.5 : 0.0;\n"
    "\n"
    "  // 0..+1023 -> -1..1\n"
    "  float pos_x = ((a_pos.x + vertex_offset) / 512.0) - 1.0;\n"
    "  float pos_y = ((a_pos.y + vertex_offset) / -256.0) + 1.0;\n"
    "\n"
    "#if PGXP_DEPTH\n"
    "  // Ignore mask Z when using PGXP depth.\n"
    "  float pos_z = a_pos.w;\n"
    "  float pos_w = a_pos.w;\n"
    "#else\n"
    "  float pos_z = a_pos.z;\n"
    "  float pos_w = a_pos.w;\n"
    "#endif\n"
    "\n"
    "#if API_OPENGL || API_OPENGL_ES\n"
    "  // 0..1 to -1..1 depth range.\n"
    "  pos_z = (pos_z * 2.0) - 1.0;\n"
    "#endif\n"
    "\n"
    "  // NDC space Y flip in Vulkan.\n"
    "#if API_OPENGL || API_OPENGL_ES || API_VULKAN\n"
    "  pos_y = -pos_y;\n"
    "#endif\n"
    "\n"
    "  v_pos = float4(pos_x * pos_w, pos_y * pos_w, pos_z * pos_w, pos_w);\n"
    "\n"
    "  v_col0 = a_col0;\n"
    "  #if TEXTURED\n"
    "    v_tex0 = float2(uint2(a_texcoord & 0xFFFFu, a_texcoord >> 16));\n"
    "    #if !PALETTE && !PAGE_TEXTURE\n"
    "      v_tex0 *= u_resolution_scale;\n"
    "    #endif\n"
    "\n"
    "    #if !PAGE_TEXTURE\n"
    "      // base_x,base_y,palette_x,palette_y\n"
    "      v_texpage.x = (a_texpage & 15u) * 64u;\n"
    "      v_texpage.y = ((a_texpage >> 4) & 1u) * 256u;\n"
    "      #if PALETTE\n"
    "        v_texpage.z = ((a_texpage >> 16) & 63u) * 16u;\n"
    "        v_texpage.w = ((a_texpage >> 22) & 511u);\n"
    "      #endif\n"
    "    #endif\n"
    "\n"
    "    #if UV_LIMITS\n"
    "      v_uv_limits = a_uv_limits * 255.0;\n"
    "\n"
    "      #if FORCE_ROUND_TEXCOORDS && PALETTE\n"
    "        // Add 0.5 to the upper bounds when upscaling, to work around interpolation differences.\n"
    "        // Limited to force-round-texcoord hack, to avoid breaking other games.\n"
    "        v_uv_limits.zw += 0.5;\n"
    "      #elif !PAGE_TEXTURE && !PALETTE\n"
    "        // Treat coordinates as being in upscaled space, and extend the UV range to all \"upscaled\"\n"
    "        // pixels. This means 1-pixel-high polygon-based framebuffer effects won't be downsampled.\n"
    "        // (e.g. Mega Man Legends 2 haze effect)\n"
    "        v_uv_limits *= u_resolution_scale;\n"
    "        v_uv_limits.zw += u_resolution_scale_minus_one;\n"
    "      #endif\n"
    "    #endif\n"
    "  #endif\n"
    "}\n");

  return take_string(&ss);
}

 char* gpu_hw_shadergen_generate_batch_fragment_shader(
  gpu_hw_shadergen_t* sg,
  gpu_hw_batch_render_mode_t render_mode, gpu_transparency_mode_t transparency,
  gpu_hw_batch_texture_mode_t texture_mode, gpu_texture_filter_t texture_filtering, 
  bool is_blended_texture_filtering, bool upscaled, bool msaa, bool per_sample_shading,
  bool uv_limits, bool force_round_texcoords, bool modulation_crop, bool true_color,
  bool dithering, bool scaled_dithering, bool disable_color_perspective, bool interlacing,
  bool scaled_interlacing, bool check_mask, bool write_mask_as_depth, bool use_rov,
  bool use_rov_depth, bool rov_depth_test, bool rov_depth_write)
{
  DebugAssert(!true_color || !dithering); /* Should not be doing dithering+true color. */
  DebugAssert(transparency == GPU_TRANSPARENCY_MODE_DISABLED ||
              render_mode == GPU_HW_BATCH_RENDER_MODE_SHADER_BLEND);
  DebugAssert((!rov_depth_test && !rov_depth_write) || (use_rov && use_rov_depth));

  const bool textured = (texture_mode != GPU_HW_BATCH_TEXTURE_MODE_DISABLED);
  const bool palette =
    (texture_mode == GPU_HW_BATCH_TEXTURE_MODE_PALETTE_4BIT ||
     texture_mode == GPU_HW_BATCH_TEXTURE_MODE_PALETTE_8BIT);
  const bool page_texture = (texture_mode == GPU_HW_BATCH_TEXTURE_MODE_PAGE_TEXTURE);
  const bool shader_blending = (render_mode == GPU_HW_BATCH_RENDER_MODE_SHADER_BLEND);
  const bool use_dual_source = (!shader_blending && !use_rov && sg->base.supports_dual_source_blend &&
                                ((render_mode != GPU_HW_BATCH_RENDER_MODE_TRANSPARENCY_DISABLED &&
                                  render_mode != GPU_HW_BATCH_RENDER_MODE_ONLY_OPAQUE) || 
                                 is_blended_texture_filtering));

  small_string_t ss; small_string_init(&ss);
  wh3(sg, &ss, use_rov, shader_blending && !use_rov, use_dual_source);
  dm_b(sg, &ss, "TRANSPARENCY", render_mode != GPU_HW_BATCH_RENDER_MODE_TRANSPARENCY_DISABLED);
  dm_b(sg, &ss, "TRANSPARENCY_ONLY_OPAQUE", render_mode == GPU_HW_BATCH_RENDER_MODE_ONLY_OPAQUE);
  dm_b(sg, &ss, "TRANSPARENCY_ONLY_TRANSPARENT", render_mode == GPU_HW_BATCH_RENDER_MODE_ONLY_TRANSPARENT);
  dm_i(sg, &ss, "TRANSPARENCY_MODE", (s32)transparency);
  dm_b(sg, &ss, "SHADER_BLENDING", shader_blending);
  dm_b(sg, &ss, "CHECK_MASK_BIT", check_mask);
  dm_b(sg, &ss, "TEXTURED", textured);
  dm_b(sg, &ss, "PALETTE", palette);
  dm_b(sg, &ss, "PALETTE_4_BIT", texture_mode == GPU_HW_BATCH_TEXTURE_MODE_PALETTE_4BIT);
  dm_b(sg, &ss, "PALETTE_8_BIT", texture_mode == GPU_HW_BATCH_TEXTURE_MODE_PALETTE_8BIT);
  dm_b(sg, &ss, "PAGE_TEXTURE", page_texture);
  dm_b(sg, &ss, "DITHERING", dithering);
  dm_b(sg, &ss, "DITHERING_SCALED", dithering && scaled_dithering);
  dm_b(sg, &ss, "INTERLACING", interlacing);
  dm_b(sg, &ss, "INTERLACING_SCALED", interlacing && scaled_interlacing);
  dm_b(sg, &ss, "MODULATION_CROP", modulation_crop);
  dm_b(sg, &ss, "TRUE_COLOR", true_color);
  dm_b(sg, &ss, "TEXTURE_FILTERING", texture_filtering != GPU_TEXTURE_FILTER_NEAREST);
  dm_b(sg, &ss, "TEXTURE_ALPHA_BLENDING", is_blended_texture_filtering);
  dm_b(sg, &ss, "UV_LIMITS", uv_limits);
  dm_b(sg, &ss, "USE_ROV", use_rov);
  dm_b(sg, &ss, "USE_ROV_DEPTH", use_rov_depth);
  dm_b(sg, &ss, "ROV_DEPTH_TEST", rov_depth_test);
  dm_b(sg, &ss, "ROV_DEPTH_WRITE", rov_depth_write);
  dm_b(sg, &ss, "USE_DUAL_SOURCE", use_dual_source);
  dm_b(sg, &ss, "WRITE_MASK_AS_DEPTH", write_mask_as_depth);
  dm_b(sg, &ss, "FORCE_ROUND_TEXCOORDS", force_round_texcoords);
  dm_b(sg, &ss, "UPSCALED", upscaled);

  /* Used for converting to normalized coordinates for sampling. */
  ss_appendf(&ss, "CONSTANT float2 RCP_VRAM_SIZE = float2(1.0 / float(%d), 1.0 / float(%d));\n",
             (int)VRAM_WIDTH, (int)VRAM_HEIGHT);

  write_color_conversion_functions(sg, &ss);
  write_batch_uniform_buffer(sg, &ss);
  dt(sg, &ss, "samp0", 0, false);

  if (use_rov)
  {
    di(sg, &ss, "rov_color", 0, false);
    if (use_rov_depth)
      di(sg, &ss, "rov_depth", 1, true);
  }

  if (sg->base.glsl)
    SS_LIT(&ss, "CONSTANT int[16] s_dither_values = int[16]( ");
  else
    SS_LIT(&ss, "CONSTANT int s_dither_values[] = {");
  for (u32 i = 0; i < 16; i++)
  {
    if (i > 0) SS_LIT(&ss, ", ");
    ss_appendf(&ss, "%d", DITHER_MATRIX[i / 4][i % 4]);
  }
  if (sg->base.glsl)
    SS_LIT(&ss, " );\n");
  else
    SS_LIT(&ss, "};\n");

  SS_LIT(&ss,
    "\n"
    "uint3 ApplyDithering(uint2 coord, uint3 icol)\n"
    "{\n"
    "  #if (DITHERING_SCALED != 0 || UPSCALED == 0)\n"
    "    uint2 fc = coord & uint2(3u, 3u);\n"
    "  #else\n"
    "    uint2 fc = uint2(float2(coord) * u_rcp_resolution_scale) & uint2(3u, 3u);\n"
    "  #endif\n"
    "  int offset = s_dither_values[fc.y * 4u + fc.x];\n"
    "  return uint3(clamp((int3(icol) + offset) >> 3, 0, 31));\n"
    "}\n"
    "\n"
    "#if TEXTURED\n"
    "CONSTANT float4 TRANSPARENT_PIXEL_COLOR = float4(0.0, 0.0, 0.0, 0.0);\n"
    "\n"
    "#if PALETTE\n"
    "  #define TEXPAGE_VALUE uint4\n"
    "#else\n"
    "  #define TEXPAGE_VALUE uint2\n"
    "#endif\n"
    "\n"
    "#if UV_LIMITS\n"
    "  #define DECLARE_UV_LIMITS(coords, uv_limits) coords, uv_limits\n"
    "  #define APPLY_UV_LIMITS(coords, uv_limits) clamp((coords), uv_limits.xy, uv_limits.zw)\n"
    "#else\n"
    "  #define DECLARE_UV_LIMITS(coords, uv_limits) coords\n"
    "  #define APPLY_UV_LIMITS(coords, uv_limits) (coords)\n"
    "#endif\n"
    "\n"
    "uint2 ApplyTextureWindow(uint2 coords)\n"
    "{\n"
    "  uint x = (uint(coords.x) & u_texture_window_and.x) | u_texture_window_or.x;\n"
    "  uint y = (uint(coords.y) & u_texture_window_and.y) | u_texture_window_or.y;\n"
    "  return uint2(x, y);\n"
    "}\n"
    "\n"
    "uint2 FloatToIntegerCoords(DECLARE_UV_LIMITS(float2 coords, float4 uv_limits))\n"
    "{\n"
    "  // With the vertex offset applied at 1x resolution scale, we want to round the texture coordinates.\n"
    "  // Floor them otherwise, as it currently breaks when upscaling as the vertex offset is not applied.\n"
    "  // Apply UV limits in the opposite order, because flooring will never round up, whereas rounding can.\n"
    "#if UPSCALED == 0 || FORCE_ROUND_TEXCOORDS != 0\n"
    "  float2 rounded_coords = roundEven(coords);\n"
    "  float2 clamped_coords = APPLY_UV_LIMITS(rounded_coords, uv_limits);\n"
    "  return uint2(clamped_coords);\n"
    "#else\n"
    "  float2 clamped_coords = APPLY_UV_LIMITS(coords, uv_limits);\n"
    "  float2 floored_coords = floor(clamped_coords);\n"
    "  return uint2(floored_coords);\n"
    "#endif\n"
    "}\n"
    "\n"
    "#if PAGE_TEXTURE\n"
    "\n"
    "float4 SampleFromPageTexture(DECLARE_UV_LIMITS(float2 coords, float4 uv_limits))\n"
    "{\n"
    "  // Cached textures.\n"
    "  uint2 icoord = ApplyTextureWindow(FloatToIntegerCoords(DECLARE_UV_LIMITS(coords, uv_limits)));\n"
    "#if UPSCALED\n"
    "  float2 fpart = frac(coords);\n"
    "  coords = (float2(icoord) + fpart);\n"
    "#else\n"
    "  // Drop fractional part.\n"
    "  coords = float2(icoord);\n"
    "#endif\n"
    "\n"
    "  // Normalize.\n"
    "  coords = coords * (1.0f / 256.0f);\n"
    "  return SAMPLE_TEXTURE(samp0, coords);\n"
    "}\n"
    "\n"
    "#endif\n"
    "\n"
    "#if !PAGE_TEXTURE || TEXTURE_FILTERING\n"
    "\n"
    "float4 SampleFromVRAM(TEXPAGE_VALUE texpage, DECLARE_UV_LIMITS(float2 coords, float4 uv_limits))\n"
    "{\n"
    "  #if PAGE_TEXTURE\n"
    "    return SampleFromPageTexture(DECLARE_UV_LIMITS(coords, uv_limits));\n"
    "  #elif PALETTE\n"
    "    uint2 icoord = ApplyTextureWindow(FloatToIntegerCoords(DECLARE_UV_LIMITS(coords, uv_limits)));\n"
    "\n"
    "    uint2 vicoord;\n"
    "    #if PALETTE_4_BIT\n"
    "      // 4bit will never wrap, since it's in the last texpage row.\n"
    "      vicoord = uint2(texpage.x + (icoord.x / 4u), texpage.y + icoord.y);\n"
    "    #elif PALETTE_8_BIT\n"
    "      // 8bit can wrap in the X direction.\n"
    "      vicoord = uint2((texpage.x + (icoord.x / 2u)) & 0x3FFu, texpage.y + icoord.y);\n"
    "    #endif\n"
    "\n"
    "    // load colour/palette\n"
    "    // use texelFetch()/load for native resolution to work around point sampling precision\n"
    "    // in some drivers, such as older AMD and Mali Midgard\n"
    "    #if !UPSCALED\n"
    "      float4 texel = LOAD_TEXTURE(samp0, int2(vicoord), 0);\n"
    "    #else\n"
    "      float4 texel = SAMPLE_TEXTURE_LEVEL(samp0, float2(vicoord) * RCP_VRAM_SIZE, 0.0);\n"
    "    #endif\n"
    "    uint vram_value = RGBA8ToRGBA5551(texel);\n"
    "\n"
    "    // apply palette\n"
    "    #if PALETTE_4_BIT\n"
    "      uint subpixel = icoord.x & 3u;\n"
    "      uint palette_index = (vram_value >> (subpixel * 4u)) & 0x0Fu;\n"
    "      uint2 palette_icoord = uint2((texpage.z + palette_index), texpage.w);\n"
    "    #elif PALETTE_8_BIT\n"
    "      // can only wrap in X direction for 8-bit, 4-bit will fit in texpage size.\n"
    "      uint subpixel = icoord.x & 1u;\n"
    "      uint palette_index = (vram_value >> (subpixel * 8u)) & 0xFFu;\n"
    "      uint2 palette_icoord = uint2(((texpage.z + palette_index) & 0x3FFu), texpage.w);\n"
    "    #endif\n"
    "\n"
    "    #if !UPSCALED\n"
    "      return LOAD_TEXTURE(samp0, int2(palette_icoord), 0);\n"
    "    #else\n"
    "      return SAMPLE_TEXTURE_LEVEL(samp0, float2(palette_icoord) * RCP_VRAM_SIZE, 0.0);\n"
    "    #endif\n"
    "  #else\n"
    "    // Direct texturing - usually render-to-texture effects.\n"
    "    #if !UPSCALED\n"
    "      uint2 icoord = ApplyTextureWindow(FloatToIntegerCoords(DECLARE_UV_LIMITS(coords, uv_limits)));\n"
    "      uint2 vicoord = (texpage.xy + icoord) & uint2(1023, 511);\n"
    "      return LOAD_TEXTURE(samp0, int2(vicoord), 0);\n"
    "    #else\n"
    "      // Coordinates are already upscaled, we need to downscale them to apply the texture\n"
    "      // window, then re-upscale/offset. We can't round here, because it could result in\n"
    "      // going outside of the texture window.\n"
    "      float2 ncoords = APPLY_UV_LIMITS(coords, uv_limits) * u_rcp_resolution_scale;\n"
    "      float2 nfpart = frac(ncoords);\n"
    "      uint2 nicoord = ApplyTextureWindow(uint2(floor(ncoords)));\n"
    "      uint2 nvicoord = (texpage.xy + nicoord) & uint2(1023, 511);\n"
    "      ncoords = (float2(nvicoord) + nfpart);\n"
    "      return SAMPLE_TEXTURE_LEVEL(samp0, ncoords * RCP_VRAM_SIZE, 0.0);\n"
    "    #endif\n"
    "  #endif\n"
    "}\n"
    "\n"
    "#endif // !PAGE_TEXTURE || TEXTURE_FILTERING\n"
    "\n"
    "#endif // TEXTURED\n");

  const u32 num_fragment_outputs = use_rov ? 0u : (use_dual_source ? 2u : 1u);
  if (textured && page_texture)
  {
    /* Emit the filter helper before the FS entry point.  Only
     * BILINEAR / BILINEAR_BIN_ALPHA produce non-empty output today; the
     * other 6 filters (JINC2/xBR/MMPX/Scale*) stay deferred via the
     * inner `#if 0`.  Calling unconditionally for all non-NEAREST filters
     * keeps the helper-emission flow uniform. */
    if (texture_filtering != GPU_TEXTURE_FILTER_NEAREST)
      write_batch_texture_filter(sg, &ss, texture_filtering);

    if (uv_limits)
    {
      static const shadergen_io_pair_t addl[] = { { "nointerpolation", "float4 v_uv_limits" } };
      dfep(sg, &ss, 1, 1, addl, 1, true, num_fragment_outputs,
           use_dual_source, write_mask_as_depth, msaa, per_sample_shading, false,
           disable_color_perspective, shader_blending && !use_rov, use_rov);
    }
    else
    {
      dfep(sg, &ss, 1, 1, NULL, 0, true, num_fragment_outputs,
           use_dual_source, write_mask_as_depth, msaa, per_sample_shading, false,
           disable_color_perspective, shader_blending && !use_rov, use_rov);
    }
  }
  else if (textured)
  {
    /* Same filter-helper hook as the page_texture branch above.
     * BILINEAR-only today; the rest are deferred. */
    if (texture_filtering != GPU_TEXTURE_FILTER_NEAREST)
      write_batch_texture_filter(sg, &ss, texture_filtering);

    if (uv_limits)
    {
      const shadergen_io_pair_t addl[] = {
        { "nointerpolation", palette ? "uint4 v_texpage" : "uint2 v_texpage" },
        { "nointerpolation", "float4 v_uv_limits" },
      };
      dfep(sg, &ss, 1, 1, addl, 2, true, num_fragment_outputs,
           use_dual_source, write_mask_as_depth, msaa, per_sample_shading, false,
           disable_color_perspective, shader_blending && !use_rov, use_rov);
    }
    else
    {
      const shadergen_io_pair_t addl[] = {
        { "nointerpolation", palette ? "uint4 v_texpage" : "uint2 v_texpage" },
      };
      dfep(sg, &ss, 1, 1, addl, 1, true, num_fragment_outputs,
           use_dual_source, write_mask_as_depth, msaa, per_sample_shading, false,
           disable_color_perspective, shader_blending && !use_rov, use_rov);
    }
  }
  else
  {
    dfep(sg, &ss, 1, 0, NULL, 0, true, num_fragment_outputs,
         use_dual_source, write_mask_as_depth, msaa, per_sample_shading, false,
         disable_color_perspective, shader_blending && !use_rov, use_rov);
  }

  SS_LIT(&ss,
    "\n"
    "{\n"
    "  uint3 vertcol = uint3(v_col0.rgb * float3(255.0, 255.0, 255.0));\n"
    "  uint2 fragpos = uint2(v_pos.xy);\n"
    "\n"
    "  bool semitransparent;\n"
    "  uint3 icolor;\n"
    "  float ialpha;\n"
    "  float oalpha;\n"
    "\n"
    "  #if INTERLACING\n"
    "    #if INTERLACING_SCALED || !UPSCALED\n"
    "      if ((fragpos.y & 1u) == u_interlaced_displayed_field)\n"
    "        discard;\n"
    "    #else\n"
    "      if ((uint(v_pos.y * u_rcp_resolution_scale) & 1u) == u_interlaced_displayed_field)\n"
    "        discard;\n"
    "    #endif\n"
    "  #endif\n"
    "\n"
    "  #if TEXTURED\n"
    "    float4 texcol;\n"
    "    #if PAGE_TEXTURE && !TEXTURE_FILTERING\n"
    "      texcol = SampleFromPageTexture(DECLARE_UV_LIMITS(v_tex0, v_uv_limits));\n"
    "      if (VECTOR_EQ(texcol, TRANSPARENT_PIXEL_COLOR))\n"
    "        discard;\n"
    "\n"
    "      ialpha = 1.0;\n"
    "    #elif TEXTURE_FILTERING\n"
    "      #if PAGE_TEXTURE\n"
    "        FilteredSampleFromVRAM(VECTOR_BROADCAST(TEXPAGE_VALUE, 0u), v_tex0, v_uv_limits, texcol, ialpha);\n"
    "      #else\n"
    "        FilteredSampleFromVRAM(v_texpage, v_tex0, v_uv_limits, texcol, ialpha);\n"
    "      #endif\n"
    "      if (ialpha < 0.5)\n"
    "        discard;\n"
    "    #else\n"
    "      texcol = SampleFromVRAM(v_texpage, DECLARE_UV_LIMITS(v_tex0, v_uv_limits));\n"
    "      if (VECTOR_EQ(texcol, TRANSPARENT_PIXEL_COLOR))\n"
    "        discard;\n"
    "\n"
    "      ialpha = 1.0;\n"
    "    #endif\n"
    "\n"
    "    semitransparent = (texcol.a >= 0.5);\n"
    "\n"
    "    // If not using true color, truncate the framebuffer colors to 5-bit.\n"
    "    #if !TRUE_COLOR\n"
    "      icolor = uint3(texcol.rgb * float3(255.0, 255.0, 255.0)) >> 3;\n"
    "      #if MODULATION_CROP\n"
    "        icolor = (icolor * (vertcol >> 3)) >> 1;\n"
    "      #else\n"
    "        icolor = (icolor * vertcol) >> 4;\n"
    "      #endif\n"
    "      #if DITHERING\n"
    "        icolor = ApplyDithering(fragpos, icolor);\n"
    "      #else\n"
    "        icolor = min(icolor >> 3, uint3(31u, 31u, 31u));\n"
    "      #endif\n"
    "    #else\n"
    "      icolor = uint3(texcol.rgb * float3(255.0, 255.0, 255.0));\n"
    "      #if MODULATION_CROP\n"
    "        icolor = (icolor * (vertcol >> 3)) >> 4;\n"
    "      #else\n"
    "        icolor = (icolor * vertcol) >> 7;\n"
    "      #endif\n"
    "      icolor = min(icolor, uint3(255u, 255u, 255u));\n"
    "    #endif\n"
    "\n"
    "    // Compute output alpha (mask bit)\n"
    "    oalpha = float(u_set_mask_while_drawing ? 1 : int(semitransparent));\n"
    "  #else\n"
    "    // All pixels are semitransparent for untextured polygons.\n"
    "    semitransparent = true;\n"
    "    icolor = vertcol;\n"
    "    ialpha = 1.0;\n"
    "\n"
    "    #if DITHERING\n"
    "      icolor = ApplyDithering(fragpos, icolor);\n"
    "    #else\n"
    "      #if !TRUE_COLOR\n"
    "        icolor >>= 3;\n"
    "      #endif\n"
    "    #endif\n"
    "\n"
    "    // However, the mask bit is cleared if set mask bit is false.\n"
    "    oalpha = float(u_set_mask_while_drawing);\n"
    "  #endif\n"
    "\n"
    "  #if SHADER_BLENDING\n"
    "    #if USE_ROV\n"
    "      BEGIN_ROV_REGION;\n"
    "      float4 bg_col = ROV_LOAD(rov_color, fragpos);\n"
    "      float4 o_col0;\n"
    "      bool discarded = false;\n"
    "\n"
    "      #if ROV_DEPTH_TEST\n"
    "        float bg_depth = ROV_LOAD(rov_depth, fragpos).r;\n"
    "        discarded = (v_pos.z > bg_depth);\n"
    "      #endif\n"
    "      #if CHECK_MASK_BIT\n"
    "        discarded = discarded || (bg_col.a != 0.0);\n"
    "      #endif        \n"
    "    #else\n"
    "      float4 bg_col = LAST_FRAG_COLOR;\n"
    "      #if CHECK_MASK_BIT\n"
    "        if (bg_col.a != 0.0)\n"
    "          discard;\n"
    "      #endif\n"
    "    #endif\n"
    "\n"
    "    // Work in normalized space for true colour, matches HW blend.\n"
    "    float4 fg_col = float4(float3(icolor), oalpha);\n"
    "    #if TRUE_COLOR\n"
    "      fg_col.rgb /= 255.0;\n"
    "    #elif TRANSPARENCY // rgb not used in check-mask only\n"
    "      bg_col.rgb = roundEven(bg_col.rgb * 31.0);\n"
    "    #endif\n"
    "\n"
    "    o_col0.a = fg_col.a;\n"
    "\n"
    "    #if TEXTURE_FILTERING && TEXTURE_ALPHA_BLENDING\n"
    "      #if TRANSPARENCY_MODE == 0 // Half BG + Half FG.\n"
    "        o_col0.rgb = (bg_col.rgb * saturate(0.5 / ialpha)) + (fg_col.rgb * (ialpha * 0.5));\n"
    "      #elif TRANSPARENCY_MODE == 1 // BG + FG\n"
    "        o_col0.rgb = (bg_col.rgb * saturate(1.0 / ialpha)) + (fg_col.rgb * ialpha);\n"
    "      #elif TRANSPARENCY_MODE == 2 // BG - FG\n"
    "        o_col0.rgb = (bg_col.rgb * saturate(1.0 / ialpha)) - (fg_col.rgb * ialpha);\n"
    "      #elif TRANSPARENCY_MODE == 3 // BG + 1/4 FG.\n"
    "        o_col0.rgb = (bg_col.rgb * saturate(1.0 / ialpha)) + (fg_col.rgb * (0.25 * ialpha));\n"
    "      #else\n"
    "        o_col0.rgb = (fg_col.rgb * ialpha) + (bg_col.rgb * (1.0 - ialpha));\n"
    "      #endif\n"
    "    #else\n"
    "      #if TRANSPARENCY_MODE == 0 // Half BG + Half FG.\n"
    "        o_col0.rgb = (bg_col.rgb * 0.5) + (fg_col.rgb * 0.5);\n"
    "      #elif TRANSPARENCY_MODE == 1 // BG + FG\n"
    "        o_col0.rgb = bg_col.rgb + fg_col.rgb;\n"
    "      #elif TRANSPARENCY_MODE == 2 // BG - FG\n"
    "        o_col0.rgb = bg_col.rgb - fg_col.rgb;\n"
    "      #elif TRANSPARENCY_MODE == 3 // BG + 1/4 FG.\n"
    "        o_col0.rgb = bg_col.rgb + (fg_col.rgb * 0.25);\n"
    "      #else\n"
    "        o_col0.rgb = fg_col.rgb;\n"
    "      #endif\n"
    "    #endif\n"
    "\n"
    "    // 16-bit truncation.\n"
    "    #if !TRUE_COLOR && TRANSPARENCY\n"
    "      o_col0.rgb = floor(o_col0.rgb);\n"
    "    #endif\n"
    "\n"
    "    #if TRANSPARENCY\n"
    "      // If pixel isn't marked as semitransparent, replace with previous colour.\n"
    "      o_col0 = semitransparent ? o_col0 : fg_col;\n"
    "    #endif\n"
    "\n"
    "    // Normalize for non-true-color.\n"
    "    #if !TRUE_COLOR\n"
    "      o_col0.rgb /= 31.0;\n"
    "    #endif\n"
    "\n"
    "    #if USE_ROV\n"
    "      if (!discarded)\n"
    "      {\n"
    "        ROV_STORE(rov_color, fragpos, o_col0);\n"
    "        #if USE_ROV_DEPTH && ROV_DEPTH_WRITE\n"
    "          ROV_STORE(rov_depth, fragpos, float4(v_pos.z, 0.0, 0.0, 0.0));\n"
    "        #endif\n"
    "      }\n"
    "      END_ROV_REGION;\n"
    "    #endif\n"
    "  #else\n"
    "    // Premultiply alpha so we don't need to use a colour output for it.\n"
    "    float premultiply_alpha = ialpha;\n"
    "    #if TRANSPARENCY\n"
    "      premultiply_alpha = ialpha * (semitransparent ? u_src_alpha_factor : 1.0);\n"
    "    #endif\n"
    "\n"
    "    float3 color;\n"
    "    #if !TRUE_COLOR\n"
    "      // We want to apply the alpha before the truncation to 16-bit, otherwise we'll be passing a 32-bit precision color\n"
    "      // into the blend unit, which can cause a small amount of error to accumulate.\n"
    "      color = floor(float3(icolor) * premultiply_alpha) / 31.0;\n"
    "    #else\n"
    "      // True color is actually simpler here since we want to preserve the precision.\n"
    "      color = (float3(icolor) * premultiply_alpha) / 255.0;\n"
    "    #endif\n"
    "\n"
    "    #if TRANSPARENCY && TEXTURED\n"
    "      // Apply semitransparency. If not a semitransparent texel, destination alpha is ignored.\n"
    "      if (semitransparent)\n"
    "      {\n"
    "        #if USE_DUAL_SOURCE\n"
    "          o_col0 = float4(color, oalpha);\n"
    "          o_col1 = float4(0.0, 0.0, 0.0, u_dst_alpha_factor / ialpha);\n"
    "        #else\n"
    "          o_col0 = float4(color, oalpha);\n"
    "        #endif\n"
    "\n"
    "        #if WRITE_MASK_AS_DEPTH\n"
    "          o_depth = oalpha * v_pos.z;\n"
    "        #endif\n"
    "\n"
    "        #if TRANSPARENCY_ONLY_OPAQUE\n"
    "          discard;\n"
    "        #endif\n"
    "      }\n"
    "      else\n"
    "      {\n"
    "        #if USE_DUAL_SOURCE\n"
    "          o_col0 = float4(color, oalpha);\n"
    "          o_col1 = float4(0.0, 0.0, 0.0, 1.0 - ialpha);\n"
    "        #else\n"
    "          o_col0 = float4(color, oalpha);\n"
    "        #endif\n"
    "\n"
    "        #if WRITE_MASK_AS_DEPTH\n"
    "          o_depth = oalpha * v_pos.z;\n"
    "        #endif\n"
    "\n"
    "        #if TRANSPARENCY_ONLY_TRANSPARENT\n"
    "          discard;\n"
    "        #endif\n"
    "      }\n"
    "    #elif TRANSPARENCY\n"
    "      // We shouldn't be rendering opaque geometry only when untextured, so no need to test/discard here.\n"
    "      #if USE_DUAL_SOURCE\n"
    "        o_col0 = float4(color, oalpha);\n"
    "        o_col1 = float4(0.0, 0.0, 0.0, u_dst_alpha_factor / ialpha);\n"
    "      #else\n"
    "        o_col0 = float4(color, oalpha);\n"
    "      #endif\n"
    "\n"
    "      #if WRITE_MASK_AS_DEPTH\n"
    "        o_depth = oalpha * v_pos.z;\n"
    "      #endif\n"
    "    #else\n"
    "      // Non-transparency won't enable blending so we can write the mask here regardless.\n"
    "      o_col0 = float4(color, oalpha);\n"
    "\n"
    "      #if USE_DUAL_SOURCE\n"
    "        o_col1 = float4(0.0, 0.0, 0.0, 1.0 - ialpha);\n"
    "      #endif\n"
    "\n"
    "      #if WRITE_MASK_AS_DEPTH\n"
    "        o_depth = oalpha * v_pos.z;\n"
    "      #endif\n"
    "    #endif\n"
    "  #endif\n"
    "}\n");

  return take_string(&ss);
}

 char* gpu_hw_shadergen_generate_vram_extract_fragment_shader(gpu_hw_shadergen_t* sg, u32 resolution_scale,
                                                              u32 multisamples, bool color_24bit, bool depth_buffer) 
{
  const bool msaa = (multisamples > 1);

  small_string_t ss; small_string_init(&ss);
  wh(sg, &ss);
  write_color_conversion_functions(sg, &ss);

  dm_b(sg, &ss, "COLOR_24BIT", color_24bit);
  dm_b(sg, &ss, "DEPTH_BUFFER", depth_buffer);
  dm_b(sg, &ss, "MULTISAMPLING", msaa);
  ss_appendf(&ss, "CONSTANT uint RESOLUTION_SCALE = %uu;\n", resolution_scale);
  ss_appendf(&ss, "CONSTANT uint2 VRAM_SIZE = uint2(%d, %d) * RESOLUTION_SCALE;\n", (int)VRAM_WIDTH, (int)VRAM_HEIGHT);
  ss_appendf(&ss, "CONSTANT uint MULTISAMPLES = %uu;\n", multisamples);

  static const char* members[] = { "uint2 u_vram_offset", "float u_skip_x", "float u_line_skip" };
  dub(sg, &ss, members, 3, true);
  dt(sg, &ss, "samp0", 0, msaa);
  if (depth_buffer)
    dt(sg, &ss, "samp1", 1, msaa);

  SS_LIT(&ss,
    "\n"
    "float4 LoadVRAM(int2 coords)\n"
    "{\n"
    "#if MULTISAMPLING\n"
    "  float4 value = LOAD_TEXTURE_MS(samp0, coords, 0u);\n"
    "  FOR_UNROLL (uint sample_index = 1u; sample_index < MULTISAMPLES; sample_index++)\n"
    "    value += LOAD_TEXTURE_MS(samp0, coords, sample_index);\n"
    "  value /= float(MULTISAMPLES);\n"
    "  return value;\n"
    "#else\n"
    "  return LOAD_TEXTURE(samp0, coords, 0);\n"
    "#endif\n"
    "}\n"
    "\n"
    "#if DEPTH_BUFFER\n"
    "float LoadDepth(int2 coords)\n"
    "{\n"
    "  // Need to duplicate because different types in different languages...\n"
    "#if MULTISAMPLING\n"
    "  float value = LOAD_TEXTURE_MS(samp1, coords, 0u).r;\n"
    "  FOR_UNROLL (uint sample_index = 1u; sample_index < MULTISAMPLES; sample_index++)\n"
    "    value += LOAD_TEXTURE_MS(samp1, coords, sample_index).r;\n"
    "  value /= float(MULTISAMPLES);\n"
    "  return value;\n"
    "#else\n"
    "  return LOAD_TEXTURE(samp1, coords, 0).r;\n"
    "#endif\n"
    "}\n"
    "#endif\n"
    "\n"
    "float3 SampleVRAM24(uint2 icoords)\n"
    "{\n"
    "  // load adjacent 16-bit texels\n"
    "  uint2 clamp_size = uint2(1024, 512);\n"
    "\n"
    "  // relative to start of scanout\n"
    "  uint2 vram_coords = u_vram_offset + uint2((icoords.x * 3u) / 2u, icoords.y);\n"
    "  uint s0 = RGBA8ToRGBA5551(LoadVRAM(int2((vram_coords % clamp_size) * RESOLUTION_SCALE)));\n"
    "  uint s1 = RGBA8ToRGBA5551(LoadVRAM(int2(((vram_coords + uint2(1, 0)) % clamp_size) * RESOLUTION_SCALE)));\n"
    "\n"
    "  // select which part of the combined 16-bit texels we are currently shading\n"
    "  uint s1s0 = ((s1 << 16) | s0) >> ((icoords.x & 1u) * 8u);\n"
    "\n"
    "  // extract components and normalize\n"
    "  return float3(float(s1s0 & 0xFFu) / 255.0, float((s1s0 >> 8u) & 0xFFu) / 255.0,\n"
    "                float((s1s0 >> 16u) & 0xFFu) / 255.0);\n"
    "}\n");

  dfep(sg, &ss, 0, 1, NULL, 0, true, depth_buffer ? 2u : 1u,
       false, false, false, false, false, false, false, false);
  SS_LIT(&ss,
    "\n"
    "{\n"
    "  // Have to floor because SV_Position is at the pixel center.\n"
    "  float2 v_pos_floored = floor(v_pos.xy);\n"
    "  uint2 icoords = uint2(v_pos_floored.x + u_skip_x, v_pos_floored.y * u_line_skip);\n"
    "  int2 wrapped_coords = int2((icoords + u_vram_offset) % VRAM_SIZE);\n"
    "\n"
    "  #if COLOR_24BIT\n"
    "    o_col0 = float4(SampleVRAM24(icoords), 1.0);\n"
    "  #else\n"
    "    o_col0 = float4(LoadVRAM(wrapped_coords).rgb, 1.0);\n"
    "  #endif\n"
    "\n"
    "  #if DEPTH_BUFFER\n"
    "    o_col1 = float4(LoadDepth(wrapped_coords), 0.0, 0.0, 0.0);\n"
    "  #endif\n"
    "}\n");

  return take_string(&ss);
}

/* TODO: replacement-blit and replacement-merge; texture
 *       replacement / external-png path not in cupid-ps1 scope. */
char* gpu_hw_shadergen_generate_vram_replacement_blit_fragment_shader(gpu_hw_shadergen_t* sg)
{
  (void)sg;
  return NULL;
}

char* gpu_hw_shadergen_generate_wireframe_geometry_shader(gpu_hw_shadergen_t* sg)
{
  small_string_t ss; small_string_init(&ss);
  wh(sg, &ss);

  if (sg->base.glsl)
  {
    SS_LIT(&ss,
      "\n"
      "layout(triangles) in;\n"
      "layout(line_strip, max_vertices = 6) out;\n"
      "\n"
      "void main()\n"
      "{\n"
      "  gl_Position = gl_in[0].gl_Position;\n"
      "  EmitVertex();\n"
      "  gl_Position = gl_in[1].gl_Position;\n"
      "  EmitVertex();\n"
      "  EndPrimitive();\n"
      "  gl_Position = gl_in[1].gl_Position;\n"
      "  EmitVertex();\n"
      "  gl_Position = gl_in[2].gl_Position;\n"
      "  EmitVertex();\n"
      "  EndPrimitive();\n"
      "  gl_Position = gl_in[2].gl_Position;\n"
      "  EmitVertex();\n"
      "  gl_Position = gl_in[0].gl_Position;\n"
      "  EmitVertex();\n"
      "  EndPrimitive();\n" 
      "}\n");
  }
  else
  {
    SS_LIT(&ss,
      "\n"
      "struct GSInput\n"
      "{\n"
      "  float4 col0 : COLOR0;\n"
      "  float4 pos : SV_Position;\n"
      "};\n"
      "\n"
      "struct GSOutput\n"
      "{\n"
      "  float4 pos : SV_Position;\n"
      "};\n"
      "\n"
      "GSOutput GetVertex(GSInput vi)\n"
      "{\n"
      "  GSOutput vo;\n"
      "  vo.pos = vi.pos;\n"
      "  return vo;\n"
      "}\n"
      "\n"
      "[maxvertexcount(6)]\n"
      "void main(triangle GSInput input[3], inout LineStream<GSOutput> output)\n"
      "{\n"
      "  output.Append(GetVertex(input[0]));\n"
      "  output.Append(GetVertex(input[1]));\n"
      "  output.RestartStrip();\n"
      "\n"
      "  output.Append(GetVertex(input[1]));\n"
      "  output.Append(GetVertex(input[2]));\n"
      "  output.RestartStrip();\n"
      "\n"
      "  output.Append(GetVertex(input[2]));\n"
      "  output.Append(GetVertex(input[0]));\n"
      "  output.RestartStrip();\n" 
      "}\n");
  }

  return take_string(&ss);
}

char* gpu_hw_shadergen_generate_wireframe_fragment_shader(gpu_hw_shadergen_t* sg)
{
  small_string_t ss; small_string_init(&ss);
  wh(sg, &ss);
  dfep(sg, &ss, 0, 0, NULL, 0, false, 1, false, false, false, false, false, false, false, false);
  SS_LIT(&ss,
    "\n"
    "{\n"
    "  o_col0 = float4(1.0, 1.0, 1.0, 0.5);\n" 
    "}\n");
  return take_string(&ss);
}

char* gpu_hw_shadergen_generate_vram_read_fragment_shader(gpu_hw_shadergen_t* sg, u32 resolution_scale, u32 multisamples)
{
  const bool msaa = (multisamples > 1);

  small_string_t ss; small_string_init(&ss);
  wh(sg, &ss);
  write_color_conversion_functions(sg, &ss);

  dm_b(sg, &ss, "MULTISAMPLING", msaa);
  ss_appendf(&ss, "CONSTANT uint RESOLUTION_SCALE = %uu;\n", resolution_scale);
  ss_appendf(&ss, "CONSTANT uint MULTISAMPLES = %uu;\n", multisamples);

  static const char* members[] = { "uint2 u_base_coords", "uint2 u_size" };
  dub(sg, &ss, members, 2, true);
  dt(sg, &ss, "samp0", 0, msaa);

  SS_LIT(&ss,
    "\n"
    "float4 LoadVRAM(int2 coords)\n"
    "{\n"
    "#if MULTISAMPLING\n"
    "  float4 value = LOAD_TEXTURE_MS(samp0, coords, 0u);\n"
    "  FOR_UNROLL (uint sample_index = 1u; sample_index < MULTISAMPLES; sample_index++)\n"
    "    value += LOAD_TEXTURE_MS(samp0, coords, sample_index);\n"
    "  value /= float(MULTISAMPLES);\n"
    "  return value;\n"
    "#else\n"
    "  return LOAD_TEXTURE(samp0, coords, 0);\n"
    "#endif\n"
    "}\n"
    "\n"
    "uint SampleVRAM(uint2 coords)\n"
    "{\n"
    "  if (RESOLUTION_SCALE == 1u)\n"
    "    return RGBA8ToRGBA5551(LoadVRAM(int2(coords)));\n"
    "\n"
    "  // Box filter for downsampling.\n"
    "  float4 value = float4(0.0, 0.0, 0.0, 0.0);\n"
    "  uint2 base_coords = coords * uint2(RESOLUTION_SCALE, RESOLUTION_SCALE);\n"
    "  for (uint offset_x = 0u; offset_x < RESOLUTION_SCALE; offset_x++)\n"
    "  {\n"
    "    for (uint offset_y = 0u; offset_y < RESOLUTION_SCALE; offset_y++)\n"
    "      value += LoadVRAM(int2(base_coords + uint2(offset_x, offset_y)));\n"
    "  }\n"
    "  value /= float(RESOLUTION_SCALE * RESOLUTION_SCALE);\n"
    "  return RGBA8ToRGBA5551(value);\n"
    "}\n");

  dfep(sg, &ss, 0, 1, NULL, 0, true, 1, false, false, false, false, false, false, false, false);
  SS_LIT(&ss,
    "\n"
    "{\n"
    "  uint2 sample_coords = uint2(uint(v_pos.x) * 2u, uint(v_pos.y));\n"
    "  sample_coords += u_base_coords;\n"
    "\n"
    "  // We're encoding as 32-bit, so the output width is halved and we pack two 16-bit pixels in one 32-bit pixel.\n"
    "  uint left = SampleVRAM(sample_coords);\n"
    "  uint right = SampleVRAM(uint2(sample_coords.x + 1u, sample_coords.y));\n"
    "\n"
    "  o_col0 = float4(float(left & 0xFFu), float((left >> 8) & 0xFFu),\n"
    "                  float(right & 0xFFu), float((right >> 8) & 0xFFu))\n"
    "            / float4(255.0, 255.0, 255.0, 255.0);\n"
    "}");

  return take_string(&ss);
}

char* gpu_hw_shadergen_generate_vram_write_fragment_shader(gpu_hw_shadergen_t* sg, bool use_buffer, bool use_ssbo,
                                                            bool write_mask_as_depth, bool write_depth_as_rt)
{
  Assert(!write_mask_as_depth || (write_mask_as_depth != write_depth_as_rt));

  small_string_t ss; small_string_init(&ss);
  wh(sg, &ss);
  write_color_conversion_functions(sg, &ss);

  dm_b(sg, &ss, "WRITE_MASK_AS_DEPTH", write_mask_as_depth);
  dm_b(sg, &ss, "WRITE_DEPTH_AS_RT", write_depth_as_rt);
  dm_b(sg, &ss, "USE_BUFFER", use_buffer);

  ss_appendf(&ss, "CONSTANT float2 VRAM_SIZE = float2(%d.0, %d.0);\n", (int)VRAM_WIDTH, (int)VRAM_HEIGHT);

  static const char* members[] = {
    "float2 u_base_coords", "float2 u_end_coords", "float2 u_size",
    "float u_resolution_scale", "uint u_buffer_base_offset", "uint u_mask_or_bits", "float u_depth_value" 
  };
  dub(sg, &ss, members, 7, true);

  if (!use_buffer)
  {
    dt_int(sg, &ss, "samp0", 0, false, true, true);
  }
  else if (use_ssbo && sg->base.glsl)
  {
    SS_LIT(&ss, "layout(std430");
    if (shadergen_is_vulkan(&sg->base))
      SS_LIT(&ss, ", set = 0, binding = 0");
    else if (shadergen_is_metal(&sg->base))
      SS_LIT(&ss, ", set = 1, binding = 0");
    else if (sg->base.use_glsl_binding_layout)
      SS_LIT(&ss, ", binding = 0");

    SS_LIT(&ss, ") readonly restrict buffer SSBO {\n");
    SS_LIT(&ss, "  uint ssbo_data[];\n");
    SS_LIT(&ss, "};\n\n");

    SS_LIT(&ss, "#define GET_VALUE(buffer_offset) (ssbo_data[(buffer_offset) / 2u] >> (((buffer_offset) % 2u) * 16u))\n\n");
  }
  else
  {
    dtb(sg, &ss, "samp0", 0, true, true);
    SS_LIT(&ss, "#define GET_VALUE(buffer_offset) (LOAD_TEXTURE_BUFFER(samp0, int(buffer_offset)).r)\n\n");
  }

  dfep(sg, &ss, 0, 1, NULL, 0, true, 1u + (write_depth_as_rt ? 1u : 0u),
       false, write_mask_as_depth, false, false, false, false, false, false);
  SS_LIT(&ss,
    "\n"
    "{\n"
    "  float2 coords = floor(v_pos.xy / u_resolution_scale);\n"
    "\n"
    "  // make sure it's not oversized and out of range\n"
    "  if ((coords.x < u_base_coords.x && coords.x >= u_end_coords.x) ||\n"
    "      (coords.y < u_base_coords.y && coords.y >= u_end_coords.y))\n"
    "  {\n"
    "    discard;\n"
    "  }\n"
    "\n"
    "  // find offset from the start of the row/column\n"
    "  float2 offset;\n"
    "  offset.x = (coords.x < u_base_coords.x) ? (VRAM_SIZE.x - u_base_coords.x + coords.x) : (coords.x - u_base_coords.x);\n"
    "  offset.y = (coords.y < u_base_coords.y) ? (VRAM_SIZE.y - u_base_coords.y + coords.y) : (coords.y - u_base_coords.y);\n"
    "\n"
    "#if !USE_BUFFER\n"
    "  uint value = LOAD_TEXTURE(samp0, int2(offset), 0).x;\n"
    "#else\n"
    "  uint buffer_offset = u_buffer_base_offset + uint((offset.y * u_size.x) + offset.x);\n"
    "  uint value = GET_VALUE(buffer_offset) | u_mask_or_bits;\n"
    "#endif\n"
    "\n"
    "  o_col0 = RGBA5551ToRGBA8(value);\n"
    "#if WRITE_MASK_AS_DEPTH\n"
    "  o_depth = (o_col0.a == 1.0) ? u_depth_value : 0.0;\n"
    "#elif WRITE_DEPTH_AS_RT\n"
    "  o_col1 = float4(1.0f, 0.0f, 0.0f, 0.0f);\n"
    "#endif\n"
    "}");

  return take_string(&ss);
}

char* gpu_hw_shadergen_generate_vram_copy_fragment_shader(gpu_hw_shadergen_t* sg, bool write_mask_as_depth,
                                                           bool write_depth_as_rt)
{
  Assert(!write_mask_as_depth || (write_mask_as_depth != write_depth_as_rt));

  /* TODO: This won't currently work because we can't bind the texture to both the shader and framebuffer. */
  const bool msaa = false;

  small_string_t ss; small_string_init(&ss);
  wh(sg, &ss);
  dm_b(sg, &ss, "WRITE_MASK_AS_DEPTH", write_mask_as_depth);
  dm_b(sg, &ss, "WRITE_DEPTH_AS_RT", write_depth_as_rt);
  dm_b(sg, &ss, "MSAA_COPY", msaa);

  static const char* members[] = {
    "float2 u_src_coords", "float2 u_dst_coords", "float2 u_end_coords", "float2 u_vram_size",
    "float u_resolution_scale", "bool u_set_mask_bit", "float u_depth_value" 
  };
  dub(sg, &ss, members, 7, true);

  dt(sg, &ss, "samp0", 0, msaa);
  dfep(sg, &ss, 0, 1, NULL, 0, true, 1u + (write_depth_as_rt ? 1u : 0u),
       false, write_mask_as_depth, false, false, false, false, false, false);
  SS_LIT(&ss,
    "\n"
    "{\n"
    "  float2 dst_coords = floor(v_pos.xy);\n"
    "\n"
    "  // make sure it's not oversized and out of range\n"
    "  if ((dst_coords.x < u_dst_coords.x && dst_coords.x >= u_end_coords.x) ||\n"
    "      (dst_coords.y < u_dst_coords.y && dst_coords.y >= u_end_coords.y))\n"
    "  {\n"
    "    discard;\n"
    "  }\n"
    "\n"
    "  // find offset from the start of the row/column\n"
    "  float2 offset;\n"
    "  offset.x = (dst_coords.x < u_dst_coords.x) ? (u_vram_size.x - u_dst_coords.x + dst_coords.x) : (dst_coords.x - u_dst_coords.x);\n"
    "  offset.y = (dst_coords.y < u_dst_coords.y) ? (u_vram_size.y - u_dst_coords.y + dst_coords.y) : (dst_coords.y - u_dst_coords.y);\n"
    "\n"
    "  // find the source coordinates to copy from\n"
    "  float2 offset_coords = u_src_coords + offset;\n"
    "  float2 src_coords = offset_coords - (floor(offset_coords / u_vram_size) * u_vram_size);\n"
    "\n"
    "  // sample and apply mask bit\n"
    "#if MSAA_COPY\n"
    "  float4 color = LOAD_TEXTURE_MS(samp0, int2(src_coords), f_sample_index);\n"
    "#else\n"
    "  float4 color = LOAD_TEXTURE(samp0, int2(src_coords), 0);\n"
    "#endif\n"
    "  o_col0 = float4(color.xyz, u_set_mask_bit ? 1.0 : color.a);\n"
    "#if WRITE_MASK_AS_DEPTH\n"
    "  o_depth = (u_set_mask_bit ? 1.0f : ((o_col0.a == 1.0) ? u_depth_value : 0.0));\n"
    "#elif WRITE_DEPTH_AS_RT\n"
    "  o_col1 = float4(1.0f, 0.0f, 0.0f, 0.0f);\n"
    "#endif\n"
    "}");

  return take_string(&ss);
}

char* gpu_hw_shadergen_generate_vram_fill_fragment_shader(gpu_hw_shadergen_t* sg, bool wrapped, bool interlaced,
                                                           bool write_mask_as_depth, bool write_depth_as_rt)
{
  Assert(!write_mask_as_depth || (write_mask_as_depth != write_depth_as_rt));

  small_string_t ss; small_string_init(&ss);
  wh(sg, &ss);
  dm_b(sg, &ss, "WRITE_MASK_AS_DEPTH", write_mask_as_depth);
  dm_b(sg, &ss, "WRITE_DEPTH_AS_RT", write_depth_as_rt);
  dm_b(sg, &ss, "WRAPPED", wrapped);
  dm_b(sg, &ss, "INTERLACED", interlaced);

  static const char* members[] = {
    "uint2 u_dst_coords", "uint2 u_end_coords", "float4 u_fill_color", "uint u_interlaced_displayed_field"
  };
  dub(sg, &ss, members, 4, true);

  dfep(sg, &ss, 0, 1, NULL, 0, interlaced || wrapped, 1u + (write_depth_as_rt ? 1u : 0u),
       false, write_mask_as_depth, false, false, false, false, false, false);
  SS_LIT(&ss,
    "\n"
    "{\n"
    "#if INTERLACED || WRAPPED\n"
    "  uint2 dst_coords = uint2(v_pos.xy);\n"
    "#endif\n"
    "\n"
    "#if INTERLACED\n"
    "  if ((dst_coords.y & 1u) == u_interlaced_displayed_field)\n"
    "    discard;\n"
    "#endif\n"
    "\n"
    "#if WRAPPED\n"
    "  // make sure it's not oversized and out of range\n"
    "  if ((dst_coords.x < u_dst_coords.x && dst_coords.x >= u_end_coords.x) ||\n"
    "      (dst_coords.y < u_dst_coords.y && dst_coords.y >= u_end_coords.y))\n"
    "  {\n"
    "    discard;\n"
    "  }\n"
    "#endif\n"
    "\n"
    "  o_col0 = u_fill_color;\n"
    "#if WRITE_MASK_AS_DEPTH\n"
    "  o_depth = u_fill_color.a;\n"
    "#elif WRITE_DEPTH_AS_RT\n"
    "  o_col1 = float4(1.0f, 0.0f, 0.0f, 0.0f);\n"
    "#endif\n"
    "}");

  return take_string(&ss);
}

char* gpu_hw_shadergen_generate_vram_update_depth_fragment_shader(gpu_hw_shadergen_t* sg, bool msaa)
{
  small_string_t ss; small_string_init(&ss);
  wh(sg, &ss);
  dm_b(sg, &ss, "MULTISAMPLING", msaa);
  dt(sg, &ss, "samp0", 0, msaa);
  dfep(sg, &ss, 0, 1, NULL, 0, true, 0, false, true, false, false, msaa, false, false, false);

  SS_LIT(&ss,
    "\n"
    "{\n"
    "#if MULTISAMPLING\n"
    "  o_depth = LOAD_TEXTURE_MS(samp0, int2(v_pos.xy), f_sample_index).a;\n"
    "#else\n"
    "  o_depth = LOAD_TEXTURE(samp0, int2(v_pos.xy), 0).a;\n"
    "#endif\n" 
    "}\n");

  return take_string(&ss);
}

char* gpu_hw_shadergen_generate_vram_copy_depth_fragment_shader(gpu_hw_shadergen_t* sg, bool msaa)
{
  small_string_t ss; small_string_init(&ss);
  wh(sg, &ss);
  dm_b(sg, &ss, "MULTISAMPLED", msaa);
  dt(sg, &ss, "samp0", 0, msaa);
  dfep(sg, &ss, 0, 1, NULL, 0, msaa, 1, false, false, msaa, msaa, msaa, false, false, false);

  SS_LIT(&ss,
    "\n"
    "{\n"
    "#if MULTISAMPLED\n"
    "  o_col0 = float4(LOAD_TEXTURE_MS(samp0, int2(v_pos.xy), int(f_sample_index)).r, 0.0, 0.0, 0.0);\n"
    "#else\n"
    "  o_col0 = float4(SAMPLE_TEXTURE(samp0, v_tex0).r, 0.0, 0.0, 0.0);\n"
    "#endif\n" 
    "}\n");

  return take_string(&ss);
}

char* gpu_hw_shadergen_generate_vram_clear_depth_fragment_shader(gpu_hw_shadergen_t* sg, bool write_depth_as_rt)
{
  small_string_t ss; small_string_init(&ss);
  wh(sg, &ss);
  dm_b(sg, &ss, "WRITE_DEPTH_AS_RT", write_depth_as_rt);
  dfep(sg, &ss, 0, 1, NULL, 0, false, write_depth_as_rt ? 1u : 0u,
       false, false, false, false, false, false, false, false);

  SS_LIT(&ss,
    "\n"
    "{\n"
    "#if WRITE_DEPTH_AS_RT\n"
    "  o_col0 = float4(1.0f, 0.0f, 0.0f, 0.0f);\n"
    "#endif\n" 
    "}\n");

  return take_string(&ss);
}

/* TODO: adaptive downsample uniform-buffer + 4 generators
 *       (mip / blur / composite + vertex shader). */
static void write_adaptive_downsample_uniform_buffer(gpu_hw_shadergen_t* sg, small_string_t* ss)
{
  static const char* members[] = {
    "float2 u_uv_min", "float2 u_uv_max", "float2 u_pixel_size", "float u_lod"
  };
  dub(sg, ss, members, 4, true);
}
char* gpu_hw_shadergen_generate_adaptive_downsample_vertex_shader(gpu_hw_shadergen_t* sg) { (void)sg; return NULL; }
char* gpu_hw_shadergen_generate_adaptive_downsample_mip_fragment_shader(gpu_hw_shadergen_t* sg) { (void)sg; return NULL; }
char* gpu_hw_shadergen_generate_adaptive_downsample_blur_fragment_shader(gpu_hw_shadergen_t* sg) { (void)sg; return NULL; }
char* gpu_hw_shadergen_generate_adaptive_downsample_composite_fragment_shader(gpu_hw_shadergen_t* sg) { (void)sg; return NULL; }

char* gpu_hw_shadergen_generate_box_sample_downsample_fragment_shader(gpu_hw_shadergen_t* sg, u32 factor)
{
  small_string_t ss; small_string_init(&ss);
  wh(sg, &ss);
  static const char* members[] = { "uint2 u_base_coords" };
  dub(sg, &ss, members, 1, true);
  dt(sg, &ss, "samp0", 0, false);

  ss_appendf(&ss, "CONSTANT uint FACTOR = %uu;\n", factor);

  dfep(sg, &ss, 0, 1, NULL, 0, true, 1, false, false, false, false, false, false, false, false);
  SS_LIT(&ss,
    "\n"
    "{\n"
    "  float3 color = float3(0.0, 0.0, 0.0);\n"
    "  uint2 base_coords = u_base_coords + uint2(v_pos.xy) * uint2(FACTOR, FACTOR);\n"
    "  for (uint offset_x = 0u; offset_x < FACTOR; offset_x++)\n"
    "  {\n"
    "    for (uint offset_y = 0u; offset_y < FACTOR; offset_y++)\n"
    "      color += LOAD_TEXTURE(samp0, int2(base_coords + uint2(offset_x, offset_y)), 0).rgb;\n"
    "  }\n"
    "  color /= float(FACTOR * FACTOR);\n"
    "  o_col0 = float4(color, 1.0);\n" 
    "}\n");

  return take_string(&ss);
}

/* TODO: GenerateReplacementMergeFragmentShader; texture
 *       replacement merge path. */
char* gpu_hw_shadergen_generate_replacement_merge_fragment_shader(gpu_hw_shadergen_t* sg, bool replacement,
                                                                   bool semitransparent, bool bilinear_filter)
{
  (void)sg; (void)replacement; (void)semitransparent; (void)bilinear_filter;
  return NULL;
}
