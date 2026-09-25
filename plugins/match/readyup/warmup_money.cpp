// Warmup money (see warmup_money.h).
#include "readyup/warmup_money.h"

#include "readyup/config.h"
#include "readyup/host.h"
#include "readyup/logging.h"
#include "readyup/modes.h"
#include "readyup/plugin_api.h"

#include <cstring>

namespace readyup {
namespace {

int g_offMoneyServices = -2;  // CCSPlayerController::m_pInGameMoneyServices (-2 = not looked up)
int g_offAccount = -2;        // CCSPlayerController_InGameMoneyServices::m_iAccount
double g_nextTick = 0;
bool g_purchaseSeen = false;

bool Resolve(const ru_api* a) {
  if (g_offMoneyServices == -2 || g_offAccount == -2) {
    if (a->entity_system_status(a->self) != RU_ENTSYS_OK) return false;
    g_offMoneyServices = a->schema_offset(a->self, "CCSPlayerController", "m_pInGameMoneyServices");
    g_offAccount = a->schema_offset(a->self, "CCSPlayerController_InGameMoneyServices", "m_iAccount");
    if (g_offMoneyServices < 0 || g_offAccount < 0) {
      Print("warmup-money: schema fields missing (services=%d account=%d); warmup money is not topped up\n",
            g_offMoneyServices, g_offAccount);
    }
  }
  return g_offMoneyServices >= 0 && g_offAccount >= 0;
}

struct TopUp {
  const ru_api* a;
  int changed;
};

int TopUpPlayer(void* user, const ru_player* p) {
  auto* t = static_cast<TopUp*>(user);
  if (p->is_bot || p->slot < 0 || !p->connected) return 1;
  void* ctrl = t->a->entity_by_index(t->a->self, p->slot + 1);
  if (!ctrl) return 1;
  void* ms = nullptr;
  std::memcpy(&ms, static_cast<unsigned char*>(ctrl) + g_offMoneyServices, sizeof(ms));
  if (!ms) return 1;
  int32_t acct = 0;
  std::memcpy(&acct, static_cast<unsigned char*>(ms) + g_offAccount, sizeof(acct));
  if (acct >= kWarmupMoney) return 1;
  const int32_t full = kWarmupMoney;
  std::memcpy(static_cast<unsigned char*>(ms) + g_offAccount, &full, sizeof(full));
  t->a->entity_mark_changed(t->a->self, ctrl);
  ++t->changed;
  return 1;
}

void OnPurchase(void*, const char*, const ru_game_event*) { g_purchaseSeen = true; }

}  // namespace

void WarmupMoneyInstall(const ru_api* api) {
  g_offMoneyServices = g_offAccount = -2;
  if (!api->subscribe_game_event(api->self, "item_purchase", &OnPurchase, nullptr)) {
    Print("warmup-money: could not subscribe to item_purchase (topped up once a second instead)\n");
  }
}

void WarmupMoneyTick(double now) {
  if (!g_purchaseSeen && now < g_nextTick) return;
  g_purchaseSeen = false;
  g_nextTick = now + 1.0;
  if (!WarmupMoneyActive(GetModeString(), Cfg().warmup_money)) return;
  const ru_api* a = host::Api();
  if (!a || !Resolve(a)) return;
  TopUp t{a, 0};
  a->for_each_player(a->self, &TopUpPlayer, &t);
  if (t.changed) Debug("warmup-money: topped up %d player(s) to %d\n", t.changed, kWarmupMoney);
}

}  // namespace readyup
