#include "core/json_codec.hpp"

#include <nlohmann/json.hpp>

namespace hm {

using nlohmann::json;

namespace {

json attr_to_json(const AttrValue& v) {
  return std::visit([](const auto& x) { return json(x); }, v);
}

Result<Attrs> attrs_from(const json& j) {
  if (!j.is_object()) return make_error("bad_attrs", "attrs must be an object");
  if (j.size() > kMaxAttrs) return make_error("bad_attrs", "too many attrs");
  Attrs out;
  for (const auto& [k, v] : j.items()) {
    if (k.empty() || k.size() > 128) return make_error("bad_attrs", "attr key length");
    if (v.is_boolean()) {
      out.emplace(k, v.get<bool>());
    } else if (v.is_number_integer()) {
      out.emplace(k, v.get<std::int64_t>());
    } else if (v.is_number_float()) {
      out.emplace(k, v.get<double>());
    } else if (v.is_string()) {
      auto s = v.get<std::string>();
      if (s.size() > kMaxStringBytes) return make_error("bad_attrs", "attr string too long");
      out.emplace(k, std::move(s));
    } else {
      return make_error("bad_attrs", "attr values must be scalar");
    }
  }
  return out;
}

template <typename T>
bool read_field(const json& j, const char* key, T& out) {
  auto it = j.find(key);
  if (it == j.end()) return false;
  try {
    out = it->get<T>();
    return true;
  } catch (const json::exception&) {
    return false;
  }
}

}  // namespace

std::string attrs_to_json(const Attrs& attrs) {
  json j = json::object();
  for (const auto& [k, v] : attrs) j[k] = attr_to_json(v);
  return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

Result<Attrs> attrs_from_json(std::string_view text) {
  json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded()) return make_error("bad_json", "attrs is not valid JSON");
  return attrs_from(j);
}

json event_to_json(const Event& e) {
  json j;
  if (e.id != 0) j["id"] = e.id;
  j["v"] = kSchemaVersion;
  j["boot_id"] = e.boot_id;
  j["source"] = e.source;
  j["seq"] = e.seq;
  j["mono_ns"] = e.mono_ns;
  j["wall_ns"] = e.wall_ns;
  j["wall_quality"] = std::string(to_string(e.wall_quality));
  j["kind"] = e.kind;
  j["severity"] = std::string(to_string(e.severity));
  j["subject"] = e.subject;
  json a = json::object();
  for (const auto& [k, v] : e.attrs) a[k] = attr_to_json(v);
  j["attrs"] = std::move(a);
  return j;
}

std::string event_to_json_line(const Event& e) {
  return event_to_json(e).dump(-1, ' ', false, json::error_handler_t::replace);
}

Result<Event> event_from_json_line(std::string_view line) {
  if (line.size() > kMaxEventJsonBytes) return make_error("too_large", "event line exceeds 64 KiB");
  json j = json::parse(line, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return make_error("bad_json", "not a JSON object");

  int version = 0;
  if (!read_field(j, "v", version) || version < 1 || version > kSchemaVersion) {
    return make_error("bad_version", "missing or unsupported schema version");
  }
  Event e;
  std::string sev, wq;
  if (!read_field(j, "boot_id", e.boot_id) || e.boot_id.empty() || e.boot_id.size() > 64 ||
      !read_field(j, "source", e.source) || e.source.empty() || e.source.size() > 64 ||
      !read_field(j, "seq", e.seq) || !read_field(j, "mono_ns", e.mono_ns) ||
      !read_field(j, "kind", e.kind) || e.kind.empty() || e.kind.size() > 64 ||
      !read_field(j, "severity", sev)) {
    return make_error("bad_event", "missing or invalid required field");
  }
  if (e.mono_ns < 0) return make_error("bad_event", "mono_ns must be non-negative");
  auto s = severity_from_string(sev);
  if (!s) return make_error("bad_event", "unknown severity");
  e.severity = *s;
  read_field(j, "wall_ns", e.wall_ns);
  if (read_field(j, "wall_quality", wq)) {
    if (wq == "synced") e.wall_quality = WallQuality::synced;
    else if (wq == "unsynced") e.wall_quality = WallQuality::unsynced;
    else if (wq == "stepped") e.wall_quality = WallQuality::stepped;
  }
  read_field(j, "subject", e.subject);
  if (e.subject.size() > kMaxStringBytes) return make_error("bad_event", "subject too long");
  if (auto it = j.find("attrs"); it != j.end()) {
    auto a = attrs_from(*it);
    if (!a) return a.error();
    e.attrs = std::move(a).value();
  }
  return e;
}

}  // namespace hm
