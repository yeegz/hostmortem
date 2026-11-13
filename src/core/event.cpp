#include "core/event.hpp"

namespace hm {

namespace {
template <typename T>
const T* get_if_attr(const Attrs& attrs, std::string_view key) {
  auto it = attrs.find(key);
  if (it == attrs.end()) return nullptr;
  return std::get_if<T>(&it->second);
}
}  // namespace

std::optional<std::int64_t> Event::attr_int(std::string_view key) const {
  if (const auto* v = get_if_attr<std::int64_t>(attrs, key)) return *v;
  return std::nullopt;
}

std::optional<double> Event::attr_num(std::string_view key) const {
  if (const auto* v = get_if_attr<double>(attrs, key)) return *v;
  if (const auto* v = get_if_attr<std::int64_t>(attrs, key)) return static_cast<double>(*v);
  return std::nullopt;
}

std::optional<bool> Event::attr_bool(std::string_view key) const {
  if (const auto* v = get_if_attr<bool>(attrs, key)) return *v;
  return std::nullopt;
}

std::optional<std::string> Event::attr_str(std::string_view key) const {
  if (const auto* v = get_if_attr<std::string>(attrs, key)) return *v;
  return std::nullopt;
}

std::string_view to_string(Severity s) noexcept {
  switch (s) {
    case Severity::debug: return "debug";
    case Severity::info: return "info";
    case Severity::notice: return "notice";
    case Severity::warning: return "warning";
    case Severity::error: return "error";
    case Severity::critical: return "critical";
  }
  return "info";
}

std::optional<Severity> severity_from_string(std::string_view s) noexcept {
  if (s == "debug") return Severity::debug;
  if (s == "info") return Severity::info;
  if (s == "notice") return Severity::notice;
  if (s == "warning") return Severity::warning;
  if (s == "error") return Severity::error;
  if (s == "critical") return Severity::critical;
  return std::nullopt;
}

std::string_view to_string(WallQuality q) noexcept {
  switch (q) {
    case WallQuality::unknown: return "unknown";
    case WallQuality::unsynced: return "unsynced";
    case WallQuality::synced: return "synced";
    case WallQuality::stepped: return "stepped";
  }
  return "unknown";
}

}  // namespace hm
