#pragma once
// Ride export: FIT (Virtual Ride, port of CoreKick fit.rs), TCX and CSV.
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "Engine.h"

namespace exporter {

uint16_t crc16(const uint8_t* d, size_t n);
std::vector<uint8_t> toFit(const RideSummary& s, const Samples& sm, std::time_t startUtc, int utcOffsetS);
std::string toTcx(const RideSummary& s, const Samples& sm, std::time_t startUtc, const std::string& notes);
std::string toCsv(const Samples& sm);

// Writes CoreRay_<date>.fit/.tcx/.csv into the rides folder; returns the folder path
std::string saveRide(const RideSummary& s, const Samples& sm, std::time_t startUtc, const std::string& name, std::string* baseOut = nullptr);
std::string ridesDir();

// basic FIT validation (header/data CRC, sizes) used by the tests
bool validateFit(const std::vector<uint8_t>& b, std::string& err, size_t* records = nullptr);

}  // namespace exporter
