#include "detector/detector.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace sensor_monitor::detector {

std::string toString(Severity s) {
  switch (s) {
    case Severity::kOk:
      return "OK";
    case Severity::kWarning:
      return "WARNING";
    case Severity::kCritical:
      return "CRITICAL";
  }
  return "UNKNOWN";
}

void DetectorConfig::validate() const {
  if (window_size == 0) {
    throw std::invalid_argument("DetectorConfig: window_size must be > 0");
  }
  if (min_samples == 0) {
    throw std::invalid_argument("DetectorConfig: min_samples must be > 0");
  }
  if (anomaly_threshold <= 0.0) {
    throw std::invalid_argument(
        "DetectorConfig: anomaly_threshold must be > 0");
  }
  if (warning_threshold <= 0.0 || critical_threshold <= 0.0) {
    throw std::invalid_argument(
        "DetectorConfig: severity thresholds must be > 0");
  }
  if (critical_threshold < warning_threshold) {
    throw std::invalid_argument(
        "DetectorConfig: critical_threshold must be >= warning_threshold");
  }
}

namespace {

void applyKey(DetectorConfig& cfg, const std::string& key,
              const std::string& value) {
  if (key == "window_size") {
    cfg.window_size = static_cast<std::size_t>(std::stoul(value));
  } else if (key == "anomaly_threshold") {
    cfg.anomaly_threshold = std::stod(value);
  } else if (key == "warning_threshold") {
    cfg.warning_threshold = std::stod(value);
  } else if (key == "critical_threshold") {
    cfg.critical_threshold = std::stod(value);
  } else if (key == "min_samples") {
    cfg.min_samples = static_cast<std::size_t>(std::stoul(value));
  }
  // Unknown keys are ignored for forward compatibility.
}

std::string trim(const std::string& s) {
  const char* ws = " \t\r\n";
  const auto begin = s.find_first_not_of(ws);
  if (begin == std::string::npos) return "";
  const auto end = s.find_last_not_of(ws);
  return s.substr(begin, end - begin + 1);
}

}  // namespace

DetectorConfig DetectorConfig::loadFromFile(const std::string& path) {
  DetectorConfig cfg;
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("DetectorConfig: cannot open file: " + path);
  }
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    applyKey(cfg, trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
  }
  cfg.validate();
  return cfg;
}

}  // namespace sensor_monitor::detector
