#pragma once
// SQLite store with CoreKick's schema (rides / routes / events / ftp_history / meta),
// so CoreKick's history can be imported wholesale and rows stay interchangeable.
#include <optional>
#include <string>
#include <vector>

#include "core/Records.h"
#include "core/Route.h"

struct sqlite3;

class Db {
public:
    ~Db();
    bool open(const std::string& path);
    bool isOpen() const { return db_ != nullptr; }
    static std::string defaultPath();

    std::optional<std::string> meta(const std::string& key);
    void setMeta(const std::string& key, const std::string& value);

    // rides
    void saveRide(const RideRecord& r);
    void updateSummary(const RideSummary& s);
    std::vector<RideSummary> rideSummaries();  // newest first
    std::optional<RideRecord> ride(const std::string& id);
    void deleteRide(const std::string& id);
    std::optional<RideRecord> bestRouteRide(const std::string& routeId, int laps);
    std::vector<RideSummary> routeRides(const std::string& routeId);
    std::vector<RideSummary> eventRides(const std::string& eventId);

    // routes
    std::vector<Route> routes();
    std::optional<Route> route(const std::string& id);
    void saveRoute(const Route& r);
    void deleteRoute(const std::string& id);

    // events
    std::vector<Event> events();
    std::optional<Event> event(const std::string& id);
    void saveEvent(const Event& e);
    void deleteEvent(const std::string& id);

    // FTP history
    void addFtp(const FtpEntry& e);
    std::vector<FtpEntry> ftpHistory();

    // Import rides, routes, events, FTP history and Strava settings from a CoreKick database.
    // Returns counts in `msg`. Existing rows (same id) are kept.
    bool importCoreKick(const std::string& path, std::string& msg);
    void seedIfEmpty(double ftp);

private:
    bool exec(const std::string& sql);
    std::vector<std::string> jsonColumn(const std::string& sql, const std::string& bind = "");
    sqlite3* db_ = nullptr;
};
