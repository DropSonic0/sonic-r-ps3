#ifndef ADDSIGNED_SHADERS_H
#define ADDSIGNED_SHADERS_H

#if defined(SONICR_PS3)

static const char *addsigned_vshader =
"struct out_vertex {\n"
"    float4 position : POSITION;\n"
"    float4 color : COLOR;\n"
"    float2 texCoord : TEXCOORD0;\n"
"};\n"
"uniform float4x4 modelViewProj;\n"
"out_vertex vmain(float4 position : POSITION, float4 color : COLOR, float2 texCoord : TEXCOORD0)\n"
"{\n"
"    out_vertex OUT;\n"
"    OUT.position = mul(modelViewProj, position);\n"
"    OUT.color = color;\n"
"    OUT.texCoord = texCoord;\n"
"    return OUT;\n"
"}\n";

static const char *addsigned_fshader =
"struct out_vertex {\n"
"    float4 color : COLOR;\n"
"    float2 texCoord : TEXCOORD0;\n"
"};\n"
"uniform sampler2D decal : TEXUNIT0;\n"
"float4 fmain(in out_vertex VAR) : COLOR\n"
"{\n"
"    float4 texel = tex2D(decal, VAR.texCoord);\n"
"    float3 lit = texel.rgb + (VAR.color.rgb - 0.5);\n"
"    return float4(saturate(lit), texel.a * VAR.color.a);\n"
"}\n";

#endif
#endif // ADDSIGNED_SHADERS_H