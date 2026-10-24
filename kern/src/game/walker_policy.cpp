#include "kern/game/walker_policy.h"
#include "kern/core/dmath.h"

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
      b[static_cast<std::size_t>(o)] = layer + 1 < weights.size() ? (sum > 0 ? sum : kern::dm::expm1(sum)) : sum;  // ELU (alpha 1)
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

// ---- the pick-up detector (robotd's, duck-control pickup.rs) ---------------------------------------

namespace {

struct OnnxTensor {
  std::vector<i64> dims;
  std::vector<f32> f;
  std::vector<i64> i;
};

bool parse_tensor(Span<const u8> b, OnnxTensor& t, std::string& name) {
  std::vector<Field> sub;
  if (!fields(b, sub)) return false;
  i32 type = 1;
  Span<const u8> raw;
  for (const Field& g : sub) {
    if (g.number == 1 && g.type == 0) t.dims.push_back(static_cast<i64>(g.value));
    if (g.number == 1 && g.type == 2) {
      std::size_t i = 0;
      u64 d = 0;
      while (i < g.bytes.size && varint(g.bytes, i, d)) t.dims.push_back(static_cast<i64>(d));
    }
    if (g.number == 2 && g.type == 0) type = static_cast<i32>(g.value);
    if (g.number == 4 && g.type == 2) t.f = floats(g.bytes);
    if (g.number == 7 && g.type == 2) {
      std::size_t i = 0;
      u64 d = 0;
      while (i < g.bytes.size && varint(g.bytes, i, d)) t.i.push_back(static_cast<i64>(d));
    }
    if (g.number == 7 && g.type == 0) t.i.push_back(static_cast<i64>(g.value));
    if (g.number == 8) name = text(g.bytes);
    if (g.number == 9) raw = g.bytes;
  }
  if (!raw.empty()) {
    if (type == 1) t.f = floats(raw);
    else if (type == 7) {
      t.i.resize(raw.size / 8);
      if (!t.i.empty()) std::memcpy(t.i.data(), raw.data, t.i.size() * 8);
    } else {
      return false;
    }
  }
  return type == 1 || type == 7;
}

struct OnnxNode {
  std::string op;
  std::vector<std::string> in, out;
  std::unordered_map<std::string, std::vector<i64>> ints;
  std::unordered_map<std::string, f32> floats;
  OnnxTensor value;  // a Constant's
  i64 attr(const std::string& k, i64 fallback) const {
    const auto it = ints.find(k);
    return it == ints.end() || it->second.empty() ? fallback : it->second[0];
  }
};

bool parse_node(Span<const u8> b, OnnxNode& n) {
  std::vector<Field> sub, at;
  if (!fields(b, sub)) return false;
  for (const Field& g : sub) {
    if (g.number == 1) n.in.push_back(text(g.bytes));
    if (g.number == 2) n.out.push_back(text(g.bytes));
    if (g.number == 4) n.op = text(g.bytes);
    if (g.number != 5 || !fields(g.bytes, at)) continue;  // AttributeProto
    std::string name;
    std::vector<i64> ints;
    bool has_float = false;
    f32 fl = 0;
    for (const Field& a : at) {
      if (a.number == 1) name = text(a.bytes);
      if (a.number == 2 && a.type == 5) { std::memcpy(&fl, a.bytes.data, 4); has_float = true; }
      if ((a.number == 3 || a.number == 8) && a.type == 0) ints.push_back(static_cast<i64>(a.value));
      if (a.number == 8 && a.type == 2) {
        std::size_t i = 0;
        u64 d = 0;
        while (i < a.bytes.size && varint(a.bytes, i, d)) ints.push_back(static_cast<i64>(d));
      }
      if (a.number == 5 && a.type == 2) {
        std::string ignored;
        if (!parse_tensor(a.bytes, n.value, ignored)) return false;
      }
    }
    if (has_float) n.floats[name] = fl;
    n.ints[name] = std::move(ints);
  }
  return true;
}

inline f32 elu(f32 x, f32 alpha) { return x > 0 ? x : alpha * kern::dm::expm1(x); }

}  // namespace

bool parse_onnx_pickup(Span<const u8> bytes, PickupDetector& out, std::string* error) {
  std::vector<Field> model, graph;
  if (!fields(bytes, model)) return fail(error, "not an ONNX file (protobuf)");
  out = PickupDetector{};
  Span<const u8> graph_bytes;
  for (const Field& f : model)
    if (f.number == 7 && f.type == 2) graph_bytes = f.bytes;
  if (graph_bytes.empty() || !fields(graph_bytes, graph)) return fail(error, "ONNX file without a graph");
  std::unordered_map<std::string, OnnxTensor> inits;
  std::vector<OnnxNode> nodes;
  for (const Field& f : graph) {
    if (f.type != 2) continue;
    if (f.number == 5) {
      OnnxTensor t;
      std::string name;
      if (!parse_tensor(f.bytes, t, name)) return fail(error, "bad ONNX tensor");
      inits[name] = std::move(t);
    } else if (f.number == 1) {
      OnnxNode n;
      if (!parse_node(f.bytes, n)) return fail(error, "bad ONNX node");
      nodes.push_back(std::move(n));
    }
  }
  auto init = [&](const OnnxNode& n, std::size_t k) -> const OnnxTensor* {
    if (k >= n.in.size()) return nullptr;
    const auto it = inits.find(n.in[k]);
    return it == inits.end() ? nullptr : &it->second;
  };
  std::size_t k = 0;
  auto next = [&](const char* op) { return k < nodes.size() && nodes[k].op == op ? &nodes[k++] : nullptr; };
  // Normalise and mask.
  const OnnxNode* sub = next("Sub");
  const OnnxNode* div = next("Div");
  const OnnxNode* mul = next("Mul");
  const OnnxTensor *mean = sub ? init(*sub, 1) : nullptr, *std_ = div ? init(*div, 1) : nullptr, *mask = mul ? init(*mul, 1) : nullptr;
  if (!mean || !std_ || !mask || mean->f.size() != static_cast<std::size_t>(kPickupFeatures) || std_->f.size() != mean->f.size() || mask->f.size() != mean->f.size())
    return fail(error, "not the pick-up detector (Sub, Div, Mul over 63 features)");
  out.mean = mean->f;
  out.stdev = std_->f;
  out.mask = mask->f;
  // The columns the convolutions read.
  const OnnxNode* constant = next("Constant");
  const OnnxNode* gather = next("Gather");
  if (!constant || !gather || gather->attr("axis", 0) != 2 || constant->value.i.empty()) return fail(error, "expected the column Gather");
  for (i64 c : constant->value.i) {
    if (c < 0 || c >= kPickupFeatures) return fail(error, "Gather index out of range");
    out.columns.push_back(static_cast<i32>(c));
  }
  const OnnxNode* tr = next("Transpose");
  if (!tr || tr->ints.count("perm") == 0 || tr->ints.at("perm") != std::vector<i64>{0, 2, 1}) return fail(error, "expected Transpose(0, 2, 1)");
  // Convolutions over time.
  i32 channels = static_cast<i32>(out.columns.size()), length = kPickupWindow;
  bool alpha_set = false;
  auto take_alpha = [&](const OnnxNode& e) {
    const auto it = e.floats.find("alpha");
    const f32 a = it == e.floats.end() ? 1.0f : it->second;
    if (alpha_set && a != out.alpha) return false;
    out.alpha = a;
    alpha_set = true;
    return true;
  };
  while (const OnnxNode* conv = next("Conv")) {
    const OnnxTensor *w = init(*conv, 1), *b = init(*conv, 2);
    if (!w || !b || w->dims.size() != 3 || w->dims[1] != channels || static_cast<i64>(b->f.size()) != w->dims[0]) return fail(error, "Conv shapes do not chain");
    if (conv->attr("group", 1) != 1 || conv->attr("dilations", 1) != 1) return fail(error, "unsupported Conv (group, dilation)");
    for (const char* pad : {"pads"})
      if (conv->ints.count(pad))
        for (i64 v : conv->ints.at(pad)) if (v != 0) return fail(error, "unsupported Conv padding");
    PickupDetector::Conv c;
    c.out = static_cast<i32>(w->dims[0]);
    c.in = channels;
    c.kernel = static_cast<i32>(w->dims[2]);
    c.stride = static_cast<i32>(conv->attr("strides", 1));
    c.w = w->f;
    c.b = b->f;
    if (c.stride < 1 || length < c.kernel) return fail(error, "Conv window too short");
    length = (length - c.kernel) / c.stride + 1;
    channels = c.out;
    out.convs.push_back(std::move(c));
    const OnnxNode* e = next("Elu");
    if (!e || !take_alpha(*e)) return fail(error, "expected Elu after a Conv");
  }
  if (out.convs.empty() || !next("Flatten")) return fail(error, "expected Conv... Flatten");
  out.dims.push_back(channels * length);
  // The head.
  while (const OnnxNode* g = next("Gemm")) {
    const OnnxTensor *w = init(*g, 1), *b = init(*g, 2);
    if (!w || !b || w->dims.size() != 2 || g->attr("transB", 0) != 1 || w->dims[1] != out.dims.back() || static_cast<i64>(b->f.size()) != w->dims[0])
      return fail(error, "Gemm shapes do not chain");
    out.weights.push_back(w->f);
    out.biases.push_back(b->f);
    out.dims.push_back(static_cast<i32>(w->dims[0]));
    if (const OnnxNode* e = next("Elu")) { if (!take_alpha(*e)) return fail(error, "ELU alphas differ"); }
    else break;
  }
  if (out.dims.back() != 1) return fail(error, "the head does not end in one logit");
  next("Constant");
  next("Squeeze");
  if (!next("Sigmoid") || k != nodes.size()) return fail(error, "expected the Sigmoid at the end");
  return true;
}

f32 PickupDetector::run(const f32* window) const {
  // Normalise, mask and pick the columns; laid out channel-major, as after the Transpose.
  const i32 n_in = static_cast<i32>(columns.size());
  std::vector<f32> a(static_cast<std::size_t>(n_in * kPickupWindow)), b;
  for (i32 t = 0; t < kPickupWindow; ++t)
    for (i32 c = 0; c < n_in; ++c) {
      const auto col = static_cast<std::size_t>(columns[static_cast<std::size_t>(c)]);
      a[static_cast<std::size_t>(c * kPickupWindow + t)] = (window[static_cast<std::size_t>(t * kPickupFeatures) + col] - mean[col]) / stdev[col] * mask[col];
    }
  i32 length = kPickupWindow;
  for (const Conv& cv : convs) {
    const i32 n = (length - cv.kernel) / cv.stride + 1;
    b.assign(static_cast<std::size_t>(cv.out * n), 0.0f);
    for (i32 o = 0; o < cv.out; ++o)
      for (i32 t = 0; t < n; ++t) {
        f32 sum = cv.b[static_cast<std::size_t>(o)];
        for (i32 c = 0; c < cv.in; ++c) {
          const f32* w = cv.w.data() + static_cast<std::size_t>((o * cv.in + c) * cv.kernel);
          const f32* x = a.data() + static_cast<std::size_t>(c * length + t * cv.stride);
          for (i32 j = 0; j < cv.kernel; ++j) sum += w[j] * x[j];
        }
        b[static_cast<std::size_t>(o * n + t)] = elu(sum, alpha);
      }
    std::swap(a, b);
    length = n;
  }
  for (std::size_t layer = 0; layer < weights.size(); ++layer) {
    const i32 ni = dims[layer], no = dims[layer + 1];
    b.assign(static_cast<std::size_t>(no), 0.0f);
    for (i32 o = 0; o < no; ++o) {
      f32 sum = biases[layer][static_cast<std::size_t>(o)];
      const f32* row = weights[layer].data() + static_cast<std::size_t>(o * ni);
      for (i32 i = 0; i < ni; ++i) sum += row[i] * a[static_cast<std::size_t>(i)];
      b[static_cast<std::size_t>(o)] = layer + 1 < weights.size() ? elu(sum, alpha) : sum;
    }
    std::swap(a, b);
  }
  return 1.0f / (1.0f + kern::dm::exp(-a[0]));
}

const PickupDetector* pickup_detector() {
  static const u8 kBytes[] = {
#include "../../third_party/microduck/pickup_detector.inc"
  };
  static const std::unique_ptr<PickupDetector> d = [] {
    auto p = std::make_unique<PickupDetector>();
    if (!parse_onnx_pickup(Span<const u8>(kBytes, sizeof kBytes), *p)) p.reset();
    return p;
  }();
  return d.get();
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
