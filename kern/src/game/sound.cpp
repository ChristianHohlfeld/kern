#include "kern/game/sound.h"

#include <algorithm>
#include <cmath>

namespace kern::game {

namespace {

bool is_builtin(std::string_view ref) { return ref.rfind("builtin:", 0) == 0; }
bool is_silent(std::string_view ref) { return ref.empty() || ref == "none" || ref == "auto"; }
f32 damp(f32 current, f32 target, f32 rate, f32 dt) { return target + (current - target) * std::exp(-rate * dt); }

}  // namespace

std::string engine_sound_for(const ControllerComponent& c) {
  if (c.engine_sound != "auto") return is_silent(c.engine_sound) ? std::string() : c.engine_sound;
  switch (c.mode) {
    case ControlMode::hovercar: return "builtin:hover";
    case ControlMode::flyer: return "builtin:hover";
    case ControlMode::vehicle: return "builtin:engine";
    case ControlMode::rolling:
    case ControlMode::character:
    case ControlMode::soldier: return "";
    case ControlMode::walker: return "builtin:servo";
  }
  return "";
}

// ---------------------------------------------------------------------------
// SoundBank

SoundBank::SoundBank(audio::Mixer& mixer) : mixer_(mixer) {}
SoundBank::~SoundBank() = default;

SoundBank::Entry* SoundBank::entry(std::string_view ref) {
  const auto it = entries_.find(std::string(ref));
  return it == entries_.end() ? nullptr : &it->second;
}

const SoundBank::Entry* SoundBank::entry(std::string_view ref) const {
  const auto it = entries_.find(std::string(ref));
  return it == entries_.end() ? nullptr : &it->second;
}

void SoundBank::request(std::string_view ref) {
  if (is_silent(ref)) return;
  Entry& e = entries_[std::string(ref)];
  if (e.clip || e.failed || e.queued || e.requested) return;
  if (is_builtin(ref)) {
    if (!audio::find_builtin(ref.substr(8))) {
      e.failed = true;
      e.error = "unknown built-in sound";
      return;
    }
    e.queued = true;
    queue_.emplace_back(ref);
  } else {
    e.requested = true;  // the host decodes it (pending_imports)
    const auto a = assets_.find(std::string(ref));
    if (a != assets_.end()) e.src = a->second;
  }
}

audio::ClipId SoundBank::clip(std::string_view ref) {
  if (is_silent(ref)) return 0;
  if (const Entry* e = entry(ref); e && e->clip) return e->clip;
  request(ref);
  return 0;
}

void SoundBank::request_level(const Level& level) {
  sync_assets(level);
  const WorldSettings& w = level.world;
  request(w.music);
  request(w.ambience);
  bool goal = false, lose = w.time_limit > 0 || w.kill_z > -1e8f;
  for (const Actor& a : level.actors) {
    if (a.body) {
      request(a.body->impact_sound);
      if (a.body->impact_sound == "builtin:impact") request("builtin:impact_light");
    }
    for (const JointComponent& j : a.joints) request(j.break_sound);
    if (a.controller) {
      request(engine_sound_for(*a.controller));
      request(a.controller->jump_sound);
      if (a.controller->boost) request(a.controller->boost_sound);
      if (a.controller->mode == ControlMode::character || a.controller->mode == ControlMode::soldier) request("builtin:step");
      if (a.controller->mode != ControlMode::hovercar && a.controller->mode != ControlMode::flyer) request("builtin:land");
      request("builtin:powerup");
    }
    if (a.weapon) { request(a.weapon->fire_sound); request(a.weapon->reload_sound); request("builtin:click"); }
    if (a.health) { request(a.health->hit_sound); request(a.health->death_sound); }
    if (a.pickup) { request(a.pickup->sound); goal = true; }
    if (a.trigger) {
      const char* fallback = a.trigger->action == TriggerAction::goal ? "builtin:goal"
                             : a.trigger->action == TriggerAction::hazard ? "builtin:hazard"
                             : a.trigger->action == TriggerAction::checkpoint ? "builtin:checkpoint" : "builtin:confirm";
      request(a.trigger->sound == "auto" ? std::string_view(fallback) : std::string_view(a.trigger->sound));
      goal = true;
      lose |= a.trigger->action == TriggerAction::hazard;
      if (a.trigger->action == TriggerAction::goal) request("builtin:error");
    }
    if (a.audio) request(a.audio->sound);
  }
  if (goal || w.goal != GameGoal::none) request("builtin:goal");
  if (lose) request("builtin:lose");
}

void SoundBank::sync_assets(const Level& level) {
  assets_.clear();
  for (const SoundAsset& s : level.sounds) assets_[s.id] = s.src;
  for (auto it = entries_.begin(); it != entries_.end();) {
    Entry& e = it->second;
    if (is_builtin(it->first)) { ++it; continue; }
    const SoundAsset* s = level.sound(it->first);
    if (!s || (e.src && s->src != e.src)) {
      if (e.clip) mixer_.destroy_clip(e.clip);
      it = entries_.erase(it);
      continue;
    }
    if (!e.src) e.src = s->src;
    ++it;
  }
}

bool SoundBank::prepare(f64 budget_ms) {
  const f64 start = now_ms();
  for (;;) {
    if (!job_active_) {
      if (queue_.empty()) return true;
      const std::string ref = queue_.front();
      queue_.pop_front();
      Entry* e = entry(ref);
      if (!e || e->clip || !mixer_.ready()) {
        if (e && !mixer_.ready()) { e->queued = false; }
        continue;
      }
      if (!job_.start(std::string_view(ref).substr(8), mixer_.sample_rate())) {
        e->queued = false;
        e->failed = true;
        e->error = "unknown built-in sound";
        continue;
      }
      job_active_ = true;
    }
    const f64 left = budget_ms > 0 ? budget_ms - (now_ms() - start) : 0;
    if (budget_ms > 0 && left <= 0) return false;
    if (!job_.step(budget_ms > 0 ? left : 0)) return false;
    job_active_ = false;
    const std::string ref = "builtin:" + job_.name();
    u32 channels = 1;
    std::vector<f32> pcm = job_.take(channels);
    Entry& e = entries_[ref];
    e.queued = false;
    const audio::BuiltinSound* info = audio::find_builtin(job_.name());
    e.clip = mixer_.adopt_clip(std::move(pcm), channels, ref, info && info->loop);
    if (!e.clip) { e.failed = true; e.error = last_error(); }
  }
}

bool SoundBank::ready(std::string_view ref) const {
  const Entry* e = entry(ref);
  return e && e->clip;
}

bool SoundBank::failed(std::string_view ref) const {
  const Entry* e = entry(ref);
  return e && e->failed;
}

std::vector<std::string> SoundBank::pending_imports() const {
  std::vector<std::string> out;
  for (const auto& [ref, e] : entries_)
    if (!is_builtin(ref) && e.requested && !e.clip && !e.failed && e.src) out.push_back(ref);
  std::sort(out.begin(), out.end());
  return out;
}

const std::string* SoundBank::source(std::string_view id) const {
  const Entry* e = entry(id);
  return e && e->src ? e->src.get() : nullptr;
}

bool SoundBank::set_pcm(std::string_view id, const f32* samples, u32 frames, u32 channels, u32 rate) {
  Entry* e = entry(id);
  if (!e || is_builtin(id) || !mixer_.ready()) return false;
  if (e->clip) { mixer_.destroy_clip(e->clip); e->clip = 0; }
  // Down-mix anything beyond stereo is done by the host; here 1 or 2 channels.
  e->clip = mixer_.create_clip(samples, frames, channels, rate, std::string(id));
  e->requested = false;
  if (!e->clip) {
    e->failed = true;
    e->error = last_error();
    return false;
  }
  e->failed = false;
  return true;
}

void SoundBank::set_failed(std::string_view id, std::string reason) {
  Entry* e = entry(id);
  if (!e) return;
  e->failed = true;
  e->requested = false;
  e->error = std::move(reason);
}

f32 SoundBank::duration(std::string_view ref) const {
  const Entry* e = entry(ref);
  if (!e || !e->clip) return 0;
  const audio::ClipInfo* info = mixer_.clip_info(e->clip);
  return info ? info->duration : 0;
}

void SoundBank::clear() {
  for (auto& [ref, e] : entries_)
    if (e.clip) mixer_.destroy_clip(e.clip);
  entries_.clear();
  assets_.clear();
  queue_.clear();
  job_active_ = false;
  job_ = audio::BuiltinJob();
}

// ---------------------------------------------------------------------------
// GameAudio

GameAudio::GameAudio(audio::Mixer& mixer, SoundBank& bank) : mixer_(mixer), bank_(bank) {}
GameAudio::~GameAudio() = default;

void GameAudio::set_muted(bool muted) {
  muted_ = muted;
  mixer_.bus(audio::Bus::master).muted = muted;
}

void GameAudio::apply_settings(const WorldSettings& w) {
  master_ = clamp(w.master_volume, 0.0f, 4.0f);
  mixer_.bus(audio::Bus::master).volume = master_;
  mixer_.bus(audio::Bus::master).muted = muted_;
  mixer_.bus(audio::Bus::music).volume = clamp(w.music_volume, 0.0f, 4.0f);
  mixer_.bus(audio::Bus::ambience).volume = clamp(w.ambience_volume, 0.0f, 4.0f);
  const f32 reverb = clamp(w.reverb, 0.0f, 1.0f);
  mixer_.bus(audio::Bus::sfx).reverb_send = reverb * 0.7f;
  mixer_.bus(audio::Bus::voice).reverb_send = reverb * 0.5f;
  mixer_.bus(audio::Bus::ambience).reverb_send = reverb * 0.3f;
  mixer_.bus(audio::Bus::music).reverb_send = 0;
  mixer_.bus(audio::Bus::ui).reverb_send = 0;
  mixer_.set_reverb(0.55f + reverb * 0.4f, 0.45f, 0.12f + reverb * 0.5f);
  mixer_.doppler_factor = clamp(w.doppler, 0.0f, 4.0f);
}

audio::VoiceId GameAudio::start_loop(std::string_view ref, audio::Bus bus, f32 volume) {
  const audio::ClipId c = bank_.clip(ref);
  if (!c) return 0;
  audio::PlayParams p;
  p.loop = true;
  p.bus = bus;
  p.volume = volume;
  p.fade_in = 0.6f;
  p.priority = 100;
  return mixer_.play(c, p);
}

void GameAudio::begin(const GameWorld& world) {
  end(0.05f);
  active_ = true;
  session_ = world.session();
  paused_ = false;
  const Level& level = world.level();
  bank_.request_level(level);
  apply_settings(level.world);
  music_ref_ = level.world.music;
  ambience_ref_ = level.world.ambience;
  engine_ref_.clear();
  if (const Actor* p = world.player() ? level.find(world.player()) : nullptr; p && p->controller) engine_ref_ = engine_sound_for(*p->controller);
  for (const Actor& a : level.actors) {
    if (!a.audio || !a.audio->autoplay || is_silent(a.audio->sound)) continue;
    Source s;
    s.actor = a.id;
    s.comp = *a.audio;
    s.wait = max_of(0.0f, a.audio->delay);
    sources_.push_back(std::move(s));
  }
  has_eye_ = false;
  cues_played_ = cues_dropped_ = 0;
}

void GameAudio::end(f32 fade) {
  for (Source& s : sources_)
    if (s.voice) mixer_.stop(s.voice, fade);
  sources_.clear();
  for (audio::VoiceId* v : {&music_, &ambience_, &engine_})
    if (*v) { mixer_.stop(*v, fade); *v = 0; }
  waiting_.clear();
  if (paused_) mixer_.set_all_paused(false);
  paused_ = false;
  active_ = false;
}

void GameAudio::play_cue(const SoundCue& cue) {
  const audio::ClipId c = bank_.clip(cue.sound);
  audio::PlayParams p;
  p.volume = cue.volume;
  p.pitch = cue.pitch;
  p.bus = cue.bus;
  p.spatial = cue.spatial;
  p.position = cue.position;
  p.min_distance = 3;
  p.max_distance = 80;
  p.priority = cue.bus == audio::Bus::ui ? 50 : 10;
  if (mixer_.play(c, p)) ++cues_played_;
  else ++cues_dropped_;
}

void GameAudio::update(GameWorld& world, const Camera& cam, f32 dt) {
  if (!world.running()) {
    if (active_) end();
    return;
  }
  if (!active_ || session_ != world.session()) begin(world);
  const Level& level = world.level();
  apply_settings(level.world);
  dt = clamp(dt, 0.0f, 0.25f);
  // Pause follows the world (Pause in Play / Simulate).
  if (world.paused() != paused_) {
    paused_ = world.paused();
    mixer_.set_all_paused(paused_);
  }
  // Listener at the camera.
  audio::Listener l;
  l.position = cam.eye;
  l.forward = normalize(cam.target - cam.eye, {1, 0, 0});
  l.up = cam.up;
  if (has_eye_ && dt > 0) l.velocity = (cam.eye - last_eye_) * (1.0f / dt);
  if (length(l.velocity) > 200) l.velocity = {};  // a camera cut, not motion
  last_eye_ = cam.eye;
  has_eye_ = true;
  mixer_.set_listener(l);
  if (paused_) {
    world.sound_cues().clear();
    return;
  }

  // Music and ambience start as soon as their clips are ready.
  if (!music_ && !is_silent(music_ref_) && !bank_.failed(music_ref_)) music_ = start_loop(music_ref_, audio::Bus::music, 1);
  if (!ambience_ && !is_silent(ambience_ref_) && !bank_.failed(ambience_ref_)) ambience_ = start_loop(ambience_ref_, audio::Bus::ambience, 1);

  // The player's engine follows speed and throttle.
  const ActorId player = world.player();
  const Actor* pa = player ? level.find(player) : nullptr;
  if (!engine_ref_.empty() && pa && pa->controller) {
    if (!engine_) {
      const audio::ClipId c = bank_.clip(engine_ref_);
      if (c) {
        audio::PlayParams p;
        p.loop = true;
        p.volume = 0;
        p.spatial = true;
        p.min_distance = 4;
        p.max_distance = 120;
        p.doppler = 0;  // the camera follows the player: no self-Doppler
        p.priority = 90;
        engine_ = mixer_.play(c, p);
      }
    }
    if (engine_) {
      const ControllerComponent& c = *pa->controller;
      const Hud& hud = world.hud();
      const f32 top = max_of(1.0f, c.boost ? c.boost_speed : c.max_speed);
      const f32 speed = clamp(hud.speed / top, 0.0f, 1.2f);
      const f32 throttle = std::fabs(world.last_input().move_y) + (world.last_input().boost && c.boost ? 0.5f : 0.0f);
      const bool live = hud.status == GameStatus::playing;
      const f32 target = live ? clamp(0.32f + speed * 0.45f + throttle * 0.18f, 0.0f, 0.95f) : 0.12f;
      engine_volume_ = damp(engine_volume_, target, 6, dt);
      mixer_.set_volume(engine_, engine_volume_, 0.05f);
      mixer_.set_pitch(engine_, clamp(0.72f + speed * 0.95f + throttle * 0.08f, 0.5f, 2.2f));
      Mat4 m;
      if (world.actor_matrix(player, m)) mixer_.set_position(engine_, m.translation(), {});
    }
  }

  // Audio sources follow their actors.
  for (Source& s : sources_) {
    if (s.done) continue;
    Mat4 m;
    const bool placed = world.actor_matrix(s.actor, m);
    const Vec3 pos = placed ? m.translation() : Vec3{};
    Vec3 vel;
    if (s.has_last && dt > 0) vel = (pos - s.last) * (1.0f / dt);
    if (length(vel) > 300) vel = {};
    s.last = pos;
    s.has_last = true;
    if (!world.actor_alive(s.actor)) {
      if (s.voice) { mixer_.stop(s.voice, 0.15f); s.voice = 0; }
      continue;
    }
    if (!s.voice) {
      if (s.wait > 0) { s.wait -= dt; continue; }
      const audio::ClipId c = bank_.clip(s.comp.sound);
      if (!c) {
        if (bank_.failed(s.comp.sound)) s.done = true;
        continue;
      }
      audio::PlayParams p;
      p.volume = s.comp.volume;
      p.pitch = s.comp.pitch;
      p.loop = s.comp.loop;
      p.bus = s.comp.bus;
      p.spatial = s.comp.spatial;
      p.position = pos;
      p.velocity = vel;
      p.min_distance = s.comp.min_distance;
      p.max_distance = s.comp.max_distance;
      p.rolloff = s.comp.rolloff;
      p.doppler = s.comp.doppler;
      p.fade_in = s.comp.loop ? 0.3f : 0;
      p.priority = s.comp.loop ? 40 : 20;
      s.voice = mixer_.play(c, p);
      if (!s.voice) continue;
    } else if (!mixer_.playing(s.voice)) {
      s.voice = 0;
      if (!s.comp.loop) s.done = true;
      continue;
    }
    if (s.comp.spatial) mixer_.set_position(s.voice, pos, vel);
    if (s.comp.pitch_by_speed != 0) {
      const f32 speed = length(vel);
      mixer_.set_pitch(s.voice, clamp(s.comp.pitch * (1 + s.comp.pitch_by_speed * speed * 0.1f), 0.05f, 8.0f));
    }
  }

  // Cues of the simulation; a cue whose clip is still rendering waits up to 0.25 s.
  for (SoundCue& c : world.sound_cues()) waiting_.push_back({std::move(c), 0});
  world.sound_cues().clear();
  for (std::size_t i = 0; i < waiting_.size();) {
    Waiting& w = waiting_[i];
    if (bank_.ready(w.cue.sound)) {
      play_cue(w.cue);
    } else if (bank_.failed(w.cue.sound) || (w.age += dt) > 0.25f) {
      bank_.request(w.cue.sound);
      ++cues_dropped_;
    } else {
      bank_.request(w.cue.sound);
      ++i;
      continue;
    }
    waiting_.erase(waiting_.begin() + static_cast<std::ptrdiff_t>(i));
  }
  if (waiting_.size() > 64) waiting_.erase(waiting_.begin(), waiting_.begin() + static_cast<std::ptrdiff_t>(waiting_.size() - 64));
}

}  // namespace kern::game
