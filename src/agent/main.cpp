// hmd: the Hostmortem recording agent.
#include <cstring>
#include <iostream>
#include <string>

#include "agent/agent.hpp"
#include "core/log.hpp"

int main(int argc, char** argv) {
  hm::AgentOptions opts;
  std::string config_path = "/etc/hostmortem/hmd.conf";
  bool config_given = false;
  std::string db_override;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "hmd: " << a << " needs a value\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--config") { config_path = next(); config_given = true; }
    else if (a == "--db") db_override = next();
    else if (a == "--run-for") {
      auto d = hm::parse_duration(next());
      if (!d) { std::cerr << "hmd: bad --run-for\n"; return 2; }
      opts.run_for = *d;
    }
    else if (a == "--no-kmsg") opts.enable_kmsg = false;
    else if (a == "--debug") hm::log::set_level(hm::log::Level::debug);
    else if (a == "--version") { std::cout << "hmd " HM_VERSION "\n"; return 0; }
    else if (a == "-h" || a == "--help") {
      std::cout << "usage: hmd [--config FILE] [--db FILE] [--run-for DURATION] [--no-kmsg] [--debug]\n";
      return 0;
    } else {
      std::cerr << "hmd: unknown argument " << a << "\n";
      return 2;
    }
  }
  auto cfg = hm::load_config(config_path);
  if (!cfg) {
    if (config_given || cfg.error().code != "config_unreadable") {
      std::cerr << "hmd: " << cfg.error().message << "\n";
      return 2;
    }
    cfg = hm::Config{};  // no config file: run with documented defaults
    hm::log::info("agent", "no config at " + config_path + ", using defaults");
  }
  opts.config = std::move(cfg).value();
  if (!db_override.empty()) opts.config.db_path = db_override;
  return hm::run_agent(opts);
}
