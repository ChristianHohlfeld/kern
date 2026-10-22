// Sound for games: the bank that turns sound references into mixer clips and
// the director that plays a running GameWorld through the mixer.
//
//   audio::Mixer mixer;  mixer.init(48000);
//   SoundBank bank(mixer);
//   GameAudio audio(mixer, bank);
//   world.start(level, SimMode::play);  audio.begin(world);
//   each frame: bank.prepare(4); audio.update(world, camera, dt);  host pulls mixer.render()
//   world.stop();  audio.end();
//
// Sound references are "builtin:<name>" (synthesised on demand, see
// audio/synth.h) or the id of an imported SoundAsset (the host decodes the
// file - Web Audio's decodeAudioData in the browser - and hands PCM to
// SoundBank::set_pcm). Clips are prepared in time slices, so loading a level
// with music never stalls a frame; a cue whose clip is not ready yet waits a
// moment and is dropped if it would sound late.
#pragma once

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "kern/audio/audio.h"
#include "kern/audio/synth.h"
#include "kern/game/level.h"
#include "kern/game/runtime.h"

namespace kern::game {

class SoundBank {
 public:
  explicit SoundBank(audio::Mixer& mixer);
  ~SoundBank();
  SoundBank(const SoundBank&) = delete;
  SoundBank& operator=(const SoundBank&) = delete;

  audio::Mixer& mixer() { return mixer_; }

  /// The clip for a reference, 0 while it is not ready (the reference is requested).
  audio::ClipId clip(std::string_view ref);
  /// Queues a reference for preparation (built-ins render, imports wait for PCM).
  void request(std::string_view ref);
  /// Requests every sound a level uses (so Play starts with everything in place).
  void request_level(const Level& level);
  /// Tracks the level's imported assets: clips of removed or replaced assets are dropped.
  void sync_assets(const Level& level);
  /// Renders queued built-in sounds for about `budget_ms` (0 = all now). True when none is queued.
  bool prepare(f64 budget_ms = 4);
  bool ready(std::string_view ref) const;
  bool failed(std::string_view ref) const;
  /// Requested imported sounds that need PCM from the host.
  std::vector<std::string> pending_imports() const;
  /// The source (data URL) of an imported sound for the host decoder.
  const std::string* source(std::string_view id) const;
  /// Decoded PCM of an imported sound (interleaved, any rate); false when invalid.
  bool set_pcm(std::string_view id, const f32* samples, u32 frames, u32 channels, u32 rate);
  void set_failed(std::string_view id, std::string reason);
  /// Duration in seconds of a ready clip (0 otherwise).
  f32 duration(std::string_view ref) const;
  /// Forgets every clip (the mixer's clips are destroyed).
  void clear();
  u32 queued() const { return static_cast<u32>(queue_.size() + (job_active_ ? 1 : 0)); }

 private:
  struct Entry {
    audio::ClipId clip = 0;
    bool queued = false, failed = false, requested = false;
    std::shared_ptr<const std::string> src;  // imported: the asset's source (identity)
    std::string error;
  };
  Entry* entry(std::string_view ref);
  const Entry* entry(std::string_view ref) const;

  audio::Mixer& mixer_;
  std::unordered_map<std::string, Entry> entries_;
  std::unordered_map<std::string, std::shared_ptr<const std::string>> assets_;  // imported id -> source
  std::deque<std::string> queue_;   // built-ins waiting for render
  audio::BuiltinJob job_;
  bool job_active_ = false;
};

/// Plays a running GameWorld: music, ambience, audio sources, the player's
/// engine and every cue of the simulation, with the camera as listener.
class GameAudio {
 public:
  GameAudio(audio::Mixer& mixer, SoundBank& bank);
  ~GameAudio();

  /// Starts the level's music, ambience and autoplay sources.
  void begin(const GameWorld& world);
  /// Per rendered frame. Begins with every new run of the world (start,
  /// restart), ends when it stops; pauses with it.
  void update(GameWorld& world, const Camera& listener, f32 dt);
  /// Fades everything of the game out.
  void end(f32 fade = 0.25f);
  bool active() const { return active_; }
  /// Applies the world's master / music / ambience volumes, reverb and Doppler.
  void apply_settings(const WorldSettings& w);
  /// Editor master mute (kept across begin/end).
  void set_muted(bool muted);
  bool muted() const { return muted_; }

  u32 cues_played() const { return cues_played_; }
  u32 cues_dropped() const { return cues_dropped_; }

 private:
  struct Source {
    ActorId actor = 0;
    AudioComponent comp;
    audio::VoiceId voice = 0;
    f32 wait = 0;            // seconds left of the start delay
    bool done = false;       // a one-shot that finished
    Vec3 last;
    bool has_last = false;
  };
  struct Waiting {
    SoundCue cue;
    f32 age = 0;
  };
  void play_cue(const SoundCue& cue);
  audio::VoiceId start_loop(std::string_view ref, audio::Bus bus, f32 volume);

  audio::Mixer& mixer_;
  SoundBank& bank_;
  bool active_ = false, paused_ = false, muted_ = false;
  u32 session_ = 0;
  std::vector<Source> sources_;
  std::vector<Waiting> waiting_;
  std::string music_ref_, ambience_ref_, engine_ref_;
  audio::VoiceId music_ = 0, ambience_ = 0, engine_ = 0;
  f32 engine_volume_ = 0;
  Vec3 last_eye_;
  bool has_eye_ = false;
  f32 master_ = 1;
  u32 cues_played_ = 0, cues_dropped_ = 0;
};

/// The engine loop a controller uses ("" when it has none).
std::string engine_sound_for(const ControllerComponent& c);

}  // namespace kern::game
