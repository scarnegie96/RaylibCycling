#include "Audio.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "raylib.h"

namespace sfx {

static Sound g_sounds[COUNT];
static bool g_ok = false;

// ---------------------------------------------------------------- ambience (audio thread)
namespace amb {
constexpr int RATE = 44100;
std::atomic<float> tRain{0}, tWind{0}, tBirds{0}, tCrickets{0}, tHail{0}, master{1};
std::atomic<int> thunderPending{0};
std::atomic<float> thunderDelay{0}, thunderStrength{0};
AudioStream stream{};
bool running = false;

struct Gen {
    uint32_t rng = 0x1234567;
    float rain = 0, wind = 0, birds = 0, crickets = 0, hail = 0, gain = 1;
    float hp = 0, hpIn = 0, brown = 0, lp1 = 0, lp2 = 0, windLfo = 0;
    double t = 0;
    // birds
    double nextChirp = 1.0, chirpStart = -1, chirpLen = 0;
    float chirpF0 = 3000, chirpF1 = 4000, chirpPhase = 0;
    int notesLeft = 0;
    // crickets
    float crPhase = 0;
    // thunder
    double thunderAt = -1, thunderT = -1;
    float thStrength = 0, thLp = 0, thLp2 = 0;
    // hail ticks
    float tick = 0;
    float white() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (rng & 0xFFFFFF) / float(0x800000) - 1.f; }
    float rnd01() { return (white() + 1.f) * 0.5f; }
} g;

void callback(void* buffer, unsigned int frames) {
    short* out = static_cast<short*>(buffer);
    const float dt = 1.f / RATE;
    float trg[5] = {tRain.load(), tWind.load(), tBirds.load(), tCrickets.load(), tHail.load()};
    float m = master.load();
    if (thunderPending.exchange(0)) {
        g.thunderAt = g.t + thunderDelay.load();
        g.thStrength = thunderStrength.load();
    }
    for (unsigned i = 0; i < frames; ++i) {
        const float k = 1.f - std::exp(-dt / 1.5f);
        g.rain += (trg[0] - g.rain) * k; g.wind += (trg[1] - g.wind) * k; g.birds += (trg[2] - g.birds) * k;
        g.crickets += (trg[3] - g.crickets) * k; g.hail += (trg[4] - g.hail) * k;
        g.gain += (m - g.gain) * (1.f - std::exp(-dt / 0.2f));
        float w = g.white();
        float s = 0;
        // rain: bright hiss + random drops
        g.hp = 0.92f * (g.hp + w - g.hpIn); g.hpIn = w;
        float drops = (g.rnd01() < 0.002f * g.rain) ? g.white() * 0.8f : 0.f;
        s += (g.hp * 0.16f + drops) * g.rain;
        // hail: hard little ticks
        if (g.rnd01() < 0.0015f * g.hail) g.tick = 1.f;
        g.tick *= 0.985f;
        s += g.tick * g.white() * 0.5f * g.hail;
        // wind: brown noise, slowly swelling
        g.brown = (g.brown + w * 0.02f) * 0.998f;
        g.windLfo += dt * 0.13f;
        float swell = 0.55f + 0.45f * std::sin(g.windLfo * 6.2832f) * std::sin(g.windLfo * 2.7f + 1.f);
        g.lp1 += (g.brown - g.lp1) * 0.05f;
        s += g.lp1 * 2.2f * g.wind * swell;
        // birdsong: short whistled phrases
        if (g.birds > 0.05f && g.t >= g.nextChirp && g.chirpStart < 0) {
            g.chirpStart = g.t; g.chirpLen = 0.05 + g.rnd01() * 0.09;
            g.chirpF0 = 2400 + g.rnd01() * 2600; g.chirpF1 = g.chirpF0 * (0.7f + g.rnd01() * 0.7f);
            if (g.notesLeft <= 0) g.notesLeft = 2 + int(g.rnd01() * 5);
        }
        if (g.chirpStart >= 0) {
            float u = float((g.t - g.chirpStart) / g.chirpLen);
            if (u >= 1) {
                g.chirpStart = -1;
                --g.notesLeft;
                g.nextChirp = g.t + (g.notesLeft > 0 ? 0.04 + g.rnd01() * 0.08 : 0.8 + g.rnd01() * 4.0 / std::max(0.2f, g.birds));
            } else {
                float f = g.chirpF0 + (g.chirpF1 - g.chirpF0) * u;
                g.chirpPhase += 6.2832f * f * dt;
                float env = std::sin(3.14159f * u);
                s += std::sin(g.chirpPhase) * env * env * 0.09f * g.birds;
            }
        }
        // crickets at night: pulsed high tone
        if (g.crickets > 0.02f) {
            g.crPhase += 6.2832f * 4650.f * dt;
            double cyc = std::fmod(g.t, 0.62);
            float gate = (cyc < 0.1 && std::fmod(cyc, 0.033) < 0.018) ? 1.f : 0.f;
            s += std::sin(g.crPhase) * gate * 0.025f * g.crickets;
        }
        // thunder: a crack, then a long low rumble
        if (g.thunderAt >= 0 && g.t >= g.thunderAt) { g.thunderT = 0; g.thunderAt = -1; }
        if (g.thunderT >= 0) {
            float tt = float(g.thunderT);
            float env = std::min(1.f, tt / 0.06f) * std::exp(-tt / (1.4f + g.thStrength * 1.6f));
            g.thLp += (w - g.thLp) * 0.02f;
            g.thLp2 += (g.thLp - g.thLp2) * 0.05f;
            float crack = tt < 0.25f ? w * std::exp(-tt * 18.f) * 0.5f : 0.f;
            float rumble = g.thLp2 * 9.f * (0.6f + 0.4f * std::sin(tt * 7.f + std::sin(tt * 2.3f) * 3.f));
            s += (crack + rumble) * env * (0.5f + 0.5f * g.thStrength);
            g.thunderT += dt;
            if (tt > 7) g.thunderT = -1;
        }
        s *= g.gain;
        s = std::tanh(s * 1.2f);
        out[i] = short(s * 20000);
        g.t += dt;
    }
}
}  // namespace amb

static Sound tone(std::vector<std::pair<float, float>> notes, float gap = 0.03f) {
    const int rate = 44100;
    std::vector<short> pcm;
    for (auto [freq, dur] : notes) {
        int n = int(dur * rate);
        for (int i = 0; i < n; ++i) {
            float t = float(i) / rate;
            float env = std::min(1.f, t / 0.008f) * std::exp(-t * 5.5f);
            float s = std::sin(2 * PI * freq * t) * 0.6f + std::sin(2 * PI * freq * 2 * t) * 0.15f;
            pcm.push_back(short(s * env * 26000));
        }
        pcm.insert(pcm.end(), int(gap * rate), 0);
    }
    Wave w{};
    w.frameCount = (unsigned)pcm.size();
    w.sampleRate = rate;
    w.sampleSize = 16;
    w.channels = 1;
    w.data = pcm.data();
    return LoadSoundFromWave(w);  // copies the data
}

void init() {
    InitAudioDevice();
    if (!IsAudioDeviceReady()) return;
    g_sounds[StepChange] = tone({{880, 0.12f}, {1320, 0.18f}});
    g_sounds[Lap] = tone({{660, 0.1f}, {880, 0.1f}, {1100, 0.22f}});
    g_sounds[HrHigh] = tone({{1200, 0.12f}, {900, 0.12f}, {700, 0.2f}});
    g_sounds[HrLow] = tone({{500, 0.12f}, {700, 0.12f}, {900, 0.2f}});
    g_sounds[Countdown] = tone({{1000, 0.08f}});
    g_sounds[Done] = tone({{523, 0.14f}, {659, 0.14f}, {784, 0.14f}, {1046, 0.4f}});
    g_ok = true;
    SetAudioStreamBufferSizeDefault(2048);
    amb::stream = LoadAudioStream(amb::RATE, 16, 1);
    SetAudioStreamCallback(amb::stream, amb::callback);
    PlayAudioStream(amb::stream);
    amb::running = true;
}

void shutdown() {
    if (!g_ok) return;
    if (amb::running) { StopAudioStream(amb::stream); UnloadAudioStream(amb::stream); amb::running = false; }
    for (auto& s : g_sounds) UnloadSound(s);
    CloseAudioDevice();
    g_ok = false;
}

void play(Kind k) { if (g_ok && amb::master.load() > 0.5f) PlaySound(g_sounds[k]); }
void ambience(const Ambience& a) {
    amb::tRain = a.rain; amb::tWind = a.wind; amb::tBirds = a.birds; amb::tCrickets = a.crickets; amb::tHail = a.hail;
}
void thunder(float delayS, float strength) {
    amb::thunderDelay = delayS;
    amb::thunderStrength = strength;
    amb::thunderPending = 1;
}
static bool g_muted = false;
void setMuted(bool m) { g_muted = m; amb::master = m ? 0.f : 1.f; }
bool muted() { return g_muted; }
void update() {}
void setVolume(float v) { if (g_ok) SetMasterVolume(v); }

}  // namespace sfx
