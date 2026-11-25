#include "Db.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include <sqlite3.h>

namespace fs = std::filesystem;

static const char* SCHEMA = R"(
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS profile (id INTEGER PRIMARY KEY CHECK (id = 1), json TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS routes (id TEXT PRIMARY KEY, created_at TEXT NOT NULL, json TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS workouts (id TEXT PRIMARY KEY, created_at TEXT NOT NULL, json TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS events (id TEXT PRIMARY KEY, created_at TEXT NOT NULL, json TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS rides (
    id TEXT PRIMARY KEY,
    started_at TEXT NOT NULL,
    route_id TEXT,
    event_id TEXT,
    laps INTEGER NOT NULL DEFAULT 1,
    completed INTEGER NOT NULL DEFAULT 0,
    route_time_s REAL,
    summary TEXT NOT NULL,
    samples TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS rides_started ON rides(started_at);
CREATE INDEX IF NOT EXISTS rides_route ON rides(route_id, laps, completed);
CREATE INDEX IF NOT EXISTS rides_event ON rides(event_id);
CREATE TABLE IF NOT EXISTS ftp_history (id INTEGER PRIMARY KEY AUTOINCREMENT, date TEXT NOT NULL, ftp REAL NOT NULL, source TEXT NOT NULL);
)";

namespace {
struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const std::string& sql) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) {
            std::fprintf(stderr, "[db] %s: %s\n", sqlite3_errmsg(db), sql.c_str());
            s = nullptr;
        }
    }
    ~Stmt() { if (s) sqlite3_finalize(s); }
    Stmt& text(int i, const std::string& v) { if (s) sqlite3_bind_text(s, i, v.c_str(), -1, SQLITE_TRANSIENT); return *this; }
    Stmt& optText(int i, const std::optional<std::string>& v) { if (s) { if (v) text(i, *v); else sqlite3_bind_null(s, i); } return *this; }
    Stmt& num(int i, double v) { if (s) sqlite3_bind_double(s, i, v); return *this; }
    Stmt& optNum(int i, const std::optional<double>& v) { if (s) { if (v) num(i, *v); else sqlite3_bind_null(s, i); } return *this; }
    Stmt& integer(int i, long long v) { if (s) sqlite3_bind_int64(s, i, v); return *this; }
    bool step() { return s && sqlite3_step(s) == SQLITE_ROW; }
    bool run() { if (!s) return false; int rc = sqlite3_step(s); return rc == SQLITE_DONE || rc == SQLITE_ROW; }
    std::string col(int i) { const unsigned char* t = sqlite3_column_text(s, i); return t ? reinterpret_cast<const char*>(t) : ""; }
    double colNum(int i) { return sqlite3_column_double(s, i); }
};

std::string home() {
    const char* h = std::getenv("HOME");
    return h ? h : ".";
}
}  // namespace

Db::~Db() { if (db_) sqlite3_close(db_); }

std::string Db::defaultPath() {
    fs::path d = fs::path(home()) / ".coreray";
    std::error_code ec;
    fs::create_directories(d, ec);
    return (d / "coreray.db").string();
}

bool Db::open(const std::string& path) {
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "[db] cannot open %s\n", path.c_str());
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    exec("PRAGMA journal_mode=WAL;");
    return exec(SCHEMA);
}

bool Db::exec(const std::string& sql) {
    if (!db_) return false;
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::fprintf(stderr, "[db] %s\n", err ? err : "error");
        sqlite3_free(err);
        return false;
    }
    return true;
}

std::vector<std::string> Db::jsonColumn(const std::string& sql, const std::string& bind) {
    std::vector<std::string> out;
    if (!db_) return out;
    Stmt st(db_, sql);
    if (!bind.empty()) st.text(1, bind);
    while (st.step()) out.push_back(st.col(0));
    return out;
}

std::optional<std::string> Db::meta(const std::string& key) {
    auto v = jsonColumn("SELECT value FROM meta WHERE key = ?1", key);
    if (v.empty() || v[0].empty()) return std::nullopt;
    return v[0];
}

void Db::setMeta(const std::string& key, const std::string& value) {
    if (!db_) return;
    Stmt(db_, "INSERT INTO meta (key, value) VALUES (?1, ?2) ON CONFLICT(key) DO UPDATE SET value = excluded.value").text(1, key).text(2, value).run();
}

// ------------------------------------------------------------------ rides
void Db::saveRide(const RideRecord& r) {
    if (!db_) return;
    const RideSummary& s = r.summary;
    Stmt(db_,
         "INSERT INTO rides (id, started_at, route_id, event_id, laps, completed, route_time_s, summary, samples) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9) "
         "ON CONFLICT(id) DO UPDATE SET summary = excluded.summary, samples = excluded.samples")
        .text(1, s.id).text(2, s.startedAt).optText(3, s.routeId).optText(4, s.eventId).integer(5, s.laps).integer(6, s.completed ? 1 : 0)
        .optNum(7, s.routeTimeS).text(8, json::dump(s.toJson())).text(9, json::dump(r.samples.toJson()))
        .run();
}

void Db::updateSummary(const RideSummary& s) {
    if (!db_) return;
    Stmt(db_, "UPDATE rides SET summary = ?2 WHERE id = ?1").text(1, s.id).text(2, json::dump(s.toJson())).run();
}

static std::vector<RideSummary> parseSummaries(const std::vector<std::string>& rows) {
    std::vector<RideSummary> out;
    for (auto& j : rows) {
        json::Value v;
        if (json::parse(j, v)) out.push_back(RideSummary::fromJson(v));
    }
    return out;
}

std::vector<RideSummary> Db::rideSummaries() { return parseSummaries(jsonColumn("SELECT summary FROM rides ORDER BY started_at DESC")); }
std::vector<RideSummary> Db::routeRides(const std::string& id) {
    return parseSummaries(jsonColumn("SELECT summary FROM rides WHERE route_id = ?1 ORDER BY started_at DESC", id));
}
std::vector<RideSummary> Db::eventRides(const std::string& id) {
    return parseSummaries(jsonColumn("SELECT summary FROM rides WHERE event_id = ?1 ORDER BY started_at DESC", id));
}

std::optional<RideRecord> Db::ride(const std::string& id) {
    if (!db_) return std::nullopt;
    Stmt st(db_, "SELECT summary, samples FROM rides WHERE id = ?1");
    st.text(1, id);
    if (!st.step()) return std::nullopt;
    json::Value s, sm;
    if (!json::parse(st.col(0), s)) return std::nullopt;
    RideRecord r;
    r.summary = RideSummary::fromJson(s);
    if (json::parse(st.col(1), sm)) r.samples = Samples::fromJson(sm);
    return r;
}

void Db::deleteRide(const std::string& id) { if (db_) Stmt(db_, "DELETE FROM rides WHERE id = ?1").text(1, id).run(); }

std::optional<RideRecord> Db::bestRouteRide(const std::string& routeId, int laps) {
    if (!db_) return std::nullopt;
    Stmt st(db_, "SELECT id FROM rides WHERE route_id = ?1 AND laps = ?2 AND completed = 1 AND route_time_s IS NOT NULL ORDER BY route_time_s ASC LIMIT 1");
    st.text(1, routeId).integer(2, laps);
    if (!st.step()) return std::nullopt;
    return ride(st.col(0));
}

// ------------------------------------------------------------------ routes
std::vector<Route> Db::routes() {
    std::vector<Route> out;
    for (auto& j : jsonColumn("SELECT json FROM routes ORDER BY created_at")) {
        json::Value v;
        if (json::parse(j, v)) if (auto r = Route::fromJson(v)) out.push_back(*r);
    }
    return out;
}

std::optional<Route> Db::route(const std::string& id) {
    auto rows = jsonColumn("SELECT json FROM routes WHERE id = ?1", id);
    if (rows.empty()) return std::nullopt;
    json::Value v;
    if (!json::parse(rows[0], v)) return std::nullopt;
    return Route::fromJson(v);
}

void Db::saveRoute(const Route& r) {
    if (!db_) return;
    Stmt(db_, "INSERT INTO routes (id, created_at, json) VALUES (?1,?2,?3) ON CONFLICT(id) DO UPDATE SET json = excluded.json")
        .text(1, r.id).text(2, r.createdAt).text(3, json::dump(r.toJson())).run();
}
void Db::deleteRoute(const std::string& id) { if (db_) Stmt(db_, "DELETE FROM routes WHERE id = ?1").text(1, id).run(); }

// ------------------------------------------------------------------ events
std::vector<Event> Db::events() {
    std::vector<Event> out;
    for (auto& j : jsonColumn("SELECT json FROM events ORDER BY created_at")) {
        json::Value v;
        if (json::parse(j, v)) if (auto e = Event::fromJson(v)) out.push_back(*e);
    }
    return out;
}
std::optional<Event> Db::event(const std::string& id) {
    auto rows = jsonColumn("SELECT json FROM events WHERE id = ?1", id);
    if (rows.empty()) return std::nullopt;
    json::Value v;
    if (!json::parse(rows[0], v)) return std::nullopt;
    return Event::fromJson(v);
}
void Db::saveEvent(const Event& e) {
    if (!db_) return;
    Stmt(db_, "INSERT INTO events (id, created_at, json) VALUES (?1,?2,?3) ON CONFLICT(id) DO UPDATE SET json = excluded.json")
        .text(1, e.id).text(2, e.createdAt).text(3, json::dump(e.toJson())).run();
}
void Db::deleteEvent(const std::string& id) { if (db_) Stmt(db_, "DELETE FROM events WHERE id = ?1").text(1, id).run(); }

// ------------------------------------------------------------------ FTP history
void Db::addFtp(const FtpEntry& e) {
    if (!db_) return;
    Stmt(db_, "INSERT INTO ftp_history (date, ftp, source) VALUES (?1,?2,?3)").text(1, e.date).num(2, e.ftp).text(3, e.source).run();
}
std::vector<FtpEntry> Db::ftpHistory() {
    std::vector<FtpEntry> out;
    if (!db_) return out;
    Stmt st(db_, "SELECT date, ftp, source FROM ftp_history ORDER BY date, id");
    while (st.step()) out.push_back({st.col(0), st.colNum(1), st.col(2)});
    return out;
}

// ------------------------------------------------------------------ CoreKick import / seeding
static long long count(sqlite3* db, const char* sql) {
    Stmt st(db, sql);
    return st.step() ? sqlite3_column_int64(st.s, 0) : 0;
}

bool Db::importCoreKick(const std::string& path, std::string& msg) {
    if (!db_) { msg = "Database not open"; return false; }
    std::error_code ec;
    if (!fs::exists(path, ec)) { msg = "CoreKick database not found"; return false; }
    long long r0 = count(db_, "SELECT COUNT(*) FROM rides"), ro0 = count(db_, "SELECT COUNT(*) FROM routes"), e0 = count(db_, "SELECT COUNT(*) FROM events");
    // read-only URI attach (the main connection is opened with SQLITE_OPEN_URI)
    std::string uri = "file:";
    for (char c : path) {
        if (c == ' ') uri += "%20"; else if (c == '%') uri += "%25"; else if (c == '?') uri += "%3F"; else if (c == '#') uri += "%23"; else uri += c;
    }
    Stmt(db_, "ATTACH DATABASE ?1 AS ck").text(1, uri + "?mode=ro").run();
    if (count(db_, "SELECT COUNT(*) FROM pragma_database_list WHERE name = 'ck'") == 0) { msg = "Could not open the CoreKick database"; return false; }
    bool ok = exec("BEGIN;"
                   "INSERT OR IGNORE INTO rides SELECT * FROM ck.rides;"
                   "INSERT OR IGNORE INTO routes SELECT * FROM ck.routes;"
                   "INSERT OR IGNORE INTO events SELECT * FROM ck.events;"
                   "INSERT INTO ftp_history (date, ftp, source) SELECT date, ftp, source FROM ck.ftp_history "
                   "  WHERE NOT EXISTS (SELECT 1 FROM ftp_history f WHERE f.date = ck.ftp_history.date AND f.ftp = ck.ftp_history.ftp);"
                   "INSERT OR IGNORE INTO meta SELECT * FROM ck.meta WHERE key LIKE 'strava_%' OR key = 'ftp_assessment';"
                   "COMMIT;");
    exec("DETACH DATABASE ck;");
    if (!ok) { exec("ROLLBACK;"); msg = "Import failed"; return false; }
    char b[200];
    std::snprintf(b, sizeof b, "Imported %lld rides, %lld routes and %lld events from CoreKick", count(db_, "SELECT COUNT(*) FROM rides") - r0,
                  count(db_, "SELECT COUNT(*) FROM routes") - ro0, count(db_, "SELECT COUNT(*) FROM events") - e0);
    msg = b;
    return true;
}

void Db::seedIfEmpty(double ftp) {
    if (!db_ || meta("seeded")) return;
    if (count(db_, "SELECT COUNT(*) FROM routes") == 0)
        for (auto& r : seedRoutes()) saveRoute(r);
    if (count(db_, "SELECT COUNT(*) FROM ftp_history") == 0) addFtp({Date::today().str(), ftp, "Initial"});
    setMeta("seeded", "1");
}
