#include "kern/game/walker_policy.h"

#include <cmath>
#include <cstring>
#include <unordered_map>

#include "kern/game/walker.h"

namespace kern::game {

namespace {

// ---- protobuf wire format (just what an ONNX model needs) ----------------------------------------
struct Field {
  u32 number = 0, type = 0;
  u64 value = 0;               // varint
  Span<const u8> bytes;        // length-delimited / fixed
};

bool varint(Span<const u8> b, std::size_t& i, u64& out) {
  out = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (i >= b.size) return false;
    const u8 x = b[i++];
    out |= static_cast<u64>(x & 0x7f) << shift;
    if (x < 0x80) return true;
  }
  return false;
}

bool fields(Span<const u8> b, std::vector<Field>& out) {
  out.clear();
  std::size_t i = 0;
  while (i < b.size) {
    u64 key = 0;
    if (!varint(b, i, key)) return false;
    Field f;
    f.number = static_cast<u32>(key >> 3);
    f.type = static_cast<u32>(key & 7);
    if (f.type == 0) {
      if (!varint(b, i, f.value)) return false;
    } else if (f.type == 1 || f.type == 5) {
      const std::size_t n = f.type == 1 ? 8 : 4;
      if (i + n > b.size) return false;
      f.bytes = Span<const u8>(b.data + i, n);
      i += n;
    } else if (f.type == 2) {
      u64 n = 0;
      if (!varint(b, i, n) || n > b.size - i) return false;
      f.bytes = Span<const u8>(b.data + i, static_cast<std::size_t>(n));
      i += static_cast<std::size_t>(n);
    } else {
      return false;
    }
    out.push_back(f);
  }
  return true;
}

std::string text(Span<const u8> b) { return std::string(reinterpret_cast<const char*>(b.data), b.size); }

std::vector<f32> floats(Span<const u8> b) {
  std::vector<f32> v(b.size / 4);
  if (!v.empty()) std::memcpy(v.data(), b.data, v.size() * 4);  // little endian, like the file
  return v;
}

bool fail(std::string* error, const char* what) {
  if (error) *error = what;
  return false;
}

std::unordered_map<std::string, std::shared_ptr<const WalkerPolicy>>& registry() {
  static std::unordered_map<std::string, std::shared_ptr<const WalkerPolicy>> r;
  return r;
}

}  // namespace

bool parse_onnx_policy(Span<const u8> bytes, WalkerPolicy& out, std::string* error) {
  std::vector<Field> model, graph, sub;
  if (!fields(bytes, model)) return fail(error, "not an ONNX file (protobuf)");
  out = WalkerPolicy{};
  Span<const u8> graph_bytes;
  for (const Field& f : model) {
    if (f.number == 7 && f.type == 2) graph_bytes = f.bytes;
    if (f.number == 14 && f.type == 2 && fields(f.bytes, sub)) {
      std::string k, v;
      for (const Field& g : sub) {
        if (g.number == 1) k = text(g.bytes);
        if (g.number == 2) v = text(g.bytes);
      }
      out.metadata.emplace_back(k, v);
    }
  }
  if (graph_bytes.empty() || !fields(graph_bytes, graph)) return fail(error, "ONNX file without a graph");
  struct Tensor { std::vector<i64> dims; std::vector<f32> data; };
  std::unordered_map<std::string, Tensor> inits;
  struct Node { std::string op; std::vector<std::string> in; };
  std::vector<Node> nodes;
  for (const Field& f : graph) {
    if (f.type != 2) continue;
    if (f.number == 5) {  // initializer (TensorProto)
      if (!fields(f.bytes, sub)) return fail(error, "bad ONNX tensor");
      Tensor t;
      std::string name;
      i32 type = 1;
      for (const Field& g : sub) {
        if (g.number == 1 && g.type == 0) t.dims.push_back(static_cast<i64>(g.value));
        if (g.number == 1 && g.type == 2) {
          std::size_t i = 0;
          u64 d = 0;
          while (i < g.bytes.size && varint(g.bytes, i, d)) t.dims.push_back(static_cast<i64>(d));
        }
        if (g.number == 2 && g.type == 0) type = static_cast<i32>(g.value);
        if (g.number == 8) name = text(g.bytes);
        if (g.number == 9) t.data = floats(g.bytes);
        if (g.number == 4 && g.type == 2) t.data = floats(g.bytes);
      }
      if (type != 1) return fail(error, "ONNX tensor is not float32");
      inits[name] = std::move(t);
    } else if (f.number == 1) {  // node
      if (!fields(f.bytes, sub)) return fail(error, "bad ONNX node");
      Node n;
      for (const Field& g : sub) {
        if (g.number == 1) n.in.push_back(text(g.bytes));
        if (g.number == 4) n.op = text(g.bytes);
      }
      nodes.push_back(std::move(n));
    }
  }
  // The exported rsl_rl actor: Sub(mean) Div(std + eps), then (Gemm Elu)* Gemm.
  if (nodes.size() < 4 || nodes[0].op != "Sub" || nodes[1].op != "Div") return fail(error, "not an rsl_rl actor (Sub, Div, Gemm...)");
  auto init = [&](const Node& n, std::size_t k) -> const Tensor* {
    if (k >= n.in.size()) return nullptr;
    auto it = inits.find(n.in[k]);
    return it == inits.end() ? nullptr : &it->second;
  };
  const Tensor* mean = init(nodes[0], 1);
  const Tensor* div = init(nodes[1], 1);
  if (!mean || !div || mean->data.size() != div->data.size()) return fail(error, "missing observation normaliser");
  out.mean = mean->data;
  out.div = div->data;
  out.dims.push_back(static_cast<i32>(mean->data.size()));
  for (std::size_t k = 2; k < nodes.size(); ++k) {
    const Node& n = nodes[k];
    const bool last = k + 1 == nodes.size();
    if (n.op == "Gemm") {
      const Tensor* w = init(n, 1);
      const Tensor* b = init(n, 2);
      if (!w || !b || w->dims.size() != 2) return fail(error, "Gemm without weights");
      const i64 n_out = w->dims[0], n_in = w->dims[1];
      if (n_in != out.dims.back() || static_cast<i64>(b->data.size()) != n_out || static_cast<i64>(w->data.size()) != n_out * n_in)
        return fail(error, "Gemm shapes do not chain");
      out.weights.push_back(w->data);
      out.biases.push_back(b->data);
      out.dims.push_back(static_cast<i32>(n_out));
      const bool elu_next = k + 1 < nodes.size() && nodes[k + 1].op == "Elu";
      if (!last && !elu_next) return fail(error, "expected Elu after a hidden Gemm");
    } else if (n.op != "Elu") {
      return fail(error, "unsupported ONNX operator");
    }
  }
  if (nodes.back().op != "Gemm") return fail(error, "the output layer is not linear");
  return true;
}

void WalkerPolicy::run(const f32* observation, f32* action) const {
  std::vector<f32> a(static_cast<std::size_t>(dims[0])), b;
  for (std::size_t i = 0; i < a.size(); ++i) a[i] = (observation[i] - mean[i]) / div[i];
  for (std::size_t layer = 0; layer < weights.size(); ++layer) {
    const i32 n_in = dims[layer], n_out = dims[layer + 1];
    b.assign(static_cast<std::size_t>(n_out), 0.0f);
    const f32* w = weights[layer].data();
    for (i32 o = 0; o < n_out; ++o) {
      f32 sum = biases[layer][static_cast<std::size_t>(o)];
      const f32* row = w + static_cast<std::size_t>(o) * static_cast<std::size_t>(n_in);
      for (i32 i = 0; i < n_in; ++i) sum += row[i] * a[static_cast<std::size_t>(i)];
      b[static_cast<std::size_t>(o)] = layer + 1 < weights.size() ? (sum > 0 ? sum : std::expm1(sum)) : sum;  // ELU (alpha 1)
    }
    std::swap(a, b);
  }
  std::memcpy(action, a.data(), a.size() * sizeof(f32));
}

std::string WalkerPolicy::meta(std::string_view key) const {
  for (const auto& [k, v] : metadata)
    if (k == key) return v;
  return {};
}

void register_walker_policy(std::shared_ptr<const WalkerPolicy> policy) {
  if (policy) registry()[policy->file] = std::move(policy);
}

std::shared_ptr<const WalkerPolicy> walker_policy(std::string_view file) {
  auto& r = registry();
  if (auto it = r.find(std::string(file)); it != r.end()) return it->second;
  if (file == "velstand.onnx") {
    auto p = std::make_shared<WalkerPolicy>(builtin_velstand_policy());
    r["velstand.onnx"] = p;
    return p;
  }
  return nullptr;
}

std::vector<std::string> walker_policy_files() {
  walker_policy("velstand.onnx");
  std::vector<std::string> out;
  for (const auto& [k, v] : registry()) out.push_back(k);
  return out;
}

const char* micro_duck_manifest() {
  static const char* const kManifest =
#include "../../third_party/microduck/micro_duck_manifest.inc"
      ;
  return kManifest;
}

}  // namespace kern::game
