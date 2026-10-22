// The standard surface shader and the extension mechanism games use to add
// their own looks without forking the engine shader.
//
// One program draws every mesh (no program switches inside the scene pass):
//   * instancing (model matrix, normal matrix, RGBA tint per instance)
//   * GPU skinning (32 bones), sun shadow map (hardware PCF), 6 point lights
//   * MeshStandardMaterial-style PBR: Lambert sun + hemisphere + sky
//     irradiance, Schlick/GGX specular, glass sky reflection
//   * alpha mask / blend, optional albedo texture, unlit materials
//   * ACES filmic tone map, optional colour grade, FogExp2 after sRGB, and a
//     linear HDR copy in attachment 1 for the bloom pass
//
// Hook points (GLSL snippets) a ShaderExtension can fill:
//   vertex_declarations   uniforms/outs of the vertex stage
//   vertex                after the default varyings; may change vColor/vOpacity/vUV
//                         (sees model, local, aColor, aUV, aNormal, iTint, iModel*)
//   fragment_declarations uniforms/ins/functions of the fragment stage
//   fragment_discard      early, may `discard` (sees vWorld, alpha, texel)
//   fragment_input        may change `linearInput` (vertex colour already linear)
//   fragment_surface      may change base, alpha, surfaceGlass, emissive (linear HDR,
//                         added after lighting) or discard (sees n, sun, uEye, vWorld, vUV)
#pragma once

#include <string>

#include "kern/gfx/mesh.h"
#include "kern/gfx/program.h"

namespace kern {

class Renderer;

struct ExtensionAttribute {
  const char* key;   // name used by hosts to supply the stream (e.g. "facade")
  u32 size;          // 1..4 floats per vertex
  const char* glsl;  // attribute name in GLSL (e.g. "aFacade")
};

class ShaderExtension {
 public:
  virtual ~ShaderExtension() = default;
  virtual const char* name() const = 0;
  /// Extra vertex streams (at most kMaxExtensionAttributes over all extensions).
  virtual Span<const ExtensionAttribute> attributes() const { return {}; }
  /// Names of the integer material parameters this extension reads.
  virtual Span<const char* const> material_keys() const { return {}; }

  virtual const char* vertex_declarations() const { return ""; }
  virtual const char* vertex() const { return ""; }
  virtual const char* fragment_declarations() const { return ""; }
  virtual const char* fragment_discard() const { return ""; }
  virtual const char* fragment_input() const { return ""; }
  virtual const char* fragment_surface() const { return ""; }

  /// Resolve uniform locations after the standard program (re)links.
  virtual void bind(const Program& program) { (void)program; }
  /// Once per scene pass, after the engine's frame uniforms.
  virtual void frame_uniforms(UniformCache& cache, const Renderer& renderer) { (void)cache; (void)renderer; }
  /// Per draw. `params` points at this extension's material parameters.
  virtual void material_uniforms(UniformCache& cache, const Material& material, const i32* params) {
    (void)cache; (void)material; (void)params;
  }

  /// Set by the renderer: index of this extension's first material parameter.
  u32 material_base = 0;
  /// Set by the renderer: vertex location of this extension's first attribute.
  u32 attribute_base = attr::extension0;
};

struct ShaderSource {
  std::string vertex, fragment;
};

/// Assembles the standard shader with the given extensions.
ShaderSource build_standard_shader(Span<ShaderExtension* const> extensions);

/// GLSL snippets shared by passes: ACES filmic (three.js r16x fit), sRGB
/// encode and the production colour grade. Each expects `uniform float uExposure`.
extern const char* const kGlslToneMap;

}  // namespace kern
