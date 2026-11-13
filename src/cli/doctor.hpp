#pragma once
#include <string>

namespace hm {
// Prints host capability checks and, if the store exists, what the agent
// last reported about each collector. Returns an exit code.
int run_doctor(const std::string& db_path, bool json);
}
