#pragma once

// Warmup money: Ready Up emulates warmup (CS2's own warmup would cover the ready HUD), so the
// money CS2's warmup refills has to be topped up by hand. In scrim and match warmup, each human
// player's account (CCSPlayerController m_pInGameMoneyServices -> m_iAccount) goes back to
// `kWarmupMoney` right after a purchase and once a second. readyup.cfg / match.cfg
// `warmup_money=0` turns it off. Log lines: `warmup-money: ...` (debug only).

#include <string>

struct ru_api;  // readyup/plugin_api.h

namespace readyup {

// What warmup.cfg / the scrim warmup rules set as mp_maxmoney.
constexpr int kWarmupMoney = 16000;

// Pure (ctest `match_rules`): top up in this ru mode string with this setting?
inline bool WarmupMoneyActive(const char* ruMode, bool enabled) {
  if (!enabled || !ruMode) return false;
  const std::string m = ruMode;
  return m == "scrim_warmup" || m == "match_warmup";
}

// readyup_plugin_load: item_purchase.
void WarmupMoneyInstall(const ru_api* api);
// on_tick.
void WarmupMoneyTick(double now);

}  // namespace readyup
