#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Page 0 retains the native X1 inventory. Pages 1/2 are X2/X3. */
enum { MMX_WEAPONS_LEGACY_STATE_SIZE = 24 };
typedef struct MmxWeaponsState {
  uint8_t page, weapon, menu_page, initialized;
  uint8_t energy[16];
  uint16_t charge;
  uint8_t cooldown, reserved;
  uint8_t fraction[16]; /* Low bytes of source-format 8.8 weapon energy. */
} MmxWeaponsState;
typedef struct MmxWeaponPose {
  int16_t left, top;
  uint16_t width, height;
  const uint8_t *pixels;
} MmxWeaponPose;
bool MmxWeaponsLoad(const char *path);
bool MmxWeaponsLoadPage(const char *path, unsigned page);
bool MmxWeaponsPageEnabled(unsigned page);
void MmxWeaponsDisable(void);
bool MmxWeaponsEnabled(void);
bool MmxWeaponsActive(void);
MmxWeaponsState MmxWeaponsGetState(void);
bool MmxWeaponsValidState(const MmxWeaponsState *state);
void MmxWeaponsSetState(MmxWeaponsState state);
const MmxWeaponPose *MmxWeaponsPose(unsigned page, unsigned weapon, unsigned group, unsigned pose);
const MmxWeaponPose *MmxWeaponsIcon(unsigned page, unsigned weapon);
const MmxWeaponPose *MmxWeaponsHudIcon(unsigned page, unsigned weapon);
const uint16_t *MmxWeaponsIconPalette(unsigned page, unsigned weapon);
const uint8_t *MmxWeaponsAnimation(unsigned page, unsigned weapon, unsigned group, unsigned *size);
const uint16_t *MmxWeaponsPalette(unsigned page, unsigned weapon, bool body);
const uint16_t *MmxWeaponsGroupPalette(unsigned page,unsigned weapon,unsigned group);
const char *MmxWeaponsLabel(unsigned page, unsigned weapon);
bool MmxWeaponsMenuVisible(const uint8_t ram[0x20000]);
void MmxWeaponsMenuTick(uint8_t ram[0x20000], unsigned direct_page);
unsigned MmxWeaponsMenuRead(uint8_t ram[0x20000], unsigned pc, unsigned direct_page,
                            unsigned index, unsigned original);
unsigned MmxWeaponsEnergyRead(unsigned address, unsigned original);
bool MmxWeaponsEnergyStore(unsigned value, bool pickup);
void MmxWeaponsEnergyOverflow(uint8_t ram[0x20000], unsigned index);
void MmxWeaponsRefill(void);
unsigned MmxWeaponsEnergyAmount(unsigned page, unsigned weapon);
bool MmxWeaponsSpend(unsigned page, unsigned weapon, unsigned cost); /* 8.8 units. */
