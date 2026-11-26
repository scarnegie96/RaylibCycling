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
    {   // cadence after a long stop: first new event only re-arms (no bogus 1-2 rpm)
        CrankCadence cc;
        cc.update(100, 0, 0.0);
        CHECK(cc.update(101, 683, 0.66));
        auto z = cc.update(101, 683, 4.0);
        CHECK(z && *z == 0.0);
        CHECK(!cc.update(102, uint16_t(683 + 40000), 45.0));       // resume after 44 s
        auto c = cc.update(103, uint16_t(683 + 40000 + 683), 45.66);
        CHECK(c && std::fabs(*c - 89.96) < 0.1);
    }
    {   // IBD with every field up to HR: offsets must line up
        // flags: avg speed(1) cad(2) avg cad(3) dist(4) res(5) power(6) avg power(7) energy(8) hr(9) = 0x03FE
        uint8_t d[] = {0xFE, 0x03,
                       0x10, 0x27,              // speed 100.00 km/h
                       0x00, 0x00,              // avg speed
                       0xB5, 0x00,              // cadence 90.5 rpm
                       0x00, 0x00,              // avg cadence
                       0x10, 0x27, 0x00,        // distance 10000 m
                       0x05, 0x00,              // resistance 5
                       0xE6, 0x00,              // power 230 W
                       0x00, 0x00,              // avg power
                       0x00, 0x00, 0x00, 0x00, 0x00,  // energy
                       0x96};                   // HR 150
        auto r = parseIndoorBikeData(d, sizeof d);
        CHECK(r && r->speed_kmh && std::fabs(*r->speed_kmh - 100.0) < 1e-9);
        CHECK(r->cadence_rpm && std::fabs(*r->cadence_rpm - 90.5) < 1e-9);
        CHECK(r->total_distance_m && *r->total_distance_m == 10000);
        CHECK(r->resistance && *r->resistance == 5);
        CHECK(r->power_w && *r->power_w == 230);
        CHECK(r->heart_rate && *r->heart_rate == 150);
    }
    {   // CPS with balance + torque + wheel + crank (flags 0x35)
        uint8_t d[] = {0x35, 0x00, 0xE6, 0x00, 0x64, 0x00, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00, 0x08, 0x2A, 0x00, 0x00, 0x08};
        auto r = parseCyclingPower(d, sizeof d);
        CHECK(r && r->power_w == 230 && r->wheel_revs && *r->wheel_revs == 16 && *r->wheel_event_2048 == 2048);
        CHECK(r->crank_revs && *r->crank_revs == 42 && *r->crank_event_1024 == 2048);
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
        auto rs = wahooSetResistance(0.0);
        CHECK(rs.size() == 3 && rs[0] == 0x40 && rs[1] == 0xFF && rs[2] == 0x3F);
    }
    {   // Zwift controllers
        auto h = zwift::rideOn();
        CHECK(std::string(h.begin(), h.end()) == "RideOn");
        uint8_t plusDown[] = {0x37, 0x08, 0x00, 0x10, 0x01}, none[] = {0x37, 0x08, 0x01, 0x10, 0x01}, minusDown[] = {0x37, 0x08, 0x01, 0x10, 0x00};
        auto a = zwift::parseButtons(plusDown, sizeof plusDown), b = zwift::parseButtons(none, sizeof none), c = zwift::parseButtons(minusDown, sizeof minusDown);
        CHECK(a && a->up && !a->down);
        CHECK(b && !b->up && !b->down);
        CHECK(c && !c->up && c->down);
        zwift::ShiftEdges ed;
        CHECK(ed.update(*a) == 1);    // press
        CHECK(ed.update(*a) == 0);    // still held: no repeat
        CHECK(ed.update(*b) == 0);    // release
        CHECK(ed.update(*c) == -1);
        uint8_t battery[] = {0x19, 0x08, 0x50}, idle[] = {0x15};
        CHECK(!zwift::parseButtons(battery, sizeof battery) && !zwift::parseButtons(idle, sizeof idle));
        // Ride: button map with the right shift-up bit (0x2000) cleared = held
        uint32_t map = 0xFFFFFFFFu & ~0x2000u;
        uint8_t ride[8] = {0x23, 0x08};
        size_t n = 2;
        for (uint32_t v = map; ; ) { uint8_t byte = v & 0x7F; v >>= 7; if (v) { ride[n++] = byte | 0x80; } else { ride[n++] = byte; break; } }
        auto r = zwift::parseButtons(ride, n);
        CHECK(r && r->up && !r->down);
        // Play: right pad (field 1 = 0), shift paddle (field 7 = 0) -> harder
        uint8_t play[] = {0x07, 0x08, 0x00, 0x38, 0x00};
        auto pl = zwift::parseButtons(play, sizeof play);
        CHECK(pl && pl->up && !pl->down);
        // Click v2 (Ride format, two buttons): right side = harder, left side = easier
        auto rideMsg = [](uint32_t clearedBits, uint8_t* out) {
            uint32_t v = 0xFFFFFFFFu & ~clearedBits;
            size_t k = 0;
            out[k++] = 0x23; out[k++] = 0x08;
            while (true) { uint8_t byte = v & 0x7F; v >>= 7; if (v) out[k++] = byte | 0x80; else { out[k++] = byte; break; } }
            return k;
        };
        uint8_t m1[10], m2[10], m3[10];
        size_t n1 = rideMsg(0x200, m1), n2 = rideMsg(0x4000, m2), n3 = rideMsg(0, m3);
        auto c1 = zwift::parseButtons(m1, n1, true), c2 = zwift::parseButtons(m2, n2, true), c3 = zwift::parseButtons(m3, n3, true);
        CHECK(c1 && c1->down && !c1->up);   // left shift-up bit on a Click: easier
        CHECK(c2 && c2->up && !c2->down);   // right shift-down bit on a Click: harder
        CHECK(c3 && !c3->up && !c3->down);  // nothing held
        CHECK(zwift::hex(m1, 3) == "23 08 FF");
        uint8_t garbage[] = {0x37, 0xFF};
        CHECK(!zwift::parseButtons(garbage, sizeof garbage));
    }
    if (failures == 0) std::printf("All protocol tests passed\n");
    return failures ? 1 : 0;
}
