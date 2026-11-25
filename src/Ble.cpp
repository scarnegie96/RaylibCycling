#include "Ble.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "Protocol.h"

#ifdef CORERAY_BLE
#include <simpleble/SimpleBLE.h>
#endif

double appClock() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

namespace {
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
std::string lowerStr(std::string s) { for (auto& c : s) c = char(std::tolower((unsigned char)c)); return s; }
}  // namespace

struct BleManager::Impl {
    mutable std::mutex m;
    bool hasAdapter = false;
    std::string adapterMsg = "Bluetooth support not compiled in";
    std::atomic<bool> scanning{false};
    double scanUntil = 0;
    std::map<std::string, DeviceInfo> found;
    Link trainer, hrm;
    std::string autoTrainer, autoHr;
    std::deque<std::string> logLines;

    // telemetry
    double power = 0, cadence = 0, lastTrainer = -1, hr = 0, lastHr = -1;
    std::optional<double> speed, trainerHr;

    // trainer command channel (coalesced: only the newest target matters)
    std::optional<TrainerCommand> pendingCmd;
    std::atomic<bool> trainerCancel{true}, hrCancel{true};
    std::thread trainerThread, hrThread, scanThread;
    std::atomic<bool> quit{false};
    std::mutex ctl;  // serialises start/stop of the supervisors

    void addLog(const std::string& s) {
        std::lock_guard<std::mutex> lk(m);
        char ts[16];
        std::snprintf(ts, sizeof ts, "%7.1f ", appClock());
        logLines.push_back(ts + s);
        while (logLines.size() > 80) logLines.pop_front();
        std::fprintf(stderr, "[ble] %s\n", s.c_str());
    }
    void setLink(bool isTrainer, LinkState st, const std::string& detail) {
        std::lock_guard<std::mutex> lk(m);
        Link& l = isTrainer ? trainer : hrm;
        l.state = st;
        l.detail = detail;
    }

#ifdef CORERAY_BLE
    std::unique_ptr<SimpleBLE::Adapter> adapter;
    std::map<std::string, SimpleBLE::Peripheral> peripherals;

    void initAdapter() {
        try {
            if (!SimpleBLE::Adapter::bluetooth_enabled()) {
                std::lock_guard<std::mutex> lk(m);
                adapterMsg = "Bluetooth is off, or this app has no Bluetooth permission";
            }
            auto list = SimpleBLE::Adapter::get_adapters();
            if (list.empty()) { std::lock_guard<std::mutex> lk(m); adapterMsg = "No Bluetooth adapter found"; return; }
            adapter = std::make_unique<SimpleBLE::Adapter>(list[0]);
            adapter->set_callback_on_scan_found([this](SimpleBLE::Peripheral p) { onScan(p); });
            adapter->set_callback_on_scan_updated([this](SimpleBLE::Peripheral p) { onScan(p); });
            std::lock_guard<std::mutex> lk(m);
            hasAdapter = true;
            adapterMsg = "Bluetooth ready";
        } catch (std::exception& e) {
            std::lock_guard<std::mutex> lk(m);
            adapterMsg = std::string("Bluetooth unavailable: ") + e.what();
        }
    }

    static std::string idOf(SimpleBLE::Peripheral& p) {
        std::string a = p.address();
        return a.empty() ? p.identifier() : a;
    }

    void onScan(SimpleBLE::Peripheral p) {
        try {
            bool ftms = false, hrs = false, cps = false;
            for (auto& s : p.services()) {
                auto u = s.uuid();
                if (u == proto::uuid16(proto::SVC_FTMS)) ftms = true;
                else if (u == proto::uuid16(proto::SVC_HRS)) hrs = true;
                else if (u == proto::uuid16(proto::SVC_CPS)) cps = true;
            }
            std::string name = p.identifier();
            std::string id = idOf(p);
            std::lock_guard<std::mutex> lk(m);
            auto it = found.find(id);
            if (it != found.end()) {  // keep what earlier adverts told us
                ftms |= it->second.ftms;
                if (name.empty()) name = it->second.name;
            }
            std::string ln = lowerStr(name);
            DeviceInfo d;
            if (ftms || ln.find("kickr") != std::string::npos) d.kind = DevKind::Trainer;
            else if (hrs) d.kind = DevKind::HeartRate;
            else if (cps) d.kind = DevKind::Power;
            else if (it != found.end()) d.kind = it->second.kind;
            else return;  // not a fitness device
            d.id = id;
            d.ftms = ftms;
            d.rssi = p.rssi();
            d.name = !name.empty() ? name : d.kind == DevKind::Trainer ? "Smart trainer" : d.kind == DevKind::HeartRate ? "Heart rate monitor" : "Power meter";
            found[id] = d;
            peripherals[id] = p;
            bool autoT = !autoTrainer.empty() && id == autoTrainer && trainer.state == LinkState::Disconnected && trainerCancel;
            bool autoH = !autoHr.empty() && id == autoHr && hrm.state == LinkState::Disconnected && hrCancel;
            if (autoT) { trainer.state = LinkState::Connecting; trainer.id = id; trainer.name = d.name; }
            if (autoH) { hrm.state = LinkState::Connecting; hrm.id = id; hrm.name = d.name; }
            if (autoT || autoH) {
                std::thread([this, id, autoT, autoH] {
                    std::lock_guard<std::mutex> g(ctl);
                    if (quit) return;
                    if (autoT) startTrainer(id);
                    if (autoH) startHrm(id);
                }).detach();
            }
        } catch (...) {
        }
    }

    std::optional<SimpleBLE::Peripheral> peripheral(const std::string& id) {
        std::lock_guard<std::mutex> lk(m);
        auto it = peripherals.find(id);
        if (it == peripherals.end()) return std::nullopt;
        return it->second;
    }

    static std::optional<std::string> findService(SimpleBLE::Peripheral& p, uint16_t chr) {
        auto c = proto::uuid16(chr);
        for (auto& s : p.services())
            for (auto& ch : s.characteristics())
                if (ch.uuid() == c) return s.uuid();
        return std::nullopt;
    }

    // ---------------------------------------------------------------- trainer
    struct Note { uint16_t chr; std::vector<uint8_t> data; };

    void startTrainer(const std::string& id) {
        stopTrainer();
        trainerCancel = false;
        {
            std::lock_guard<std::mutex> lk(m);
            trainer.state = LinkState::Connecting;
            trainer.id = id;
            auto it = found.find(id);
            trainer.name = it != found.end() ? it->second.name : "Trainer";
            trainer.detail.clear();
        }
        trainerThread = std::thread([this, id] { trainerSupervisor(id); });
    }
    void stopTrainer() {
        trainerCancel = true;
        if (trainerThread.joinable()) trainerThread.join();
    }

    void trainerSupervisor(std::string id) {
        int attempt = 0;
        std::optional<TrainerCommand> lastCmd;
        while (!trainerCancel && !quit) {
            std::string detail = "Connection closed";
            try { trainerSession(id, lastCmd); }
            catch (std::exception& e) { detail = e.what(); }
            if (trainerCancel || quit) break;
            ++attempt;
            addLog("trainer session ended: " + detail);
            setLink(true, LinkState::Reconnecting, detail + " - retrying...");
            {
                std::lock_guard<std::mutex> lk(m);
                lastTrainer = -1; power = 0; cadence = 0;
            }
            double until = appClock() + std::max(1, std::min(attempt, 5));
            while (appClock() < until && !trainerCancel && !quit) sleepMs(100);
        }
        if (auto p = peripheral(id)) try { if (p->is_connected()) p->disconnect(); } catch (...) {}
        std::lock_guard<std::mutex> lk(m);
        lastTrainer = -1; power = 0; cadence = 0;
        trainer = Link{};
    }

    void trainerSession(const std::string& id, std::optional<TrainerCommand>& lastCmd) {
        auto po = peripheral(id);
        if (!po) throw std::runtime_error("Unknown device - scan again");
        SimpleBLE::Peripheral p = *po;
        auto dropped = std::make_shared<std::atomic<bool>>(false);
        if (!p.is_connected()) p.connect();
        p.set_callback_on_disconnected([dropped] { *dropped = true; });
        auto svcIbd = findService(p, proto::CHR_INDOOR_BIKE_DATA);
        if (!svcIbd) throw std::runtime_error("No FTMS Indoor Bike Data. Update your KICKR firmware in the Wahoo app to enable FTMS.");
        auto svcCp = findService(p, proto::CHR_FTMS_CONTROL_POINT);
        if (!svcCp) throw std::runtime_error("Trainer has no FTMS Control Point");
        auto svcStatus = findService(p, proto::CHR_FTMS_STATUS);
        std::optional<proto::Range> powerRange, resRange;
        if (auto s = findService(p, proto::CHR_POWER_RANGE))
            try { auto b = p.read(*s, proto::uuid16(proto::CHR_POWER_RANGE)); powerRange = proto::parseRange(b.data(), b.size()); } catch (...) {}
        if (auto s = findService(p, proto::CHR_RESISTANCE_RANGE))
            try { auto b = p.read(*s, proto::uuid16(proto::CHR_RESISTANCE_RANGE)); resRange = proto::parseRange(b.data(), b.size()); } catch (...) {}

        struct NoteQueue { std::mutex m; std::deque<Note> notes; };
        auto nq = std::make_shared<NoteQueue>();
        auto onNote = [nq](uint16_t c) {
            return [nq, c](SimpleBLE::ByteArray b) {
                std::lock_guard<std::mutex> lk(nq->m);
                nq->notes.push_back({c, std::vector<uint8_t>(b.data(), b.data() + b.size())});
            };
        };
        // Bike data is handled directly in the callback (low latency); CP/status go through the queue.
        p.notify(*svcIbd, proto::uuid16(proto::CHR_INDOOR_BIKE_DATA), [this](SimpleBLE::ByteArray b) {
            auto d = proto::parseIndoorBikeData(b.data(), b.size());
            if (!d) return;
            std::lock_guard<std::mutex> lk(m);
            if (d->power_w) power = std::max(0, *d->power_w);
            if (d->cadence_rpm) cadence = *d->cadence_rpm;
            if (d->speed_kmh) speed = *d->speed_kmh;
            if (d->heart_rate) trainerHr = *d->heart_rate > 0 ? std::optional<double>(*d->heart_rate) : std::nullopt;
            lastTrainer = appClock();
        });
        p.indicate(*svcCp, proto::uuid16(proto::CHR_FTMS_CONTROL_POINT), onNote(proto::CHR_FTMS_CONTROL_POINT));
        if (svcStatus) try { p.notify(*svcStatus, proto::uuid16(proto::CHR_FTMS_STATUS), onNote(proto::CHR_FTMS_STATUS)); } catch (...) {}

        auto encode = [&](const TrainerCommand& c) -> proto::Bytes {
            switch (c.type) {
                case TrainerCommand::Simulation: return proto::ftmsSetSimulation(c.gradePct, c.windMps, c.crr, c.cw);
                case TrainerCommand::TargetPower: return proto::ftmsSetTargetPower(int(std::lround(c.watts)));
                case TrainerCommand::Resistance: {
                    double lo = 0, hi = 25.4;
                    if (resRange && resRange->max > resRange->min) { lo = resRange->min / 10.0; hi = resRange->max / 10.0; }
                    return proto::ftmsSetTargetResistance(lo + (hi - lo) * std::clamp(c.levelPct, 0.0, 100.0) / 100.0);
                }
            }
            return {};
        };
        auto isTarget = [](const proto::Bytes& b) { return b[0] == 0x05 || b[0] == 0x11 || b[0] == 0x04; };

        std::deque<proto::Bytes> queue{proto::ftmsRequestControl(), proto::ftmsStart()};
        if (lastCmd) queue.push_back(encode(*lastCmd));
        std::optional<std::pair<uint8_t, double>> pending;
        bool hasControl = false;
        double sessionStart = appClock();
        {
            std::lock_guard<std::mutex> lk(m);
            trainer.state = LinkState::Connected;
            trainer.detail = "Requesting control...";
            if (powerRange) trainer.powerRange = std::make_pair(int(powerRange->min), int(powerRange->max));
        }
        addLog(trainer.name + ": connected (FTMS)");

        while (!trainerCancel && !quit) {
            if (*dropped) throw std::runtime_error("Trainer disconnected");
            // newest command from the engine replaces any queued target
            {
                std::optional<TrainerCommand> c;
                {
                    std::lock_guard<std::mutex> lk(m);
                    c = pendingCmd;
                    pendingCmd.reset();
                }
                if (c) {
                    lastCmd = c;
                    queue.erase(std::remove_if(queue.begin(), queue.end(), isTarget), queue.end());
                    queue.push_back(encode(*c));
                }
            }
            if (!pending && !queue.empty()) {
                auto b = queue.front();
                queue.pop_front();
                p.write_request(*svcCp, proto::uuid16(proto::CHR_FTMS_CONTROL_POINT), SimpleBLE::ByteArray(b));
                pending = std::make_pair(b[0], appClock());
            }
            std::deque<Note> got;
            {
                std::lock_guard<std::mutex> lk(nq->m);
                got.swap(nq->notes);
            }
            for (auto& n : got) {
                if (n.chr == proto::CHR_FTMS_CONTROL_POINT) {
                    auto r = proto::parseControlResponse(n.data.data(), n.data.size());
                    if (!r) continue;
                    if (pending && pending->first == r->request_op) pending.reset();
                    if (r->request_op == 0x00 && r->result == 0x01) {
                        hasControl = true;
                        setLink(true, LinkState::Connected, "");
                    } else if (r->result == 0x05) {
                        hasControl = false;
                        queue.push_front(proto::ftmsRequestControl());
                        if (lastCmd) queue.push_back(encode(*lastCmd));
                    } else if (r->result != 0x01) {
                        char b[80];
                        std::snprintf(b, sizeof b, "FTMS op 0x%02X -> %s", r->request_op, proto::controlResultName(r->result));
                        addLog(b);
                    }
                } else if (n.chr == proto::CHR_FTMS_STATUS) {
                    auto st = proto::parseMachineStatus(n.data.data(), n.data.size());
                    if (st && *st == proto::MachineStatus::ControlPermissionLost) {
                        hasControl = false;
                        queue.push_front(proto::ftmsStart());
                        queue.push_front(proto::ftmsRequestControl());
                        if (lastCmd) queue.push_back(encode(*lastCmd));
                    }
                }
            }
            if (pending && appClock() - pending->second > 2.0) {
                char b[48];
                std::snprintf(b, sizeof b, "FTMS op 0x%02X timed out", pending->first);
                addLog(b);
                if (pending->first == 0x00 && !hasControl) queue.push_front(proto::ftmsRequestControl());
                pending.reset();
            }
            double lt;
            {
                std::lock_guard<std::mutex> lk(m);
                lt = lastTrainer;
            }
            bool stale = lt < 0 || appClock() - lt > 6;
            if (stale && appClock() - sessionStart > 8 && !p.is_connected()) throw std::runtime_error("Trainer stopped responding");
            sleepMs(25);
        }
        try { p.unsubscribe(*svcIbd, proto::uuid16(proto::CHR_INDOOR_BIKE_DATA)); } catch (...) {}
    }

    // ---------------------------------------------------------------- heart rate
    void startHrm(const std::string& id) {
        stopHrm();
        hrCancel = false;
        {
            std::lock_guard<std::mutex> lk(m);
            hrm.state = LinkState::Connecting;
            hrm.id = id;
            auto it = found.find(id);
            hrm.name = it != found.end() ? it->second.name : "Heart rate";
            hrm.detail.clear();
        }
        hrThread = std::thread([this, id] { hrmSupervisor(id); });
    }
    void stopHrm() {
        hrCancel = true;
        if (hrThread.joinable()) hrThread.join();
    }
    void hrmSupervisor(std::string id) {
        int attempt = 0;
        while (!hrCancel && !quit) {
            std::string detail = "Connection closed";
            try { hrmSession(id); }
            catch (std::exception& e) { detail = e.what(); }
            if (hrCancel || quit) break;
            ++attempt;
            setLink(false, LinkState::Reconnecting, detail + " - retrying...");
            double until = appClock() + std::min(attempt, 5);
            while (appClock() < until && !hrCancel && !quit) sleepMs(100);
        }
        if (auto p = peripheral(id)) try { if (p->is_connected()) p->disconnect(); } catch (...) {}
        std::lock_guard<std::mutex> lk(m);
        lastHr = -1;
        hrm = Link{};
    }
    void hrmSession(const std::string& id) {
        auto po = peripheral(id);
        if (!po) throw std::runtime_error("Unknown device - scan again");
        SimpleBLE::Peripheral p = *po;
        auto dropped = std::make_shared<std::atomic<bool>>(false);
        if (!p.is_connected()) p.connect();
        p.set_callback_on_disconnected([dropped] { *dropped = true; });
        auto svc = findService(p, proto::CHR_HR_MEASUREMENT);
        if (!svc) throw std::runtime_error("Device has no heart rate measurement");
        p.notify(*svc, proto::uuid16(proto::CHR_HR_MEASUREMENT), [this](SimpleBLE::ByteArray b) {
            auto h = proto::parseHeartRate(b.data(), b.size());
            if (!h) return;
            std::lock_guard<std::mutex> lk(m);
            hr = h->bpm;
            lastHr = h->bpm > 0 ? appClock() : -1;
        });
        setLink(false, LinkState::Connected, "");
        addLog(hrm.name + ": heart rate connected");
        while (!hrCancel && !quit) {
            if (*dropped) throw std::runtime_error("Heart rate monitor disconnected");
            sleepMs(200);
        }
        try { p.unsubscribe(*svc, proto::uuid16(proto::CHR_HR_MEASUREMENT)); } catch (...) {}
    }
#else
    void startTrainer(const std::string&) {}
    void stopTrainer() {}
    void startHrm(const std::string&) {}
    void stopHrm() {}
#endif
};

BleManager::BleManager() : impl_(std::make_unique<Impl>()) {
#ifdef CORERAY_BLE
    // adapter init can block briefly on macOS while CoreBluetooth powers up
    impl_->scanThread = std::thread([this] {
        impl_->initAdapter();
        while (!impl_->quit) {
            if (impl_->scanning && appClock() > impl_->scanUntil) stopScan();
            sleepMs(250);
        }
    });
#endif
}

BleManager::~BleManager() {
    impl_->quit = true;
    impl_->trainerCancel = true;
    impl_->hrCancel = true;
    std::lock_guard<std::mutex> g(impl_->ctl);
    impl_->stopTrainer();
    impl_->stopHrm();
#ifdef CORERAY_BLE
    try { if (impl_->adapter && impl_->adapter->scan_is_active()) impl_->adapter->scan_stop(); } catch (...) {}
#endif
    if (impl_->scanThread.joinable()) impl_->scanThread.join();
}

bool BleManager::available() const { std::lock_guard<std::mutex> lk(impl_->m); return impl_->hasAdapter; }
std::string BleManager::status() const { std::lock_guard<std::mutex> lk(impl_->m); return impl_->adapterMsg; }
bool BleManager::scanning() const { return impl_->scanning; }

void BleManager::startScan(int seconds) {
#ifdef CORERAY_BLE
    std::thread([this, seconds] {
        for (int i = 0; i < 40 && !available() && !impl_->quit; ++i) sleepMs(100);  // wait for adapter
        try {
            if (impl_->adapter && !impl_->adapter->scan_is_active()) impl_->adapter->scan_start();
            impl_->scanUntil = appClock() + std::clamp(seconds, 5, 600);
            impl_->scanning = impl_->adapter != nullptr;
        } catch (std::exception& e) {
            impl_->addLog(std::string("Failed to start scanning: ") + e.what());
        }
    }).detach();
#else
    (void)seconds;
#endif
}

void BleManager::stopScan() {
#ifdef CORERAY_BLE
    try { if (impl_->adapter && impl_->adapter->scan_is_active()) impl_->adapter->scan_stop(); } catch (...) {}
#endif
    impl_->scanning = false;
}

std::vector<DeviceInfo> BleManager::devices() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    std::vector<DeviceInfo> v;
    for (auto& [k, d] : impl_->found) v.push_back(d);
    std::sort(v.begin(), v.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
        auto rank = [](DevKind k) { return k == DevKind::Trainer ? 0 : k == DevKind::Power ? 1 : 2; };
        if (rank(a.kind) != rank(b.kind)) return rank(a.kind) < rank(b.kind);
        return a.rssi > b.rssi;
    });
    return v;
}

void BleManager::connectTrainer(const std::string& id) {
    { std::lock_guard<std::mutex> lk(impl_->m); impl_->trainer.state = LinkState::Connecting; impl_->trainer.id = id; }
    std::thread([this, id] { std::lock_guard<std::mutex> g(impl_->ctl); if (!impl_->quit) impl_->startTrainer(id); }).detach();
}
void BleManager::disconnectTrainer() {
    impl_->trainerCancel = true;
    std::thread([this] { std::lock_guard<std::mutex> g(impl_->ctl); impl_->stopTrainer(); }).detach();
}
void BleManager::connectHrm(const std::string& id) {
    { std::lock_guard<std::mutex> lk(impl_->m); impl_->hrm.state = LinkState::Connecting; impl_->hrm.id = id; }
    std::thread([this, id] { std::lock_guard<std::mutex> g(impl_->ctl); if (!impl_->quit) impl_->startHrm(id); }).detach();
}
void BleManager::disconnectHrm() {
    impl_->hrCancel = true;
    std::thread([this] { std::lock_guard<std::mutex> g(impl_->ctl); impl_->stopHrm(); }).detach();
}

Link BleManager::trainer() const { std::lock_guard<std::mutex> lk(impl_->m); return impl_->trainer; }
Link BleManager::hrm() const { std::lock_guard<std::mutex> lk(impl_->m); return impl_->hrm; }

void BleManager::setAutoConnect(const std::string& t, const std::string& h) {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->autoTrainer = t;
    impl_->autoHr = h;
}

void BleManager::send(const TrainerCommand& c) {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->pendingCmd = c;
}

TrainerInput BleManager::input() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    double now = appClock();
    bool fresh = impl_->lastTrainer >= 0 && now - impl_->lastTrainer < 3;
    bool hrFresh = impl_->lastHr >= 0 && now - impl_->lastHr < 5;
    TrainerInput in;
    in.powerW = fresh ? impl_->power : 0;
    in.cadenceRpm = fresh ? impl_->cadence : 0;
    in.heartRate = hrFresh ? std::optional<double>(impl_->hr) : fresh ? impl_->trainerHr : std::nullopt;
    in.connected = fresh;
    return in;
}

std::optional<double> BleManager::speedKmh() const { std::lock_guard<std::mutex> lk(impl_->m); return impl_->speed; }

std::vector<std::string> BleManager::log() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    return {impl_->logLines.begin(), impl_->logLines.end()};
}
