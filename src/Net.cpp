#include "Net.h"

#include <array>
#include <cctype>
#include <cstring>
#include <ctime>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

#include "Db.h"
#include "core/Dates.h"
#include "core/Export.h"
#include "core/Json.h"
#include "core/Records.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace net {

static std::string shq(const std::string& s) {  // single-quote for the shell
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
}

std::string percentEncode(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
        else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}

std::string percentDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            o += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else o += s[i];
    }
    return o;
}

std::string queryParam(const std::string& q, const std::string& key) {
    size_t p = 0;
    while (p <= q.size()) {
        size_t amp = q.find('&', p);
        std::string kv = q.substr(p, amp == std::string::npos ? std::string::npos : amp - p);
        size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key) {
            std::string v = kv.substr(eq + 1);
            for (auto& c : v) if (c == '+') c = ' ';
            return percentDecode(v);
        }
        if (amp == std::string::npos) break;
        p = amp + 1;
    }
    return "";
}

Response send(const Request& r) {
    Response resp;
#ifdef _WIN32
    resp.error = "Networking needs curl (not supported on Windows builds yet)";
    return resp;
#else
    // Sensitive values go through a curl config file (not the command line, where `ps` could see them).
    fs::path cfg = fs::temp_directory_path() / ("coreray-curl-" + dates::newId() + ".cfg");
    {
        std::ofstream f(cfg);
        auto q = [](const std::string& s) {
            std::string o = "\"";
            for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
            return o + "\"";
        };
        f << "url = " << q(r.url) << "\n";
        f << "request = " << q(r.method) << "\n";
        for (auto& [k, v] : r.headers) f << "header = " << q(k + ": " + v) << "\n";
        for (auto& [k, v] : r.form) f << "data-urlencode = " << q(k + "=" + v) << "\n";
        for (auto& [k, v] : r.multipart) f << "form-string = " << q(k + "=" + v) << "\n";
        if (!r.multipartFilePath.empty())
            f << "form = " << q(r.multipartFileField + "=@" + r.multipartFilePath + ";filename=" + r.multipartFileName + ";type=application/octet-stream") << "\n";
        if (!r.jsonBody.empty()) {
            f << "header = \"Content-Type: application/json\"\n";
            f << "data-binary = " << q(r.jsonBody) << "\n";
        }
        f << "max-time = " << r.timeoutS << "\n";
        f << "silent\nshow-error\n";
        f << "user-agent = \"CoreRay/0.2\"\n";
    }
    std::string cmd = "curl -K " + shq(cfg.string()) + " -w '\\n%{http_code}' 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { resp.error = "Could not run curl"; fs::remove(cfg); return resp; }
    std::array<char, 8192> buf;
    std::string out;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), p)) > 0) out.append(buf.data(), n);
    int rc = pclose(p);
    std::error_code ec;
    fs::remove(cfg, ec);
    size_t nl = out.find_last_of('\n');
    if (nl != std::string::npos) {
        resp.status = std::atoi(out.c_str() + nl + 1);
        resp.body = out.substr(0, nl);
    } else resp.body = out;
    if (resp.status == 0) resp.error = rc != 0 ? "Network error: " + resp.body : "No response";
    return resp;
#endif
}

Response get(const std::string& url, int timeoutS) {
    Request r;
    r.url = url;
    r.timeoutS = timeoutS;
    return send(r);
}

void openBrowser(const std::string& url) {
#ifdef __APPLE__
    std::string cmd = "open " + shq(url) + " >/dev/null 2>&1 &";
#elif defined(_WIN32)
    std::string cmd = "start \"\" \"" + url + "\"";
#else
    std::string cmd = "xdg-open " + shq(url) + " >/dev/null 2>&1 &";
#endif
    int rc = std::system(cmd.c_str());
    (void)rc;
}

std::optional<std::string> awaitCallback(const std::string& prefix, int timeoutS, const std::function<void(int)>& onListening,
                                         const std::function<std::string(const std::string&)>& page, std::string& err) {
#ifdef _WIN32
    err = "OAuth callback listener not supported on Windows builds yet";
    return std::nullopt;
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { err = "Could not open a local port for the callback"; return std::nullopt; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 || listen(s, 4) < 0) { close(s); err = "Could not listen on localhost"; return std::nullopt; }
    socklen_t len = sizeof a;
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    int port = ntohs(a.sin_port);
    onListening(port);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutS);
    while (std::chrono::steady_clock::now() < deadline) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        timeval tv{1, 0};
        if (select(s + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
        int c = accept(s, nullptr, nullptr);
        if (c < 0) continue;
        char buf[8192];
        ssize_t n = recv(c, buf, sizeof buf - 1, 0);
        std::string req(buf, n > 0 ? size_t(n) : 0);
        size_t sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
        std::string path = sp1 != std::string::npos && sp2 != std::string::npos ? req.substr(sp1 + 1, sp2 - sp1 - 1) : "";
        if (path.rfind(prefix, 0) != 0) {
            const char* nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            ::send(c, nf, std::strlen(nf), 0);
            close(c);
            continue;
        }
        std::string q = path.find('?') != std::string::npos ? path.substr(path.find('?') + 1) : "";
        std::string body = page(q);
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + std::to_string(body.size()) +
                           "\r\nConnection: close\r\n\r\n" + body;
        ::send(c, resp.c_str(), resp.size(), 0);
        close(c);
        close(s);
        return q;
    }
    close(s);
    err = "Timed out waiting for the browser";
    return std::nullopt;
#endif
}

}  // namespace net

// ====================================================================== Strava
namespace strava {

static const char* OAUTH_BASE = "https://www.strava.com/oauth";
static const char* DEFAULT_API = "https://www.strava.com/api/v3";

static std::string apiBase(Db& db) {
    std::string b = db.meta("strava_api_base").value_or(DEFAULT_API);
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b;
}

struct Tokens { std::string access, refresh, athleteName; long long expiresAt = 0; unsigned long long athleteId = 0; };

static std::optional<Tokens> tokens(Db& db) {
    auto j = db.meta("strava_tokens");
    json::Value v;
    if (!j || !json::parse(*j, v) || v.type != json::Value::Object) return std::nullopt;
    Tokens t;
    t.access = v.strOr("access_token"); t.refresh = v.strOr("refresh_token"); t.expiresAt = (long long)v.numOr("expires_at", 0);
    t.athleteId = (unsigned long long)v.numOr("athlete_id", 0); t.athleteName = v.strOr("athlete_name");
    if (t.access.empty()) return std::nullopt;
    return t;
}

static void saveTokens(Db& db, const Tokens& t) {
    json::Value v = json::Value::object();
    v["access_token"] = t.access; v["refresh_token"] = t.refresh; v["expires_at"] = t.expiresAt; v["athlete_id"] = t.athleteId; v["athlete_name"] = t.athleteName;
    db.setMeta("strava_tokens", json::dump(v));
}

Status status(Db& db) {
    Status s;
    auto id = db.meta("strava_client_id");
    s.configured = id && db.meta("strava_client_secret");
    if (id) s.clientId = *id;
    if (auto t = tokens(db)) { s.connected = true; s.athleteName = t->athleteName; }
    s.autoUpload = db.meta("strava_auto_upload").value_or("") == "1";
    return s;
}

bool saveClient(Db& db, const std::string& idIn, const std::string& secretIn, std::string& err) {
    auto trim = [](std::string s) { while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back(); while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin()); return s; };
    std::string id = trim(idIn), secret = trim(secretIn);
    if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) { err = "The Client ID is the number shown on strava.com/settings/api"; return false; }
    if (secret.size() < 20) { err = "That Client Secret looks too short"; return false; }
    db.setMeta("strava_client_id", id);
    db.setMeta("strava_client_secret", secret);
    return true;
}

void setAuto(Db& db, bool on) { db.setMeta("strava_auto_upload", on ? "1" : "0"); }

static const char* DONE_PAGE =
    "<!doctype html><html><body style=\"font-family:-apple-system,sans-serif;background:#0b0d12;color:#eef2f8;display:grid;place-items:center;"
    "height:100vh;margin:0\"><div style=\"text-align:center\"><h1 style=\"color:#4ade80\">Connected to Strava</h1><p>You can close this tab and "
    "return to CoreRay.</p></div></body></html>";

static bool tokenFromResponse(const net::Response& r, Tokens& t, std::string& err, bool needAthlete) {
    json::Value v;
    if (!r.ok() || !json::parse(r.body, v)) { err = "Strava rejected the token request (" + std::to_string(r.status) + "): " + r.body + r.error; return false; }
    t.access = v.strOr("access_token"); t.refresh = v.strOr("refresh_token"); t.expiresAt = (long long)v.numOr("expires_at", 0);
    if (auto a = v.get("athlete"); a && a->type == json::Value::Object) {
        t.athleteId = (unsigned long long)a->numOr("id", 0);
        std::string name = a->strOr("firstname") + " " + a->strOr("lastname");
        while (!name.empty() && name.back() == ' ') name.pop_back();
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        t.athleteName = name;
    } else if (needAthlete) { err = "Strava returned no athlete"; return false; }
    return !t.access.empty();
}

bool connect(Db& db, std::string& err) {
    auto id = db.meta("strava_client_id"), secret = db.meta("strava_client_secret");
    if (!id || !secret) { err = "Add your Strava app's Client ID and Secret first"; return false; }
    std::string state = dates::newId();
    std::string pageErr;
    std::string code;
    auto q = net::awaitCallback(
        "/strava/callback", 300,
        [&](int port) {
            std::string redirect = "http://127.0.0.1:" + std::to_string(port) + "/strava/callback";
            net::openBrowser(std::string(OAUTH_BASE) + "/authorize?client_id=" + *id + "&response_type=code&redirect_uri=" + net::percentEncode(redirect) +
                             "&approval_prompt=auto&scope=read,activity:write&state=" + state);
        },
        [&](const std::string& query) -> std::string {
            std::string e = net::queryParam(query, "error");
            if (!e.empty()) { pageErr = "Strava said: " + e; return "<h1>Strava authorisation failed</h1><p>" + e + "</p>"; }
            if (net::queryParam(query, "state") != state) { pageErr = "OAuth state mismatch - try again"; return "<h1>Invalid state</h1>"; }
            code = net::queryParam(query, "code");
            if (code.empty()) { pageErr = "Strava returned no authorisation code"; return "<h1>Missing code</h1>"; }
            if (net::queryParam(query, "scope").find("activity:write") == std::string::npos) {
                pageErr = "CoreRay needs the \xE2\x80\x9CUpload your activities\xE2\x80\x9D permission";
                return "<h1>Uploads not allowed</h1><p>Please tick \xE2\x80\x9CUpload your activities\xE2\x80\x9D and try again.</p>";
            }
            return DONE_PAGE;
        },
        err);
    if (!q) return false;
    if (!pageErr.empty()) { err = pageErr; return false; }
    net::Request r;
    r.method = "POST";
    r.url = std::string(OAUTH_BASE) + "/token";
    r.form = {{"client_id", *id}, {"client_secret", *secret}, {"code", code}, {"grant_type", "authorization_code"}};
    Tokens t;
    if (!tokenFromResponse(net::send(r), t, err, true)) return false;
    saveTokens(db, t);
    return true;
}

static std::optional<std::string> freshToken(Db& db, std::string& err) {
    auto t = tokens(db);
    if (!t) { err = "Not connected to Strava"; return std::nullopt; }
    if (t->expiresAt - (long long)std::time(nullptr) > 120) return t->access;
    auto id = db.meta("strava_client_id"), secret = db.meta("strava_client_secret");
    if (!id || !secret) { err = "Strava app credentials missing"; return std::nullopt; }
    net::Request r;
    r.method = "POST";
    r.url = std::string(OAUTH_BASE) + "/token";
    r.form = {{"client_id", *id}, {"client_secret", *secret}, {"refresh_token", t->refresh}, {"grant_type", "refresh_token"}};
    Tokens n = *t;
    if (!tokenFromResponse(net::send(r), n, err, false)) { err = "Strava token refresh failed - reconnect in Profile"; return std::nullopt; }
    n.athleteId = t->athleteId;
    n.athleteName = t->athleteName;
    saveTokens(db, n);
    return n.access;
}

void disconnect(Db& db) {
    if (auto t = tokens(db)) {
        net::Request r;
        r.method = "POST";
        r.url = std::string(OAUTH_BASE) + "/revoke";
        r.form = {{"token", t->access}};
        net::send(r);
    }
    db.setMeta("strava_tokens", "");
}

std::optional<unsigned long long> duplicateId(const std::string& e) {
    size_t i = e.find("activities/");
    if (i == std::string::npos) return std::nullopt;
    i += 11;
    std::string digits;
    while (i < e.size() && std::isdigit((unsigned char)e[i])) digits += e[i++];
    if (digits.empty()) return std::nullopt;
    return std::stoull(digits);
}

std::optional<unsigned long long> upload(Db& db, const RideRecord& rec, std::string& err, const std::function<void(const std::string&)>& progress) {
    if (rec.summary.stravaActivityId) return rec.summary.stravaActivityId;
    auto token = freshToken(db, err);
    if (!token) return std::nullopt;
    if (progress) progress("uploading");
    const RideSummary& s = rec.summary;
    auto start = dates::parseRfc3339(s.startedAt).value_or(std::time(nullptr));
    auto fit = exporter::toFit(s, rec.samples, start, dates::utcOffsetSeconds(start));
    fs::path tmp = fs::temp_directory_path() / ("coreray-" + s.id + ".fit");
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(fit.data()), std::streamsize(fit.size()));
    }
    char desc[300];
    std::snprintf(desc, sizeof desc, "%s \xC2\xB7 %.0f W avg / %.0f W NP \xC2\xB7 %.0f kJ \xC2\xB7 TSS %.0f", rideKindName(s.kind), s.avgPower, s.np, s.workKj, s.tss);
    std::string d = desc;
    if (s.testResult) { char b[64]; std::snprintf(b, sizeof b, " \xC2\xB7 FTP estimate %.0f W", s.testResult->ftp); d += b; }
    if (!s.notes.empty()) d = s.notes + "\n\n" + d;
    d += "\nRecorded with CoreRay";
    std::string base = apiBase(db);
    net::Request r;
    r.method = "POST";
    r.url = base + "/uploads";
    r.headers = {{"Authorization", "Bearer " + *token}};
    r.multipart = {{"data_type", "fit"}, {"name", s.name}, {"description", d}, {"trainer", "1"}, {"external_id", "coreray-" + s.id + ".fit"}};
    r.multipartFileField = "file";
    r.multipartFilePath = tmp.string();
    r.multipartFileName = "coreray-" + s.id + ".fit";
    auto resp = net::send(r);
    std::error_code ec;
    fs::remove(tmp, ec);
    if (!resp.ok()) {
        if (resp.status == 429) err = "Strava rate limit reached - try again in 15 minutes";
        else if (resp.status == 401 || resp.status == 403) err = "Strava refused the upload (" + std::to_string(resp.status) + "). Your API app may need a Strava subscription, or reconnect.";
        else err = "Strava upload failed (" + std::to_string(resp.status) + "): " + resp.body + resp.error;
        return std::nullopt;
    }
    json::Value st;
    if (!json::parse(resp.body, st)) { err = "Unexpected Strava response: " + resp.body; return std::nullopt; }
    if (progress) progress("processing");
    std::optional<unsigned long long> activity;
    for (int i = 0; i < 90 && !activity; ++i) {
        if (auto a = st.num("activity_id")) { activity = (unsigned long long)*a; break; }
        std::string e = st.strOr("error");
        if (!e.empty()) {
            if (auto dup = duplicateId(e)) { activity = dup; break; }
            err = "Strava couldn't process the file: " + e;
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        net::Request g;
        g.url = base + "/uploads/" + std::to_string((unsigned long long)st.numOr("id", 0));
        g.headers = {{"Authorization", "Bearer " + *token}};
        auto gr = net::send(g);
        if (!json::parse(gr.body, st)) break;
    }
    if (!activity) { err = "Strava is still processing - check your Strava feed shortly"; return std::nullopt; }
    // make sure it's filed as a Virtual Ride on the trainer
    net::Request put;
    put.method = "PUT";
    put.url = base + "/activities/" + std::to_string(*activity);
    put.headers = {{"Authorization", "Bearer " + *token}};
    put.jsonBody = R"({"sport_type":"VirtualRide","trainer":true})";
    net::send(put);
    return activity;
}

}  // namespace strava

// ====================================================================== live weather
namespace liveweather {

std::optional<weather::Live> fetch(double lat, double lon, const std::string& place, std::string& err) {
    auto r = net::get(weather::forecastUrl(lat, lon));
    if (!r.ok()) { err = "Weather lookup failed: " + (r.error.empty() ? std::to_string(r.status) : r.error); return std::nullopt; }
    auto l = weather::parseCurrent(r.body, place);
    if (!l) err = "Unexpected weather response";
    return l;
}

std::vector<weather::Place> search(const std::string& q, std::string& err) {
    auto r = net::get(weather::geocodeUrl(q));
    if (!r.ok()) { err = "Place search failed"; return {}; }
    return weather::parsePlaces(r.body);
}

}  // namespace liveweather
