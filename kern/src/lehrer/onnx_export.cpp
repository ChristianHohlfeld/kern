// ONNX export (see onnx_export.h): a small protobuf writer, just the messages a model needs.
#include "kern/lehrer/onnx_export.h"

#include <cstring>

namespace kern::lehrer {
namespace {

class Pb {
 public:
  std::vector<u8> bytes;
  void varint(u64 v) {
    while (v >= 0x80) { bytes.push_back(static_cast<u8>(v | 0x80)); v >>= 7; }
    bytes.push_back(static_cast<u8>(v));
  }
  void key(u32 field, u32 type) { varint((static_cast<u64>(field) << 3) | type); }
  void integer(u32 field, u64 v) { key(field, 0); varint(v); }
  void f32_field(u32 field, f32 v) {
    key(field, 5);
    u8 b[4];
    std::memcpy(b, &v, 4);
    bytes.insert(bytes.end(), b, b + 4);
  }
  void blob(u32 field, const void* data, std::size_t n) {
    key(field, 2);
    varint(n);
    const u8* p = static_cast<const u8*>(data);
    bytes.insert(bytes.end(), p, p + n);
  }
  void str(u32 field, const std::string& s) { blob(field, s.data(), s.size()); }
  void msg(u32 field, const Pb& m) { blob(field, m.bytes.data(), m.bytes.size()); }
};

Pb tensor(const std::string& name, const std::vector<i64>& dims, const std::vector<f32>& data) {
  Pb t;
  for (i64 d : dims) t.integer(1, static_cast<u64>(d));
  t.integer(2, 1);  // FLOAT
  t.str(8, name);
  t.blob(9, data.data(), data.size() * sizeof(f32));  // raw_data, little endian
  return t;
}

Pb value_info(const std::string& name, i64 width) {
  Pb dim1, dim2, shape, tensor_type, type, info;
  dim1.integer(1, 1);
  dim2.integer(1, static_cast<u64>(width));
  shape.msg(1, dim1);
  shape.msg(1, dim2);
  tensor_type.integer(1, 1);  // FLOAT
  tensor_type.msg(2, shape);
  type.msg(1, tensor_type);
  info.str(1, name);
  info.msg(2, type);
  return info;
}

Pb node(const std::string& op, const std::vector<std::string>& in, const std::string& out, const std::string& name) {
  Pb n;
  for (const std::string& i : in) n.str(1, i);
  n.str(2, out);
  n.str(3, name);
  n.str(4, op);
  return n;
}

}  // namespace

std::vector<u8> export_onnx(const Mlp& actor, const Normalizer& normalizer,
                            const std::vector<std::pair<std::string, std::string>>& metadata) {
  Pb graph;
  const i64 n_in = actor.inputs();
  graph.msg(1, node("Sub", {"obs", "obs_mean"}, "obs_centered", "normalizer_sub"));
  graph.msg(1, node("Div", {"obs_centered", "obs_div"}, "obs_normalized", "normalizer_div"));
  std::string x = "obs_normalized";
  for (i32 l = 0; l < actor.layers(); ++l) {
    const bool last = l == actor.layers() - 1;
    const std::string y = last ? "actions" : "gemm_" + std::to_string(l);
    Pb gemm = node("Gemm", {x, "w" + std::to_string(l), "b" + std::to_string(l)}, y, "gemm_" + std::to_string(l));
    Pb trans;
    trans.str(1, "transB");
    trans.integer(3, 1);
    trans.integer(20, 2);  // INT
    gemm.msg(5, trans);
    graph.msg(1, gemm);
    x = y;
    if (!last) {
      const std::string e = "elu_" + std::to_string(l);
      Pb elu = node("Elu", {x}, e, e);
      Pb alpha;
      alpha.str(1, "alpha");
      alpha.f32_field(2, 1.0f);
      alpha.integer(20, 1);  // FLOAT
      elu.msg(5, alpha);
      graph.msg(1, elu);
      x = e;
    }
  }
  graph.str(2, "kern_policy");
  graph.msg(5, tensor("obs_mean", {n_in}, normalizer.mean()));
  graph.msg(5, tensor("obs_div", {n_in}, normalizer.divisor()));
  for (i32 l = 0; l < actor.layers(); ++l) {
    const i32 in = actor.dims()[static_cast<std::size_t>(l)], out = actor.dims()[static_cast<std::size_t>(l) + 1];
    std::vector<f32> w(static_cast<std::size_t>(in) * static_cast<std::size_t>(out));  // [out][in] (transB)
    const f32* src = actor.weight(l);
    for (i32 i = 0; i < in; ++i)
      for (i32 o = 0; o < out; ++o) w[static_cast<std::size_t>(o) * in + i] = src[static_cast<std::size_t>(i) * out + o];
    graph.msg(5, tensor("w" + std::to_string(l), {out, in}, w));
    graph.msg(5, tensor("b" + std::to_string(l), {out}, std::vector<f32>(actor.bias(l), actor.bias(l) + out)));
  }
  graph.msg(11, value_info("obs", n_in));
  graph.msg(12, value_info("actions", actor.outputs()));
  Pb model;
  model.integer(1, 8);  // IR version 8
  model.str(2, "kern");
  model.str(3, "1");
  Pb opset;
  opset.str(1, "");
  opset.integer(2, 17);
  model.msg(8, opset);
  model.msg(7, graph);
  for (const auto& [k, v] : metadata) {
    Pb e;
    e.str(1, k);
    e.str(2, v);
    model.msg(14, e);
  }
  return model.bytes;
}

std::vector<std::pair<std::string, std::string>> micro_duck_metadata(const std::string& task) {
  // As in Pollen's networks (ball_kick_right.onnx): the robot reads the joint order and home pose.
  return {
      {"action_scale", "1.0"},
      {"command_names", "twist"},
      {"observation_names", "base_ang_vel,projected_gravity,joint_pos,joint_vel,actions,command,head_command,body_command"},
      {"joint_names", "left_hip_yaw,left_hip_roll,left_hip_pitch,left_knee,left_ankle,neck_pitch,head_pitch,head_yaw,head_roll,right_hip_yaw,right_hip_roll,right_hip_pitch,right_knee,right_ankle"},
      {"default_joint_pos", "0.000,-0.087,-0.458,-0.005,0.453,0.349,0.349,0.000,0.000,0.000,0.087,0.458,0.005,-0.453"},
      {"joint_stiffness", "1.000,1.000,1.000,1.000,1.000,1.000,1.000,1.000,1.000,1.000,1.000,1.000,1.000,1.000"},
      {"joint_damping", "0.000,0.000,0.000,0.000,0.000,0.000,0.000,0.000,0.000,0.000,0.000,0.000,0.000,0.000"},
      {"task", task},
      {"lehrer", "Kern (C++ PPO on Box3D)"},
  };
}

}  // namespace kern::lehrer
