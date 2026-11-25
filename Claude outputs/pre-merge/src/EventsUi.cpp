// Events: recurring scheduled rides (route / workout / test) with a calendar and streaks.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "App.h"
#include "raylib.h"

using namespace ui;

static const Color CARD{18, 21, 31, 236};
static const Color CARD2{26, 30, 42, 225};
static const char* COLORS[] = {"#1DB870", "#FF5C38", "#3BA3FF", "#F5B83D", "#B36BFF", "#FF4F8B"};

static Color hexColor(const std::string& h) {
    if (h.size() != 7) return Color{29, 184, 112, 255};
    unsigned v = std::strtoul(h.c_str() + 1, nullptr, 16);
    return Color{(unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v, 255};
}

// dates on which a ride was done for this event
static std::set<long> doneDays(const std::vector<RideSummary>& rides, const std::string& eventId) {
    std::set<long> s;
    for (auto& r : rides)
        if (r.eventId && *r.eventId == eventId)
            if (auto d = dates::localDate(r.startedAt)) s.insert(d->days());
    return s;
}

static std::string targetLabel(const App* app, const Event& e, const std::vector<Route>& routes, const std::vector<const wk::Workout*>& wks) {
    (void)app;
    switch (e.target.type) {
        case EventTarget::Route:
            for (auto& r : routes) if (r.id == e.target.routeId) return r.name + (e.target.laps > 1 ? " \xC3\x97" + std::to_string(e.target.laps) : "");
            return "Route";
        case EventTarget::Workout:
            for (auto* w : wks) if (w->id == e.target.workoutId) return w->name;
            return "Workout";
        default:
            for (auto& t : wk::testCatalogue()) if (t.kind == e.target.test) return t.name;
            return "FTP test";
    }
}

void App::drawEventsPage(Rectangle r) {
    float lw = std::min(560.f, r.width * 0.42f);
    Rectangle L{r.x, r.y, lw, r.height};
    panel(L, CARD);
    float x = L.x + 18, y = L.y + 14, w = L.width - 36;
    text("Events", x, y, F::Title, TEXT);
    if (button({L.x + L.width - 150, y + 2, 132, 34}, "+ New event")) {
        evDraft_ = Event{};
        evDraft_.id.clear();
        evDraft_.name = "Tuesday threshold";
        evDraft_.recurrence.freq = Freq::Weekly;
        evDraft_.recurrence.startDate = Date::today().str();
        evDraft_.recurrence.weekdays = {Date::today().weekday()};
        evDraft_.target.type = EventTarget::Workout;
        evDraft_.target.workoutId = "builtin-threshold-3x10";
        evDraft_.target.routeId = selRoute_;
        evName_.text = evDraft_.name;
        evTime_.text = evDraft_.recurrence.time;
        eventEditor_ = true;
    }
    y += 52;
    Date today = Date::today();
    auto wks = allWorkouts();
    Rectangle lr{x, y, w, L.y + L.height - y - 12};
    float rowH = 92;
    scrollUpdate(lr, evScroll_, events_.size() * (rowH + 8));
    BeginScissorMode(int(lr.x), int(lr.y), int(lr.width), int(lr.height));
    char b[200];
    if (events_.empty()) textWrapped("Schedule rides that repeat - a weekly threshold session, a Saturday long ride, a monthly FTP test. "
                                     "They show up on Home on the day, ready to start, and CoreRay keeps your streak.", {x, y, w, 100}, F::Body, DIM, 16);
    for (size_t i = 0; i < events_.size(); ++i) {
        const Event& e = events_[i];
        Rectangle row{lr.x, lr.y + i * (rowH + 8) - evScroll_, lr.width - 8, rowH};
        if (row.y + rowH < lr.y || row.y > lr.y + lr.height) continue;
        bool h = hovered(row) && CheckCollisionPointRec(GetMousePosition(), lr);
        bool sel = eventEditor_ && evDraft_.id == e.id;
        DrawRectangleRounded(row, 0.15f, 6, sel ? Color{255, 92, 56, 45} : h ? Color{40, 46, 64, 230} : CARD2);
        DrawRectangleRounded({row.x + 8, row.y + 12, 5, rowH - 24}, 1, 4, e.active ? hexColor(e.color) : FAINT);
        text(e.name, row.x + 24, row.y + 8, F::Body, e.active ? TEXT : DIM, 18);
        text(targetLabel(this, e, routes_, wks), row.x + 24, row.y + 34, F::Small, DIM, 14);
        text(recurrenceLabel(e.recurrence) + "  \xC2\xB7  " + e.recurrence.time, row.x + 24, row.y + 56, F::Small, FAINT, 13);
        // streak: consecutive past occurrences that were ridden
        auto done = doneDays(rides_, e.id);
        auto occ = occurrences(e, today.plus(-120), today.plus(-1));
        int streak = 0;
        for (auto it = occ.rbegin(); it != occ.rend(); ++it) { if (done.count(it->days())) ++streak; else break; }
        if (occursOn(e, today) && done.count(today.days())) ++streak;
        int last8 = 0, n8 = 0;
        for (auto& d : occ) if (today - d <= 56) { ++n8; if (done.count(d.days())) ++last8; }
        if (streak > 0) {
            std::snprintf(b, sizeof b, "\xF0\x9F\x94\xA5 %d", streak);
            std::snprintf(b, sizeof b, "%d in a row", streak);
            textRight(b, row.x + row.width - 14, row.y + 10, F::Small, ACCENT, 14);
        }
        if (n8) {
            std::snprintf(b, sizeof b, "%d / %d in 8 weeks", last8, n8);
            textRight(b, row.x + row.width - 14, row.y + 32, F::Small, DIM, 13);
        }
        auto next = occurrences(e, today, today.plus(60));
        if (!next.empty()) {
            const Date& nd = next[0];
            std::snprintf(b, sizeof b, "Next: %s", nd == today ? "today" : nd == today.plus(1) ? "tomorrow" : (std::string(dates::weekdayShort(nd.weekday())) + " " + fmtDateShort(nd.y, nd.m, nd.d)).c_str());
            textRight(b, row.x + row.width - 14, row.y + 56, F::Small, nd == today ? GOOD : FAINT, 13);
        }
        if (h && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            evDraft_ = e;
            evName_.text = e.name;
            evTime_.text = e.recurrence.time;
            eventEditor_ = true;
            eventDeleteArm_.clear();
        }
    }
    EndScissorMode();
    scrollBar(lr, evScroll_, events_.size() * (rowH + 8));

    Rectangle R{L.x + L.width + 20, r.y, r.width - L.width - 20, r.height};
    if (eventEditor_) { drawEventEditor(R); return; }

    // ---- calendar
    panel(R, CARD);
    x = R.x + 22; y = R.y + 18; w = R.width - 44;
    Date first{today.y, today.m, 1};
    int mm = first.m - 1 + calMonthOffset_;
    int yy = first.y + (mm >= 0 ? mm / 12 : (mm - 11) / 12);
    mm = ((mm % 12) + 12) % 12;
    first = Date{yy, mm + 1, 1};
    std::snprintf(b, sizeof b, "%s %d", dates::monthName(first.m), first.y);
    text(b, x, y, F::Title, TEXT);
    if (button({R.x + R.width - 178, y, 44, 36}, "<")) --calMonthOffset_;
    if (button({R.x + R.width - 128, y, 60, 36}, "Today")) calMonthOffset_ = 0;
    if (button({R.x + R.width - 62, y, 44, 36}, ">")) ++calMonthOffset_;
    y += 54;
    float cw = w / 7;
    for (int i = 0; i < 7; ++i) textCentered(dates::weekdayShort(i), {x + i * cw, y, cw, 20}, F::Small, DIM, 13);
    y += 26;
    Date start = first.monday();
    float ch = std::min(110.f, (R.y + R.height - y - 20) / 6);
    // rides by day
    std::map<long, int> rideCount;
    for (auto& rd : rides_) if (auto d = dates::localDate(rd.startedAt)) rideCount[d->days()]++;
    for (int k = 0; k < 42; ++k) {
        Date d = start.plus(k);
        Rectangle c{x + (k % 7) * cw + 2, y + (k / 7) * ch + 2, cw - 4, ch - 4};
        bool inMonth = d.m == first.m;
        bool isToday = d == today;
        DrawRectangleRounded(c, 0.12f, 6, isToday ? Color{255, 92, 56, 40} : inMonth ? CARD2 : Color{20, 23, 32, 160});
        text(std::to_string(d.d), c.x + 8, c.y + 5, F::Small, inMonth ? (isToday ? ACCENT : TEXT) : FAINT, 13);
        auto rc = rideCount.find(d.days());
        if (rc != rideCount.end()) {
            std::snprintf(b, sizeof b, "%d ride%s", rc->second, rc->second > 1 ? "s" : "");
            textRight(b, c.x + c.width - 6, c.y + 6, F::Small, SKY, 11);
        }
        float ey = c.y + 26;
        for (auto& e : events_) {
            if (!occursOn(e, d) || ey > c.y + c.height - 16) continue;
            bool done = doneDays(rides_, e.id).count(d.days()) > 0;
            Color ec = hexColor(e.color);
            DrawRectangleRounded({c.x + 5, ey, c.width - 10, 17}, 0.4f, 4, Color{ec.r, ec.g, ec.b, (unsigned char)(done ? 200 : d < today ? 50 : 110)});
            std::string nm = (done ? "\xE2\x9C\x93 " : "") + e.name;
            while (nm.size() > 3 && measure(nm, F::Small, 11).x > c.width - 16) nm.pop_back();
            text(nm, c.x + 9, ey + 2, F::Small, WHITE, 11);
            ey += 20;
        }
    }
}

void App::drawEventEditor(Rectangle R) {
    panel(R, CARD);
    float x = R.x + 22, y = R.y + 18, w = R.width - 44;
    bool isNew = evDraft_.id.empty();
    text(isNew ? "New event" : "Edit event", x, y, F::Title, TEXT);
    if (button({R.x + R.width - 140, y, 120, 36}, "Close")) eventEditor_ = false;
    y += 54;
    float half = (w - 20) / 2;
    text("Name", x, y, F::Small, DIM);
    textBox({x, y + 20, half, 38}, evName_, false);
    text("Time (HH:MM)", x + half + 20, y, F::Small, DIM);
    textBox({x + half + 20, y + 20, 140, 38}, evTime_, false);
    float cx = x + half + 180;
    for (auto* c : COLORS) {
        Color cc = hexColor(c);
        Rectangle sw{cx, y + 26, 26, 26};
        DrawRectangleRounded(sw, 1, 8, cc);
        if (evDraft_.color == c) DrawRectangleRoundedLinesEx({sw.x - 3, sw.y - 3, 32, 32}, 1, 8, 2, WHITE);
        if (clicked(sw)) evDraft_.color = c;
        cx += 36;
    }
    y += 74;
    text("What to ride", x, y, F::Small, DIM);
    y += 20;
    int tt = segmented({x, y, half, 34}, {"Route", "Workout", "FTP test"}, int(evDraft_.target.type));
    if (tt >= 0) evDraft_.target.type = EventTarget::Type(tt);
    y += 44;
    int d;
    auto wks = allWorkouts();
    if (evDraft_.target.type == EventTarget::Route && !routes_.empty()) {
        size_t idx = 0;
        for (size_t i = 0; i < routes_.size(); ++i) if (routes_[i].id == evDraft_.target.routeId) idx = i;
        if (stepper({x, y, w, 34}, "Route", routes_[idx].name, &d, 320)) evDraft_.target.routeId = routes_[(idx + routes_.size() + d) % routes_.size()].id;
        else evDraft_.target.routeId = routes_[idx].id;
        y += 42;
        if (routes_[idx].isLoop) {
            if (stepper({x, y, half, 34}, "Laps", std::to_string(evDraft_.target.laps), &d)) evDraft_.target.laps = std::clamp(evDraft_.target.laps + d, 1, 20);
            y += 42;
        }
    } else if (evDraft_.target.type == EventTarget::Workout) {
        size_t idx = 0;
        for (size_t i = 0; i < wks.size(); ++i) if (wks[i]->id == evDraft_.target.workoutId) idx = i;
        if (stepper({x, y, w, 34}, "Workout", wks[idx]->name, &d, 320)) evDraft_.target.workoutId = wks[(idx + wks.size() + d) % wks.size()]->id;
        else evDraft_.target.workoutId = wks[idx]->id;
        y += 42;
        workoutChart({x, y, w, 56}, wks[idx]->steps(), prof_.ftp, 1.0, -1, nullptr);
        y += 64;
    } else {
        auto cat = wk::testCatalogue();
        size_t idx = 0;
        for (size_t i = 0; i < cat.size(); ++i) if (cat[i].kind == evDraft_.target.test) idx = i;
        if (stepper({x, y, w, 34}, "Test", cat[idx].name, &d, 320)) evDraft_.target.test = cat[(idx + cat.size() + d) % cat.size()].kind;
        y += 42;
    }
    y += 6;
    text("Repeats", x, y, F::Small, DIM);
    y += 20;
    int fr = segmented({x, y, w, 34}, {"Once", "Daily", "Weekly", "Every N days"}, int(evDraft_.recurrence.freq));
    if (fr >= 0) evDraft_.recurrence.freq = Freq(fr);
    y += 44;
    Recurrence& rc = evDraft_.recurrence;
    if (rc.freq == Freq::Weekly) {
        float bw = (w - 6 * 8) / 7;
        for (int i = 0; i < 7; ++i) {
            bool on = std::find(rc.weekdays.begin(), rc.weekdays.end(), i) != rc.weekdays.end();
            if (chipButton({x + i * (bw + 8), y, bw, 32}, dates::weekdayShort(i), on)) {
                if (on) rc.weekdays.erase(std::remove(rc.weekdays.begin(), rc.weekdays.end(), i), rc.weekdays.end());
                else { rc.weekdays.push_back(i); std::sort(rc.weekdays.begin(), rc.weekdays.end()); }
            }
        }
        y += 42;
        if (stepper({x, y, half, 34}, "Every", rc.interval == 1 ? "week" : std::to_string(rc.interval) + " weeks", &d)) rc.interval = std::clamp(rc.interval + d, 1, 8);
        y += 42;
    } else if (rc.freq == Freq::EveryNDays) {
        if (stepper({x, y, half, 34}, "Every", std::to_string(rc.interval) + " days", &d)) rc.interval = std::clamp(rc.interval + d, 2, 60);
        y += 42;
    }
    Date sd = Date::parse(rc.startDate).value_or(Date::today());
    if (stepper({x, y, half, 34}, rc.freq == Freq::Once ? "Date" : "Starts", sd.str(), &d, 130)) rc.startDate = sd.plus(d).str();
    if (rc.freq != Freq::Once) {
        std::string ev = rc.endDate ? *rc.endDate : "never";
        if (stepper({x + half + 20, y, half, 34}, "Ends", ev, &d, 130)) {
            if (!rc.endDate && d > 0) rc.endDate = sd.plus(28).str();
            else if (rc.endDate) {
                Date ed = Date::parse(*rc.endDate).value_or(sd).plus(d * 7);
                if (ed < sd) rc.endDate.reset(); else rc.endDate = ed.str();
            }
        }
    }
    y += 46;
    if (toggle({x, y, half, 34}, "Active", evDraft_.active)) evDraft_.active = !evDraft_.active;
    y += 50;
    // preview of the next dates
    auto next = occurrences(evDraft_, Date::today(), Date::today().plus(42));
    std::string nx = "Next: ";
    for (size_t i = 0; i < next.size() && i < 6; ++i) nx += (i ? ", " : "") + std::string(dates::weekdayShort(next[i].weekday())) + " " + fmtDateShort(next[i].y, next[i].m, next[i].d);
    if (next.empty()) nx = "No upcoming dates";
    text(nx, x, y, F::Small, DIM, 14);

    // validate time
    bool timeOk = evTime_.text.size() == 5 && evTime_.text[2] == ':' && std::isdigit((unsigned char)evTime_.text[0]) && std::atoi(evTime_.text.substr(0, 2).c_str()) < 24 &&
                  std::atoi(evTime_.text.substr(3, 2).c_str()) < 60;
    bool ok = !evName_.text.empty() && timeOk && (rc.freq != Freq::Weekly || !rc.weekdays.empty());
    float by = R.y + R.height - 66;
    if (button({x, by, 180, 48}, "Save", true, ok)) {
        evDraft_.name = evName_.text;
        rc.time = evTime_.text;
        if (evDraft_.id.empty()) { evDraft_.id = dates::newId(); evDraft_.createdAt = dates::nowRfc3339(); }
        db_.saveEvent(evDraft_);
        events_ = db_.events();
        eventEditor_ = false;
        toast("Event saved", GOOD);
    }
    if (!timeOk) text("Time must look like 07:30", x + 196, by + 14, F::Small, BAD, 14);
    if (!isNew) {
        bool armed = eventDeleteArm_ == evDraft_.id;
        if (button({x + w - 160, by, 160, 48}, armed ? "Really delete?" : "Delete")) {
            if (armed) {
                db_.deleteEvent(evDraft_.id);
                events_ = db_.events();
                eventEditor_ = false;
                toast("Event deleted");
            } else eventDeleteArm_ = evDraft_.id;
        }
        if (occursOn(evDraft_, Date::today()) && button({x + 196, by, 180, 48}, "Start now", false)) startEvent(evDraft_);
    }
}
