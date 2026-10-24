#include <cstdio>

#include "kern/gfx/shader.h"

namespace kern {

const char* const kGlslToneMap = R"GLSL(
vec3 linearToSrgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)); }
vec3 srgbToLinear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }
vec3 rrtOdt(vec3 v) {
  vec3 a = v * (v + 0.0245786) - 0.000090537;
  vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
  return a / b;
}
// three.js r16x ACESFilmicToneMapping: exposure / 0.6, RRT+ODT fit.
vec3 acesFilmic(vec3 c) {
  const mat3 inM = mat3(vec3(0.59719, 0.07600, 0.02840), vec3(0.35458, 0.90834, 0.13383), vec3(0.04823, 0.01566, 0.83777));
  const mat3 outM = mat3(vec3(1.60475, -0.10208, -0.00327), vec3(-0.53108, 1.10813, -0.07276), vec3(-0.07367, -0.00605, 1.07602));
  c *= uExposure / 0.6;
  c = inM * c;
  c = rrtOdt(c);
  c = outM * c;
  return clamp(c, 0.0, 1.0);
}
// Colour grade on tone-mapped rgb: saturation, soft S-curve, warm highlights / cool shadows.
vec3 gradeColor(vec3 rgb) {
  float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
  rgb = mix(vec3(luma), rgb, 1.14);
  rgb = mix(rgb, rgb * rgb * (3.0 - 2.0 * rgb), 0.22);
  rgb += vec3(0.016, 0.006, -0.014) * smoothstep(0.35, 1.0, luma) + vec3(-0.006, 0.0, 0.012) * (1.0 - smoothstep(0.0, 0.3, luma));
  return rgb;
}
)GLSL";

namespace {

void append_hook(std::string& out, Span<ShaderExtension* const> extensions, const char* (ShaderExtension::*hook)() const) {
  for (ShaderExtension* e : extensions) {
    const char* code = (e->*hook)();
    if (!code || !*code) continue;
    out += "// --- ";
    out += e->name();
    out += "\n";
    out += code;
    out += "\n";
  }
}

constexpr const char* kVertexHead = R"GLSL(#version 300 es
precision highp float;
layout(location=0) in vec3 aPosition;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec3 aColor;
layout(location=3) in vec2 aUV;
layout(location=4) in vec4 iModel0;
layout(location=5) in vec4 iModel1;
layout(location=6) in vec4 iModel2;
layout(location=7) in vec4 iModel3;
layout(location=8) in vec4 iTint;
layout(location=9) in vec3 iNormal0;
layout(location=10) in vec3 iNormal1;
layout(location=11) in vec3 iNormal2;
layout(location=12) in vec4 aJoints;
layout(location=13) in vec4 aWeights;
)GLSL";

constexpr const char* kVertexUniforms = R"GLSL(
uniform int uSkinEnabled;
uniform mat4 uBones[32];
uniform mat4 uViewProjection;
out vec3 vWorld, vNormal, vColor;
out vec2 vUV;
out float vOpacity;
)GLSL";

constexpr const char* kVertexMainHead = R"GLSL(
void main() {
  mat4 model = mat4(iModel0, iModel1, iModel2, iModel3);
  vec4 local = vec4(aPosition, 1.0);
  vec3 localNormal = aNormal;
  if (uSkinEnabled == 1) {
    mat4 skin = aWeights.x * uBones[int(aJoints.x)] + aWeights.y * uBones[int(aJoints.y)]
              + aWeights.z * uBones[int(aJoints.z)] + aWeights.w * uBones[int(aJoints.w)];
    local = skin * local;
    localNormal = mat3(skin) * localNormal;
  }
  vec4 p = model * local;
  vWorld = p.xyz;
  vNormal = normalize(mat3(iNormal0, iNormal1, iNormal2) * localNormal);
  vColor = aColor * iTint.rgb;
  vOpacity = iTint.a;
  vUV = aUV;
)GLSL";

constexpr const char* kFragmentHead = R"GLSL(#version 300 es
precision highp float;
precision highp sampler2DShadow;
in vec3 vWorld, vNormal, vColor;
in vec2 vUV;
in float vOpacity;
uniform vec4 uMaterial;
uniform vec3 uSunDirection, uSunColor, uSkyColor, uGroundColor, uEye, uFogColor;
uniform vec3 uEnvZenith, uEnvHorizon, uEnvGround, uEmissive;
uniform float uFogDensity, uExposure, uAlphaCutoff, uEnvIntensity, uSunShadowBias;
uniform float uFogHeightFalloff, uFogSunScatter;
uniform sampler2D uTexture;
uniform int uAoOut;  // 1: write the indirect-light record for ambient occlusion
uniform sampler2DShadow uSunShadowMap;
uniform mat4 uSunShadowProjection;
uniform int uSunShadowFilter;    // 1, 4 or 9 taps
uniform vec3 uSunShadowTexel;    // map size, 1 / size, one texel in metres
uniform vec4 uSunShadowFade;     // eye, shadow distance (fades out over its last tenth)
uniform int uSunShadowEnabled, uHasTexture, uUnlit, uLinearColor, uGrade, uPointLightCount;
uniform vec2 uPbr;
uniform vec4 uPointPosRange[6], uPointColorIntensity[6];
layout(location=0) out vec4 outColor;
layout(location=1) out vec4 outHdr;
// Ambient occlusion record (scene target only): share of indirect light per channel x (1 - fog), albedo luminance.
layout(location=2) out vec4 outAo;
)GLSL";

constexpr const char* kFragmentFunctions = R"GLSL(
// Diffuse sky irradiance: the PMREM of a zenith/horizon gradient dome over a ground disc.
vec3 envIrradiance(vec3 n) {
  vec3 sky = mix(uEnvHorizon, uEnvZenith, 0.35);
  float w = clamp(n.z * 0.5 + 0.5, 0.0, 1.0);
  return mix(uEnvGround, sky, smoothstep(0.0, 1.0, w));
}
vec3 skyReflection(vec3 r, float sharpness) {
  return mix(uEnvHorizon, uEnvZenith, clamp(r.z, 0.0, 1.0) * sharpness);
}
)GLSL";

constexpr const char* kFragmentShadow = R"GLSL(
// Castano's optimised PCF (2013, "The Witness"; as in MJP's shadow sample): a tent-filtered 3x3 (4 taps)
// or 5x5 (9 taps) kernel from bilinear-weighted hardware comparison taps.
float shadowTap(vec2 base, vec2 o, float z) { return texture(uSunShadowMap, vec3(base + o * uSunShadowTexel.y, z)); }
float sunShadow(vec3 uvz) {
  float z = uvz.z - uSunShadowBias;
  if (uSunShadowFilter < 4) return texture(uSunShadowMap, vec3(uvz.xy, z));
  vec2 uv = uvz.xy * uSunShadowTexel.x;
  vec2 b = floor(uv + 0.5);
  float s = uv.x + 0.5 - b.x, t = uv.y + 0.5 - b.y;
  b = (b - 0.5) * uSunShadowTexel.y;
  if (uSunShadowFilter < 9) {
    float uw0 = 3.0 - 2.0 * s, uw1 = 1.0 + 2.0 * s, u0 = (2.0 - s) / uw0 - 1.0, u1 = s / uw1 + 1.0;
    float vw0 = 3.0 - 2.0 * t, vw1 = 1.0 + 2.0 * t, v0 = (2.0 - t) / vw0 - 1.0, v1 = t / vw1 + 1.0;
    float sum = uw0 * vw0 * shadowTap(b, vec2(u0, v0), z) + uw1 * vw0 * shadowTap(b, vec2(u1, v0), z)
              + uw0 * vw1 * shadowTap(b, vec2(u0, v1), z) + uw1 * vw1 * shadowTap(b, vec2(u1, v1), z);
    return sum / 16.0;
  }
  float uw0 = 4.0 - 3.0 * s, uw1 = 7.0, uw2 = 1.0 + 3.0 * s;
  float u0 = (3.0 - 2.0 * s) / uw0 - 2.0, u1 = (3.0 + s) / uw1, u2 = s / uw2 + 2.0;
  float vw0 = 4.0 - 3.0 * t, vw1 = 7.0, vw2 = 1.0 + 3.0 * t;
  float v0 = (3.0 - 2.0 * t) / vw0 - 2.0, v1 = (3.0 + t) / vw1, v2 = t / vw2 + 2.0;
  float sum = uw0 * vw0 * shadowTap(b, vec2(u0, v0), z) + uw1 * vw0 * shadowTap(b, vec2(u1, v0), z) + uw2 * vw0 * shadowTap(b, vec2(u2, v0), z)
            + uw0 * vw1 * shadowTap(b, vec2(u0, v1), z) + uw1 * vw1 * shadowTap(b, vec2(u1, v1), z) + uw2 * vw1 * shadowTap(b, vec2(u2, v1), z)
            + uw0 * vw2 * shadowTap(b, vec2(u0, v2), z) + uw1 * vw2 * shadowTap(b, vec2(u1, v2), z) + uw2 * vw2 * shadowTap(b, vec2(u2, v2), z);
  return sum / 144.0;
}
)GLSL";

constexpr const char* kFragmentMainHead = R"GLSL(
void main() {
  vec4 texel = uHasTexture == 1 ? texture(uTexture, vUV) : vec4(1.0);
  float alpha = clamp(vOpacity * uMaterial.a * texel.a, 0.0, 1.0);
  if (alpha < uAlphaCutoff) discard;
)GLSL";

constexpr const char* kFragmentBase = R"GLSL(
  vec3 rawBase = max(vec3(0.0), vColor * uMaterial.rgb * texel.rgb);
  vec3 base = linearInput ? rawBase : srgbToLinear(rawBase);
  vec3 n = normalize(vNormal), sun = normalize(uSunDirection);
  float surfaceGlass = 0.0;
  vec3 emissive = uEmissive;
)GLSL";

constexpr const char* kFragmentLighting = R"GLSL(
  float ndl = max(dot(n, sun), 0.0);
  vec3 hemis = mix(uGroundColor, uSkyColor, clamp(n.z * 0.5 + 0.5, 0.0, 1.0));
  float shadow = 1.0;
  if (uSunShadowEnabled == 1 && ndl > 0.0) {
    // Normal offset (Holbert 2011): look the shadow up a texel off the surface along its normal,
    // more where the light grazes it - no acne without a large constant bias (no detached shadows).
    vec3 lookup = vWorld + n * (uSunShadowTexel.z * (0.6 + 1.4 * (1.0 - ndl)));
    vec4 clip = uSunShadowProjection * vec4(lookup, 1.0);
    vec3 uvz = clip.xyz / clip.w * 0.5 + 0.5;
    if (uvz.x > 0.0 && uvz.x < 1.0 && uvz.y > 0.0 && uvz.y < 1.0 && uvz.z > 0.0) {
      shadow = sunShadow(uvz);
      float d = length(vWorld - uSunShadowFade.xyz);
      shadow = mix(shadow, 1.0, smoothstep(uSunShadowFade.w * 0.9, uSunShadowFade.w, d));
    }
  }
  vec3 point = vec3(0.0);
  if (uUnlit != 1) {
    for (int i = 0; i < 6; i++) {
      if (i >= uPointLightCount) break;
      vec3 delta = uPointPosRange[i].xyz - vWorld;
      float d2 = max(dot(delta, delta), 0.04);
      float radius = max(0.1, uPointPosRange[i].w);
      float attenuation = pow(max(0.0, 1.0 - d2 / (radius * radius)), 2.0) / (1.0 + d2 / (radius * radius));
      float diffuse = max(dot(n, delta * inversesqrt(d2)), 0.0);
      point += uPointColorIntensity[i].rgb * (uPointColorIntensity[i].a * 0.12) * attenuation * diffuse;
    }
  }
  const float PI = 3.141592653589793;
  float rough = clamp(uPbr.x, 0.04, 1.0), metal = clamp(uPbr.y, 0.0, 1.0);
  // Indirect light (sky, bounce, reflections) is tracked apart: ambient occlusion darkens only it,
  // sun and point lights keep their own shadowing.
  vec3 indirect = uUnlit == 1 ? vec3(0.0) : base * (1.0 - metal) * (hemis / PI + envIrradiance(n) * uEnvIntensity);
  vec3 rgb = uUnlit == 1 ? base : base * (1.0 - metal) * (uSunColor * ndl * shadow + point * PI) / PI + indirect;
  if (uUnlit != 1 && (metal > 0.0 || rough < 0.95)) {
    vec3 V = normalize(uEye - vWorld), R = reflect(-V, n);
    float NdV = clamp(dot(n, V), 0.0, 1.0);
    vec3 F0 = mix(vec3(0.04), base, metal);
    vec3 F = F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - NdV, 5.0);
    vec3 envR = mix(envIrradiance(R), skyReflection(R, 1.0), 1.0 - rough);
    rgb += F * envR * uEnvIntensity;
    indirect += F * envR * uEnvIntensity;
    vec3 H = normalize(sun + V);
    float a2 = rough * rough * rough * rough;
    float NdH = clamp(dot(n, H), 0.0, 1.0);
    float d = a2 / (PI * pow(NdH * NdH * (a2 - 1.0) + 1.0, 2.0));
    rgb += F * uSunColor * ndl * shadow * min(d, 40.0) * 0.25;
  }
  if (surfaceGlass > 0.0) {
    // Glass mirrors the sky (smooth PBR glass plus a Fresnel term), so panes read light, not black.
    vec3 V = normalize(uEye - vWorld);
    float fr = pow(1.0 - abs(dot(V, n)), 3.0);
    vec3 skyRefl = skyReflection(reflect(-V, n), 0.6) * uEnvIntensity;
    float g = surfaceGlass * (0.45 + 0.4 * fr);
    vec3 glassAdd = surfaceGlass * mix(vec3(0.05, 0.07, 0.09), vec3(0.32, 0.4, 0.5), fr);
    rgb = mix(rgb, skyRefl, g) + glassAdd;
    indirect = mix(indirect, skyRefl, g) + glassAdd;
  }
  rgb += emissive;
  outHdr = vec4(max(rgb, vec3(0.0)), alpha);
  rgb = acesFilmic(rgb);
  if (uGrade == 1) rgb = gradeColor(rgb);
  rgb = linearToSrgb(clamp(rgb, 0.0, 1.0));
  // FogExp2 after the colour-space conversion, as three.js applies it.
  vec3 toFrag = vWorld - uEye;
  float fogDist = length(toFrag);
  float fogOptical = uFogDensity * fogDist;
  if (uFogHeightFalloff > 0.0) {
    // Mean of exp(-k z) along the view ray: dense near the ground, clear above the roofs.
    float k = uFogHeightFalloff, z0 = max(uEye.z, 0.0), z1 = max(vWorld.z, 0.0), dz = z1 - z0;
    fogOptical *= abs(k * dz) > 1e-4 ? (exp(-k * z0) - exp(-k * z1)) / (k * dz) : exp(-k * z0);
  }
  float fog = 1.0 - exp(-fogOptical * fogOptical);
  vec3 fogColor = uFogColor;
  if (uFogSunScatter > 0.0 && fogDist > 1e-4) {
    // In-scattering: the haze glows in the sun's colour around the sun.
    float mu = max(dot(toFrag / fogDist, sun), 0.0);
    vec3 sunHaze = linearToSrgb(acesFilmic(uSunColor * 0.45));
    fogColor = mix(fogColor, sunHaze, uFogSunScatter * pow(mu, 8.0));
  }
  rgb = mix(rgb, fogColor, clamp(fog, 0.0, 1.0));
  outColor = vec4(clamp(rgb, 0.0, 1.0), alpha);
  if (uAoOut == 1) {
    vec3 total = max(outHdr.rgb, vec3(1e-4));
    outAo = vec4(clamp(indirect / total, 0.0, 1.0) * (1.0 - clamp(fog, 0.0, 1.0)), dot(base, vec3(0.2126, 0.7152, 0.0722)));
  } else {
    outAo = vec4(0.0, 0.0, 0.0, alpha);  // not in the AO depth (transparent, first person): unoccluded
  }
}
)GLSL";

}  // namespace

ShaderSource build_standard_shader(Span<ShaderExtension* const> extensions) {
  ShaderSource s;
  s.vertex.reserve(6000);
  s.fragment.reserve(16000);

  s.vertex += kVertexHead;
  for (ShaderExtension* e : extensions) {
    const Span<const ExtensionAttribute> attrs = e->attributes();
    for (std::size_t i = 0; i < attrs.size; ++i) {
      char line[160];
      const u32 size = attrs[i].size;
      std::snprintf(line, sizeof line, "layout(location=%u) in %s %s;\n", e->attribute_base + static_cast<u32>(i),
                    size == 1 ? "float" : size == 2 ? "vec2" : size == 3 ? "vec3" : "vec4", attrs[i].glsl);
      s.vertex += line;
    }
  }
  s.vertex += kVertexUniforms;
  append_hook(s.vertex, extensions, &ShaderExtension::vertex_declarations);
  s.vertex += kVertexMainHead;
  append_hook(s.vertex, extensions, &ShaderExtension::vertex);
  s.vertex += "  gl_Position = uViewProjection * p;\n}\n";

  s.fragment += kFragmentHead;
  s.fragment += kGlslToneMap;
  s.fragment += kFragmentFunctions;
  s.fragment += kFragmentShadow;
  append_hook(s.fragment, extensions, &ShaderExtension::fragment_declarations);
  s.fragment += kFragmentMainHead;
  append_hook(s.fragment, extensions, &ShaderExtension::fragment_discard);
  s.fragment += "  bool linearInput = uLinearColor == 1;\n";
  append_hook(s.fragment, extensions, &ShaderExtension::fragment_input);
  s.fragment += kFragmentBase;
  append_hook(s.fragment, extensions, &ShaderExtension::fragment_surface);
  s.fragment += kFragmentLighting;
  return s;
}

}  // namespace kern
