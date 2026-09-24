#include "readyup/ccommand.h"

#include "readyup/engine_surface.h"

#include <cstdint>

namespace readyup {
namespace {

constexpr int kMaxArgc = 64;         // COMMAND_MAX_ARGC (the m_Args fixed-growable capacity)
constexpr size_t kMaxArgLen = 512;   // COMMAND_MAX_LENGTH

}  // namespace

bool CCommandLayoutVerified() {
  return VerifiedLayoutField("CCommand", "argc").has_value() && VerifiedLayoutField("CCommand", "argv").has_value();
}

std::optional<std::vector<std::string>> ReadCCommandArgs(const void* cmd) {
  if (!cmd) return std::nullopt;
  const auto offArgc = VerifiedLayoutField("CCommand", "argc");
  const auto offArgv = VerifiedLayoutField("CCommand", "argv");
  if (!offArgc || !offArgv) return std::nullopt;

  const uintptr_t base = reinterpret_cast<uintptr_t>(cmd);
  int32_t argc = -1;
  uintptr_t argv = 0;
  if (!SafeReadMemory(base + static_cast<uintptr_t>(*offArgc), &argc, sizeof(argc))) return std::nullopt;
  if (argc < 0 || argc > kMaxArgc) return std::nullopt;
  std::vector<std::string> out;
  if (argc == 0) return out;
  if (!SafeReadMemory(base + static_cast<uintptr_t>(*offArgv), &argv, sizeof(argv)) || !argv) return std::nullopt;
  out.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    uintptr_t p = 0;
    if (!SafeReadMemory(argv + static_cast<uintptr_t>(i) * sizeof(uintptr_t), &p, sizeof(p)) || !p) return std::nullopt;
    auto s = SafeReadCString(p, kMaxArgLen);
    if (!s) return std::nullopt;
    out.push_back(std::move(*s));
  }
  return out;
}

std::string CCommandArgString(const std::vector<std::string>& args) {
  std::string out;
  for (size_t i = 1; i < args.size(); ++i) {
    if (i > 1) out.push_back(' ');
    out += args[i];
  }
  return out;
}

}  // namespace readyup
