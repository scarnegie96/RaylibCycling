#pragma once
// Synthesised sounds (no audio files): HR alert tones, step / lap chimes, and a live
// ambience bed (rain, wind, birdsong, night crickets, thunder) generated on the audio thread.
namespace sfx {
enum Kind { StepChange, Lap, HrHigh, HrLow, Countdown, Done, COUNT };
void init();
void shutdown();
void play(Kind k);
void setVolume(float v);

struct Ambience { float rain = 0, wind = 0, birds = 0, crickets = 0, hail = 0; };
void ambience(const Ambience& a);          // call every frame; levels are smoothed
void thunder(float delayS, float strength);
void setMuted(bool m);
bool muted();
void update();                             // keeps the stream fed (call once per frame)
}  // namespace sfx
