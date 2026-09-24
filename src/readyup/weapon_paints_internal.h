#pragma once

#include "readyup/postgres.h"

#include <cstdint>

// Shared between weapon_paints_apply.cpp and weapon_paints_cosmetics.cpp. Game thread only.
namespace readyup::weapon_paints::detail {

// Resolves the schema offsets the skins feature needs (idempotent). False if any are missing.
bool EconOffsetsReady();
int ItemDefIndexOffset();     // CEconItemView::m_iItemDefinitionIndex
int ItemInitializedOffset();  // CEconItemView::m_bInitialized (may be -1)

// Writes paint/seed/wear/StatTrak/nametag + a fake item ID onto a CEconItemView (and the fallback
// fields of `ownerEconEntity` if non-null).
void WritePaint(void* ownerEconEntity, void* itemView, uint64_t steamid64, const readyup::WeaponSkinEntry& skin);

void* EconItemViewOfWeapon(void* weapon);

}  // namespace readyup::weapon_paints::detail
