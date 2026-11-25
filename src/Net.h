#pragma once
// Networking: HTTPS via the system `curl` (always present on macOS), a one-shot
// localhost listener for OAuth callbacks, Strava (port of CoreKick strava.rs) and
// live weather from Open-Meteo. Everything here blocks: call it from a worker thread.
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/Weather.h"

namespace net {

struct Response {
    int status = 0;  // 0 = transport failure
    std::string body, error;
    bool ok() const { return status >= 200 && status < 300; }
};

struct Request {
    std::string method = "GET", url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::pair<std::string, std::string>> form;       // application/x-www-form-urlencoded
    std::vector<std::pair<std::string, std::string>> multipart;  // text fields
    std::string multipartFileField, multipartFilePath, multipartFileName;
    std::string jsonBody;
    int timeoutS = 60;
};

Response send(const Request& r);
Response get(const std::string& url, int timeoutS = 20);

std::string queryParam(const std::string& query, const std::string& key);
std::string percentDecode(const std::string& s);
std::string percentEncode(const std::string& s);

// Waits (up to timeoutS) for one HTTP request to 127.0.0.1:<port>/<pathPrefix>... Returns the query string.
// `onListening(port)` is called once the socket is ready (open the browser there).
std::optional<std::string> awaitCallback(const std::string& pathPrefix, int timeoutS, const std::function<void(int)>& onListening,
                                         const std::function<std::string(const std::string& query)>& page, std::string& err);
void openBrowser(const std::string& url);

}  // namespace net

class Db;
struct RideRecord;

namespace strava {
struct Status {
    bool configured = false, connected = false, autoUpload = false;
    std::string clientId, athleteName;
};
Status status(Db& db);
bool saveClient(Db& db, const std::string& id, const std::string& secret, std::string& err);
void setAuto(Db& db, bool on);
bool connect(Db& db, std::string& err);  // runs the browser OAuth flow (blocking, up to 5 minutes)
void disconnect(Db& db);
// Uploads the ride as a FIT Virtual Ride; returns the activity id. `progress` gets "uploading"/"processing".
std::optional<unsigned long long> upload(Db& db, const RideRecord& rec, std::string& err, const std::function<void(const std::string&)>& progress = {});
std::optional<unsigned long long> duplicateId(const std::string& err);
}  // namespace strava

namespace liveweather {
std::optional<weather::Live> fetch(double lat, double lon, const std::string& place, std::string& err);
std::vector<weather::Place> search(const std::string& query, std::string& err);
}  // namespace liveweather
