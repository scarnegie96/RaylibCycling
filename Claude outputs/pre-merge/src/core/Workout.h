#pragma once
// Structured workouts (port of CoreKick workout.rs + fitness_test.rs).
// Power targets are fractions of FTP. Free blocks hand control back to the rider
// (SIM at a set gradient); Set blocks repeat a group of blocks.
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wk {

struct Block {
    enum Type { Steady, Ramp, Intervals, Free, Set } type = Steady;
    int durationS = 0;
    double power = 0, from = 0, to = 0;           // Steady: power; Ramp: from/to
    int repeats = 1, onS = 0, offS = 0;            // Intervals / Set
    double onPower = 0, offPower = 0;
    std::optional<int> cadence;                    // Steady cadence / Intervals on-cadence
    double gradePct = 0;                           // Free
    bool effort = false;                           // Free
    std::string label;
    std::vector<Block> blocks;                     // Set

    static Block steady(int d, double p, std::optional<int> cad = {}, std::string label = "");
    static Block ramp(int d, double a, double b, std::string label = "");
    static Block intervals(int n, int on, double onp, int off, double offp, std::optional<int> cad = {});
    static Block free(int d, double grade, bool effort, std::string label = "");
    static Block set(int n, std::vector<Block> b);
};

struct Step {
    int startS = 0, durationS = 0;
    bool erg = true;        // false = free (rider controls power, trainer simulates gradePct)
    double from = 0, to = 0;
    double gradePct = 0;
    std::string label;
    bool effort = false;
    std::optional<int> cadence;
    int endS() const { return startS + durationS; }
    std::optional<double> fractionAt(double t) const;
};

struct Estimate {
    int durationS = 0;
    double avgPower = 0, np = 0, intensityFactor = 0, tss = 0, workKj = 0;
    std::array<double, 7> zoneS{};
};

struct Workout {
    std::string id, name, description, category;
    std::vector<Block> blocks;
    bool builtin = false;
    std::vector<Step> steps() const;
    int durationS() const;
    Estimate estimate(double ftp) const;
};

std::vector<Step> blocksToSteps(const std::vector<Block>& blocks);
void reflow(std::vector<Step>& steps, size_t fromIndex);
Estimate estimateSteps(const std::vector<Step>& steps, double ftp);
// (index, seconds into step); nullopt when past the end
std::optional<std::pair<size_t, double>> stepAt(const std::vector<Step>& steps, double t);

// Text format: "10m 50-75%, 3x(8m @95%, 4m @55%)", "5x 1m @120% 100rpm / 1m @50%", "2m free", "30s effort @2%", "1:30 @200w"
// Returns false and sets err on failure.
bool parseText(const std::string& src, double ftp, std::vector<Block>& out, std::string& err);
std::string toText(const std::vector<Block>& blocks);

// File formats
bool fromZwo(const std::string& text, Workout& out, std::string& err);
bool fromErgMrc(const std::string& text, double ftp, Workout& out, std::string& err);
bool importFile(const std::string& fileName, const std::string& text, double ftp, Workout& out, std::string& err);
std::string toZwo(const Workout& w);

std::vector<Workout> builtinWorkouts();
std::vector<std::string> categories();  // in display order

// ---------------------------------------------------------------- FTP tests
enum class TestKind { Ramp, Ftp20, Ftp8 };
struct TestInfo { TestKind kind; std::string name, summary, how; int approxMinutes; };
struct TestResult {
    TestKind kind;
    double ftp = 0, measuredW = 0, factor = 0, maxPower = 0;
    std::string detail;
    bool valid = false;
};
std::vector<TestInfo> testCatalogue();
std::pair<double, double> rampParams(double ftp);  // start W, step W
Workout testProtocol(TestKind k, double ftp);
std::optional<TestResult> evaluateTest(TestKind k, const std::vector<Step>& steps, const std::vector<double>& t,
                                       const std::vector<double>& power);

}  // namespace wk
