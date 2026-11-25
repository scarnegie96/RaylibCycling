// SQLite store + CoreKick import + net helpers.
#include <cstdio>
#include <filesystem>

#include "Db.h"
#include "Net.h"
#include "core/Dates.h"
#include "core/Engine.h"

static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace fs = std::filesystem;

static RideRecord makeRide(const std::string& id, const Route& r) {
    Profile p;
    RideSetup s;
    s.kind = RideKind::Route;
    s.route = r;
    RideEngine e(p, s, "2026-11-20T18:00:00+00:00");
    for (int i = 0; i < 4 * 120; ++i) e.tick(0.25, {220, 90, 140.0, true});
    return e.finish("2026-11-20T18:02:00+00:00", id);
}

int main() {
    fs::path dir = fs::temp_directory_path() / ("coreray-dbtest-" + dates::newId());
    fs::create_directories(dir);
    {
        // a "CoreKick" database: same schema, filled through a second Db
        Db ck;
        CHECK(ck.open((dir / "corekick db.sqlite").string()));
        Route r = buildFromSegments("CK route", {{3000, 0}, {1000, 4}}, 100, false);
        r.id = "ck-route";
        ck.saveRoute(r);
        ck.saveRide(makeRide("ck-ride-1", r));
        Event ev;
        ev.id = "ck-ev"; ev.name = "Tue"; ev.recurrence.startDate = "2026-11-03"; ev.recurrence.weekdays = {1};
        ck.saveEvent(ev);
        ck.addFtp({"2026-10-01", 240, "Test"});
        ck.setMeta("strava_client_id", "12345");

        Db db;
        CHECK(db.open((dir / "coreray.db").string()));
        db.seedIfEmpty(250);
        CHECK(db.routes().size() == 6 && db.ftpHistory().size() == 1);
        std::string msg;
        CHECK(db.importCoreKick((dir / "corekick db.sqlite").string(), msg));
        CHECK(msg.find("1 rides, 1 routes and 1 events") != std::string::npos);
        CHECK(db.routes().size() == 7 && db.events().size() == 1 && db.ftpHistory().size() == 2);
        CHECK(db.meta("strava_client_id").value_or("") == "12345");
        // importing twice adds nothing
        CHECK(db.importCoreKick((dir / "corekick db.sqlite").string(), msg) && msg.find("0 rides") != std::string::npos && db.ftpHistory().size() == 2);
        auto rides = db.rideSummaries();
        CHECK(rides.size() == 1 && rides[0].id == "ck-ride-1");
        auto full = db.ride("ck-ride-1");
        CHECK(full && full->samples.size() == 120);
        db.saveRide(makeRide("own", *db.route("ck-route")));
        CHECK(db.routeRides("ck-route").size() == 2);
        auto best = db.bestRouteRide("ck-route", 1);
        CHECK(!best || best->summary.completed);
        RideSummary s = rides[0];
        s.name = "Renamed";
        s.stravaActivityId = 987654321ULL;
        db.updateSummary(s);
        CHECK(db.ride("ck-ride-1")->summary.name == "Renamed" && *db.ride("ck-ride-1")->summary.stravaActivityId == 987654321ULL);
        db.deleteRide("own");
        CHECK(db.rideSummaries().size() == 1);
        CHECK(!db.importCoreKick((dir / "missing.db").string(), msg));
    }
    // net helpers
    CHECK(net::queryParam("state=abc&code=x%2Fy&scope=read,activity:write", "code") == "x/y");
    CHECK(net::queryParam("a=1&b=hello+world", "b") == "hello world");
    CHECK(net::queryParam("a=1", "z").empty());
    CHECK(net::percentEncode("a b/c") == "a%20b%2Fc");
    CHECK(strava::duplicateId("coreray.fit duplicate of <a href='/activities/12345678'>x</a>") == 12345678ULL);
    CHECK(!strava::duplicateId("some other error"));
    fs::remove_all(dir);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
