#include "Profiler.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "raylib.h"
#include "rlgl.h"

extern "C" void* glfwGetProcAddress(const char* name);   // raylib links GLFW statically

namespace prof {
namespace {

using GLuint = unsigned int;
using GLint = int;
using GLenum = unsigned int;
using GLsizei = int;
using GLuint64 = uint64_t;
#if defined(_WIN32)
#define PROF_APIENTRY __stdcall
#else
#define PROF_APIENTRY
#endif
constexpr GLenum GL_TIMESTAMP_ = 0x8E28, GL_TIME_ELAPSED_ = 0x88BF, GL_QUERY_COUNTER_BITS_ = 0x8864;
constexpr GLenum GL_QUERY_RESULT_ = 0x8866, GL_QUERY_RESULT_AVAILABLE_ = 0x8867;
typedef void(PROF_APIENTRY* PGenQueries)(GLsizei, GLuint*);
typedef void(PROF_APIENTRY* PQueryCounter)(GLuint, GLenum);
typedef void(PROF_APIENTRY* PBeginQuery)(GLenum, GLuint);
typedef void(PROF_APIENTRY* PEndQuery)(GLenum);
typedef void(PROF_APIENTRY* PGetQueryiv)(GLenum, GLenum, GLint*);
typedef void(PROF_APIENTRY* PGetQueryObjectiv)(GLuint, GLenum, GLint*);
typedef void(PROF_APIENTRY* PGetQueryObjectui64v)(GLuint, GLenum, GLuint64*);
typedef void(PROF_APIENTRY* PFinish)();
PGenQueries genQueries = nullptr;
PQueryCounter queryCounter = nullptr;
PBeginQuery beginQuery = nullptr;
PEndQuery endQuery = nullptr;
PGetQueryiv getQueryiv = nullptr;
PGetQueryObjectiv getObjiv = nullptr;
PGetQueryObjectui64v getObjui64v = nullptr;
PFinish glFinish_ = nullptr;

// How sections are timed. Drivers differ: Apple's OpenGL on Apple Silicon advertises timer
// queries but its timestamps come back empty, so we start with the best method the driver
// claims, and step down whenever a method keeps producing nothing.
enum Mode { Timestamps, Elapsed, Synced, CpuOnly };
Mode mode = CpuOnly;

constexpr int FRAMES = 4;            // in flight; results read FRAMES-1 frames later
constexpr int MAXQ = COUNT + 2;

struct Frame {
    GLuint q[MAXQ] = {};
    int sec[MAXQ] = {};              // Timestamps: section each stamp closes; Elapsed: section each query measured
    int n = 0;
    bool open = false;               // Elapsed: q[n-1] is running
    bool pending = false;
    double cpu[COUNT] = {};          // Synced / CpuOnly: wall time per section (ms)
};

bool on = false, inited = false, inFrame = false;
Frame frames[FRAMES];
int cur = 0, emptyFrames = 0;
Stats st;
std::chrono::steady_clock::time_point tFrame, tMark;

void smooth(float& v, double x) { v += float(x - v) * 0.08f; }
double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

void setMode(Mode m) {
    mode = m;
    emptyFrames = 0;
    for (auto& f : frames) { f.pending = false; f.open = false; f.n = 0; }
    for (int s = 1; s < COUNT; ++s) st.ms[s] = 0;
    st.gpuMs = 0;
    st.gpu = m == Timestamps || m == Elapsed;
    static const char* names[] = {"GPU timestamps", "GPU timers", "GPU (synced; slows the frame a little)", "CPU only"};
    st.method = names[m];
}

void init() {
    inited = true;
    auto get = [](const char* n) { return glfwGetProcAddress(n); };
    genQueries = (PGenQueries)get("glGenQueries");
    queryCounter = (PQueryCounter)get("glQueryCounter");
    beginQuery = (PBeginQuery)get("glBeginQuery");
    endQuery = (PEndQuery)get("glEndQuery");
    getQueryiv = (PGetQueryiv)get("glGetQueryiv");
    getObjiv = (PGetQueryObjectiv)get("glGetQueryObjectiv");
    getObjui64v = (PGetQueryObjectui64v)get("glGetQueryObjectui64v");
    glFinish_ = (PFinish)get("glFinish");
    bool queries = genQueries && getQueryiv && getObjiv && getObjui64v;
    GLint tsBits = 0, elBits = 0;
    if (queries) {
        if (queryCounter) getQueryiv(GL_TIMESTAMP_, GL_QUERY_COUNTER_BITS_, &tsBits);
        if (beginQuery && endQuery) getQueryiv(GL_TIME_ELAPSED_, GL_QUERY_COUNTER_BITS_, &elBits);
        for (auto& f : frames) genQueries(MAXQ, f.q);
    }
    Mode m = tsBits > 0 ? Timestamps : elBits > 0 ? Elapsed : glFinish_ ? Synced : CpuOnly;
    if (const char* e = std::getenv("CORERAY_PROFMODE")) m = Mode(std::atoi(e) & 3);   // dev: force a method
    if ((m == Timestamps && !(queries && queryCounter)) || (m == Elapsed && !(queries && beginQuery && endQuery))) m = glFinish_ ? Synced : CpuOnly;
    if (m == Synced && !glFinish_) m = CpuOnly;
    setMode(m);
    std::fprintf(stderr, "[prof] timestamp bits %d, elapsed bits %d -> %s\n", tsBits, elBits, st.method);
}

// a method that keeps giving nothing isn't working on this driver: try the next one
void noteResult(double sum) {
    if (sum > 1e-4) { emptyFrames = 0; return; }
    if (++emptyFrames < 30) return;
    if (mode == Timestamps) setMode(beginQuery && endQuery ? Elapsed : Synced);
    else if (mode == Elapsed) setMode(glFinish_ ? Synced : CpuOnly);
    std::fprintf(stderr, "[prof] no timings, switching to %s\n", st.method);
}

void harvest(Frame& f) {
    if (!f.pending) return;
    double ms[COUNT] = {};
    if (mode == Timestamps || mode == Elapsed) {
        if (f.n == 0) { f.pending = false; return; }
        GLint ready = 0;
        getObjiv(f.q[f.n - 1], GL_QUERY_RESULT_AVAILABLE_, &ready);
        if (!ready) { if (++emptyFrames >= 60) noteResult(0); return; }
        if (mode == Timestamps) {
            GLuint64 prev = 0;
            for (int i = 0; i < f.n; ++i) {
                GLuint64 t = 0;
                getObjui64v(f.q[i], GL_QUERY_RESULT_, &t);
                if (i > 0 && f.sec[i] >= 0 && t >= prev) ms[f.sec[i]] += double(t - prev) * 1e-6;
                prev = t;
            }
        } else {
            for (int i = 0; i < f.n; ++i) {
                GLuint64 t = 0;
                getObjui64v(f.q[i], GL_QUERY_RESULT_, &t);
                if (f.sec[i] >= 0) ms[f.sec[i]] += double(t) * 1e-6;
            }
        }
    } else {
        std::memcpy(ms, f.cpu, sizeof ms);
    }
    f.pending = false;
    double sum = 0;
    for (int s = 1; s < COUNT; ++s) { smooth(st.ms[s], ms[s]); sum += ms[s]; }   // (Update is CPU-timed in mark())
    smooth(st.gpuMs, sum);
    if (mode == Timestamps || mode == Elapsed) noteResult(sum);
}

// close the running section as `section` (-1 = not counted) and start the next
void boundary(int section) {
    Frame& f = frames[cur];
    rlDrawRenderBatchActive();   // raylib's batched draws belong to the section just ending
    switch (mode) {
        case Timestamps:
            if (f.n < MAXQ) { queryCounter(f.q[f.n], GL_TIMESTAMP_); f.sec[f.n++] = section; }
            break;
        case Elapsed:
            if (f.open) { endQuery(GL_TIME_ELAPSED_); f.sec[f.n - 1] = section; f.open = false; }
            if (f.n < MAXQ && section != -2) { beginQuery(GL_TIME_ELAPSED_, f.q[f.n]); f.sec[f.n++] = -1; f.open = true; }
            break;
        case Synced:
            glFinish_();   // wait for the GPU: the wall time since the last mark is then this section's GPU work
            [[fallthrough]];
        case CpuOnly:
            if (section >= 0) f.cpu[section] += msSince(tMark);
            break;
    }
    tMark = std::chrono::steady_clock::now();
}

}  // namespace

const char* name(Section s) {
    static const char* n[COUNT] = {"Update (CPU)", "Shadows", "Sky", "Terrain", "Road", "Foliage", "Grass", "Props & rider",
                                   "Water", "Clouds & fog", "Particles", "Post", "HUD"};
    return n[s];
}

void setEnabled(bool v) { on = v; }
bool enabled() { return on; }

void beginFrame() {
    if (!on) return;
    if (!inited) init();
    harvest(frames[cur]);   // the slot we're about to reuse was issued FRAMES frames ago
    Frame& f = frames[cur];
    f.pending = false;      // still not ready: drop it rather than stall
    f.n = 0;
    f.open = false;
    std::memset(f.cpu, 0, sizeof f.cpu);
    inFrame = true;
    tFrame = tMark = std::chrono::steady_clock::now();
    if (mode == Synced && glFinish_) { rlDrawRenderBatchActive(); glFinish_(); tFrame = tMark = std::chrono::steady_clock::now(); }
    boundary(-1);
}

void mark(Section s) {
    if (!on || !inFrame) return;
    if (s == Update) {
        // CPU-only work (no drawing yet): wall time, and the GPU clock restarts after it
        smooth(st.ms[Update], msSince(tMark));
        boundary(-1);
        return;
    }
    boundary(s);
}

void endFrame() {
    if (!on || !inFrame) return;
    inFrame = false;
    Frame& f = frames[cur];
    if (mode == Elapsed && f.open) { rlDrawRenderBatchActive(); endQuery(GL_TIME_ELAPSED_); f.open = false; }  // (its section stays -1)
    f.pending = true;
    if (mode == Synced || mode == CpuOnly) harvest(f);
    smooth(st.frameMs, msSince(tFrame));
    cur = (cur + 1) % FRAMES;
}

const Stats& stats() { return st; }

}  // namespace prof
