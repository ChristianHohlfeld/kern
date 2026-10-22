// Counters of the host-side WebGL2 stand-in (gl_stub.cpp).
#pragma once

#include "kern/core/base.h"

struct GlStub {
  kern::u64 buffers_created = 0, buffers_deleted = 0, vaos_created = 0, vaos_deleted = 0;
  kern::u64 textures_created = 0, textures_deleted = 0, texture_binds = 0;
  kern::u64 programs_created = 0, programs_deleted = 0;
  kern::u64 buffer_bytes = 0, buffer_data_calls = 0, buffer_sub_data_calls = 0, shader_source_bytes = 0;
  kern::u64 uniform_calls = 0, draws = 0, instances = 0;
  bool extensions_available = true;
  void reset();
};

extern GlStub g_gl;
