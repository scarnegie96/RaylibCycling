#include "Protocol.h"

#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main() {
    using namespace proto;
    CHECK(uuid16(0x1826) == "00001826-0000-1000-8000-00805f9b34fb");

    {   // Kickr Core style IBD: speed + cadence + power (flags 0x0044)
        uint8_t d[] = {0x44, 0x00, 0xC4, 0x09, 0xB4, 0x00, 0xFA, 0x00};
        auto r = parseIndoorBikeData(d, sizeof d);
        CHECK(r && r->speed_kmh && std::fabs(*r->speed_kmh - 25.0) < 1e-9);
        CHECK(r->cadence_rpm && std::fabs(*r->cadence_rpm - 90.0) < 1e-9);
        CHECK(r->power_w && *r->power_w == 250);
    }
    {   // More Data bit set -> no speed; power + HR
        uint8_t d[] = {0x41, 0x02, 0x2C, 0x01, 0x8C};
        auto r = parseIndoorBikeData(d, sizeof d);
        CHECK(r && !r->speed_kmh && r->power_w && *r->power_w == 300);
        CHECK(r->heart_rate && *r->heart_rate == 140);
    }
    {   // truncated packet must not crash
        uint8_t d[] = {0x44, 0x00, 0xC4};
        auto r = parseIndoorBikeData(d, sizeof d);
        CHECK(r && !r->speed_kmh);
    }
    {   // CPS with crank data (flag 0x20)
        uint8_t d[] = {0x20, 0x00, 0xC8, 0x00, 0x0A, 0x00, 0x00, 0x04};
        auto r = parseCyclingPower(d, sizeof d);
        CHECK(r && r->power_w == 200 && r->crank_revs && *r->crank_revs == 10 && *r->crank_event_1024 == 1024);
    }
    {   // crank cadence incl 16-bit wrap
        CrankCadence cc;
        CHECK(!cc.update(65535, 65000, 0.0));
        auto c = cc.update(0, uint16_t(65000 + 683), 0.66);  // 1 rev in 683/1024 s ~ 90 rpm
        CHECK(c && std::fabs(*c - 89.96) < 0.1);
        CHECK(!cc.update(0, uint16_t(65000 + 683), 1.0));
        auto z = cc.update(0, uint16_t(65000 + 683), 4.0);
        CHECK(z && *z == 0.0);
    }
    {   // HR 8-bit, 16-bit + RR
        uint8_t a[] = {0x00, 72};
        CHECK(parseHeartRate(a, 2)->bpm == 72);
        uint8_t b[] = {0x11, 0x2C, 0x01, 0x00, 0x04};
        auto h = parseHeartRate(b, sizeof b);
        CHECK(h && h->bpm == 300 && h->rr_ms.size() == 1 && std::fabs(h->rr_ms[0] - 1000.0) < 1e-9);
    }
    {   // FTMS encoders
        auto p = ftmsSetTargetPower(250);
        CHECK(p.size() == 3 && p[0] == 0x05 && p[1] == 0xFA && p[2] == 0x00);
        auto s = ftmsSetSimulation(-2.5, 0, 0.004, 0.51);
        CHECK(s.size() == 7 && s[0] == 0x11 && s[3] == 0x06 && s[4] == 0xFF && s[5] == 40 && s[6] == 51);
        uint8_t ok[] = {0x80, 0x05, 0x01};
        auto cr = parseControlResponse(ok, 3);
        CHECK(cr && cr->request_op == 0x05 && cr->result == 1);
    }
    {   // Wahoo
        auto g = wahooSetGrade(0.0);
        CHECK(g[0] == 0x46 && g[1] == 0x00 && g[2] == 0x80);
        auto e = wahooSetErg(200);
        CHECK(e[0] == 0x42 && e[1] == 200 && e[2] == 0);
    }
    if (failures == 0) std::printf("All protocol tests passed\n");
    return failures ? 1 : 0;
}
