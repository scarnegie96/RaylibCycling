#pragma once
// Bluetooth LE (SimpleBLE): scanning, an FTMS trainer and a heart-rate monitor.
// Modelled on CoreKick's ble.rs: each connected device runs a supervisor thread that
// (re)connects, subscribes, performs the FTMS control handshake and then multiplexes
// notifications with outgoing control-point writes. Writes are serialised (one
// outstanding request, 2 s timeout) and coalesced so only the newest target is sent.
// If the trainer drops out it reconnects and re-sends the current gradient / ERG target.
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Engine.h"

enum class DevKind { Trainer, HeartRate, Power };
enum class LinkState { Disconnected, Connecting, Connected, Reconnecting };

struct DeviceInfo {
    std::string id, name;
    int rssi = -127;
    DevKind kind = DevKind::Trainer;
    bool ftms = false;
};

struct Link {
    LinkState state = LinkState::Disconnected;
    std::string id, name, detail;
    std::optional<std::pair<int, int>> powerRange;
};

class BleManager {
public:
    BleManager();
    ~BleManager();
    BleManager(const BleManager&) = delete;
    BleManager& operator=(const BleManager&) = delete;

    bool available() const;
    std::string status() const;  // adapter status / error
    void startScan(int seconds = 60);
    void stopScan();
    bool scanning() const;
    std::vector<DeviceInfo> devices() const;

    void connectTrainer(const std::string& id);
    void disconnectTrainer();
    void connectHrm(const std::string& id);
    void disconnectHrm();
    Link trainer() const;
    Link hrm() const;
    // reconnect these automatically when a scan finds them
    void setAutoConnect(const std::string& trainerId, const std::string& hrId);

    void send(const TrainerCommand& c);
    // Current readings as engine input. Data older than 3 s counts as "not riding".
    TrainerInput input() const;
    std::optional<double> speedKmh() const;
    std::vector<std::string> log() const;

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

double appClock();
