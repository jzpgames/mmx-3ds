#include "mmx_renderer.h"
#include "mmx_colour_math.h"
#include <stddef.h>
#include "mmx_display.h"
#include "mmx_wide_policy.h"
#include "mmx_render_assets.h"
#include "mmx_zero.h"
#include "mmx_knc_bugfix.h"
#include "mmx_weapons.h"
#include "mmx_weapon_combat.h"
#include "mmx_coop_view.h"
#include "mmx_boss_rush.h"
#include "mmx_boss_rush_score.h"
#include <math.h>
#include <limits.h>

/* Mode-1 decode/composition follows SuperMetroidRecomp's sm_renderer.c.
 * The PPU remains native. Immutable raster snapshots are the only inputs to
 * presentation; this module never calls guest code or the PPU renderer. */
typedef struct Raster {
  uint8_t registers[PPU_SAVESTATE_REGS_SIZE];
  uint16_t palette[256], oam[256], vram[0x8000];
  uint8_t high_oam[32];
} Raster;
static MmxBossRushState frame_rush;
static MmxBossRushVisualState frame_rush_visual;
typedef struct Piece {
  int16_t x, y; uint16_t attr; uint8_t size, animation;
  /* Palette occupies bits 1..3; the five spare bits retain charged-shot
   * pose 0..20 at submission, without changing the capture record size. */
  uint8_t tile, palette_pose; uint16_t object;
} Piece;
static unsigned piece_pose(const Piece *p) {
  return (p->palette_pose&1)|((p->palette_pose>>3)&30);
}
enum { MAX_PIECES = 2048 };
typedef struct Frame {
  Raster lines[224];
  uint8_t ram[0x20000];
  uint32_t stock[256 * 224];
  Piece pieces[MAX_PIECES];
  unsigned piece_count, captured;
  bool valid;
  Piece expanded[MAX_PIECES];
  unsigned expanded_count;
  bool expand;
} Frame;
static Frame frame;
static unsigned frame_generation; /* bumped per finished frame */
static bool reuse_vram_snapshots;
static uint8_t vram_owner[224];
static uint32_t vram_generation;
static unsigned vram_last_line, vram_copies;
static const uint16_t *drawing_vram;
void MmxRendererSetVramSnapshotReuse(bool enabled) { reuse_vram_snapshots = enabled; }
/* 3DS revision 3: the native PPU's own image ("stock") is only presented for
 * lines that the custom compositor does not draw. Hosts may let the PPU skip
 * pixel work for every other line, and for frames that will not be shown. */
static bool stock_skipping, capture_enabled = true, frame_stage_scene, frame_select_scene, frame_flat_scene;
static bool flat_native_scene(void);
static bool stage_select_scene(void);
#ifdef __3DS__
static inline bool getenv_select_stock(void) { return false; }
#else
#include <stdlib.h>
static inline bool getenv_select_stock(void) { static int v = -1; if (v < 0) v = getenv("MMX_SELECT_STOCK") != NULL; return v; }
#endif
static unsigned stock_lines_needed;
void MmxRendererSetStockLineSkipping(bool enabled) { stock_skipping = enabled; }
void MmxRendererSetCaptureEnabled(bool enabled) { capture_enabled = enabled; }
unsigned MmxRendererVramCopies(void) { return vram_copies; }

typedef struct PilotOverlayAlignment {
  bool evaluated;
  bool valid;
  int dx;
  int dy;
} PilotOverlayAlignment;

static PilotOverlayAlignment pilot_overlay_alignment;

static void reset_pilot_overlay_alignment(void) {
  memset(&pilot_overlay_alignment, 0, sizeof(pilot_overlay_alignment));
}

static MmxZeroState frame_zero;
static MmxWeaponsState frame_weapons;
static MmxWeaponCombatState frame_weapon_combat;
static MmxCoopState frame_coop;
static MmxCoopViewBoundsState frame_bounds;
static bool frame_held;
static int peer_seat=-1,view_dx,view_dy;
void MmxRendererSetPeerView(int seat) { peer_seat=seat>=0 && seat<2?seat:-1; }
int MmxRendererGetPeerView(void) { return peer_seat; }
static int view_camera(const uint8_t *r,unsigned address) {
  return (int)(r[address]|r[address+1]<<8)+(address==0x1e4d?view_dx:view_dy);
}
void MmxRendererHoldFrame(bool held) {frame_held=held;}
static uint8_t partner_ram[0x20000];
static Raster partner_raster;
/* v1.0.1: VRAM of the hidden guest frame, double-buffered: the next hidden
 * frame runs on the main thread while the composer still draws this one. */
static Raster hidden_rasters[2];
static Raster *hidden_draw; static unsigned hidden_stage;
static const uint16_t *sprite_vram(const Raster *r) {
  return (r == &partner_raster || r == hidden_draw) ? r->vram : drawing_vram;
}
static Piece building[MAX_PIECES], latched[MAX_PIECES];
static unsigned building_count, latched_count;
static uint8_t building_stage, latched_stage;
static Piece expanded_building[MAX_PIECES], expanded_latched[MAX_PIECES];
static unsigned expanded_building_count, expanded_latched_count;
static uint16_t current_object;
static bool observed_lists;
static const uint8_t *rom;
static size_t rom_size;
static MmxRenderStats stats;
static MmxRendererPlayerOverlayProvider player_overlay_provider;
static MmxRenderPlayerOverlay frame_player_overlay;
/* The co-op partner seat's Zero, drawn from partner_ram rather than OAM. */
static MmxRenderPlayerOverlay frame_partner_overlay;
enum { MAX_WORLD_SPRITES = 8 };
static MmxRendererWorldSpriteProvider world_sprite_provider;
static MmxRenderWorldSprite frame_world_sprites[MAX_WORLD_SPRITES];
static unsigned frame_world_sprite_count;
enum { MAX_DEBUG_RECTS = 64 };
static MmxRendererDebugRectProvider debug_rect_provider;
static MmxRendererDebugRectProvider hitbox_overlay_provider;
static MmxRenderDebugRect frame_debug_rects[MAX_DEBUG_RECTS];
static unsigned frame_debug_rect_count;
static uint8_t door_cache[512 * 512];
/* Revision 16 (3DS): clear only the entries a frame touched instead of
 * 256 KB per frame (a full clear when the list overflows). */
static uint32_t door_touched[2048]; static unsigned door_touched_count; static bool door_touched_full = true;
static void door_cache_reset(void) {
  if (door_touched_full) memset(door_cache, 0, sizeof(door_cache));
  else for (unsigned i = 0; i < door_touched_count; ++i) door_cache[door_touched[i]] = 0;
  door_touched_count = 0; door_touched_full = false;
}
static int airport_sky_width;
typedef struct SubmarineBody {
  int left, top, bottom;
  unsigned shift;
  bool hidden, scrolling;
} SubmarineBody;
static SubmarineBody submarine_bodies[16];
static unsigned submarine_count;
bool g_mmx_custom_renderer;
bool g_mmx_custom_hud = true;
static bool coop_hud_compact = true;
void MmxRendererSetCompactCoopHud(bool compact) { coop_hud_compact=compact; }
bool g_mmx_expanded_sprites;
bool g_mmx_render_asset_repairs = true;
MmxRenderAspect g_mmx_custom_aspect = MMX_ASPECT_ADAPTIVE;
MmxRenderView g_mmx_custom_view = {342, 43, 16.0 / 9.0};

MmxRenderView MmxRendererViewport(MmxRenderAspect mode, int w, int h,
                                  SnesDisplayAspect display_aspect) {
  double aspect = mode == MMX_ASPECT_16_9 ? 16.0 / 9.0 :
                  mode == MMX_ASPECT_21_9 ? 21.0 / 9.0 :
                  mode == MMX_ASPECT_32_9 ? 32.0 / 9.0 :
                  w > 0 && h > 0 ? (double)w / h : 16.0 / 9.0;
  SnesDisplayFrame frame = SnesDisplayAspect_ComputeAdaptiveFrame(
      256, MMX_RENDER_HEIGHT, MMX_RENDER_MAX_WIDTH, aspect, display_aspect);
  return (MmxRenderView){frame.width, frame.extra, frame.aspect};
}
MmxDisplayViewport MmxRendererDestination(MmxRenderView view, int width, int height) {
  return SnesDisplayAspect_FitViewport(view.aspect, width, height);
}
static unsigned word(const uint8_t *p, unsigned a) { return p[a] | (p[a + 1] << 8); }
static bool zero_weapons_menu(void) {
  return MmxZeroEnabled() && !frame_zero.active_x && frame.ram[0xd1] == 2 && frame.ram[0xd2] == 4 &&
      (((frame.ram[0x1f10] == 6 || frame.ram[0x1f10] == 8) && (frame.ram[0xc3] & 128)) ||
       (frame.ram[0x1989] == 1 && word(frame.ram,0x198d) == 128 &&
        word(frame.ram,0x1990) == 160 && (frame.ram[0x199e] == 0 || frame.ram[0x199e] == 0x18)));
}
static bool zero_title_menu(void) {
  /* Retail title-menu cursor is the player object, at one of three rows.
   * Its native idle/firing animation and selection beam remain active. */
  unsigned y = word(frame.ram,0xbb0);
  return MmxZeroEnabled() && !frame_zero.active_x && frame.ram[0xd1] == 0 && frame.ram[0xba9] == 2 &&
      frame.ram[0xbbe] == 0 && word(frame.ram,0xbad) == 32 &&
      y >= 166 && y <= 214;
}
/* X1's buster charge cycles palette 1 in CGRAM itself while its sparkle
 * objects (class 1, $0C98..$0E17, $82:82ED) are live. The co-op weapon
 * palette would replace that glow with the plain body colours. */
static bool x_charging(const uint8_t *ram) {
  for (unsigned d = 0xc98; d < 0xe18; d += 32) if (ram[d] && ram[d + 10] == 1) return true;
  return false;
}
static bool zero_actor(unsigned object, unsigned animation) {
  return object == 0xba8 || object == 0xc38 || object == 0xc58 || object == 0xc78 ||
      (object == 0x1928 && animation == 0x5f) || (object == 0x1948 && animation == 0x5e) ||
      (object == 0x1968 && animation == 0x5d) ||
      (object == 0x1988 && (animation == 0 || animation == 0x18));
}
static const uint8_t *rom_at(unsigned address, size_t length) {
  if ((address & 0xffff) < 0x8000) return NULL;
  size_t offset = ((address >> 16) & 0x7f) * 0x8000 + (address & 0x7fff);
  return rom && offset <= rom_size && length <= rom_size - offset ? rom + offset : NULL;
}
void MmxRendererSetRom(const uint8_t *bytes, size_t length) {
  rom = bytes; rom_size = length; MmxRenderAssetsSetRom(bytes, length);
  MmxCoopViewsSetRom(bytes,length);
  MmxRenderAssetsPreloadRush();
}
void MmxRendererSetPlayerOverlayProvider(
    MmxRendererPlayerOverlayProvider provider) {
  player_overlay_provider = provider;
  if (!provider) {
    memset(&frame_player_overlay, 0, sizeof(frame_player_overlay));
    memset(&frame_partner_overlay, 0, sizeof(frame_partner_overlay));
  }
}
MmxRenderPlayerOverlay MmxRendererPlayerOverlaySnapshot(void) {
  return frame_player_overlay;
}
void MmxRendererSetWorldSpriteProvider(MmxRendererWorldSpriteProvider provider) {
  world_sprite_provider = provider;
  if (!provider) frame_world_sprite_count = 0;
}
unsigned MmxRendererWorldSpriteSnapshot(MmxRenderWorldSprite *out,
                                        unsigned max) {
  unsigned count = frame_world_sprite_count < max ? frame_world_sprite_count : max;
  if (out && count)
    memcpy(out, frame_world_sprites, count * sizeof(*out));
  return count;
}
void MmxRendererSetHitboxOverlayProvider(MmxRendererDebugRectProvider provider) {
  hitbox_overlay_provider = provider;
  if (!provider && !debug_rect_provider) frame_debug_rect_count = 0;
}
void MmxRendererSetDebugRectProvider(MmxRendererDebugRectProvider provider) {
  debug_rect_provider = provider;
  if (!provider) frame_debug_rect_count = 0;
}
unsigned MmxRendererDebugRectSnapshot(MmxRenderDebugRect *out,
                                      unsigned max) {
  unsigned count = frame_debug_rect_count < max ? frame_debug_rect_count : max;
  if (out && count)
    memcpy(out, frame_debug_rects, count * sizeof(*out));
  return count;
}
void MmxRendererReset(void) {
  memset(&frame_zero, 0, sizeof(frame_zero));
  memset(&frame_player_overlay, 0, sizeof(frame_player_overlay));
  memset(&frame_partner_overlay, 0, sizeof(frame_partner_overlay));
  reset_pilot_overlay_alignment();
  frame_world_sprite_count = 0;
  frame_debug_rect_count = 0;
  frame.valid = false; frame.captured = 0; vram_copies = 0;
  building_count = latched_count = 0;
  building_stage = latched_stage = 0xff;
  expanded_building_count = expanded_latched_count = 0;
  current_object = 0; observed_lists = false;
}
static Piece make_piece(const uint8_t *p, int x, int y, unsigned flip,
                        unsigned attributes, unsigned base, unsigned animation, unsigned object, unsigned pose) {
  int size = p[4] & 0x20 ? 16 : 8;
  x += flip & 0x40 ? -(int8_t)p[1] - size : (int8_t)p[1];
  y += flip & 0x80 ? -(int8_t)p[2] - size : (int8_t)p[2];
  unsigned attr = (((p[4] & 0xce) | attributes) ^ flip) << 8;
  attr |= (p[3] + base) & 255;
  unsigned retained_pose=(animation==0x0e || animation==0x9e) ? pose&31 : 0;
  return (Piece){(int16_t)x, (int16_t)y, (uint16_t)attr, (uint8_t)size,
      (uint8_t)animation, p[3], (uint8_t)((p[4]&14)|(retained_pose&1)|((retained_pose&30)<<3)), (uint16_t)object};
}
static const uint8_t *sprite_arrangement(unsigned animation, unsigned f) {
  const uint8_t *pointer = rom_at(0x8d8000 + animation * 3, 3);
  if (!pointer) return NULL;
  unsigned address = word(pointer, 0) | (pointer[2] << 16);
  pointer = rom_at(address + f * 3, 3);
  if (!pointer) return NULL;
  address = word(pointer, 0) | (pointer[2] << 16);
  const uint8_t *arrangement = rom_at(address, 1);
  return arrangement && rom_at(address, 1 + arrangement[0] * 4) ? arrangement : NULL;
}
/* X1's Ride Armor pilot frames (groups $6A/$6B) hold one 16x16 piece: the
 * pilot's head. The other pieces are arms and shoulders, and which one comes
 * first varies (punch frames list an arm first), so anchoring Zero's helmet to
 * the first piece sank or shifted it for those frames. Offsets of the head
 * piece from the body origin; the first piece if a frame has none. */
static void pilot_head_offset(const uint8_t *layout, int *dx, int *dy) {
  unsigned head = 0;
  for (unsigned i = 0; i < layout[0]; ++i)
    if (layout[i * 4 + 4] & 0x20) { head = i; break; }
  *dx = (int8_t)layout[head * 4 + 1];
  *dy = (int8_t)layout[head * 4 + 2];
}
static void expand_object(const uint8_t *ram, unsigned object) {
  if (object < 0x20 || object > 0x1fe0) return;
  unsigned animation = ram[object + 0x16], f = ram[object + 0x17] & 127;
  const uint8_t *arrangement = sprite_arrangement(animation, f);
  if (!arrangement) return;
  int x = (int16_t)(word(ram, object + 5) - word(ram, 0x1e4d));
  int y = (int16_t)(word(ram, object + 8) + (int8_t)ram[object + 0x19] - word(ram, 0x1e50));
  unsigned base = MmxWidePolicy_CrusherTileBase(ram, (uint16_t)object, ram[object + 0x18]);
  for (unsigned i = 0; i < arrangement[0] && expanded_building_count < MAX_PIECES; ++i)
    expanded_building[expanded_building_count++] = make_piece(arrangement + i * 4, x, y,
        ram[object + 0x11] & 0x40, ram[object + 0x11] & 0x3f, base, animation, object, f);
}
static bool fortress_sound_actor(unsigned object) {
  /* $88:D359 uses Zero's previous pose while playing the offscreen room
   * sounds. It still owns a release timer; suppress only its margin art. */
  const uint8_t *r = frame.ram;
  return r[0x1f7a] == 9 && object >= 0xe68 && object < 0x1228 &&
      (object & 63) == 0x28 && r[object + 10] == 0x66 &&
      r[object + 1] == 2 && r[object + 2] == 6;
}
static unsigned fortress_waiting_pieces(Piece out[128], const Piece *pieces, unsigned piece_count) {
  const uint8_t *r = frame.ram;
  unsigned camera = word(r, 0x1e4d);
  if (r[0x1f7a] != 9 || r[0x1f08] != 4 || r[0x1f7d] >= 2 ||
      camera < 0x900 || camera > 0xa80 || word(r, 0x1e50) != 0x500) return 0;
  bool live[3] = {false, false, false};
  for (unsigned d = 0xe68; d < 0x1228; d += 64) if (r[d]) {
    bool submitted = false;
    for (unsigned i = 0; i < piece_count; ++i) submitted |= pieces[i].object == d;
    /* Initializers set their states one frame before submitting art. Keep
     * the preview through that gap, but never replace later hidden poses. */
    if (r[d + 10] == 0x67 && r[d + 1])
      live[0] = submitted || r[d + 1] != 2 || r[d + 2] != 0x1a || r[d + 3] != 0;
    if (r[d + 10] == 0x66 && r[d + 1] == 2 && r[d + 2] >= 8 && r[d + 3])
      live[1] = submitted || r[d + 2] != 8 || r[d + 3] != 2;
    if (r[d + 10] == 0x64 && r[d + 1] >= 4) live[2] = true;
  }
  /* $87:EDBB creates a separate looping electrical effect (kind 1, $10/$2E).
   * Its later allocation must not leave an empty cage between the previews
   * and native actors. Once submitted, retain its original animation timing. */
  for (unsigned d = 0x1928; d < 0x1d08; d += 32)
    if (r[d] && r[d + 10] == 0x10 && r[d + 11] == 0x2e && r[d + 22] == 0x9d)
      for (unsigned i = 0; i < piece_count; ++i) live[2] |= pieces[i].object == d;
  /* $83:E858 and $88:D3DB start these waiting poses. Section 4 has loaded
   * the shared resources; Zero's pose needs its private dynamic CHR transfer.
   * Preview only the margins until each real actor submits art: changing
   * native allocation order here breaks the Vile/Zero cutscene handoff. */
  unsigned count = 0;
  for (unsigned actor = 0; actor < 3; ++actor) if (!live[actor]) {
    unsigned animation = actor == 2 ? 0x9d : actor ? 0x53 : 0x52;
    /* Zero's $11 sequence blinks every six frames. The cage's $01 sequence
     * cycles through frames $01..$0F, one frame each. */
    unsigned pose = actor == 2 ? 1 + word(r, 0xb9c) % 15 :
                    actor ? 0x20 + (word(r, 0xb9c) % 12 >= 6) : 0;
    const uint8_t *a = sprite_arrangement(animation, pose);
    if (!a) continue;
    int x = actor == 2 ? 0xb5f : actor ? 0xb60 : 0xb30;
    int y = actor == 2 ? 0x58b : 0x58e;
    unsigned attributes = actor == 2 ? r[0x18396] : actor ? 0x2c : 0x29;
    for (unsigned i = 0; i < a[0] && count < 128; ++i)
      out[count++] = make_piece(a + i * 4, x - (int)camera,
          y - (int)word(r, 0x1e50), 0, attributes, actor == 2 ? r[0x18296] : 0, animation, 0, pose);
  }
  return count;
}
static void expand_queues(const uint8_t *ram) {
  /* D56F's actual six priority queues, captured before D6A7 can exhaust OAM.
   * Keep its order: queues 0..2, weapon objects, X, queues 3..5. No arbitrary
   * scan of dormant object slots and no additional guest objects or writes. */
  for (unsigned group = 0; group < 6; ++group) {
    if (group == 3) {
      for (unsigned d = 0xc38; d <= 0xc78; d += 0x20)
        if (ram[d] && ram[d + 14]) expand_object(ram, d);
      if (ram[0xbb6]) expand_object(ram, 0xba8);
    }
    unsigned count = ram[0xe7 + group];
    if (count > 32) count = 32;
    for (unsigned i = 0; i < count; ++i) expand_object(ram, word(ram, 0x920 + group * 64 + i * 2));
  }
}
void MmxRendererObserveObject(const uint8_t ram[0x20000], uint16_t object) {
  if (!g_mmx_custom_renderer || !ram) return;
  if (building_stage != ram[0x1f7a]) {
    building_count = expanded_building_count = 0; observed_lists = false;
    building_stage = ram[0x1f7a];
  }
  current_object = object;
  /* Two bosses and their attacks exceed the retail OAM submission budget.
   * Preserve the native queue and full arrangements in Rush too. */
  if (observed_lists || (!g_mmx_expanded_sprites && !MmxCoopViewsOnline() &&
      MmxBossRushGetState().mode!=MMX_RUSH_PLAYING)) return;
  observed_lists = true;
  expand_queues(ram);
}
MmxRendererPieceMark MmxRendererMarkPieces(void) {
  return (MmxRendererPieceMark){building_count,expanded_building_count,current_object,building_stage,observed_lists};
}
void MmxRendererRewindPieces(MmxRendererPieceMark mark) {
  if (mark.building<=building_count) building_count=mark.building;
  if (mark.expanded<=expanded_building_count) expanded_building_count=mark.expanded;
  current_object=mark.object;building_stage=mark.stage;observed_lists=mark.observed;
}
void MmxRendererRecordPiece(const uint8_t ram[0x20000], uint16_t d) {
  if (!g_mmx_custom_renderer || !ram || d > 0xffe0 || building_count >= MAX_PIECES) return;
  if (building_stage != ram[0x1f7a]) {
    building_count = 0;
    building_stage = ram[0x1f7a];
  }
  unsigned pointer = word(ram, d + 0x18) | (ram[d + 0x1a] << 16);
  const uint8_t *p = rom_at(pointer, 5);
  if (!p) return;
  int x = (int16_t)word(ram, d), y = (int16_t)word(ram, d + 2);
  unsigned animation = current_object && current_object < 0x1fe0 ? ram[current_object + 0x16] : 255;
  building[building_count++] = make_piece(p, x, y, ram[d + 0xb], ram[d + 0xf],
                                         ram[d + 0x10], animation, current_object,
                                         current_object && current_object<0x1fe0 ? ram[current_object+23]&127 : 0);
}
void MmxRendererLatchSprites(void) {
  if ((frame_held || MmxZeroSwapping() || MmxWeaponsTimeActive()) && !building_count) return;
  /* Menu fades suspend OAM construction while the PPU repeats old sprites.
   * Keep Zero and private boss attribution until native OAM retires them. */
  bool rush=MmxBossRushGetState().mode==MMX_RUSH_PLAYING;
  if ((MmxZeroEnabled() || rush) && !building_count && latched_stage == building_stage) {
    unsigned count = 0;
    for (unsigned i = 0; i < latched_count; ++i)
      if ((MmxZeroEnabled() && zero_actor(latched[i].object,latched[i].animation)) ||
          (rush && MmxBossRushOwner(latched[i].object)>=0)) latched[count++] = latched[i];
    latched_count = count;
  } else {
    latched_count = building_count;
    latched_stage = building_stage;
    memcpy(latched, building, building_count * sizeof(*building));
  }
  building_count = 0;
  expanded_latched_count = expanded_building_count;
  memcpy(expanded_latched, expanded_building, expanded_building_count * sizeof(Piece));
  expanded_building_count = 0; observed_lists = false; current_object = 0;
}
static void trace_objects(const uint8_t *ram) {
  static FILE *log;
  static bool checked;
  static unsigned tick, previous[16];
  ++tick;
  if (!checked) {
    checked = true;
    const char *path = getenv("MMX_RENDER_OBJECT_TRACE");
    if (path && *path) log = fopen(path, "w");
    if (log) fputs("frame,object,state,camera,player_x,enemy_x,enemy_y,pieces,stage,id,health,player_y\n", log);
  }
  unsigned stage = ram[0x1f7a];
  if (!log) return;
  for (unsigned i = 0; i < 16; ++i) {
    unsigned d = i == 15 ? 0xe18 : 0xe68 + i * 64;
    bool selected = i == 15 ? stage == 8 :
        MmxWidePolicy_IsBossEncounter(ram[d + 10]) ||
        (stage == 8 && ram[d + 10] == 0x36) || (stage == 6 && ram[d + 10] == 0x37);
    unsigned state = ram[d] && selected ? ram[d + 1] + 1u : 0;
    if (state == previous[i] && (!state || (tick & 15))) continue;
    previous[i] = state;
    fprintf(log, "%u,%04x,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", tick, d, (int)state - 1,
        word(ram, 0x1e4d), word(ram, 0xbad), word(ram, d + 5), word(ram, d + 8), latched_count,
        stage, ram[d + 10], ram[d + 0x27], word(ram, 0xbb0));
    fflush(log);
  }
}
void MmxRendererBeginFrame(const uint8_t ram[0x20000]) {
  MmxRenderPlayerOverlay overlay;
  MmxRenderWorldSprite world_sprites[MAX_WORLD_SPRITES];
  MmxRenderDebugRect debug_rects[MAX_DEBUG_RECTS];
  unsigned world_sprite_count = 0;
  unsigned debug_rect_count = 0;
  reset_pilot_overlay_alignment();
  frame_rush=MmxBossRushGetState();
  frame_rush_visual=MmxBossRushVisualGetState();
  memset(&frame_coop,0,sizeof(frame_coop));
  memset(&frame_partner_overlay, 0, sizeof(frame_partner_overlay));
  memset(&overlay, 0, sizeof(overlay));
  MmxZeroState live_zero = MmxZeroGetState();
  if (player_overlay_provider && player_overlay_provider(ram, &live_zero, &overlay))
    frame_player_overlay = overlay;
  else
    memset(&frame_player_overlay, 0, sizeof(frame_player_overlay));
  memset(world_sprites, 0, sizeof(world_sprites));
  if (world_sprite_provider)
    world_sprite_count = world_sprite_provider(world_sprites,
                                               MAX_WORLD_SPRITES);
  if (world_sprite_count > MAX_WORLD_SPRITES)
    world_sprite_count = MAX_WORLD_SPRITES;
  frame_world_sprite_count = world_sprite_count;
  if (world_sprite_count)
    memcpy(frame_world_sprites, world_sprites,
           world_sprite_count * sizeof(*frame_world_sprites));
  memset(debug_rects, 0, sizeof(debug_rects));
  /* The developer overlay outlines every object, a superset of any
   * mod's own debug boxes, so it takes the frame when both are on. */
  MmxRendererDebugRectProvider rects =
      hitbox_overlay_provider ? hitbox_overlay_provider : debug_rect_provider;
  if (rects)
    debug_rect_count = rects(debug_rects, MAX_DEBUG_RECTS);
  if (debug_rect_count > MAX_DEBUG_RECTS)
    debug_rect_count = MAX_DEBUG_RECTS;
  frame_debug_rect_count = debug_rect_count;
  if (debug_rect_count)
    memcpy(frame_debug_rects, debug_rects,
           debug_rect_count * sizeof(*frame_debug_rects));
  trace_objects(ram);
  frame.valid = false; frame.captured = 0;
  memcpy(frame.ram, ram, sizeof(frame.ram));
  frame_stage_scene = MmxWidePolicy_IsStageScene(frame.ram);
  /* v1.0.1: the stage-select screen (mode 2/0/1) is drawn by both
   * compositors too (their `select` lines); its stock pixels cost ~7 ms per
   * frame on the main thread and made that screen drop frames. */
  frame_select_scene = !frame_stage_scene && !getenv_select_stock() && stage_select_scene();
  frame_flat_scene = !frame_stage_scene && !frame_select_scene && flat_native_scene();
  stock_lines_needed = 0;
  frame_zero = MmxZeroGetState();
  frame_bounds=MmxCoopViewsGetBoundsState();
  frame_weapons = MmxWeaponsGetState();
  frame_weapon_combat = MmxWeaponsGetCombatState();
  if ((frame_held || MmxZeroSwapping() || MmxWeaponsTimeActive()) && !latched_count) {
    /* A mid-swap load has no preceding submission. Rebuild the frozen
     * native queues, including margin actors, before matching live OAM. */
    expanded_building_count = 0; expand_queues(ram);
    latched_stage = ram[0x1f7a];
    latched_count = expanded_latched_count = expanded_building_count;
    memcpy(latched,expanded_building,latched_count * sizeof(Piece));
    memcpy(expanded_latched,expanded_building,latched_count * sizeof(Piece));
    expanded_building_count = 0;
  }
  frame.piece_count = latched_stage == ram[0x1f7a] ? latched_count : 0;
  memcpy(frame.pieces, latched, frame.piece_count * sizeof(*latched));
  if (MmxZeroEnabled()) {
    /* Loading a snapshot can begin on a fade before the first observed
     * submission. Reconstruct candidates, then match them against live OAM. */
    const unsigned actors[] = {0xba8,0xc38,0xc58,0xc78,0x1928,0x1948,0x1968,0x1988};
    for (unsigned j = 0; j < sizeof(actors)/sizeof(actors[0]); ++j) {
      unsigned d = actors[j]; bool found = false;
      if (d >= 0x1928 && !zero_weapons_menu()) continue;
      for (unsigned i = 0; i < frame.piece_count; ++i) found |= frame.pieces[i].object == d;
      /* $81:8AAA disables armor at death but leaves +$0E set. Unlike the
       * body, D56F submits these parts only while their active byte is set.
       * Reconstructing dormant parts makes expanded lists retain white
       * armor after the native body burst. Recorded OAM keeps its epoch. */
      bool inactive_armor=d>=0xc38 && d<=0xc78 && !ram[d];
      if (found || !ram[d + 14] || inactive_armor) continue;
      const uint8_t *a = sprite_arrangement(ram[d + 22],ram[d + 23] & 127);
      if (!a) continue;
      int x = (int16_t)(word(ram,d + 5) - word(ram,0x1e4d));
      int y = (int16_t)(word(ram,d + 8) + (int8_t)ram[d + 25] - word(ram,0x1e50));
      if (d >= 0x1928 && zero_weapons_menu()) { x = word(ram,d + 5); y = word(ram,d + 8); }
      for (unsigned i = 0; i < a[0] && frame.piece_count < MAX_PIECES; ++i)
        frame.pieces[frame.piece_count++] = make_piece(a + i * 4,x,y,ram[d + 17] & 64,
            ram[d + 17] & 63,ram[d + 24],ram[d + 22],d,ram[d+23]&127);
    }
  }
  frame.expanded_count = latched_stage == ram[0x1f7a] ? expanded_latched_count : 0;
  memcpy(frame.expanded, expanded_latched, frame.expanded_count * sizeof(Piece));
  frame.expand = g_mmx_expanded_sprites || MmxCoopViewsOnline() ||
      (frame_rush.mode==MMX_RUSH_PLAYING && MmxWidePolicy_IsStageScene(frame.ram) &&
       !zero_weapons_menu() &&
       !MmxWeaponsMenuVisible(frame.ram));
  if(frame_rush.mode==MMX_RUSH_PLAYING && frame.expand && !frame.expanded_count && frame.piece_count)
    frame.expand=false; /* A fade retained native OAM, not a new expanded queue. */
  if (MmxZeroEnabled() && frame.expand) for (unsigned i = 0; i < frame.piece_count; ++i) {
    Piece p = frame.pieces[i]; if (!zero_actor(p.object,p.animation)) continue;
    bool found = false;
    for (unsigned j = 0; j < expanded_latched_count; ++j) found |= frame.expanded[j].object == p.object;
    if (!found && frame.expanded_count < MAX_PIECES) frame.expanded[frame.expanded_count++] = p;
  }
}
static bool pilot_overlay_oam_match(const Piece *piece) {
  for (unsigned line = 0; line < 224; ++line) {
    const Raster *r = &frame.lines[line];
    for (unsigned slot = 16; slot < 128; ++slot) {
      unsigned pos = r->oam[slot * 2];
      unsigned hi = r->high_oam[slot / 4] >> (slot % 4 * 2);
      int ox = (pos & 255) | ((hi & 1) << 8);
      if (ox >= 256) ox -= 512;
      if (ox == piece->x && (pos >> 8) == ((unsigned)piece->y & 255) &&
          r->oam[slot * 2 + 1] == piece->attr) return true;
    }
  }
  return false;
}
static bool rush_pause_art_needed(void) {
  if(frame_rush.mode!=MMX_RUSH_PLAYING) return false;
  for(unsigned i=0;i<frame.piece_count;++i) {
    const Piece *p=frame.pieces+i;
    if(p->object<0xe68 || p->object>=0x1d08 ||
        (p->object>=0x1928 && p->object<=0x1988)) continue;
    if(frame_rush.owner[(p->object-0xe68)/32] && pilot_overlay_oam_match(p)) return true;
  }
  return false;
}
static void align_wide_pilot_overlay(const MmxRenderPlayerOverlay *overlay,
                                     unsigned animation, int *zx, int *zy) {
  const MmxRenderPlayerOverlayPlane *plane = overlay ? &overlay->body : NULL;
  int native_left = 10000, native_top = 10000;
  int native_right = -10000, native_bottom = -10000;
  int overlay_left = 10000, overlay_top = 10000;
  int overlay_right = -10000, overlay_bottom = -10000;
  if (!plane || !plane->pixels || plane->width <= MMX_ZERO_WIDTH ||
      !plane->height || !overlay->palette || !overlay->palette_count ||
      !zx || !zy) return;
  if (pilot_overlay_alignment.evaluated) {
    if (pilot_overlay_alignment.valid) {
      *zx += pilot_overlay_alignment.dx;
      *zy += pilot_overlay_alignment.dy;
    }
    return;
  }
  pilot_overlay_alignment.evaluated = true;
  for (unsigned i = 0; i < frame.piece_count; ++i) {
    const Piece *piece = &frame.pieces[i];
    if (piece->object != 0xba8 || piece->animation != animation ||
        !pilot_overlay_oam_match(piece)) continue;
    if (piece->x < native_left) native_left = piece->x;
    if (piece->y < native_top) native_top = piece->y;
    if (piece->x + piece->size - 1 > native_right)
      native_right = piece->x + piece->size - 1;
    if (piece->y + piece->size - 1 > native_bottom)
      native_bottom = piece->y + piece->size - 1;
  }
  if (native_left > native_right || native_top > native_bottom) return;
  native_left -= view_dx;
  native_right -= view_dx;
  native_top -= view_dy;
  native_bottom -= view_dy;
  for (int row = 0; row < plane->height; ++row) for (int col = 0; col < plane->width; ++col) {
    unsigned pixel = plane->pixels[row * plane->width + col];
    int x, y;
    if (!pixel || pixel >= overlay->palette_count) continue;
    x = *zx + (overlay->facing_left ? 63 - (plane->origin_x + col) :
        plane->origin_x + col - 64);
    y = *zy - 64 + plane->origin_y + row;
    if (x < overlay_left) overlay_left = x;
    if (y < overlay_top) overlay_top = y;
    if (x > overlay_right) overlay_right = x;
    if (y > overlay_bottom) overlay_bottom = y;
  }
  if (overlay_left > overlay_right || overlay_top > overlay_bottom) return;
  /* Align the authored cockpit canvas by its visible bounds. The native pilot
   * OAM is the stable pose anchor; the player/armor positions are the same
   * world point, but are not the visible Zero anchor inside the wide canvas. */
  pilot_overlay_alignment.dx =
      (native_left + native_right - overlay_left - overlay_right) / 2;
  pilot_overlay_alignment.dy =
      (native_top + native_bottom - overlay_top - overlay_bottom) / 2;
  pilot_overlay_alignment.valid = true;
  *zx += pilot_overlay_alignment.dx;
  *zy += pilot_overlay_alignment.dy;
}
/* Kept out of the public header: the Saber ROM test uses the renderer's
 * captured OAM and the translation actually applied by Draw to assert the
 * real presentation result, without duplicating the alignment decision. */
bool MmxRendererRidePilotBoundsForTest(int native_box[4], int drawn_box[4]) {
  const MmxRenderPlayerOverlayPlane *plane = &frame_player_overlay.body;
  int native_left = 10000, native_top = 10000;
  int native_right = -10000, native_bottom = -10000;
  int overlay_left = 10000, overlay_top = 10000;
  int overlay_right = -10000, overlay_bottom = -10000;
  int zx, zy;
  unsigned animation;
  if (!native_box || !drawn_box || !frame.valid ||
      !frame_player_overlay.active || frame.ram[0x0baa] != 0x2c ||
      !plane->pixels || !plane->width || !plane->height ||
      !frame_player_overlay.palette || !frame_player_overlay.palette_count ||
      !pilot_overlay_alignment.valid) return false;
  animation = frame.ram[0x0bbe];
  if (animation != 0x6a && animation != 0x6b) return false;
  for (unsigned i = 0; i < frame.piece_count; ++i) {
    const Piece *piece = &frame.pieces[i];
    if (piece->object != 0xba8 || piece->animation != animation ||
        !pilot_overlay_oam_match(piece)) continue;
    if (piece->x - view_dx < native_left) native_left = piece->x - view_dx;
    if (piece->y - view_dy < native_top) native_top = piece->y - view_dy;
    if (piece->x + piece->size - 1 - view_dx > native_right)
      native_right = piece->x + piece->size - 1 - view_dx;
    if (piece->y + piece->size - 1 - view_dy > native_bottom)
      native_bottom = piece->y + piece->size - 1 - view_dy;
  }
  if (native_left > native_right || native_top > native_bottom) return false;
  zx = (int16_t)(word(frame.ram, 0x0bad) - view_camera(frame.ram, 0x1e4d));
  zy = (int16_t)(word(frame.ram, 0x0bb0) - view_camera(frame.ram, 0x1e50)) +
      MmxZeroPoseOffsetY(frame.ram);
  for (int row = 0; row < plane->height; ++row) for (int col = 0; col < plane->width; ++col) {
    unsigned pixel = plane->pixels[row * plane->width + col];
    int x, y;
    if (!pixel || pixel >= frame_player_overlay.palette_count) continue;
    x = zx + (frame_player_overlay.facing_left ?
        63 - (plane->origin_x + col) : plane->origin_x + col - 64);
    y = zy - 64 + plane->origin_y + row;
    if (x < overlay_left) overlay_left = x;
    if (y < overlay_top) overlay_top = y;
    if (x > overlay_right) overlay_right = x;
    if (y > overlay_bottom) overlay_bottom = y;
  }
  if (overlay_left > overlay_right || overlay_top > overlay_bottom) return false;
  native_box[0] = native_left;
  native_box[1] = native_top;
  native_box[2] = native_right;
  native_box[3] = native_bottom;
  drawn_box[0] = overlay_left + pilot_overlay_alignment.dx;
  drawn_box[1] = overlay_top + pilot_overlay_alignment.dy;
  drawn_box[2] = overlay_right + pilot_overlay_alignment.dx;
  drawn_box[3] = overlay_bottom + pilot_overlay_alignment.dy;
  return true;
}
/* Test-only orientation probe. The native pilot's OAM pieces should share one
 * horizontal-flip bit; expose that captured bit beside the overlay mirror so
 * the ROM test can prove both facings use the same authored orientation. */
bool MmxRendererRidePilotFacingForTest(bool *native_hflip,
                                       bool *drawn_mirror) {
  bool found = false;
  bool hflip = false;
  unsigned animation;
  if (!native_hflip || !drawn_mirror || !frame.valid ||
      !frame_player_overlay.active || frame.ram[0x0baa] != 0x2c ||
      !pilot_overlay_alignment.valid)
    return false;
  animation = frame.ram[0x0bbe];
  if (animation != 0x6a && animation != 0x6b) return false;
  for (unsigned i = 0; i < frame.piece_count; ++i) {
    const Piece *piece = &frame.pieces[i];
    bool piece_hflip;
    if (piece->object != 0xba8 || piece->animation != animation ||
        !pilot_overlay_oam_match(piece)) continue;
    piece_hflip = (piece->attr & 0x4000) != 0;
    if (!found) {
      hflip = piece_hflip;
      found = true;
    } else if (hflip != piece_hflip) {
      return false;
    }
  }
  if (!found) return false;
  *native_hflip = hflip;
  *drawn_mirror = frame_player_overlay.facing_left;
  return true;
}
bool MmxRendererSkipStockLine(const Ppu *p, unsigned line) {
  if (!stock_skipping || line < 1 || line > 224) return false;
  if (!capture_enabled) return true;
  /* Both stereo compositors (the native pair path and draw_frame) draw a
   * mode-1 line of a stage scene themselves and never read its stock pixels.
   * frame_stage_scene is MmxWidePolicy_IsStageScene(frame.ram), the same
   * snapshot the compositors test. */
  if (p && (frame_stage_scene || frame_select_scene || frame_flat_scene) && (p->bgmode & 7) == 1) return true;
  ++stock_lines_needed;
  return false;
}
void MmxRendererCaptureLine(const Ppu *p, unsigned line) {
  if (!capture_enabled) return;
  if (!p || line < 1 || line > 224 || line != frame.captured + 1) return;
  if (line == 1) vram_copies = 0;
  Raster *r = &frame.lines[line - 1];
  memcpy(r->registers, p, sizeof(r->registers));
  memcpy(r->palette, p->cgram, sizeof(r->palette));
  memcpy(r->oam, p->oam, sizeof(r->oam));
  memcpy(r->high_oam, p->highOam, sizeof(r->high_oam));
  if (!reuse_vram_snapshots || line == 1 || p->vramWriteCount != vram_generation) {
    memcpy(r->vram, p->vram, sizeof(r->vram));
    vram_last_line = line - 1; vram_generation = p->vramWriteCount;
    ++vram_copies;
  }
  vram_owner[line - 1] = (uint8_t)vram_last_line;
  ++frame.captured;
}
/* v1.0.1: 30 Hz presentation keeps one guest frame of each pair. MMX draws
 * some sprites only on alternate frames (X's post-hit blink, Storm Tornado,
 * the second half of Fire Wave), which then never reached the screen. The
 * hidden frame's OAM is noted here; a hidden sprite whose tile word appears
 * in neither the shown frame nor the previously shown one existed only on
 * the hidden phase, so it is added to the shown frame in a free slot (lowest
 * priority). Objects that merely changed animation were in the previous
 * shown frame and get no ghost. X (absent while blinking, $0BB6 == 0) is
 * added on alternate images: a 15 Hz blink instead of vanishing. */
typedef struct FlickerObject { int x, y, size; uint16_t attr; } FlickerObject;
static FlickerObject flicker_objects[128];
static unsigned flicker_count;
/* Hidden-phase sprites, under the shown frame's own sprites, with the
 * hidden frame's tiles. Shared by both compositors. */
static void draw_flicker_objects(const Ppu *p, int y, MmxRenderView view, uint16_t *objects,
                                 int *object_colors);
static void sprite(const Ppu *p, const Raster *r, int x, int sy, unsigned attr, int size,
                   int y, MmxRenderView view, uint16_t *out, bool margins_only,
                   const MmxSpriteAsset *asset, unsigned raw_tile, int *object_color,
                   bool full_coordinates, unsigned zero_icon, bool red_tint);
typedef struct OamSet { uint16_t oam[256]; uint8_t high[32]; uint8_t obsel; bool valid; } OamSet;
static OamSet hidden_oam, previous_oam;
static unsigned flicker_phase;
unsigned g_mmx_flicker_merged; /* sprites added to the last frame (stats) */
static int oam_visible(const OamSet *o, unsigned slot, int *x, int *y, int *size) {
  static const unsigned sizes[8][2] = {{8,16},{8,32},{8,64},{16,32},{16,64},{32,64},{16,32},{16,32}};
  unsigned pos = o->oam[slot * 2], hi = o->high[slot / 4] >> ((slot & 3) * 2);
  int sx = (pos & 255) | ((hi & 1) << 8); if (sx >= 256) sx -= 512;
  int sz = (int)sizes[o->obsel >> 5][(hi >> 1) & 1];
  if (sx + sz <= 0 || sx >= 256) return 0;
  unsigned sy = pos >> 8; bool rows = false;
  for (int r = 0; r < sz && !rows; ++r) rows = ((sy + (unsigned)r) & 255) < 224;
  if (!rows) return 0;
  if (x) *x = sx; if (y) *y = (int)sy; if (size) *size = sz;
  return 1 + ((hi >> 1) & 1);  /* 1 small, 2 large */
}
static bool oam_has_word(const OamSet *o, uint16_t word, int kind) {
  for (unsigned k = 0; k < 128; ++k)
    if (o->oam[k * 2 + 1] == word && oam_visible(o, k, NULL, NULL, NULL) == kind) return true;
  return false;
}
void MmxRendererNoteHiddenFrame(const Ppu *p) {
  if (!p) { hidden_oam.valid = false; return; }
  memcpy(hidden_rasters[hidden_stage].vram, p->vram, sizeof(hidden_rasters[0].vram));
  memcpy(hidden_oam.oam, p->oam, sizeof(hidden_oam.oam));
  memcpy(hidden_oam.high, p->highOam, sizeof(hidden_oam.high));
  hidden_oam.obsel = p->obsel; hidden_oam.valid = true;
}
static void merge_flicker_sprites(void) {
  g_mmx_flicker_merged = 0; flicker_count = 0;
  OamSet shown; Raster *first = &frame.lines[0];
  memcpy(shown.oam, first->oam, sizeof(shown.oam)); memcpy(shown.high, first->high_oam, sizeof(shown.high));
  shown.obsel = first->registers[offsetof(Ppu, obsel)]; shown.valid = true;
  bool uniform = true;
  for (unsigned y = 1; y < 224 && uniform; ++y)
    uniform = frame.lines[y].registers[offsetof(Ppu, obsel)] == shown.obsel &&
        !memcmp(frame.lines[y].oam, first->oam, sizeof(first->oam)) &&
        !memcmp(frame.lines[y].high_oam, first->high_oam, sizeof(first->high_oam));
  OamSet previous = previous_oam;
  previous_oam = shown; /* history keeps the unmerged frame */
  bool usable = uniform && hidden_oam.valid && previous.valid && hidden_oam.obsel == shown.obsel &&
      MmxWidePolicy_IsStageScene(frame.ram);
  hidden_oam.valid = false;
  ++flicker_phase;
  if (!usable) return;
  int xx = (int16_t)(word(frame.ram, 0xbad) - word(frame.ram, 0x1e4d));
  int xy = (int16_t)(word(frame.ram, 0xbb0) - word(frame.ram, 0x1e50));
  /* X's body uses the dynamic tiles $00-$1F around his position. */
#define X_PIECE(w, x, y, size) (((w) & 0x1ff) < 0x20 && (x) + (size) > xx - 24 && (x) < xx + 24 && \
                                (y) + (size) > xy - 32 && (y) < xy + 32)
  bool x_blinking = true;
  for (unsigned k = 0; k < 128 && x_blinking; ++k) {
    int x, y, size;
    if (oam_visible(&shown, k, &x, &y, &size) && X_PIECE(shown.oam[k * 2 + 1], x, y, size)) x_blinking = false;
  }
  flicker_count = 0;
  for (unsigned k = 0; k < 128; ++k) {
    int x, y, size, kind = oam_visible(&hidden_oam, k, &x, &y, &size);
    if (!kind) continue;
    uint16_t w = hidden_oam.oam[k * 2 + 1];
    if (oam_has_word(&shown, w, kind) || oam_has_word(&previous, w, kind)) continue;
    /* X visible this frame: his hidden-phase pieces are an older pose. */
    if (X_PIECE(w, x, y, size) && (!x_blinking || (flicker_phase & 1))) continue;
    /* Slot order: drawn from high to low like OAM, all behind the shown
     * frame's own sprites. */
    flicker_objects[flicker_count++] = (FlickerObject){x, y, size, w};
  }
  g_mmx_flicker_merged = flicker_count;
  if (flicker_count) {
    hidden_draw = &hidden_rasters[hidden_stage];
    memcpy(hidden_draw->palette, frame.lines[0].palette, sizeof(hidden_draw->palette));
    hidden_stage ^= 1;
  }
#undef X_PIECE
}
static void draw_flicker_objects(const Ppu *p, int y, MmxRenderView view, uint16_t *objects,
                                 int *object_colors) {
  if (!hidden_draw) return;
  for (int i = (int)flicker_count - 1; i >= 0; --i) {
    const FlickerObject *o = &flicker_objects[i];
    sprite(p, hidden_draw, o->x, o->y, o->attr, o->size, y, view, objects,
        false, NULL, 0, object_colors, false, false, false);
  }
}
bool MmxRendererEndFrame(const uint32_t stock[256 * 224]) {
  if (!stock || frame.captured != 224) return false;
  merge_flicker_sprites();
  /* With skipping, a frame without stock lines left none to copy. */
  if (!stock_skipping || stock_lines_needed)
    memcpy(frame.stock, stock, sizeof(frame.stock));
  ++frame_generation;
  return frame.valid = true;
}
MmxRenderStats MmxRendererGetStats(void) { return stats; }
const uint32_t *MmxRendererStockFrame(void) { return frame.valid ? frame.stock : NULL; }
void MmxRendererCoopFrame(const MmxCoopState *s) {
  if (!s || !s->initialized) { memset(&frame_coop,0,sizeof(frame_coop)); return; }
  frame_coop = *s;
  bool menu=((frame.ram[0x1f10]==6 || frame.ram[0x1f10]==8) && (frame.ram[0xc3]&128)) ||
      (frame.ram[0x1989]==1 && word(frame.ram,0x198d)==128 && word(frame.ram,0x1990)==160 &&
       (frame.ram[0x199e]==0 || frame.ram[0x199e]==0x18));
  if (menu) {
    const MmxCoopPlayer *owner=&s->players[s->menu_last];
    frame_zero=owner->zero;frame_weapons=owner->weapons;frame_weapon_combat=owner->combat;
    memcpy(frame.ram+0xba8,owner->body,sizeof(owner->body));
    /* The menu shows its owner's body, which need not be the live seat's. */
    MmxRenderPlayerOverlay overlay;
    memset(&overlay,0,sizeof(overlay));
    if (player_overlay_provider && player_overlay_provider(frame.ram,&frame_zero,&overlay))
      frame_player_overlay=overlay;
    else memset(&frame_player_overlay,0,sizeof(frame_player_overlay));
  }
  memcpy(partner_ram,frame.ram,sizeof(partner_ram));
  memcpy(partner_ram+0xba8,s->players[s->current^1].body,sizeof(s->players[s->current^1].body));
  memcpy(partner_ram+0xc38,s->players[s->current^1].auxiliaries,sizeof(s->players[s->current^1].auxiliaries));
  memcpy(partner_ram+0x1228,s->players[s->current^1].shots,sizeof(s->players[s->current^1].shots));
  const MmxCoopPlayer *partner=&s->players[s->current^1];
  memset(&frame_partner_overlay,0,sizeof(frame_partner_overlay));
  if (!menu && partner->character==MMX_COOP_ZERO && player_overlay_provider) {
    MmxRenderPlayerOverlay overlay;
    memset(&overlay,0,sizeof(overlay));
    if (player_overlay_provider(partner_ram,&partner->zero,&overlay))
      frame_partner_overlay=overlay;
  }
}
bool MmxRendererSaveCapture(const char *path) {
  if (!frame.valid || !path) return false;
  /* Sharing is presentation metadata, not a new file format. Materialize
   * only on an explicit capture save, outside the normal frame path. */
  for (unsigned y = 0; y < 224; ++y) if (vram_owner[y] != y) {
    memcpy(frame.lines[y].vram, frame.lines[vram_owner[y]].vram, sizeof(frame.lines[y].vram));
    vram_owner[y] = (uint8_t)y;
  }
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  uint32_t header[] = {0x4d4d5843, frame_coop.initialized ? 23 : 22,
      sizeof(frame) + sizeof(frame_zero) + sizeof(frame_weapons) + sizeof(frame_weapon_combat) +
      (frame_coop.initialized ? sizeof(frame_coop) : 0) + sizeof(frame_rush) + sizeof(frame_rush_visual)+sizeof(frame_bounds)};
  bool ok = fwrite(header, sizeof(header), 1, f) == 1 && fwrite(&frame, sizeof(frame), 1, f) == 1 &&
      fwrite(&frame_zero, sizeof(frame_zero), 1, f) == 1 &&
      fwrite(&frame_weapons, sizeof(frame_weapons), 1, f) == 1 &&
      fwrite(&frame_weapon_combat, sizeof(frame_weapon_combat), 1, f) == 1;
  if (ok && frame_coop.initialized) ok = fwrite(&frame_coop,sizeof(frame_coop),1,f) == 1;
  if(ok) ok=fwrite(&frame_rush,sizeof(frame_rush),1,f)==1;
  if(ok) ok=fwrite(&frame_rush_visual,sizeof(frame_rush_visual),1,f)==1;
  if(ok) ok=fwrite(&frame_bounds,sizeof(frame_bounds),1,f)==1;
  return fclose(f) == 0 && ok;
}
bool MmxRendererLoadCapture(const char *path) {
  if (!path) return false;
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  uint32_t h[3] = {0};
  frame.valid = false;
  reset_pilot_overlay_alignment();
  memset(&frame_zero, 0, sizeof(frame_zero));
  memset(&frame_weapons, 0, sizeof(frame_weapons));
  memset(&frame_weapon_combat, 0, sizeof(frame_weapon_combat));
  memset(&frame_coop,0,sizeof(frame_coop));
  memset(&frame_bounds,0,sizeof(frame_bounds));
  memset(&frame_rush,0,sizeof(frame_rush));
  memset(&frame_rush_visual,0,sizeof(frame_rush_visual));
  bool ok = fread(h, sizeof(h), 1, f) == 1 && h[0] == 0x4d4d5843 &&
      ((h[1] == 2 && h[2] == sizeof(frame)) || (h[1] == 3 && h[2] == sizeof(frame) + MMX_ZERO_LEGACY_STATE_SIZE) ||
       (h[1] == 4 && h[2] == sizeof(frame) + MMX_ZERO_ANIMATION_STATE_SIZE) ||
       (h[1] == 5 && h[2] == sizeof(frame) + MMX_ZERO_COMBAT_STATE_SIZE) ||
       (h[1] == 6 && h[2] == sizeof(frame) + MMX_ZERO_SWAP_STATE_SIZE) ||
       (h[1] == 7 && h[2] == sizeof(frame) + MMX_ZERO_HEALTH_STATE_SIZE) ||
       (h[1] == 8 && h[2] == sizeof(frame) + MMX_ZERO_HEALTH_STATE_SIZE + MMX_WEAPONS_LEGACY_STATE_SIZE) ||
       (h[1] == 9 && h[2] == sizeof(frame) + MMX_ZERO_HEALTH_STATE_SIZE + MMX_WEAPONS_LEGACY_STATE_SIZE + MMX_WEAPON_COMBAT_LEGACY_SIZE) ||
       (h[1] == 10 && h[2] == sizeof(frame) + MMX_ZERO_HEALTH_STATE_SIZE + sizeof(frame_weapons) + MMX_WEAPON_COMBAT_LEGACY_SIZE) ||
       (h[1] == 11 && h[2] == sizeof(frame) + MMX_ZERO_HEALTH_STATE_SIZE + sizeof(frame_weapons) + MMX_WEAPON_COMBAT_DAMAGE_SIZE) ||
       (h[1] == 12 && h[2] == sizeof(frame) + MMX_ZERO_HEALTH_STATE_SIZE + sizeof(frame_weapons) + sizeof(frame_weapon_combat)) ||
       (h[1] == 13 && h[2] == sizeof(frame) + MMX_ZERO_HEALTH_STATE_SIZE + sizeof(frame_weapons) + sizeof(frame_weapon_combat) + MMX_COOP_LEGACY_STATE_SIZE) ||
       (h[1] == 14 && h[2] == sizeof(frame) + sizeof(frame_zero) + sizeof(frame_weapons) + sizeof(frame_weapon_combat)) ||
       ((h[1] == 15 || h[1] == 17) && h[2] == sizeof(frame) + sizeof(frame_zero) + sizeof(frame_weapons) + sizeof(frame_weapon_combat) + sizeof(frame_coop)) ||
       (h[1] == 16 && h[2] == sizeof(frame) + sizeof(frame_zero) + sizeof(frame_weapons) + sizeof(frame_weapon_combat)) ||
       ((h[1]>=18 && h[1]<=23) && h[2]==sizeof(frame)+sizeof(frame_zero)+sizeof(frame_weapons)+sizeof(frame_weapon_combat)+sizeof(frame_rush)+((h[1]&1)?sizeof(frame_coop):0)+(h[1]>=20?sizeof(frame_rush_visual):0)+(h[1]>=22?sizeof(frame_bounds):0))) &&
      fread(&frame, sizeof(frame), 1, f) == 1 &&
      frame.captured == 224 && frame.piece_count <= MAX_PIECES && frame.expanded_count <= MAX_PIECES && frame.valid;
  size_t zero_size = h[1] == 3 ? MMX_ZERO_LEGACY_STATE_SIZE :
      h[1] == 4 ? MMX_ZERO_ANIMATION_STATE_SIZE :
      h[1] == 5 ? MMX_ZERO_COMBAT_STATE_SIZE :
      h[1] == 6 ? MMX_ZERO_SWAP_STATE_SIZE : h[1] < 14 ? MMX_ZERO_HEALTH_STATE_SIZE : sizeof(frame_zero);
  if (ok && h[1] >= 3) ok = fread(&frame_zero, zero_size, 1, f) == 1 &&
      frame_zero.active_x <= 1 && frame_zero.swap_phase <= 6 && frame_zero.swap_tick <= 30 &&
      frame_zero.swap_y >= -320 && frame_zero.swap_y <= 0 &&
      frame_zero.slash <= 46 && frame_zero.air <= 1 && frame_zero.anim_valid <= 1 && frame_zero.anim_pose < 117 &&
      frame_zero.burst <= 2 && (!frame_zero.burst || (frame_zero.burst_offset >= 272 &&
        frame_zero.burst_offset + 3 <= MMX_ZERO_ANIMATION_BYTES && frame_zero.burst_timer));
  if (ok && h[1] >= 8) ok = fread(&frame_weapons, h[1] >= 10 ? sizeof(frame_weapons) : MMX_WEAPONS_LEGACY_STATE_SIZE, 1, f) == 1 &&
      MmxWeaponsValidState(&frame_weapons);
  if (ok && h[1] >= 9) ok = fread(&frame_weapon_combat,
      h[1]>=12 ? sizeof(frame_weapon_combat) : h[1]==11 ? MMX_WEAPON_COMBAT_DAMAGE_SIZE : MMX_WEAPON_COMBAT_LEGACY_SIZE, 1, f) == 1 &&
      MmxWeaponsValidCombatState(&frame_weapon_combat);
  if (ok && h[1]<16) {
    /* Older captures only kept palette bits. Use their saved pose as the
     * best available approximation; all new captures retain submission. */
    for (unsigned list=0;list<2;++list) {
      Piece *pieces=list ? frame.expanded : frame.pieces;
      unsigned count=list ? frame.expanded_count : frame.piece_count;
      for (unsigned i=0;i<count;++i) {
        Piece *p=&pieces[i];
        if ((p->animation==0x0e || p->animation==0x9e) && p->object<0x1fe0) {
          unsigned pose=frame.ram[p->object+23]&31;
          p->palette_pose=(p->palette_pose&14)|(pose&1)|((pose&30)<<3);
        }
      }
    }
  }
  if (ok && (h[1] == 13 || h[1] == 15 || h[1] == 17 || h[1]==19 || h[1]==21 || h[1]==23)) {
    MmxCoopState coop;
    if (h[1] == 13) {
      uint8_t legacy[MMX_COOP_LEGACY_STATE_SIZE];
      ok = fread(legacy,sizeof(legacy),1,f) == 1;
      if (ok) MmxCoopImportLegacy(&coop,legacy);
    } else ok = fread(&coop,sizeof(coop),1,f) == 1;
    ok = ok && coop.initialized == 1;
    for (unsigned i=0;ok && i<2;++i) ok = coop.players[i].character <= MMX_COOP_ZERO &&
        coop.players[i].status <= MMX_COOP_FALLEN && MmxZeroValidState(&coop.players[i].zero) &&
        MmxWeaponsValidState(&coop.players[i].weapons) && MmxWeaponsValidCombatState(&coop.players[i].combat);
    if (ok) MmxRendererCoopFrame(&coop);
  }
  if(ok && h[1]>=18) ok=fread(&frame_rush,sizeof(frame_rush),1,f)==1 && MmxBossRushValidState(&frame_rush);
  if(ok && h[1]>=20) ok=fread(&frame_rush_visual,sizeof(frame_rush_visual),1,f)==1 && MmxBossRushVisualValidState(&frame_rush_visual);
  if(ok && h[1]>=22) ok=fread(&frame_bounds,sizeof(frame_bounds),1,f)==1 && MmxCoopViewsValidBoundsState(&frame_bounds);
  ok = ok && fgetc(f) == EOF;
  fclose(f);
  for (unsigned y = 0; y < 224; ++y) vram_owner[y] = (uint8_t)y;
  vram_copies = 224;
  frame.valid = ok; return ok;
}

bool MmxRendererStageTile(const uint8_t ram[0x20000], unsigned layer,
                          int x, int y, uint16_t *entry) {
  if (!ram || !entry || layer > 1 || x < 0 || y < 0 || x >= 8192 || y >= 8192) return false;
  unsigned layout = layer ? 0xec00 : 0xe800;
  unsigned screens = layer ? 0xa600 : 0x2000;
  unsigned screen = ram[layout + (y >> 8) * 32 + (x >> 8)];
  unsigned address = (screens + screen * 512 + ((y & 0xf0) << 1) + ((x & 0xf0) >> 3)) & 0xffff;
  unsigned metatile = word(ram, address);
  unsigned pointer = 0xb95 + layer * 3;
  unsigned table = word(ram, pointer) | (ram[pointer + 2] << 16);
  address = (table & 0xff0000) | ((table + metatile * 8 + ((y & 8) ? 4 : 0) + ((x & 8) ? 2 : 0)) & 0xffff);
  const uint8_t *tile = rom_at(address, 2);
  if (!tile) return false;
  *entry = (uint16_t)word(tile, 0); return true;
}
static bool door_body(int x, int y) {
  if (x < 0 || y < 0 || x >= 8192 || y >= 8192) return false;
  unsigned key = (y >> 4) * 512 + (x >> 4);
  if (door_cache[key]) return door_cache[key] == 2;
  door_cache[key] = 1;
  if (door_touched_count < sizeof(door_touched) / sizeof(*door_touched)) door_touched[door_touched_count++] = key;
  else door_touched_full = true;
  x &= ~15; y &= ~15;
  for (int row = 0; row < 3; ++row) {
    uint16_t entries[3][4];
    bool valid = true;
    for (int r = 0; r < 3; ++r) for (int q = 0; q < 4; ++q)
      valid &= MmxRendererStageTile(frame.ram, 0, x + (q & 1) * 8,
                                     y + (r - row) * 16 + (q >> 1) * 8, &entries[r][q]);
    if (valid && MmxWidePolicy_IsBossDoorBody(entries, row)) { door_cache[key] = 2; return true; }
  }
  return false;
}
static unsigned tile_pixel(const uint16_t *vram, unsigned address, int x, int y, unsigned bpp) {
  unsigned a = (address + y) & 0x7fff, shift = 7 - x;
  unsigned bits = vram[a] >> shift;
  unsigned pixel = (bits & 1) | ((bits >> 7) & 2);
  if (bpp == 4) { bits = vram[(a + 8) & 0x7fff] >> shift;
    pixel |= ((bits & 1) << 2) | ((bits >> 5) & 8); }
  return pixel;
}
static void prepare_stage_planes(void) {
  airport_sky_width = 0; submarine_count = 0;
  if (frame.ram[0x1f7a] == 5 && frame.ram[0x1e89] == 0x0e &&
      word(frame.ram, 0x1e90) == 0 && word(frame.ram, 0x1e50) >= 0x300) {
    /* The airport panorama ends partway through screen 2 (640 pixels in
     * the retail map). Later cells belong to other mechanisms and contain
     * intentional holes. Discover the continuous sky band from its top row
     * and reflect that edge, instead of exposing those unpainted cells. */
    unsigned x; uint16_t tile;
    for (x = 0; x < 1024; x += 8)
      if (!MmxRendererStageTile(frame.ram, 1, x, 0, &tile) || !(tile & 1023)) break;
    if (x >= 256 && x < 1024) airport_sky_width = (int)x;
  }
  if (frame.ram[0x1f7a] != 1) return;
  for (unsigned d = 0xe68; d <= 0x1228; d += 64) {
    const uint8_t *r = frame.ram;
    if (!r[d] || r[d + 10] != 0x21 || !(r[d + 11] & 0x80) || r[d + 1] != 0) continue;
    unsigned variant = r[d + 11] & 0x7f;
    if (variant >= 3) continue;
    /* $82:AE81 / $86:CBEC describe the buried submarine's BG1 body.
     * State 4 starts its real rise. Before that, the source-art rectangle
     * must stay empty even when an adaptive margin can already see it. */
    const uint8_t *top = rom_at(0x86cbec + variant * 2, 2);
    const uint8_t *bottom = rom_at(0x86cbf2 + variant * 2, 2);
    if (!top || !bottom || word(bottom, 0) < word(top, 0)) continue;
    unsigned shift = word(r, d + 0x34);
    bool scrolling = (r[d + 2] == 2 || r[d + 2] == 4) && r[0xba1] == 2 &&
        word(r, 0x1f28) == word(top, 0) && word(r, 0x1f2a) == word(bottom, 0) &&
        shift > 0 && shift <= 0x50;
    submarine_bodies[submarine_count++] = (SubmarineBody){
        ((int)word(r, d + 5)) & ~31, (int)word(top, 0), (int)word(bottom, 0) + 1,
        shift, r[d + 2] != 4, scrolling};
  }
}
static unsigned menu_glyph(unsigned c) {
  if (c >= 'A' && c <= 'L') return 0xc4 + c - 'A';
  if (c >= 'M' && c <= 'P') return 0xd4 + c - 'M';
  if (c >= 'R' && c <= 'W') return 0xd8 + c - 'R';
  return c == '.' ? 0xdf : c == 'X' ? 0x9d : 0;
}
static uint16_t weapon_menu_tile(unsigned tx, unsigned ty, uint16_t original) {
  if (!frame_weapons.menu_page || !MmxWeaponsMenuVisible(frame.ram) || ty < 5 || ty > 14) return original;
  unsigned column = tx >= 17, local = tx - (column ? 13 : 0), item = (ty - 5) / 2 + column * 5;
  if (!item || item > 8) return original;
  if (local == 4 || local == 5) return 0; /* Icons use their original source-game palette below. */
  if (!(ty & 1) && local >= 6 && local <= 14) {
    unsigned flags = frame.ram[0x1ed2] == item ? 0x0800 : 0x2800;
    if (local == 6) return (uint16_t)(flags | 0x82);
    if (local == 14) return (uint16_t)(flags | 0x4082);
    int filled = frame_weapons.energy[(frame_weapons.menu_page - 1) * 8 + item - 1] - (int)(local - 7) * 4;
    if (filled < 0) filled = 0;
    if (filled > 4) filled = 4;
    return (uint16_t)(flags | (0x83 + filled));
  }
  if ((ty & 1) && local >= 7 && local <= 14) {
    const char *label = MmxWeaponsLabel(frame_weapons.menu_page, item);
    unsigned i = local - 7;
    return (uint16_t)(i < strlen(label) ? 0x1c00 | menu_glyph((unsigned char)label[i]) : 0);
  }
  return original;
}
static int weapon_menu_icon(int x, int y) {
  if (!frame_weapons.menu_page || !MmxWeaponsMenuVisible(frame.ram) || y < 40 || y >= 120) return -1;
  int column = x >= 136, left = column ? 136 : 32;
  if (x < left || x >= left + 16) return -1;
  unsigned row = (y - 40) / 16, item = row + column * 5;
  if (!item || item > 8) return -1;
  const MmxWeaponPose *pose = MmxWeaponsIcon(frame_weapons.menu_page, item);
  const uint16_t *colors = MmxWeaponsIconPalette(frame_weapons.menu_page, item);
  if (!pose || !colors) return -1;
  /* Icons occupy the original 16x16 cells, centered without resampling. */
  int px = x - left - (16 - (int)pose->width) / 2;
  int py = (y - 40) % 16 - (16 - (int)pose->height) / 2;
  if (px < 0 || py < 0 || px >= pose->width || py >= pose->height) return -1;
  unsigned pixel = pose->pixels[py * pose->width + px];
  return pixel ? colors[pixel] : -1;
}
static uint16_t background(const Ppu *p, const Raster *r, unsigned layer, int x, int y, bool stage, int *private_color) {
  static const unsigned low[] = {8, 7, 1}, high[] = {12, 11, 3};
  if(stage && layer==0 && frame.ram[0x1f7a]==4 &&
      (frame_rush.mode==MMX_RUSH_PLAYING || frame_rush.mode==MMX_RUSH_PREPARING)) {
    int hook_x=frame_rush.camera_x+MMX_RUSH_ARENA_CENTER_X;
    int sx=x+view_camera(frame.ram,0x1e4d)-hook_x+32;
    int sy=y+view_camera(frame.ram,0x1e50)-(int)frame_rush.camera_y;
    int pixel=MmxRenderAssetsRushCeilingPixel((unsigned)sx,(unsigned)sy);
    if(pixel>=0) {
      *private_color=pixel&0x7fff;
      return (uint16_t)(((pixel&0x10000?high[0]:low[0])<<12)|1);
    }
  }
  bool margin = x < 0 || x >= 256;
  /* Launch's BG3 water plane is blended over the world on the subscreen.
   * Repeat that plane while retaining the vertical waterline/scroll. Other
   * BG3 uses, including dialogue, stay within their native screen bounds. */
  bool water = frame.ram[0x1f7a] == 1 && (p->screenEnabled[0] & 4) &&
      !(p->screenEnabled[1] & 4) && (p->cgwsel & 2) && (p->cgadsub & 0x44) == 0x44;
  if (stage && layer == 2 && margin && !water) return 0;
  /* Thunder Slimer's moving BG2 surface is already resident in VRAM.
   * Draw it through the margins using its live scroll, rather than hiding
   * it at 4:3 or reconstructing dormant bubbles from the stage map. */
  bool slime_surface=stage && layer==1 && frame.ram[0x1f7a]==6 && frame.ram[0x1e89]==0x0c;
  int asset_x = -1;
  bool armadillo_lower_shaft = false;
  unsigned bpp = layer == 2 ? 2 : 4, size = PPU_bigTiles(p, layer) ? 16 : 8;
  int px = (x + p->hScroll[layer]) & 1023, py = (y + p->vScroll[layer]) & 1023;
  if(stage && layer==2 && water) py=(py+view_dy)&1023;
  unsigned sc = p->bgXsc[layer], tx = px / size, ty = py / size;
  unsigned a = (sc & 0xfc) * 256 + (tx & 31) + (ty & 31) * 32;
  if ((sc & 1) && (tx & 32)) a += 1024;
  if ((sc & 2) && (ty & 32)) a += (sc & 1) ? 2048 : 1024;
  uint16_t tile = drawing_vram[a & 0x7fff];
  if(!stage && layer==2 && frame_rush.menu && (frame_rush.mode==MMX_RUSH_OFF ||
      (frame_rush.mode==MMX_RUSH_LOADING && !frame_rush.stage_started))) {
    /* Native title lettering is ASCII-indexed BG3 CHR. Keep its spacing,
     * cyan/orange palettes, raster brightness and priority for all four rows. */
    if((ty==20 || ty==22 || ty==24) && tx>=10 && tx<21)
      tile=(uint16_t)((tile&~0x1c00)|(((ty-20)/2==frame_rush.selection)?0x400:0));
    if(ty==26 && tx>=10 && tx<19)
      tile=(uint16_t)(0x2000|(frame_rush.selection==3?0x400:0)|"BOSS RUSH"[tx-10]);
  }
  if (layer == 0 && !stage) {
    int icon = weapon_menu_icon(x, y);
    if (icon >= 0) { *private_color = icon; return 0x8001; }
    tile = weapon_menu_tile(tx, ty, tile);
  }
  if (stage && size == 8 && layer < 2 && !slime_surface &&
      (x < 0 || x >= 256 || view_dx || view_dy)) {
    int wx, wy;
    if (layer == 0) {
      wx = MmxDisplay_ExpandStageScroll((uint16_t)word(frame.ram, 0x1e4d), p->hScroll[0]) + view_dx + x;
      wy = MmxDisplay_ExpandStageScroll((uint16_t)word(frame.ram, 0x1e50), p->vScroll[0]) + view_dy + y;
      /* $81:F9B7 joins the shaft at camera $1F00,$0600 to $0100,$0800.
       * Project its continuation before that native-coordinate relocation,
       * so the wide sides already show the lower room while X falls. */
      if (frame.ram[0x1f7a] == 3 && word(frame.ram, 0x1e4d) == 0x1f00 &&
          word(frame.ram, 0x1e50) >= 0x500 && word(frame.ram, 0x1e50) <= 0x600 && wy >= 0x600) {
        wx -= 0x1e00; wy += 0x200; armadillo_lower_shaft = true;
      }
      for (unsigned i = 0; i < submarine_count; ++i) {
        const SubmarineBody *body = &submarine_bodies[i];
        /* $82:B414 subtracts the burial offset into $C4; the $80:84CB
         * raster IRQ applies it to the whole BG1 scanline. Only the body
         * occupies that band in 4:3. Restore the terrain's original scroll
         * outside its columns in the wider view, including during the rise.
         * Match the captured IRQ value so other raster bands stay intact. */
        if (body->scrolling && (p->vScroll[0] & 1023) == (word(frame.ram, 0xc4) & 1023)) {
          if (wx < body->left || wx >= body->left + 160) wy += body->shift;
          break;
        }
      }
    } else {
      int stream_x = (int16_t)word(frame.ram, 0x1e8d), stream_y = (int16_t)word(frame.ram, 0x1e90);
      wx = stream_x + (((p->hScroll[1] - stream_x + 512) & 1023) - 512) + x;
      wy = stream_y + (((p->vScroll[1] - stream_y + 512) & 1023) - 512) + y;
      /* $00:DF08 dispatches BG2 follow modes. Retain the raster offset and
       * apply the exact half/full camera relation; actor/script modes stay
       * canonical while their shared scene owns both views. */
      unsigned mode=frame.ram[0x1e89];
      int nx=word(frame.ram,0x1e4d),ny=word(frame.ram,0x1e50);
      if(mode==2 || mode==14) wx+=((nx+view_dx)>>1)-(nx>>1);
      else if(mode==4 || mode==16) wx+=view_dx;
      if(mode==2 || mode==16) wy+=((ny+view_dy)>>1)-(ny>>1);
      else if(mode==4 || mode==14) wy+=view_dy;
      /* The boat's BG2 body occupies one source screen; surrounding map
       * cells are staging art. The controller scrolls that screen into view. */
      if (frame.ram[0x1f7a] == 1 && frame.ram[0x1e89] == 0x0c &&
          (wx < 0xb00 || wx >= 0xc00 || wy < 0 || wy >= 256)) return 0;
      /* The sea floor behind the raised bank was never exposed by the
       * native camera: its $0370..038F band includes foreground scraps.
       * Continue the adjacent authored seabed across that occluded band. */
      if (frame.ram[0x1f7a] == 1 && frame.ram[0x1e89] == 0x0e &&
          wx >= 0x500 && wx < 0x700 && wy >= 0x370 && wy < 0x390)
        wx = 0x700 | (wx & 255);
      /* The serpent's sea backdrop begins at source $C00. Its preceding
       * actor staging cells are empty; extend the sea edge into the margin. */
      if (frame.ram[0x1f7a] == 1 && frame.ram[0x1e89] == 0x0e &&
          stream_x >= 0xc00 && stream_x < 0xe00 && wx < 0xc00) wx = 0xc00;
      /* Highway's final arena switches to the sky plane at BG2 x=$A00.
       * Earlier columns are intentionally empty at this vertical scroll;
       * extend the arena's sky edge when a wide view reaches behind it. */
      if (frame.ram[0x1f7a] == 0 && stream_x >= 0xa00 && wx < 0xa00) wx = 0xa00;
    }
    /* Reconstruct prepared map data independently of circular VRAM history.
     * Outside authored terrain, reflect only the background edge. */
    if (wx < 0) wx = -wx - 1;
    if (layer == 1 && airport_sky_width && wx >= airport_sky_width) {
      wx %= 2 * airport_sky_width;
      if (wx >= airport_sky_width) wx = 2 * airport_sky_width - wx - 1;
    }
    if (layer == 0) {
      unsigned stage_id = frame.ram[0x1f7a];
      if (stage_id < 13 && rom_size > 0x30d24 + stage_id * 3 + 2) {
        const uint8_t *t = rom + 0x30d24 + stage_id * 3;
        const uint8_t *bounds = rom_at(word(t, 0) | (t[2] << 16), 2);
        if (bounds && bounds[0] && bounds[0] <= 32) {
          int width = bounds[0] * 256;
          if (wx >= width) wx = width * 2 - wx - 1;
          if (wx < 0) wx = 0;
        }
      }
      for (unsigned i = 0; i < submarine_count; ++i) {
        const SubmarineBody *body = &submarine_bodies[i];
        if (body->hidden && wx >= body->left && wx < body->left + 160 && wy >= body->top && wy < body->bottom) {
          wx -= 256; break; /* The preceding water screen has no source body. */
        }
      }
      if (door_body(wx, wy)) {
        bool left = door_body(wx - 16, wy), right = door_body(wx + 16, wy);
        if (left || right) {
          int boundary = (wx & ~15) + (right ? 16 : 0);
          bool view_left = view_camera(frame.ram, 0x1e4d) + 128 < boundary;
          /* Retain the column facing the current room. Only its duplicate
           * samples the neighboring wall; a distant closed door stays visible. */
          if (view_left && left) wx += 16;
          if (!view_left && right) wx -= 16;
        }
      }
    }
    uint16_t mapped;
    if (MmxRendererStageTile(frame.ram, layer, wx, wy, &mapped)) {
      tile = mapped; px = wx; py = wy;
      /* These backdrops move at half speed. Express the map column as the player
       * X at which it crosses the native view's center (camera+128). */
      asset_x = layer == 1 && (frame.ram[0x1f7a] <= 2 || frame.ram[0x1f7a] == 4) ? wx * 2 - 128 : wx;
      /* Late Launch BG2 remains ocean/ruins behind the hallway. The later
       * foreground palette events belong to the cliff and boss room. */
      if (layer == 1 && frame.ram[0x1f7a] == 1 && frame.ram[0x1e89] == 0x0e &&
          word(frame.ram, 0x1e8d) >= 0xc00 && asset_x >= 0x1a20) asset_x = 0x1a1f;
      /* Chill's BG2 sky palette also changes with elevation. Its cave-exit
       * X transition owns foreground art only; keep the live sky colors. */
      if (layer == 1 && frame.ram[0x1f7a] == 8) asset_x = -1;
      /* Highway's incoming airship uses BG2 as a moving actor surface.
       * Its source columns are not terrain coordinates: projecting them
       * selects earlier road palettes/CHR across the native view seams.
       * The complete ship resources are resident; retain its live binding. */
      if (layer == 1 && frame.ram[0x1f7a] == 0 && frame.ram[0x1e89] == 0x0c)
        asset_x = -1;
    }
  }
  int cx = px & (size - 1), cy = py & (size - 1);
  if (tile & 0x4000) cx = size - 1 - cx;
  if (tile & 0x8000) cy = size - 1 - cy;
  unsigned number = ((tile & 1023) + cx / 8 + cy / 8 * 16) & 1023;
  unsigned address = (PPU_bgTileAdr(p, layer) + number * bpp * 4) & 0x7fff;
  const uint8_t *bits = stage && g_mmx_render_asset_repairs && bpp == 4 && !(frame.ram[0x1f7a] == 1 && layer == 1) ?
      MmxRenderAssetsBackgroundTile(frame.ram, asset_x, address) : NULL;
  unsigned pixel;
  if (bits) {
    bits += (cy & 7) * 2;
    unsigned shift = 7 - (cx & 7);
    pixel = ((bits[0] >> shift) & 1) | (((bits[1] >> shift) & 1) << 1) |
        (((bits[16] >> shift) & 1) << 2) | (((bits[17] >> shift) & 1) << 3);
  } else pixel = tile_pixel(drawing_vram, address, cx & 7, cy & 7, bpp);
  if (!pixel) return 0;
  unsigned index = (((tile >> 10) & 7) << bpp) | pixel;
  const MmxBackgroundPalette *palette = stage && g_mmx_render_asset_repairs ?
      (armadillo_lower_shaft ? MmxRenderAssetsBackgroundPalettePhase(frame.ram, 1) :
       MmxRenderAssetsBackgroundPalette(frame.ram, asset_x)) : NULL;
  if (palette && palette->valid[index]) *private_color = palette->colors[index];
  if(stage && layer==0 && frame.ram[0x1f7a]==4 && index>=0x50 && index<0x60 &&
      (frame_rush.mode==MMX_RUSH_PLAYING || frame_rush.mode==MMX_RUSH_PREPARING)) {
    int wy=y+view_camera(frame.ram,0x1e50)-(int)frame_rush.camera_y;
    if(wy>=192 && wy<224) {
      /* The belt animates by cycling BG palette $50. Match its physical
       * carry: use live colors with Mammoth, the native first phase otherwise.
       * Explicit live colors also keep expanded margins animating in sync. */
      const MmxBackgroundPalette *belt=MmxRenderAssetsBackgroundPalettePhase(frame.ram,(frame.ram[0x1f96]&0x40)?9:4);
      if(MmxBossRushConveyorRunning(&frame_rush)) *private_color=r->palette[index];
      else if(belt && belt->valid[index]) *private_color=belt->colors[index];
    }
  }
  unsigned priority = tile & 0x2000 ? (layer == 2 && (p->bgmode & 8) ? 15 : high[layer]) : low[layer];
  return (uint16_t)((priority << 12) | (layer << 8) | index);
}
static void sprite(const Ppu *p, const Raster *r, int x, int sy, unsigned attr, int size,
                    int y, MmxRenderView view, uint16_t *out, bool margins_only,
                    const MmxSpriteAsset *asset, unsigned raw_tile, int *object_color,
                    bool full_coordinates, unsigned zero_icon, bool red_tint) {
  int row = full_coordinates ? y - sy : (y - sy) & 255;
  if (row < 0 || row >= size) return;
  if (attr & 0x8000) row = size - 1 - row;
  unsigned base = (p->obsel & 7) * 8192;
  if (attr & 256) base += (((p->obsel >> 3) & 3) + 1) * 4096;
  unsigned z = ((((attr >> 12) & 3) * 4 + 2) << 12) |
               ((attr & 0x800 ? 4 : 6) << 8) | (128 + ((attr >> 9) & 7) * 16);
  for (int c = 0; c < size; ++c) {
    int dx = x + c, dest = dx + view.extra;
    if (dest < 0 || dest >= view.width || (margins_only && dx >= 0 && dx < 256)) continue;
    int cx = attr & 0x4000 ? size - 1 - c : c;
    unsigned number = asset && !asset->live_tiles ? raw_tile : attr & 255;
    unsigned tile = ((((number >> 4) + row / 8) & 15) << 4) | (((number & 15) + cx / 8) & 15);
    unsigned pixel;
    if (asset && !asset->live_tiles) {
      const uint8_t *bits = asset->tiles + tile * 32 + (row & 7) * 2;
      unsigned shift = 7 - (cx & 7);
      pixel = ((bits[0] >> shift) & 1) | (((bits[1] >> shift) & 1) << 1) |
          (((bits[16] >> shift) & 1) << 2) | (((bits[17] >> shift) & 1) << 3);
    } else pixel = tile_pixel(sprite_vram(r), base + tile * 16, cx & 7, row & 7, 4);
    int hud_color = zero_icon ? MmxZeroHudColor(cx, row) : -1;
    if (hud_color >= 0) pixel = 1;
    else if (hud_color == -2) pixel = 0;
    if (pixel) {
      out[dest] = (uint16_t)(z | pixel);
      object_color[dest] = hud_color >= 0 ? hud_color :
          asset && !asset->live_colors ? asset->colors[pixel] : -1;
      if (red_tint) {
        unsigned color=object_color[dest]>=0 ? (unsigned)object_color[dest] : r->palette[(z|pixel)&255];
        unsigned red=color&31,green=(color>>5)&31,blue=(color>>10)&31;
        /* Keep the original highlights/neutral outline. Convert only the
         * blue ramp to red, using its existing dark-to-light shading. */
        if (blue>red && blue>green)
          object_color[dest]=(int)(blue | ((red<green ? red : green)<<5) | (red<<10));
      }
      if (x + c < 0 || x + c >= 256) ++stats.margin_sprite_pixels;
    }
  }
}
static bool window(const Ppu *p, int layer, int x, int extra) {
  unsigned flags = (p->windowsel >> (layer * 4)) & 15;
  int l1 = p->window1left == 0 ? -extra : p->window1left;
  int r1 = p->window1right == 255 ? 255 + extra : p->window1right;
  int l2 = p->window2left == 0 ? -extra : p->window2left;
  int r2 = p->window2right == 255 ? 255 + extra : p->window2right;
  bool a = (x >= l1 && x <= r1) != ((flags & 1) != 0);
  bool b = (x >= l2 && x <= r2) != ((flags & 4) != 0);
  if (!(flags & 2)) return (flags & 8) && b;
  if (!(flags & 8)) return a;
  switch ((p->wbgobjlog >> (layer * 2)) & 3) {
    case 0: return a || b; case 1: return a && b; case 2: return a != b; default: return a == b;
  }
}
/* A window masks destination pixels, not an entire depth plane. Keep the
 * world sampled in stereo outside dialogue; BG3 text and OBJ remain fixed. */
/* Revision 16: the stage-select screen ($D1/$D2/$D3 = 2/0/1). BG2 holds
 * the central picture (Sigma or the hovered stage), BG1 the portraits and
 * panel, BG3 the frame, OBJ the cursor. The picture sits behind the frame
 * at the scene-plane depth, the portraits a third as deep, frame and cursor
 * at the screen. */
static bool stage_select_scene(void) {
  return frame.ram[0xd1] == 2 && frame.ram[0xd2] == 0 && frame.ram[0xd3] == 1;
}
/* v1.0.1: the remaining mode-1 screens outside the stages (title, intro,
 * password, menus) drawn flat by both compositors instead of copying the
 * stock PPU's pixels. */
static bool flat_native_scene(void) {
  if (getenv_select_stock()) return false;
  return frame.ram[0xd1] == 0 && frame.ram[0xd2] == 0 && frame.ram[0xd3] == 0;
}
unsigned MmxRendererStereoLayer(void) {
  return stage_select_scene() ? 1u : MmxStereoStageLayer(frame.ram[0x1f7a]);
}
static bool stereo_plane_allowed(const Ppu *p) {
  unsigned mask = 1u << MmxRendererStereoLayer();
  return !((p->mosaic & mask) && (p->mosaic & 0xf0)) &&
      !(frame.ram[0x1f7a] == 6 && frame.ram[0x1e89] == 0x0c);
}
/* Revision 16 (3DS): depth inside BG1. Where the scene plane (BG2) is
 * hidden -- Chill Penguin's base and arena are painted entirely on BG1 --
 * a frame had no visible disparity. The stage's own collision table tells
 * terrain from scenery: a BG1 block with no collision (back walls, pillars,
 * windows) is sampled at half the scene plane's disparity, solid blocks
 * (floors, walls, doors) and every actor stay at zero. Front pixels win;
 * where a shifted sample would land under terrain, the nearest scenery
 * pixel continues the wall instead of exposing BG2. Presentation only; at
 * zero depth nothing changes. Both compositors share these helpers. */
static unsigned native_window_layers(const Ppu *p);
bool g_mmx_stereo_bg1_depth = true;
static unsigned bg1_frame_level; /* 0..BG1_LEVELS, eased per frame */
enum { BG1_LEVELS = 8 };
static int bg1_depth_offset(int shift) {
  int half = shift > 0 ? (shift + 1) / 2 : shift < 0 ? -((-shift + 1) / 2) : 0;
  return half * (int)bg1_frame_level / BG1_LEVELS;
}
static const uint8_t *bg1_collision_table(void) {
  unsigned ptr = frame.ram[0xb92] | (frame.ram[0xb93] << 8) | ((unsigned)frame.ram[0xb94] << 16);
  return rom_at(ptr, 0x400);
}
/* Revision 17: BG1 scenery depth is for rooms painted on BG1 alone (Chill
 * Penguin's base, the boss arenas). Where the scene plane is visible it
 * already carries the depth, and pushing BG1 decorations back as well
 * detaches them from the terrain they hang on (highway posts, Spark
 * Mandrill's rungs). Decided once per frame from a sparse sample of the
 * main screen: BG2 winning over BG1 on more than ~1% of the samples turns
 * it off (3% with hysteresis to turn it back off once on). */
static bool bg1_frame_state;
static unsigned bg1_frame_generation = UINT_MAX;
static bool bg1_frame_depth(void) {
  if (bg1_frame_generation == frame_generation) return bg1_frame_level > 0;
  bg1_frame_generation = frame_generation;
  const uint16_t *saved = drawing_vram;
  unsigned total = 0, visible = 0;
  for (int y = 8; y < 224; y += 16) {
    const Raster *r = &frame.lines[y]; Ppu p;
    memcpy(&p, r->registers, PPU_SAVESTATE_REGS_SIZE);
    if ((p.inidisp & 128) || !(p.inidisp & 15) || (p.bgmode & 7) != 1) continue;
    total += 64;
    if (!(p.screenEnabled[0] & 2)) continue;
    drawing_vram = frame.lines[vram_owner[y]].vram;
    for (int x = 2; x < 256; x += 4) {
      int c1 = -1, c2 = -1;
      uint16_t v2 = background(&p, r, 1, x, y + 1, true, &c2);
      if (!(v2 & 255)) continue;
      uint16_t v1 = (p.screenEnabled[0] & 1) ? background(&p, r, 0, x, y + 1, true, &c1) : 0;
      if (!(v1 & 255) || v2 > v1) ++visible;
    }
  }
  drawing_vram = saved;
  bool on = bg1_frame_state ? visible * 100 <= total * 3 : visible * 100 < total;
  bg1_frame_state = total && on;
  /* Ease in/out over a few frames instead of popping at the threshold. */
  if (bg1_frame_state && bg1_frame_level < BG1_LEVELS) ++bg1_frame_level;
  else if (!bg1_frame_state && bg1_frame_level) --bg1_frame_level;
  return bg1_frame_level > 0;
}
/* BG1 and BG2 may sit on different screens (Sting Chameleon's fight moves
 * BG2 to the subscreen for his camouflage): BG1 scenery then follows BG1's
 * own enables. */
static bool bg1_depth_active(const Ppu *p, int shift) {
  if (!shift || MmxRendererStereoLayer() != 1 || !g_mmx_stereo_bg1_depth) return false;
  if (!(p->screenEnabled[0] & 1)) return false;
  if (p->mosaic & 1) return false;
  if (native_window_layers(p)) return false;
  if (PPU_bigTiles(p, 0)) return false;
  if (bg1_collision_table() == NULL) return false;
  return bg1_frame_depth();
}
/* High-priority BG1 tiles are drawn over the actors: always foreground. */
static void bg1_priority_front(uint8_t *back, const uint16_t *row, int n) {
  for (int i = 0; i < n; ++i) if ((row[i] & 255) && (row[i] & 0x4000)) back[i] = 0;
}
static bool bg1_select_depth_active(const Ppu *p) {
  if ((p->screenEnabled[0] & 1) != ((p->screenEnabled[0] >> 1) & 1) ||
      (p->screenEnabled[1] & 1) != ((p->screenEnabled[1] >> 1) & 1)) return false;
  return (p->screenEnabled[0] & 1) && !(p->mosaic & 1) && !native_window_layers(p);
}
/* back[x - lo] = 1 when the BG1 block under screen column x has no
 * collision. y is the BG line (screen line + 1), as background() takes. */
/* Collision code of the BG1 world block at (wx, wy); -1 outside the map. */
static int bg1_block_code(const uint8_t *table, int wx, int wy) {
  if (!table || wx < 0 || wy < 0 || wx >= 8192 || wy >= 8192) return -1;
  unsigned screen = frame.ram[0xe800 + (wy >> 8) * 32 + (wx >> 8)];
  unsigned address = (0x2000 + screen * 512 + ((wy & 0xf0) << 1) + ((wx & 0xf0) >> 3)) & 0xffff;
  return table[word(frame.ram, address) & 0x3ff];
}
static inline bool bg1_ladder_code(int code) { return code == 0x12 || code == 0x13; }
static void bg1_back_row(const Ppu *p, int y, int lo, int hi, uint8_t *back) {
  const uint8_t *table = bg1_collision_table();
  int wx0 = MmxDisplay_ExpandStageScroll((uint16_t)word(frame.ram, 0x1e4d), p->hScroll[0]);
  int wy = MmxDisplay_ExpandStageScroll((uint16_t)word(frame.ram, 0x1e50), p->vScroll[0]) + y;
  /* v1.0.1: a block row is 16 lines tall, so consecutive lines usually ask
   * for the same answer. Cached per finished frame and request. */
  static uint8_t cache[1024]; static bool cache_valid;
  static unsigned cache_generation; static const uint8_t *cache_table;
  static int cache_wx0, cache_wy, cache_lo, cache_hi;
  const bool cacheable = hi > lo && hi - lo <= (int)sizeof(cache);
  if (cacheable && cache_valid && cache_generation == frame_generation && cache_table == table &&
      cache_wx0 == wx0 && cache_wy == (wy >> 4) && cache_lo == lo && cache_hi == hi) {
    memcpy(back, cache, (size_t)(hi - lo));
    return;
  }
  for (int x = lo; x < hi;) {
    int wx = wx0 + x, span = 8 - (wx & 7);
    if (x + span > hi) span = hi - x;
    /* 0 = air, $0D = water (Launch Octopus): passable scenery. Slopes
     * ($01-$0C), ladders ($12/$13) and solids ($36-$3F) are terrain. */
    int code = bg1_block_code(table, wx, wy);
    uint8_t value = code == 0 || code == 0x0d;
    /* v1.0.1: a ladder's rails overhang its 16-pixel block by up to 8
     * pixels on each side (Spark Mandrill's dark shafts). Those columns stay
     * with the ladder so it is never split between two depths. */
    if (value && (bg1_ladder_code(bg1_block_code(table, wx - 8, wy)) ||
                  bg1_ladder_code(bg1_block_code(table, wx + 8, wy)))) value = 0;
    memset(back + (x - lo), value, (size_t)span);
    x += span;
  }
  if (cacheable) {
    memcpy(cache, back, (size_t)(hi - lo)); cache_valid = true;
    cache_generation = frame_generation; cache_table = table;
    cache_wx0 = wx0; cache_wy = wy >> 4; cache_lo = lo; cache_hi = hi;
  }
}
/* map[x] = source column for screen column x (x = 0..count-1 measured from
 * first) or INT16_MIN for an opaque terrain pixel, which stays put. */
static void bg1_remap(const uint8_t *back, const uint16_t *row, int lo, int hi,
                      int first, int count, int s, int16_t *map) {
  for (int i = 0; i < count; ++i) {
    int x = first + i;
    if (!back[x - lo] && (row[x - lo] & 255)) { map[i] = INT16_MIN; continue; }
    int c = x + s, step = s > 0 ? -1 : 1;
    if (c < lo) c = lo; if (c >= hi) c = hi - 1;
    while (c != x && !back[c - lo]) c += step;
    map[i] = (int16_t)c;
  }
}
static int frame_stereo_offset(MmxStereoEye eye, unsigned amount) {
  if (stage_select_scene()) return MmxStereoBackgroundOffset(eye, amount);
  return MmxStereoStageOffset(eye, amount, frame.ram[0x1f7a]);
}
unsigned MmxRendererStereoDepthPixels(unsigned amount) {
  if (!frame.valid || !MmxWidePolicy_IsStageScene(frame.ram)) return 0;
  for (unsigned y = 0; y < 224; ++y) {
    Ppu p; memcpy(&p, frame.lines[y].registers, PPU_SAVESTATE_REGS_SIZE);
    if (!(p.inidisp & 128) && (p.inidisp & 15) && (p.bgmode & 7) == 1 &&
        ((p.screenEnabled[0] | p.screenEnabled[1]) & (1u << MmxRendererStereoLayer())) && stereo_plane_allowed(&p))
      return (unsigned)frame_stereo_offset(MMX_STEREO_LEFT, amount);
  }
  return 0;
}
typedef struct LightBeam { int left[224], right[224]; } LightBeam;
static unsigned spark_lights(LightBeam beams[2], int extra) {
  /* $87:A7F6 builds 8-bit HDMA windows from the ROM's rounded beam profile.
   * Rebuild only that color window in signed host coordinates. The native
   * generator clamps a left-moving light at zero and waits for a right-side
   * arrival to enter 256 pixels; neither limitation describes a wide view. */
  if (!g_mmx_render_asset_repairs || frame.ram[0x1f7a] != 6 ||
      frame.ram[0x1f0a] != 4 || frame.ram[0x1e89] != 2) return 0;
  const uint8_t *curve = rom_at(0x86d136, 25);
  if (!curve) return 0;
  /* Sprite/HDMA submission precedes the next camera update in frame.ram.
   * Use the captured raster scroll, as terrain reconstruction does. */
  int camera_x = MmxDisplay_ExpandStageScroll((uint16_t)word(frame.ram, 0x1e4d),
      (uint16_t)word(frame.lines[0].registers, 14));
  int camera_y = MmxDisplay_ExpandStageScroll((uint16_t)word(frame.ram, 0x1e50),
      (uint16_t)word(frame.lines[0].registers, 22));
  camera_x+=view_dx;camera_y+=view_dy;
  unsigned count = 0;
  for (unsigned d = 0xe68; d <= 0x1228 && count < 2; d += 64) {
    const uint8_t *r = frame.ram;
    if (!r[d] || r[d + 10] != 0x37 || r[d + 1] != 2 || r[d + 0x1c] ||
        (r[d + 0x2d] != 0x40 && r[d + 0x2d] != 0x80)) continue;
    bool right = r[d + 0x0b] == 1, fading = r[d + 3] != 0;
    int tip = (int16_t)(word(r, d + 0x22) - camera_x) + (right ? -24 : 24);
    /* A fading beam keeps the native screen-space top cached at A6F7.
     * Its world position still needs the peer camera's vertical offset. */
    int top = fading ? (int16_t)word(r, d + 0x36) - view_dy :
        (int16_t)(word(r, d + 0x24) - camera_y) - 24;
    unsigned height = fading ? r[d + 0x3b] : 49;
    int inset = fading ? r[d + 0x1f] : 0;
    LightBeam *beam = &beams[count++];
    for (int y = 0; y < 224; ++y) { beam->left[y] = 1; beam->right[y] = 0; }
    unsigned index = 0, remaining = curve[0]; int edge = fading ? 0 : 12;
    for (unsigned row = 0; row < height && row < 49; ++row) {
      int y = top + (int)row;
      if (y >= 0 && y < 224) {
        beam->left[y] = right ? -extra + inset : tip + edge;
        beam->right[y] = right ? tip - edge : 255 + extra - inset;
      }
      if (!fading) {
        if (remaining) --remaining;
        else if (index < 25) {
          remaining = curve[index++];
          if (remaining >= 5) { remaining -= 5; ++edge; } else --edge;
        }
      }
    }
  }
  return count;
}
static bool condition(unsigned mode, bool inside) { return mode == 3 || (mode == 1 && !inside) || (mode == 2 && inside); }
static uint32_t colour(const Ppu *p, const uint16_t *palette, const uint8_t brightness[32], uint16_t main, uint16_t sub, bool inside, int object_color, const int bg_colors[3], bool dialogue_margin, uint16_t camouflage_bg) {
  unsigned rgb = palette[main & 255], layer = (main >> 8) & 15;
  if (object_color >= 0 && (layer == 4 || layer == 6)) rgb = (unsigned)object_color;
  if (layer < 3 && bg_colors[layer] >= 0) rgb = (unsigned)bg_colors[layer];
  bool camouflage=object_color>=0 && (object_color&0x8000) && (layer==4 || layer==6);
  if(camouflage) {
    /* $88:85DE selects full additive OBJ/subscreen math. Black fades into
     * the room; applying it privately leaves X and the other boss alone. */
    unsigned bg_layer=(camouflage_bg>>8)&15;
    unsigned backdrop=bg_layer<3 && bg_colors[bg_layer]>=0?(unsigned)bg_colors[bg_layer]:palette[camouflage_bg&255];
    unsigned added=0;
    for(unsigned shift=0;shift<15;shift+=5) {
      unsigned component=((rgb>>shift)&31)+((backdrop>>shift)&31);
      added|=(component>31?31:component)<<shift;
    }
    rgb=added;
  }
  bool clipped = condition(p->cgwsel >> 6, inside);
  bool math = !condition((p->cgwsel >> 4) & 3, inside) && ((p->cgadsub & 63) & (1u << layer));
  if ((dialogue_margin && layer == 5) || camouflage) math = false;
  unsigned other = p->fixedColor;
  bool half = math && (p->cgadsub & 64) && !clipped;
  if (math && (p->cgwsel & 2)) {
    if (sub & 255) {
      unsigned sub_layer = (sub >> 8) & 15;
      other = object_color >= 0 && (sub_layer == 4 || sub_layer == 6) ? (unsigned)object_color : palette[sub & 255];
      if (sub_layer < 3 && bg_colors[sub_layer] >= 0) other = (unsigned)bg_colors[sub_layer];
    } else half = false;
  }
  uint32_t result = 0;
  for (int component = 0; component < 3; ++component) {
    int c = clipped ? 0 : (rgb >> (component * 5)) & 31;
    if (math) { int second = (other >> (component * 5)) & 31;
      c += p->cgadsub & 128 ? -second : second;
      if (c < 0) c = 0;
      if (half) c /= 2;
      if (c > 31) c = 31;
    }
    result |= (uint32_t)brightness[c] << (16 - component * 8);
  }
  return result;
}
static void weapon_sprite_row(const MmxWeaponShot *s, unsigned pose, int x, int sy,
                              int y, MmxRenderView view, uint16_t *objects, int *colors) {
  const MmxWeaponPose *p = MmxWeaponsPose(s->page,s->weapon,s->group,pose);
  const uint16_t *palette = MmxWeaponsGroupPalette(s->page,s->weapon,s->group);
  if (!p || !palette) return;
  int row = (s->facing&128 ? -(y-sy)-1 : y-sy) - p->top;
  if (row < 0 || row >= p->height) return;
  for (int col=0;col<p->width;++col) {
    unsigned pixel=p->pixels[row*p->width+col];
    int dx=x+((s->facing&64) ? -1-p->left-col : p->left+col)+view.extra;
    if (pixel && dx>=0 && dx<view.width) {
      objects[dx]=(uint16_t)(0xa680|pixel); colors[dx]=palette[pixel];
    }
  }
}
static void moved_enemy_row(const MmxWeaponShot *s,const Ppu *p,const Raster *r,int y,
                            MmxRenderView view,uint16_t *objects,int *colors) {
  unsigned d=(unsigned)s->origin_x;
  if(d<0xe68 || d>=0x1228 || !frame.ram[d] || frame.ram[d+10]!=s->origin_y) return;
  unsigned group=frame.ram[d+22];
  const uint8_t *a=sprite_arrangement(group,frame.ram[d+23]&127);if(!a) return;
  const MmxSpriteAsset *asset=MmxRenderAssetsObjectSprite(frame.ram,d,group);
  int x=(s->x>>8)-(int)view_camera(frame.ram,0x1e4d);
  int sy=(s->y>>8)+(int8_t)frame.ram[d+25]-(int)view_camera(frame.ram,0x1e50);
  for(int i=(int)a[0]-1;i>=0;--i) {
    Piece piece=make_piece(a+i*4,x,sy,frame.ram[d+17]&64,
        frame.ram[d+17]&63,frame.ram[d+24],group,d,frame.ram[d+23]&127);
    unsigned attr=asset ? (piece.attr&0xf000)|((asset->attributes&15)<<8)|
        (asset->live_tiles?piece.attr&255:0) : piece.attr;
    if(y<piece.y || y>=piece.y+piece.size) continue;
    if(s->weapon==6 && !s->charged && s->muzzle_pose!=3 && s->age>30 && (s->age&1)) {
      uint16_t flash[MMX_RENDER_MAX_WIDTH]={0};int flash_colors[MMX_RENDER_MAX_WIDTH];
      sprite(p,r,piece.x,piece.y,attr,piece.size,y,view,flash,false,asset,piece.tile,flash_colors,true,false,false);
      for(int x=0;x<view.width;++x) if(flash[x]) {
        objects[x]=flash[x];colors[x]=r->palette[128+(flash[x]&15)];
      }
    } else sprite(p,r,piece.x,piece.y,attr,piece.size,y,view,objects,false,asset,piece.tile,colors,true,false,false);
  }
}
static void weapon_effects_row(const MmxWeaponCombatState *combat,const Ppu *p,const Raster *r,
                               int y,MmxRenderView view,uint16_t *objects,int *object_colors) {
  for (unsigned i=0;i<24;++i) {
    const MmxWeaponShot *s=i<8 ? combat->shots+i : combat->effects+i-8;
    if (!s->active || !s->age) continue;
    if(i<8 && s->page==1 && s->weapon==1 && !s->charged &&
        s->muzzle_pose>=1 && s->muzzle_pose<=4)
      moved_enemy_row(s,p,r,y,view,objects,object_colors);
    if(s->page==2 && s->weapon==2) {
      if(s->variant==1) moved_enemy_row(s,p,r,y,view,objects,object_colors);
      if(s->variant==2 && s->muzzle_pose==4) continue;
    }
    if(s->page==2 && s->weapon==6) {
      if(i>=8 && s->variant==2) {moved_enemy_row(s,p,r,y,view,objects,object_colors);continue;}
      if(i<8 && (s->variant==1 || (s->charged && s->muzzle_pose!=1))) continue;
    }
    if (s->page==2 && s->weapon==3 && s->charged && (!s->variant || !s->muzzle_pose)) continue;
    if (s->page==1 && s->weapon==1 && s->charged) continue;
    if (i>=8 && s->page==1 && !(s->weapon==8 && s->variant==5) && (s->weapon==1 ? s->age==1 || !((s->age-1+s->tether_pose)&1) :
        !(combat->tick&1))) continue; /* Original debris/sparkle flicker. */
    if (i<8 && s->page==1 && s->weapon==1 && s->muzzle_pose==5) continue;
    if (s->page==1 && s->weapon==8 && s->charged && s->tether_pose) continue;
    if (s->page==1 && s->weapon==2 && s->charged && (!s->variant || !s->muzzle_pose)) continue;
    if (s->page==2 && s->weapon==7 && s->variant!=2 && s->muzzle_pose==(s->charged ? 4 : 7)) continue;
    if (s->page==1 && s->weapon==4) {
      if (s->charged && s->muzzle_pose && s->radius) {
        static const uint8_t flash[6]={15,16,16,17,17,18};
        MmxWeaponShot center=*s;center.facing=s->tether_pose ? 64 : 0;
        weapon_sprite_row(&center,flash[6-s->radius],s->origin_x-view_camera(frame.ram,0x1e4d),
            s->origin_y-view_camera(frame.ram,0x1e50),y,view,objects,object_colors);
      } else if (!s->charged && (s->muzzle_pose==2 || s->muzzle_pose==3 || s->muzzle_pose==5)) {
        unsigned duration=s->origin_x>=1024 ? 1 : s->origin_x>=512 ? 2 : 3;
        weapon_sprite_row(s,9+(unsigned)s->origin_y/duration%5,(s->x>>8)-view_camera(frame.ram,0x1e4d),
            (s->y>>8)+8-view_camera(frame.ram,0x1e50),y,view,objects,object_colors);
      }
    }
    if (s->page==1 && s->weapon==6 && s->variant) {
      /* X2 $81:849D and $9ABF: nine links, every eighth of the span. */
      unsigned pose=(s->charged ? 20 : 5)+s->tether_pose/3;
      for (int link=8;link>=0;--link) {
        int x=(s->origin_x*(8-link)+(s->x>>8)*link)/8;
        weapon_sprite_row(s,pose,x-view_camera(frame.ram,0x1e4d),
            (s->y>>8)-view_camera(frame.ram,0x1e50),y,view,objects,object_colors);
      }
    }
    if (s->charged && s->page == 2 && s->weapon == 4 && s->variant!=4) {
      int ox=s->origin_x-view_camera(frame.ram,0x1e4d), oy=s->origin_y-view_camera(frame.ram,0x1e50);
      weapon_sprite_row(s,s->tether_pose,ox,oy,y,view,objects,object_colors);
      weapon_sprite_row(s,s->muzzle_pose,ox,oy,y,view,objects,object_colors);
    }
    if (s->page == 2 && s->weapon == 5 && s->variant >= 2) {
      /* Original X3 trail actors use poses 14/15 and delayed positions.
       * Rays have constant velocity, so their history is reconstructed
       * exactly without extra transient slots or unsaved renderer state. */
      for (int delay=2;delay>=1;--delay) {
        int remaining = delay - (s->variant == 3 ? s->radius : 0);
        if (remaining > 0 && s->age > (unsigned)delay+1)
          weapon_sprite_row(s,13+delay,((s->x-s->vx*remaining)>>8)-view_camera(frame.ram,0x1e4d),
              ((s->y-s->vy*remaining)>>8)-view_camera(frame.ram,0x1e50),y,view,objects,object_colors);
      }
      if (s->variant == 3) continue;
    }
    weapon_sprite_row(s,s->pose,(s->x>>8)-view_camera(frame.ram,0x1e4d),
        (s->y>>8)-view_camera(frame.ram,0x1e50),y,view,objects,object_colors);
  }
}
static void teleport_actor_row(const uint8_t *ram,const MmxZeroState *zero,const Ppu *p,
                               const Raster *r,int y,MmxRenderView view,uint16_t *objects,int *object_colors) {
  unsigned pose = MmxZeroSwapPose(zero);
  int x = (int16_t)(word(ram,0xbad) - view_camera(ram,0x1e4d));
  int sy = (int16_t)(word(ram,0xbb0) - view_camera(ram,0x1e50)) + zero->swap_y;
  unsigned flip = ram[0xbb9] & 64;
  if (!zero->active_x) {
    const uint8_t *pixels = MmxZeroTeleportPose(pose);
    int row = y - (sy - 8) + 64;
    if (pixels && row >= 0 && row < 128) for (int col = 0; col < 128; ++col) {
      unsigned pixel = pixels[row * 128 + col];
      int dx = x + (flip ? 63 - col : col - 64) + view.extra;
      if (pixel && dx >= 0 && dx < view.width) {
        objects[dx] = (uint16_t)(0xa680 | (pixel & 15));
        object_colors[dx] = MmxZeroColors()[pixel];
      }
    }
  } else {
    const MmxSpriteAsset *a = MmxRenderAssetsTeleportX(pose);
    const uint8_t *layout = a ? sprite_arrangement(0,pose) : NULL;
    if (layout) for (int i = layout[0] - 1; i >= 0; --i) {
      Piece s = make_piece(layout + i * 4,x,sy,flip,0x22,0,0,0xba8,pose);
      sprite(p,r,s.x,s.y,s.attr,s.size,y,view,objects,false,a,s.tile,object_colors,true,false,false);
    }
  }
}
static void x1_weapon_palette(MmxSpriteAsset *asset,const uint16_t *colors,unsigned page) {
  if(!colors) return;
  memcpy(asset->colors,colors,32);asset->live_tiles=true;
  /* X1's face uses index 1 for eye whites. X2 puts an unused dark red
   * there ($048E); its corresponding white is index 13. Adapt only the
   * palette overlay on X1 tiles, not the original source special poses. */
  if(page==1) asset->colors[1]=colors[13];
}
static void coop_sting_palette(const uint8_t *ram,MmxSpriteAsset *asset) {
  if(!ram[0xc31]) return;
  /* The active projectile owns the phase ($83:9C22), independently for each
   * seat. The ordinary weapon palette must not erase this native effect. */
  for(unsigned d=0x1228;d<0x1428;d+=64) if(ram[d] && ram[d+10]==0x11) {
    MmxRenderAssetsStingPalette(ram[d+0x39],asset->colors);return;
  }
}
static void prepare_partner_graphics(void) {
  /* The partner is drawn from this tick's body, unlike the anchor's latched
   * OAM. Native $84:8FCA queues pose CHR at $0500 for the NEXT NMI ($80:8332).
   * Preview those pending OBJ transfers only in the partner's private raster
   * so a new X arrangement never reads the preceding pose's tiles. This uses
   * captured state, including WRAM-backed armor/effects, and changes no guest
   * memory, transfer timing, or save/rollback layout. */
  partner_raster = frame.lines[0];
  unsigned end = frame.ram[0xa3] & ~7u;
  for (unsigned offset=0;offset<end;offset+=8) {
    const uint8_t *dma=frame.ram+0x500+offset;
    unsigned dest=word(dma,1),size=word(dma,3),source=word(dma,5),bank=dma[7];
    /* Sprite records use contiguous word writes. Leave tilemap/remapped
     * transfers to the native raster rather than predicting their effects. */
    if(dma[0]!=0x80 || dest<0x6000 || dest>=0x8000 || !size || (size&1) ||
        size>(0x8000-dest)*2 || size>0x10000-source) continue;
    const uint8_t *bytes=bank==0x7e || bank==0x7f ?
        frame.ram+(bank-0x7e)*0x10000+source : rom_at(bank<<16|source,size);
    if(!bytes) continue;
    for(unsigned i=0;i<size;i+=2) partner_raster.vram[dest+i/2]=(uint16_t)word(bytes,i);
  }
}
static void player_overlay_plane_row(
    const MmxRenderPlayerOverlayPlane *plane, const uint16_t *palette,
    unsigned palette_count, int y, int zx, int zy, bool mirror, unsigned z,
    MmxRenderView view, uint16_t *objects, int *object_colors);
static void player_overlay_row(const MmxRenderPlayerOverlay *overlay, int y,
                               int zx, int zy, unsigned z, MmxRenderView view,
                               uint16_t *objects, int *object_colors) {
  if (overlay->blade_layer == 1)
    player_overlay_plane_row(&overlay->blade, overlay->palette, overlay->palette_count,
        y, zx, zy, overlay->facing_left, z, view, objects, object_colors);
  player_overlay_plane_row(&overlay->body, overlay->palette, overlay->palette_count,
      y, zx, zy, overlay->facing_left, z, view, objects, object_colors);
  if (overlay->blade_layer == 2)
    player_overlay_plane_row(&overlay->blade, overlay->palette, overlay->palette_count,
        y, zx, zy, overlay->facing_left, z, view, objects, object_colors);
}
static void coop_partner_row(const Ppu *ppu,const Raster *r,int y,MmxRenderView view,
                             uint16_t *objects,int *colors) {
  const MmxCoopPlayer *partner = &frame_coop.players[frame_coop.current^1];
  if (!frame_coop.initialized || partner->status != MMX_COOP_ALIVE || frame_coop.scene_phase==2) return;
  const uint8_t *ram = partner_ram;
  if (partner->zero.swap_phase) {
    teleport_actor_row(ram,&partner->zero,ppu,r,y,view,objects,colors);return;
  }
  bool zero = partner->character == MMX_COOP_ZERO;
  const MmxWeaponShot *cast=NULL;
  for (unsigned i=0;i<8;++i) {
    const MmxWeaponShot *s=partner->combat.shots+i;
    if (s->active && s->charged && s->page==2 &&
        ((s->weapon==3 && !s->variant && s->muzzle_pose) || (s->weapon==6 && s->variant==1))) cast=s;
  }
  MmxSpriteAsset weapon_palette={0};
  const uint16_t *weapon_colors=MmxWeaponsPalette(partner->weapons.page,partner->weapons.weapon,true);
  if(!partner->weapons.page) {
    const MmxSpriteAsset *native=MmxRenderAssetsWeaponX(ram[0xbdb]/2,true);
    if(native) weapon_colors=native->colors;
  }
  x1_weapon_palette(&weapon_palette,weapon_colors,partner->weapons.page);
  coop_sting_palette(ram,&weapon_palette);
  if(!zero && MmxKncBugfixActive(frame_coop.current^1))
    MmxRenderAssetsStingPalette(MmxKncBugfixPhase(),weapon_palette.colors);
  if (zero && ram[0xbb6]) {
    const uint8_t *body = MmxZeroPose(ram,&partner->zero);
    const uint8_t *blade = MmxZeroBlade(&partner->zero);
    const uint8_t *charge = !ram[0xbdb] && !partner->weapons.weapon ? MmxZeroChargePose(&partner->zero) : NULL;
    int x = (int16_t)(word(ram,0xbad)-view_camera(ram,0x1e4d));
    int sy = (int16_t)(word(ram,0xbb0)-view_camera(ram,0x1e50))+MmxZeroPoseOffsetY(ram);
    if (cast) {
      MmxZeroState z=partner->zero;z.burst=z.air=0;blade=NULL;
      if (cast->weapon==3) z.slash=cast->pose==39?1:cast->pose==41?3:cast->pose==29?6:
          cast->pose==30?9:cast->pose==31?12:cast->pose==32?15:21;
      else {
        z.slash=0;z.anim_valid=1;
        z.anim_pose=cast->pose==39 || cast->pose==40?0:
            cast->pose==41 || cast->pose==44?0x43:cast->pose==29 || cast->pose==45?0x44:
            cast->pose==35?0x45:cast->pose==36?0x46:0x47;
      }
      body=MmxZeroPose(ram,&z);
    }
    bool pilot=ram[0xbbe]==0x6a || ram[0xbbe]==0x6b;
    if (pilot) {
      const uint8_t *a=sprite_arrangement(ram[0xbbe],ram[0xbbf]&127);
      if (a && a[0]) {
        int hx,hy;pilot_head_offset(a,&hx,&hy);
        int dx=hx+5;x+=(ram[0xbb9]&64)?-dx:dx;sy+=8+hy+20;
      }
      body=MmxZeroMenuPose();blade=charge=NULL;
    }
    int row = y-sy+64;
    unsigned priority = ((ram[0xbb9]>>4 & 3)*4+2)<<12;
    /* A Saber attack replaces the body; the partner's Ride Armor pilot keeps
     * the native cockpit art, which has no OAM here to align the sheet to. */
    bool overlay = frame_partner_overlay.active && !pilot && !cast;
    if (overlay) player_overlay_row(&frame_partner_overlay,y,x,sy,priority|0x680,view,objects,colors);
    if (!overlay && body && row>=0 && row<128 && (!pilot || (row>=44 && row<64))) for (int col=0;col<128;++col) {
      unsigned pixel = body[row*128+col];
      if (blade && blade[row*128+col]) pixel = blade[row*128+col];
      int dest = x + ((ram[0xbb9]&64) ? 63-col : col-64) + view.extra;
      if (pixel && dest>=0 && dest<view.width) {
        objects[dest] = (uint16_t)(priority|0x680|(pixel&15));
        colors[dest] = pixel>=16 && pixel<32 ? MmxZeroBodyColors(&partner->zero)[pixel-16] : MmxZeroColors()[pixel];
      }
      if (charge && charge[row*128+col] && dest>=0 && dest<view.width) {
        unsigned pixel = charge[row*128+col];
        unsigned palette = partner->zero.charge>=81 && partner->zero.charge<201 ? 32 : 0;
        objects[dest] = (uint16_t)(0xe680|pixel); colors[dest] = MmxZeroColors()[palette+pixel];
      }
    }
  }
  if (!zero && cast && ram[0xbb6]) weapon_sprite_row(cast,cast->pose,
      (int16_t)(word(ram,0xbad)-view_camera(ram,0x1e4d)),
      (int16_t)(word(ram,0xbb0)-view_camera(ram,0x1e50)),y,view,objects,colors);
  for (unsigned i=0;i<24;++i) {
    unsigned d = i==0 ? 0xba8 : i<16 ? 0xc38+(i-1)*32 : 0x1228+(i-16)*64;
    /* Armor parts ($0C38/$0C58/$0C78) are visible when .0E is nonzero, as
     * native D56F submits them (expand_queues); bit 7 alone hid a partner X's
     * helmet, arms and boots whenever Zero drove the world. */
    bool armor = d>=0xc38 && d<=0xc78;
    if (d==0xba8 ? !ram[d+1] || !ram[d+14] :
        !ram[d] || !(armor ? ram[d+14] : ram[d+14]&128)) continue;
    if(d<0xc98 && !ram[0xbb6]) continue; /* Armor follows its owner's Sting blink. */
    if (zero && (d==0xba8 || d<0xc98 || MmxZeroNativeChargeObject(d,ram[d+10]))) continue;
    if (cast && d<0xc98) continue;
    if (d>=0x1228 && partner->combat.shots[(d-0x1228)/64].active) continue;
    unsigned group = ram[d+22];
    const uint8_t *a = sprite_arrangement(group,ram[d+23]&127);
    if (!a) continue;
    int x = (int16_t)(word(ram,d+5)-view_camera(ram,0x1e4d));
    int sy = (int16_t)(word(ram,d+8)+(int8_t)ram[d+25]-view_camera(ram,0x1e50));
    for (int j=(int)a[0]-1;j>=0;--j) {
      Piece s = make_piece(a+j*4,x,sy,ram[d+17]&64,ram[d+17]&63,ram[d+24],group,d,ram[d+23]&127);
      const MmxSpriteAsset *asset=!zero && weapon_colors && zero_actor(d,group) &&
          (s.attr&0x0e00) ? &weapon_palette : NULL;
      /* Normal Spark and its two wall fragments share original group $47.
       * Another seat can replace the shared $6200/$6300 weapon upload. */
      if(d>=0x1228 && ram[d+10]==0x0c && group==0x47 && (s.attr&0x0e00)==0x0600)
        asset=MmxRenderAssetsWeaponX(6,false);
      if(d>=0x1228 && ((ram[d+10]==3 && group==0x9e) ||
          ((ram[d+10]==1 || ram[d+10]==2) && group==0x0e))) {
        const MmxSpriteAsset *beam=MmxRenderAssetsChargedBuster(group,ram[d+23]&127);
        if(beam) asset=beam;
      }
      sprite(ppu,zero?r:&partner_raster,s.x,s.y,s.attr,s.size,y,view,objects,false,asset,s.tile,colors,true,false,false);
    }
  }
}
static void coop_meter_row(const Ppu *ppu,const Raster *r,int y,MmxRenderView view,
                           uint16_t *objects,int *colors,int x,unsigned value,unsigned maximum,
                           unsigned palette,const MmxSpriteAsset *art,int icon_y,bool inverted) {
  /* The native $D82C/$D94A meter uses overlapping 16px strips. Retain its
   * partial-strip placement and OAM order, including the cap above max HP. */
  typedef struct {int y;unsigned tile;} Strip;
  Strip strips[8];unsigned count=0;
  if(maximum>32) maximum=32;
  if(value>maximum) value=maximum;
  /* Mirror Zero's frame, but keep remaining energy at the bottom. */
  if(inverted) value=maximum-value;
  int top=64,remaining=(int)value;
  while(remaining>0) {
    remaining-=8;int sy=top-(remaining<0?remaining*2:0);
    strips[count++]=(Strip){sy,0x80};top=sy-16;
  }
  remaining=(int)maximum-(int)value;
  do {
    remaining-=8;int sy=top-(remaining<0?remaining*2:0);
    strips[count++]=(Strip){sy,0x82};top=sy-16;
  } while(remaining>=0);
  strips[count++]=(Strip){top,0x84};
  for(int i=(int)count-1;i>=0;--i) {
    unsigned tile=strips[i].tile;
    int sy=strips[i].y+icon_y-80;
    unsigned attr=0x3000|(palette<<9);
    if(inverted) {
      sy=icon_y+80-strips[i].y;
      if(tile==0x80) tile=0x82;
      else if(tile==0x82) tile=0x80;
      attr|=0x8000;
    }
    sprite(ppu,r,x,sy,attr|tile,16,y,view,
        objects,false,art,tile,colors,true,false,false);
  }
}
static void coop_inverted_badge_row(const Ppu *ppu,const Raster *r,int y,MmxRenderView view,
                                    uint16_t *objects,int *colors,int x,int icon_y,unsigned palette,
                                    const MmxSpriteAsset *frame_art,const MmxSpriteAsset *icon_art,
                                    const MmxWeaponPose *icon,bool zero) {
  /* Use the meter's own frame/palette for the top cap. Replace only the
   * upright symbol panel, leaving its sides continuous with the meter. */
  sprite(ppu,r,x,icon_y,0xb086|(palette<<9),16,y,view,objects,false,
      frame_art,0x86,colors,true,false,false);
  int row=y-icon_y;
  int panel_bottom=palette==3?14:13;
  if(row<3 || row>panel_bottom) return;
  /* Leave the cap's white highlight intact above the dark weapon panel. */
  int source_row=row-(palette==3);
  for(unsigned col=2;col<14;++col) {
    if(row==panel_bottom && (col==2 || col==13)) continue;
    int dx=x+(int)col+view.extra;
    if(dx<0 || dx>=view.width) continue;
    int color=zero?MmxZeroHudColor(col,(unsigned)source_row):-1;
    if(color<0) {
      unsigned tile=palette==2?0x86:0x20;
      unsigned number=(((tile>>4)+(unsigned)source_row/8)<<4)|((tile&15)+col/8);
      unsigned pixel;
      if(icon) pixel=icon->pixels[source_row*16+col];
      else if(icon_art && !icon_art->live_tiles) {
        const uint8_t *bits=icon_art->tiles+number*32+(source_row&7)*2;
        unsigned shift=7-(col&7);
        pixel=((bits[0]>>shift)&1)|(((bits[1]>>shift)&1)<<1)|
            (((bits[16]>>shift)&1)<<2)|(((bits[17]>>shift)&1)<<3);
      } else pixel=tile_pixel(sprite_vram(r),(ppu->obsel&7)*8192+number*16,col&7,source_row&7,4);
      color=frame_art?frame_art->colors[pixel]:r->palette[128+palette*16+pixel];
    }
    objects[dx]=0xe6a1;colors[dx]=color;
  }
}
static void coop_hud_row(const Ppu *ppu,const Raster *r,int y,MmxRenderView view,bool anchored,
                         uint16_t *objects,int *colors) {
  for(unsigned seat=0;seat<2;++seat) {
    const MmxCoopPlayer *player=&frame_coop.players[seat];
    if(seat && player->status==MMX_COOP_ABSENT) continue;
    bool inverted=coop_hud_compact && player->character==MMX_COOP_ZERO;
    int x=8+(coop_hud_compact?0:(int)seat*32)-(anchored?view.extra:0),icon_y=inverted?100:80;
    unsigned hp=player->status==MMX_COOP_FALLEN?0:player->body[0x27]&127;
    coop_meter_row(ppu,r,y,view,objects,colors,x,hp,frame.ram[0x1f9a],2,NULL,icon_y,inverted);
    if(inverted) coop_inverted_badge_row(ppu,r,y,view,objects,colors,x,icon_y,2,NULL,NULL,NULL,true);
    else sprite(ppu,r,x,icon_y,0x3486,16,y,view,objects,false,NULL,0,colors,true,
        player->character==MMX_COOP_ZERO,false);
    unsigned page=player->weapons.page;
    unsigned weapon=page?player->weapons.weapon:player->body[0x33]/2;
    if(!weapon || weapon>8) continue; /* Keep this seat's reserved blank column. */
    unsigned energy=page?player->weapons.energy[(page-1)*8+weapon-1]:player->energy[weapon*2-1];
    const MmxSpriteAsset *native=page?NULL:MmxRenderAssetsWeaponX(weapon,false);
    const uint16_t *palette=page?MmxWeaponsPalette(page,weapon,false):native?native->colors:NULL;
    MmxSpriteAsset bar={0};bar.live_tiles=true;
    if(palette) memcpy(bar.colors,palette,sizeof(bar.colors));
    coop_meter_row(ppu,r,y,view,objects,colors,x+16,energy,28,3,palette?&bar:NULL,icon_y,inverted);
    if(inverted) {
      coop_inverted_badge_row(ppu,r,y,view,objects,colors,x+16,icon_y,3,palette?&bar:NULL,
          native,page?MmxWeaponsHudIcon(page,weapon):NULL,false);
    } else if(!page) {
      sprite(ppu,r,x+16,icon_y,0x3620,16,y,view,objects,false,native,0x20,colors,true,false,false);
    } else {
      const MmxWeaponPose *icon=MmxWeaponsHudIcon(page,weapon);int row=y-icon_y;
      if(icon && palette && row>=0 && row<16) for(int col=0;col<16;++col) {
        unsigned pixel=icon->pixels[row*16+col];int dx=x+16+col+view.extra;
        if(pixel && dx>=0 && dx<view.width) {objects[dx]=(uint16_t)(0xe6b0|pixel);colors[dx]=palette[pixel];}
      }
    }
  }
}
static void player_overlay_plane_row(
    const MmxRenderPlayerOverlayPlane *plane, const uint16_t *palette,
    unsigned palette_count, int y, int zx, int zy, bool mirror, unsigned z,
    MmxRenderView view, uint16_t *objects, int *object_colors) {
  if (!plane || !plane->pixels || !plane->width || !plane->height ||
      !palette || !palette_count) return;
  int row = y - zy + 64 - plane->origin_y;
  if (row < 0 || row >= plane->height) return;
  for (int col = 0; col < plane->width; ++col) {
    unsigned pixel = plane->pixels[row * plane->width + col];
    int canvas_col = plane->origin_x + col;
    int dx;
    if (!pixel || pixel >= palette_count) continue;
    dx = zx + (mirror ? 63 - canvas_col : canvas_col - 64) + view.extra;
    if (dx >= 0 && dx < view.width) {
      objects[dx] = (uint16_t)(z | (pixel & 15));
      object_colors[dx] = palette[pixel];
    }
  }
}
static void world_sprite_row(const MmxRenderWorldSprite *sprite, int y,
                             MmxRenderView view, uint16_t *objects,
                             int *object_colors) {
  int sx, sy, row;
  if (!sprite || !sprite->pixels || !sprite->width || !sprite->height ||
      !sprite->palette || !sprite->palette_count) return;
  sx = (int)(sprite->world_x - (int32_t)view_camera(frame.ram, 0x1e4d));
  sy = (int)(sprite->world_y - (int32_t)view_camera(frame.ram, 0x1e50));
  row = y - sy - sprite->origin_y;
  if (row < 0 || row >= sprite->height) return;
  for (int col = 0; col < sprite->width; ++col) {
    unsigned pixel = sprite->pixels[row * sprite->width + col];
    int dx = sx + (sprite->facing_left ?
        -1 - sprite->origin_x - col : sprite->origin_x + col) + view.extra;
    if (!pixel || pixel >= sprite->palette_count ||
        dx < 0 || dx >= view.width) continue;
    objects[dx] = (uint16_t)(sprite->z | pixel);
    object_colors[dx] = sprite->palette[pixel];
  }
}
static uint32_t debug_rgb555(uint16_t color) {
  unsigned red = color & 31;
  unsigned green = (color >> 5) & 31;
  unsigned blue = (color >> 10) & 31;
  red = (red << 3) | (red >> 2);
  green = (green << 3) | (green >> 2);
  blue = (blue << 3) | (blue >> 2);
  return (red << 16) | (green << 8) | blue;
}
static void draw_debug_rect(uint32_t *out, MmxRenderView view,
                            const MmxRenderDebugRect *rect) {
  int64_t left, top, right, bottom;
  int x0, x1;

  if (!out || !rect || !rect->w || !rect->h) return;
  left = (int64_t)rect->world_x - view_camera(frame.ram, 0x1e4d) + view.extra;
  top = (int64_t)rect->world_y - view_camera(frame.ram, 0x1e50);
  right = left + rect->w - 1;
  bottom = top + rect->h - 1;
  x0 = left < 0 ? 0 : left >= view.width ? view.width - 1 : (int)left;
  x1 = right < 0 ? 0 : right >= view.width ? view.width - 1 : (int)right;
  if (left >= view.width || right < 0 || top >= MMX_RENDER_HEIGHT ||
      bottom < 0 || x0 > x1)
    return;

  const uint32_t color = debug_rgb555(rect->rgb555);
  if (top >= 0 && top < MMX_RENDER_HEIGHT)
    for (int x = x0; x <= x1; ++x) out[(int)top * view.width + x] = color;
  if (bottom >= 0 && bottom < MMX_RENDER_HEIGHT && bottom != top)
    for (int x = x0; x <= x1; ++x) out[(int)bottom * view.width + x] = color;
  if (left >= 0 && left < view.width && bottom - top > 1) {
    int y0 = top + 1 < 0 ? 0 : top + 1 >= MMX_RENDER_HEIGHT ? MMX_RENDER_HEIGHT : (int)top + 1;
    int y1 = bottom - 1 >= MMX_RENDER_HEIGHT ? MMX_RENDER_HEIGHT - 1 : (int)bottom - 1;
    for (int y = y0; y <= y1; ++y) out[y * view.width + (int)left] = color;
  }
  if (right >= 0 && right < view.width && right != left && bottom - top > 1) {
    int y0 = top + 1 < 0 ? 0 : top + 1 >= MMX_RENDER_HEIGHT ? MMX_RENDER_HEIGHT : (int)top + 1;
    int y1 = bottom - 1 >= MMX_RENDER_HEIGHT ? MMX_RENDER_HEIGHT - 1 : (int)bottom - 1;
    for (int y = y0; y <= y1; ++y) out[y * view.width + (int)right] = color;
  }
}
static void rush_hud_row(const Ppu *p,const Raster *r,int y,MmxRenderView view,
                         uint16_t *objects,int *colors) {
  for(unsigned i=0;i<2;++i) {
    const MmxBossRushBoss *b=&frame_rush.bosses[i];if(!b->phase) continue;
    int x=view.width-view.extra-40+(int)i*16;
    coop_meter_row(p,r,y,view,objects,colors,x,b->health,32,2,NULL,80,false);
    /* $D94A's native boss footer, rather than the player's X badge. */
    sprite(p,r,x,80,0x34aa,16,y,view,objects,false,NULL,0xaa,colors,true,false,false);
  }
}
static void rush_native_text(uint32_t *out,MmxRenderView view,int x,int y,const char *s,unsigned palette) {
  for(int row=0;row<8;++row) {
    if(y+row<0 || y+row>=224) continue;
    const Raster *r=&frame.lines[y+row];Ppu p={0};memcpy(&p,r->registers,sizeof(r->registers));
    if((p.inidisp&128) && palette<8) continue;
    for(unsigned n=0;s[n];++n) for(int col=0;col<8;++col) {
      int dx=x+(int)n*8+col;if(dx<0 || dx>=view.width) continue;
      unsigned pixel=tile_pixel(frame.lines[vram_owner[y+row]].vram,((p.bgTileAdr>>8)&15)*4096+(unsigned char)s[n]*8,col,row,2);
      if(!pixel) continue;
      /* Keep the original options font's three shades. Mammoth's stage
       * repurposes the live text palette, making the counter/labels black.
       * Gameplay text still follows native blanking and raster brightness;
       * results outlive the native death palette fade. */
      static const uint16_t menu_colors[2][4]={{0,0x7fff,0x7fe0,0x4080},{0,0x7fff,0x7fff,0x7fe0}};
      unsigned color=palette>=7?menu_colors[palette==9][pixel]:r->palette[palette*4+pixel];uint32_t rgb=0;
      for(unsigned c=0;c<3;++c) {
        unsigned v=(color>>(c*5))&31;v=(v<<3)|(v>>2);if(palette<8) v=v*(p.inidisp&15)/15;
        rgb|=v<<(16-c*8);
      }
      out[(y+row)*view.width+dx]=rgb;
    }
  }
}
static bool draw_frame(uint32_t *out, MmxRenderView view, bool hud, int plane_offset) {
  if (!out || !frame.valid || view.width < 256 || view.width > MMX_RENDER_MAX_WIDTH ||
      view.extra != (view.width - 256) / 2 || (view.width & 1)) return false;
  memset(&stats, 0, sizeof(stats)); stats.pieces = frame.piece_count;
  door_cache_reset();
  memset(out, 0, (size_t)view.width * 224 * sizeof(*out));
  if(frame_rush.mode==MMX_RUSH_FINISHED) {
    /* Own the whole page so native death flashes, HUD badges and the world
     * cannot bleed through the options-style results screen. */
    int center=view.width/2;char score[40];
    rush_native_text(out,view,center-36,48,"GAME OVER",8);
    snprintf(score,sizeof(score),"DEFEATED %u",(unsigned)frame_rush.defeated);
    rush_native_text(out,view,center-(int)strlen(score)*4,80,score,8);
    uint32_t best=MmxBossRushHighScore();if(best<frame_rush.defeated) best=frame_rush.defeated;
    snprintf(score,sizeof(score),"HIGH SCORE %u",(unsigned)best);
    rush_native_text(out,view,center-(int)strlen(score)*4,96,score,8);
    rush_native_text(out,view,center-20,136,"RETRY",frame_rush.result_selection?8:9);
    rush_native_text(out,view,center-36,160,"MAIN MENU",frame_rush.result_selection?9:8);
    rush_native_text(out,view,center-68,frame_rush.result_selection?160:136,">",9);
    return true;
  }
  bool zero_menu = zero_weapons_menu();
  bool weapon_menu = MmxWeaponsMenuVisible(frame.ram);
  /* Plain X's menu normally uses the stock frame. During a Rush fade it
   * still needs private art until the last matching boss OAM disappears. */
  bool menu = zero_menu || weapon_menu ||
      (!MmxWidePolicy_IsStageScene(frame.ram) && rush_pause_art_needed());
  bool zero_title = zero_title_menu();
  bool stage = MmxWidePolicy_IsStageScene(frame.ram) && !menu;
  const bool select = !stage && !menu && stage_select_scene();
  const bool flat = !stage && !menu && !select && flat_native_scene();
  bool rush_hud = stage && frame_rush.loaded;
  view_dx=view_dy=0;
  if(stage && peer_seat>=0 && frame_coop.initialized && MmxCoopViewsOnline()) {
    MmxCoopView camera=MmxCoopViewWithBounds(frame.ram,&frame_coop,(unsigned)peer_seat,&frame_bounds);
    view_dx=camera.x-(int)word(frame.ram,0x1e4d);
    view_dy=camera.y-(int)word(frame.ram,0x1e50);
  }
  if(stage && frame_coop.initialized &&
      frame_coop.players[frame_coop.current^1].character==MMX_COOP_X)
    prepare_partner_graphics();
  bool swapping = stage && MmxZeroEnabled() && frame_zero.swap_phase;
  unsigned weapon_page = menu ? frame_weapons.menu_page : frame_weapons.page;
  unsigned weapon_item = menu ? frame.ram[0x1ed2] : frame_weapons.weapon;
  const uint16_t *weapon_colors = MmxWeaponsPalette(weapon_page, weapon_item, true);
  if(stage && frame_coop.initialized && !weapon_page) {
    const MmxSpriteAsset *native=MmxRenderAssetsWeaponX(frame.ram[0xbdb]/2,true);
    if(native) weapon_colors=native->colors;
  }
  MmxSpriteAsset x_weapon_palette = {0};
  x1_weapon_palette(&x_weapon_palette,weapon_colors,weapon_page);
  if(stage && frame_coop.initialized) coop_sting_palette(frame.ram,&x_weapon_palette);
  unsigned knc_bugfix_seat=frame_coop.initialized?frame_coop.current:0;
  bool knc_bugfix=stage && (!MmxZeroEnabled() || frame_zero.active_x) &&
      MmxKncBugfixActive(knc_bugfix_seat);
  if(knc_bugfix) {
    if(!weapon_colors) {
      const MmxSpriteAsset *native=MmxRenderAssetsWeaponX(frame.ram[0xbdb]/2,true);
      if(native) x1_weapon_palette(&x_weapon_palette,native->colors,0);
    }
    MmxRenderAssetsStingPalette(MmxKncBugfixPhase(),x_weapon_palette.colors);
    weapon_colors=x_weapon_palette.colors;
  }
  prepare_stage_planes();
  LightBeam beams[2];
  unsigned beam_count = stage ? spark_lights(beams, view.extra) : 0;
  unsigned palette_fade = stage && g_mmx_render_asset_repairs ?
      MmxRenderAssetsDeathPaletteFade(frame.ram, frame.lines[0].palette) : 0;
  const Piece *pieces = frame.expand && g_mmx_render_asset_repairs ? frame.expanded : frame.pieces;
  unsigned piece_count = frame.expand && g_mmx_render_asset_repairs ? frame.expanded_count : frame.piece_count;
  const uint8_t *zero = stage || zero_menu || zero_title ? MmxZeroPose(frame.ram, &frame_zero) : NULL;
  const uint8_t *blade = stage && zero ? MmxZeroBlade(&frame_zero) : NULL;
  const uint8_t *charge = stage && zero ? MmxZeroChargePose(&frame_zero) : NULL;
  const MmxWeaponShot *triad=NULL,*gravity=NULL;
  if (stage && !swapping) for (unsigned i=0;i<8;++i) {
    const MmxWeaponShot *s=frame_weapon_combat.shots+i;
    if (s->active && s->page==2 && s->weapon==3 && s->charged && !s->variant && s->muzzle_pose) triad=s;
    if (s->active && s->page==2 && s->weapon==6 && s->charged && s->variant==1) gravity=s;
  }
  if (triad && zero) {
    /* X3 Zero has no ground-punch action. Reuse his original windup and
     * crouched strike body poses, with the X3 Triad action's own timing. */
    MmxZeroState strike=frame_zero;strike.burst=0;strike.air=0;
    strike.slash=triad->pose==39?1:triad->pose==41?3:triad->pose==29?6:
        triad->pose==30?9:triad->pose==31?12:triad->pose==32?15:21;
    zero=MmxZeroPose(frame.ram,&strike);blade=NULL;
  }
  if(gravity && zero) {
    /* Zero never used Gravity Well in X3. Adapt original group-4A raised-arm
     * body poses to the source cast timing, including its held pose. */
    MmxZeroState cast=frame_zero;cast.burst=cast.slash=0;cast.anim_valid=1;
    cast.anim_pose=gravity->pose==39 || gravity->pose==40?0:
        gravity->pose==41 || gravity->pose==44?0x43:gravity->pose==29 || gravity->pose==45?0x44:
        gravity->pose==35?0x45:gravity->pose==36?0x46:0x47;
    zero=MmxZeroPose(frame.ram,&cast);blade=NULL;
  }
  const MmxWeaponShot *cast_body=triad?triad:gravity;
  Piece waiting[128];
  unsigned waiting_count = stage && g_mmx_render_asset_repairs ?
      fortress_waiting_pieces(waiting, pieces, piece_count) : 0;
  const MmxSpriteAsset *waiting_zero = waiting_count ? MmxRenderAssetsCaptiveZero() : NULL;
  const MmxSpriteAsset *piece_assets[MAX_PIECES] = {0};
  MmxSpriteAsset camouflage_art;
  MmxSpriteAsset rush_flash_art[2];
  bool rush_flash_ready[2]={false,false};
  bool camouflage_ready=false;
  if ((stage || frame_rush.mode==MMX_RUSH_PLAYING) && g_mmx_render_asset_repairs) for (unsigned i = 0; i < piece_count; ++i) {
    const Piece *s = &pieces[i];
    const MmxSpriteAsset *a = stage ? MmxRenderAssetsObjectSprite(frame.ram, s->object, s->animation) : NULL;
    /* The menu reuses $1928..$1988 for armor and its player preview. Other
     * still-visible boss pieces retain private art through the transition. */
    if(frame_rush.mode==MMX_RUSH_PLAYING && s->object>=0xe68 && s->object<0x1d08 &&
        !(menu && s->object>=0x1928 && s->object<=0x1988)) {
      unsigned owner=frame_rush.owner[(s->object-0xe68)/32];
      if(owner && owner<=2) {
        const MmxSpriteAsset *private=MmxRenderAssetsRushObjectSprite(frame.ram,s->object,
            kMmxBossRushBosses[frame_rush.bosses[owner-1].id].stage,s->animation);
        if(private) a=private;
        unsigned seat=owner-1;
        if(private && s->object==frame_rush.bosses[seat].object &&
            frame_rush.bosses[seat].id==5 && s->animation==0x23 &&
            frame_rush_visual.initialized[seat] &&
            frame_rush_visual.generation[seat]==frame_rush.bosses[seat].generation) {
          if(!camouflage_ready) {
            camouflage_art=*private;
            memcpy(camouflage_art.colors,frame_rush_visual.colors[seat],sizeof(camouflage_art.colors));
            /* $C1 is entry 24 in the per-actor scene flags. */
            if(frame_rush.bosses[seat].globals[24]==2)
              for(unsigned c=0;c<16;++c) camouflage_art.colors[c]|=0x8000;
            camouflage_ready=true;
          }
          a=&camouflage_art;
        }
        /* Keep private CHR, but let the native palette-zero hit flash use
         * live CGRAM. Mammoth's trunk copies its parent's $11 at $82:9665. */
        bool body=s->object==frame_rush.bosses[seat].object ||
            (frame_rush.bosses[seat].id==7 && frame.ram[s->object+10]==0x0e &&
             word(frame.ram,s->object+12)==frame_rush.bosses[seat].object);
        if(private && body && !(s->attr&0x0e00) && (private->attributes&14)) {
          if(!rush_flash_ready[seat]) {
            rush_flash_art[seat]=*a;rush_flash_art[seat].live_colors=true;
            rush_flash_ready[seat]=true;
          }
          a=&rush_flash_art[seat];
        }
      }
    }
    /* OBJ palette 0 is the shared hit flash. If the current CHR binding
     * still matches, that deliberate palette change must remain live. */
    bool hit_flash = a && a->current &&
        (s->attr & 255) == ((s->tile + a->tile_base) & 255) &&
        ((s->attr >> 8) & 15) == (a->attributes & 1);
    /* Keep current allocations and their live flashes/animation. Repair
     * missing or stale bindings using the ROM resource's own palette. */
    if (a && !hit_flash && (!a->current || (s->attr & 255) != ((s->tile + a->tile_base) & 255) ||
        ((s->attr >> 8) & 15) != (unsigned)((a->attributes & 15) | (s->palette_pose&14)) ||
        (s->object == 0xe18 && MmxRenderAssetsRideArmorPalettePending(frame.ram,
            frame.lines[0].palette + 128 + ((s->attr >> 9) & 7) * 16)))) piece_assets[i] = a;
  }
  int16_t crystal_ripple[224];MmxWeaponsTimeRipple(&frame_weapon_combat,crystal_ripple);
  if (frame_coop.initialized && MmxWeaponsTimePhase(&frame_coop.players[frame_coop.current^1].combat)==1 &&
      MmxWeaponsTimePhase(&frame_weapon_combat)!=1)
    MmxWeaponsTimeRipple(&frame_coop.players[frame_coop.current^1].combat,crystal_ripple);
  for (int y = 0; y < 224; ++y) {
    const Raster *r = &frame.lines[y]; Ppu p;
    drawing_vram = frame.lines[vram_owner[y]].vram;
    memcpy(&p, r->registers, PPU_SAVESTATE_REGS_SIZE);
    if (beam_count) p.cgwsel = (p.cgwsel & 0xcf) | 0x20;
    if ((p.bgmode & 7) != 1 || (!stage && !menu && !zero_title && !frame_rush.menu && !select && !flat)) {
      memcpy(out + y * view.width + view.extra, frame.stock + y * 256, 256 * sizeof(*out));
      ++stats.fallback_lines; continue;
    }
    ++stats.custom_lines;
    if (p.inidisp & 128) continue;
    /* Re-sample the background BEFORE priority/transparency/color math; a
     * finished-frame shift would drag HUD/terrain and leave exposed holes.
     * Window masks stay at screen coordinates. Thunder Slimer uses BG2 as an
     * actor surface, so it cannot use the distant-background profile. */
    int stereo_shift = (stage || select) && stereo_plane_allowed(&p) ? plane_offset : 0;
    unsigned stereo_layer = MmxRendererStereoLayer();
    /* Revision 16: select-screen portraits, as the native compositor. */
    const int select_bg1 = select && stereo_shift && bg1_select_depth_active(&p) ?
        (stereo_shift > 0 ? (stereo_shift + 2) / 3 : -((-stereo_shift + 2) / 3)) : 0;
    /* Revision 16: BG1 scenery depth, same rule as the native compositor. */
    static uint16_t depth_row[MMX_RENDER_MAX_WIDTH + 2 * MMX_STEREO_MAX_SOURCE_OFFSET];
    static int depth_color[MMX_RENDER_MAX_WIDTH + 2 * MMX_STEREO_MAX_SOURCE_OFFSET];
    static uint8_t depth_back[MMX_RENDER_MAX_WIDTH + 2 * MMX_STEREO_MAX_SOURCE_OFFSET];
    static int16_t depth_map[MMX_RENDER_MAX_WIDTH];
    const bool depth1 = stage && !crystal_ripple[y] && bg1_depth_active(&p, stereo_shift);
    const int depth_lo = -view.extra;
    if (depth1) {
      const int hi = view.width - view.extra;
      for (int x = depth_lo; x < hi; ++x) {
        depth_color[x - depth_lo] = -1;
        depth_row[x - depth_lo] = background(&p, r, 0, x, y + 1, stage, &depth_color[x - depth_lo]);
      }
      bg1_back_row(&p, y + 1, depth_lo, hi, depth_back);
      bg1_priority_front(depth_back, depth_row, hi - depth_lo);
      bg1_remap(depth_back, depth_row, depth_lo, hi, -view.extra, view.width,
                bg1_depth_offset(stereo_shift), depth_map);
    }
    uint8_t brightness[32];
    for (int c = 0; c < 32; ++c) brightness[c] = (uint8_t)(((c << 3) | (c >> 2)) * (p.inidisp & 15) / 15);
    /* Dialogue masks BG1/BG2 for the panel and subtracts white from the
     * exposed backdrop ($81:915E). That global subtraction also blacks out
     * Storm's transparent sky between clouds in the newly exposed margins.
     * Recognize the captured panel setup across stages; preserve its native
     * window/text and every other color effect, including transition fades. */
    bool dialogue_backdrop = g_mmx_render_asset_repairs && p.cgadsub == 0xa0 &&
        p.cgwsel == 0x80 && p.fixedColor == 0x7fff && p.windowsel == 0x22 &&
        p.screenWindowed[0] == 3 && (p.screenEnabled[0] & 7) == 7;
    uint16_t objects[MMX_RENDER_MAX_WIDTH] = {0};
    int object_colors[MMX_RENDER_MAX_WIDTH];
    for (int x = 0; x < view.width; ++x) object_colors[x] = -1;
    if (flicker_count && stage && (p.screenEnabled[0] | p.screenEnabled[1]) & 16)
      draw_flicker_objects(&p, y, view, objects, object_colors);
    for (int i = (int)waiting_count - 1; i >= 0; --i) {
      Piece s = waiting[i];
      const MmxSpriteAsset *asset = s.animation == 0x53 ? waiting_zero : NULL;
      sprite(&p, r, s.x, s.y, s.attr, s.size, y, view, objects, true, asset, s.tile, object_colors, true, false, false);
    }
    bool replaced[128] = {false};
    bool zero_drawn[2] = {false,false};
    for (int i = (int)piece_count - 1; i >= 0; --i) {
      Piece s = pieces[i]; const MmxSpriteAsset *asset = piece_assets[i];
      bool frozen_enemy=stage && (MmxWeaponsFrozenEnemy(&frame_weapon_combat,s.object) ||
          MmxWeaponsMovedEnemy(&frame_weapon_combat,s.object) || (frame_coop.initialized &&
          (MmxWeaponsFrozenEnemy(&frame_coop.players[frame_coop.current^1].combat,s.object) ||
           MmxWeaponsMovedEnemy(&frame_coop.players[frame_coop.current^1].combat,s.object))));
      if (g_mmx_render_asset_repairs && fortress_sound_actor(s.object) &&
          (s.x >= 256 || s.x + s.size <= 0)) continue;
      /* Recorded pieces already obey the retail submission budget. Draw
       * their entire footprint, including x=255 which native D76A clips.
       * Only the explicit expanded list can add pieces beyond that budget. */
      bool center = g_mmx_render_asset_repairs;
      /* $80:9A3E spawns READY in $1CE8; $81:F091 selects group $19.
       * Restrict the recolor to this actor during the arrival phase. */
      bool red_ready=stage && MmxZeroEnabled() && !frame_zero.active_x &&
          frame.ram[0xd3]==2 && s.object==0x1ce8 && s.animation==0x19;
      bool red_death = stage && s.animation == 0x1d && MmxZeroDeathOrbRed(frame.ram,s.object);
      bool coop_buster=stage && frame_coop.initialized && s.object>=0x1228 && s.object<0x1428 &&
          ((frame.ram[s.object+10]==3 && s.animation==0x9e) ||
           ((frame.ram[s.object+10]==1 || frame.ram[s.object+10]==2) && s.animation==0x0e));
      bool menu_body = s.object == 0x1988 && (s.animation == 0 || s.animation == 0x18);
      bool zero_body = zero && (s.object == 0xba8 || menu_body);
      bool triad_x_body=cast_body && !zero && s.object==0xba8;
      bool triad_armor=cast_body && (s.object==0xc38 || s.object==0xc58 || s.object==0xc78);
      bool swap_actor = swapping && (s.object == 0xba8 || s.object == 0xc38 ||
          s.object == 0xc58 || s.object == 0xc78 || s.object == 0xc98);
      bool zero_charge = zero && stage && MmxZeroNativeChargeObject(s.object,frame.ram[s.object+10]);
      bool zero_armor = zero && (s.object == 0xc38 || s.object == 0xc58 || s.object == 0xc78 ||
          (zero_menu && (s.object == 0x1928 || s.object == 0x1948 || s.object == 0x1968)));
      bool oam_match = false;
      if (g_mmx_render_asset_repairs || coop_buster || zero_body || zero_armor || zero_charge || swap_actor || red_ready || red_death || triad_x_body || triad_armor || frozen_enemy) for (int slot = 16; slot < 128; ++slot) {
        unsigned pos = r->oam[slot * 2], hi = r->high_oam[slot / 4] >> (slot % 4 * 2);
        int ox = (pos & 255) | ((hi & 1) << 8); if (ox >= 256) ox -= 512;
        if (ox == s.x && (pos >> 8) == ((unsigned)s.y & 255) && r->oam[slot * 2 + 1] == s.attr) {
          oam_match = true;
          replaced[slot] = true; center = true;
        }
      }
      if(frame_rush.mode==MMX_RUSH_PLAYING && !frame.expand && s.object>=0xe68 && s.object<0x1d08 &&
          frame_rush.owner[(s.object-0xe68)/32] && !oam_match) continue;
      /* Match original OAM before reprojection. HUD and native script panels
       * keep screen coordinates; world pieces use the selected peer origin. */
      if(stage) {s.x-=view_dx;s.y-=view_dy;}
      /* Resource bindings own CHR/palette, not OBJ priority. Kuwanger's
       * platform backs deliberately sit behind the tower even though their
       * foreground pieces use the same resource. Preserve both priority bits
       * when substituting art, including a missing section's resource. */
      unsigned attr = asset ? (s.attr & 0xf000) | ((asset->attributes & 15) << 8) |
          (asset->live_tiles ? s.attr & 255 : 0) : s.attr;
      /* Cross-stage actors have no live allocation. Their resource supplies
       * the native priority as well as CHR/palette; retain authored flips. */
      if(asset && frame_rush.mode==MMX_RUSH_PLAYING && s.object>=0xe68 && s.object<0x1d08 &&
          frame_rush.owner[(s.object-0xe68)/32])
        attr=(attr&~0x3000u)|((asset->attributes&0x30)<<8);
      if (asset && asset->live_colors) attr = (attr & ~0x0e00u) | (s.attr & 0x0e00u);
      if (zero_armor || swap_actor || triad_armor || frozen_enemy) continue;
      if (zero_charge && MmxZeroHasChargeArt() && !frame.ram[0xbdb] && !weapon_item) continue;
      if (triad_x_body) {
        if (oam_match && !zero_drawn[0]) {
          zero_drawn[0]=true;
          weapon_sprite_row(cast_body,cast_body->pose,
              (int16_t)(word(frame.ram,0xbad)-view_camera(frame.ram,0x1e4d)),
              (int16_t)(word(frame.ram,0xbb0)-view_camera(frame.ram,0x1e50)),y,view,objects,object_colors);
        }
        continue;
      }
      if (zero_body && !oam_match && !(frame.expand && frame.ram[s.object + 14] &&
          (view_dx || view_dy || s.x + s.size <= 0 || s.x >= 256))) continue;
      if(stage && frame_coop.initialized && frame_zero.active_x && frame.ram[0xc31] &&
          s.object>=0xba8 && s.object<0xc98 && !oam_match && !view_dx && !view_dy &&
          s.x<256 && s.x+s.size>0) continue;
      if (zero_body) {
        if (!zero_drawn[menu_body]) {
          zero_drawn[menu_body] = true;
          const uint8_t *body = menu_body ? MmxZeroMenuPose() : zero;
          int zx = menu_body ? 128 : (int16_t)(word(frame.ram, 0xbad) - view_camera(frame.ram,0x1e4d));
          int zy = menu_body ? 152 : (int16_t)(word(frame.ram, 0xbb0) - view_camera(frame.ram,0x1e50)) + MmxZeroPoseOffsetY(frame.ram);
          bool pilot = !menu_body && (s.animation==0x6a || s.animation==0x6b);
          bool player_overlay = !menu_body && frame_player_overlay.active;
          if(pilot && !player_overlay) {
            /* X1 switches to a separate pilot group on boarding. Its pose
             * numbers are not movement poses. Keep its authored entry/walk/
             * punch offsets and expose Zero's original helmet/shoulders over
             * the cockpit. Vanilla X3 Zero has no Ride Armor pilot artwork. */
            /* Anchor on the native head piece this frame actually submitted.
             * The armor's sprites are built before it moves, so placing the
             * helmet from the pilot's RAM position led a jumping or lunging
             * armor by one frame of motion and the head floated above it. */
            const Piece *head=NULL;
            for(unsigned k=0;k<frame.piece_count && !head;++k) {
              const Piece *q=&frame.pieces[k];
              if(q->object==0xba8 && q->animation==s.animation && q->size==16) head=q;
            }
            if(head) {
              zx=head->x-view_dx+((head->attr&0x4000)?11:5);
              zy=head->y-view_dy+20;
            } else {
              const uint8_t *layout=sprite_arrangement(s.animation,frame.ram[0xbbf]&127);
              if(layout && layout[0]) {
                int hx,hy;pilot_head_offset(layout,&hx,&hy);
                int dx=hx+5;
                zx+=(frame.ram[0xbb9]&64)?-dx:dx;
                zy+=8+hy+20;
              }
            }
            body=MmxZeroMenuPose();
          }
          if (player_overlay) {
            if (pilot)
              align_wide_pilot_overlay(&frame_player_overlay, s.animation,
                                       &zx, &zy);
            unsigned z = ((((s.attr >> 12) & 3) * 4 + 2) << 12) | 0x680;
            player_overlay_row(&frame_player_overlay, y, zx, zy, z, view,
                               objects, object_colors);
          }
          int row = y - zy + 64;
          if (!player_overlay && row >= 0 && row < MMX_ZERO_HEIGHT && (!pilot || (row>=44 && row<64))) {
            const uint16_t *colors = MmxZeroColors();
            unsigned z = ((((s.attr >> 12) & 3) * 4 + 2) << 12) | 0x680;
            for (int col = 0; col < MMX_ZERO_WIDTH; ++col) {
              unsigned pixel = body[row * MMX_ZERO_WIDTH + col];
              if (!menu_body && blade && blade[row * MMX_ZERO_WIDTH + col]) pixel = blade[row * MMX_ZERO_WIDTH + col];
              int dx = zx + ((!menu_body && (frame.ram[0xbb9] & 64)) ? 63 - col : col - 64) + view.extra;
              if (pixel && dx >= 0 && dx < view.width) {
                objects[dx] = (uint16_t)(z | (pixel & 15));
                /* Charged Sting cycles X1's live player palette. Map Zero's
                 * material/shade roles to that palette instead of discarding
                 * the native cycle; skin and the dark outline remain legible. */
                static const uint8_t sting_shade[16] = {0,9,4,6,1,14,15,7,8,9,11,10,4,2,3,12};
                bool sting = !menu_body && frame.ram[0xc31] && pixel >= 16 && pixel < 32;
                if (sting) {
                  objects[dx] = (uint16_t)((z & ~255u) | (144 + sting_shade[pixel - 16]));
                  object_colors[dx] = -1; /* Live CGRAM already includes fades. */
                } else object_colors[dx] = !menu_body && pixel >= 16 && pixel < 32 ?
                    MmxZeroBodyColors(&frame_zero)[pixel - 16] : colors[pixel];
              }
            }
          }
        }
        continue;
      }
      if (zero_charge) {
        /* Spread the original motes around Zero's taller body. Keep each
         * sparkle's pixel art and native animation, timing and visibility. */
        int cx = (int16_t)(word(frame.ram,0xbad) - view_camera(frame.ram,0x1e4d));
        int cy = (int16_t)(word(frame.ram,0xbb0) - view_camera(frame.ram,0x1e50));
        s.x += (s.x + s.size / 2 - cx) / 4;
        s.y += (s.y + s.size / 2 - cy) / 4 - 6;
      }
      /* Extended choices have their own original X palette. Their native
       * buster proxy must not show the palette of an unrelated locked X1
       * weapon while navigating the pause screen. Preserve hit flashes. */
      /* The weapon palette is OBJ palette 1 (CGRAM 144..159, $81:9E8F).
       * Pieces drawn with another palette (the charge glow, armor parts)
       * keep live CGRAM: substituting them lost the glow and flattened
       * the boots. */
      if ((frame_zero.active_x || knc_bugfix) && weapon_colors && zero_actor(s.object, s.animation) &&
          (menu || ((attr & 0x0e00) == 0x0200 && (!x_charging(frame.ram) || knc_bugfix)))) asset = &x_weapon_palette;
      if(stage && frame_coop.initialized && s.object>=0x1228 && s.object<0x1428 &&
          frame.ram[s.object+10]==0x0c && s.animation==0x47 && (s.attr&0x0e00)==0x0600)
        asset=MmxRenderAssetsWeaponX(6,false);
      if(coop_buster) {
        const MmxSpriteAsset *beam=MmxRenderAssetsChargedBuster(s.animation,piece_pose(&s));
        if(beam) asset=beam;
      }
      sprite(&p, r, s.x, s.y, attr, s.size, y, view, objects, !center, asset, s.tile, object_colors, true, false, red_ready || red_death);
    }
    if (stage) coop_partner_row(&p,r,y,view,objects,object_colors);
    int bar_first = -1, bar_count = 0;
    if (hud) for (int slot = 16; slot <= 48; ++slot) {
      unsigned pos = r->oam[slot * 2], attr = r->oam[slot * 2 + 1];
      unsigned tile = attr & 255, hi = (r->high_oam[slot / 4] >> (slot % 4 * 2)) & 1;
      bool bar = !hi && (pos & 255) >= 216 && (pos >> 8) < 96 && ((attr >> 9) & 7) == 2 &&
          (tile == 128 || tile == 130 || tile == 132 || tile == 134 || tile == 170);
      if (bar) { if (bar_first < 0) bar_first = slot; ++bar_count; }
      else if (bar_first >= 0) break;
    }
    static const int sizes[8][2] = {{8,16},{8,32},{8,64},{16,32},{16,64},{32,64},{16,32},{16,32}};
    bool coop_hud=stage && frame_coop.initialized && r->oam[0]==0x5008 && r->oam[1]==0x3486;
    for (int slot = 127; slot >= 0; --slot) {
      if(coop_hud && slot<16) continue;
      if (replaced[slot]) continue;
      unsigned pos = r->oam[slot * 2], attr = r->oam[slot * 2 + 1];
      unsigned hi = r->high_oam[slot / 4] >> (slot % 4 * 2);
      int x = (pos & 255) | ((hi & 1) << 8), sy = pos >> 8;
      if (x >= 256) x -= 512;
      int size = sizes[p.obsel >> 5][(hi >> 1) & 1];
      if (x + size <= 0 || x >= 256) continue;
      /* Only the player's HUD badge: not weapon icons, bosses or actors.
       * This remains visible while the player blinks or has no body pieces. */
      bool zero_icon = (stage || zero_menu) && MmxZeroEnabled() && !frame_zero.active_x && slot == 0 &&
          x == 8 && sy == 80 && attr == 0x3486 && size == 16;
      bool anchored = stage && hud && sy < 96 && (slot < 16 || (bar_count >= 4 && slot >= bar_first && slot < bar_first + bar_count));
      bool reprojected = stage && !anchored && (view_dx || view_dy);
      if(reprojected) {
        /* OAM hides unused slots below the native viewport while retaining
         * their old tiles. Moving those slots into a distant peer view would
         * resurrect stale player/platform strips. A genuine top-edge sprite
         * wraps past y=255; unwrap it before applying the world-view offset. */
        if(sy>=224 && sy+size<=256) continue;
        if(sy>=224) sy-=256;
        x-=view_dx;sy-=view_dy;
      }
      if (anchored) { if (x < 25) x -= view.extra; else if (x >= 216) x += view.extra; }
      if (stage && frame_weapons.page && frame_weapons.weapon && slot == 7 && (pos & 255) == 24 && sy == 80 && attr == 0x3620) {
        /* Dedicated source gameplay footer, with its original frame,
         * pixels and weapon palette. Menu icons are a different asset. */
        const MmxWeaponPose *icon=MmxWeaponsHudIcon(frame_weapons.page,frame_weapons.weapon);
        const uint16_t *colors=MmxWeaponsPalette(frame_weapons.page,frame_weapons.weapon,false);
        int row=y-sy;
        if (icon && colors && row>=0 && row<16) for(int col=0;col<16;++col) {
          unsigned pixel=icon->pixels[row*16+col];
          int dx=x+col+view.extra;
          if(pixel && dx>=0 && dx<view.width) {
            objects[dx]=(uint16_t)(0xe6b0|pixel);object_colors[dx]=colors[pixel];
          }
        }
        continue;
      }
      sprite(&p, r, x, sy, attr, size, y, view, objects, false, NULL, 0, object_colors, reprojected, zero_icon, false);
    }
    if (charge && zero_drawn[0] && !swapping) {
      int zx=(int16_t)(word(frame.ram,0xbad)-view_camera(frame.ram,0x1e4d));
      int zy=(int16_t)(word(frame.ram,0xbb0)-view_camera(frame.ram,0x1e50))-8;
      int row=y-zy+64;
      unsigned palette=frame_zero.charge>=81 && frame_zero.charge<201 ? 32 : 0;
      if(row>=0 && row<MMX_ZERO_HEIGHT) for(int col=0;col<MMX_ZERO_WIDTH;++col) {
        unsigned pixel=charge[row*MMX_ZERO_WIDTH+col];
        int dx=zx+((frame.ram[0xbb9]&64)?63-col:col-64)+view.extra;
        if(pixel && dx>=0 && dx<view.width) {
          objects[dx]=(uint16_t)(0xe680|pixel); object_colors[dx]=MmxZeroColors()[palette+pixel];
        }
      }
    }
    if (stage && !swapping) {
      weapon_effects_row(&frame_weapon_combat,&p,r,y,view,objects,object_colors);
      if (frame_coop.initialized && frame_coop.players[frame_coop.current^1].status==MMX_COOP_ALIVE && !frame_coop.scene_owner)
        weapon_effects_row(&frame_coop.players[frame_coop.current^1].combat,&p,r,y,view,objects,object_colors);
    }
    if (stage) for (unsigned i = 0; i < frame_world_sprite_count; ++i)
      world_sprite_row(&frame_world_sprites[i], y, view, objects, object_colors);
    if (swapping) teleport_actor_row(frame.ram,&frame_zero,&p,r,y,view,objects,object_colors);
    if(coop_hud) coop_hud_row(&p,r,y,view,hud,objects,object_colors);
    if(rush_hud) rush_hud_row(&p,r,y,view,objects,object_colors);
    for (int sx = 0; sx < view.width; ++sx) {
      int x = sx - view.extra;
      if (menu && (x < 0 || x >= 256)) { out[y * view.width + sx] = 0; continue; }
      uint16_t screens[2] = {0x500, 0x500}, bg[3] = {0};
      int bg_colors[3] = {-1, -1, -1};
      for (int layer = 0; layer < 3; ++layer) if ((p.screenEnabled[0] | p.screenEnabled[1]) & (1 << layer)) {
        int bx = x + ((unsigned)layer == stereo_layer ? stereo_shift : 0), by = y + 1;
        if (layer == 0) bx += select_bg1;
        if (stage && layer==0) by+=crystal_ripple[y];
        if (p.mosaic & (1 << layer)) { int size = (p.mosaic >> 4) + 1;
          bx -= ((bx % size) + size) % size; by -= by % size; }
        if (depth1 && layer == 0) {
          int src = depth_map[sx] == INT16_MIN ? x : depth_map[sx];
          bg[0] = depth_row[src - depth_lo]; bg_colors[0] = depth_color[src - depth_lo];
          continue;
        }
        bg[layer] = background(&p, r, layer, bx, by, stage, &bg_colors[layer]);
      }
      if (palette_fade) {
        for (int layer = 0; layer < 3; ++layer) if (bg_colors[layer] >= 0)
          bg_colors[layer] = MmxRenderAssetsFadeColor((uint16_t)bg_colors[layer], palette_fade);
        if (object_colors[sx] >= 0)
          object_colors[sx] = MmxRenderAssetsFadeColor((uint16_t)object_colors[sx], palette_fade)|(object_colors[sx]&0x8000);
      }
      uint16_t camouflage_bg=0x500;
      for (int sub = 0; sub < 2; ++sub) {
        for (int layer = 0; layer < 3; ++layer)
          if ((p.screenEnabled[sub] & (1 << layer)) &&
              (!(p.screenWindowed[sub] & (1 << layer)) || !window(&p, layer, x, view.extra)) && bg[layer] > screens[sub]) screens[sub] = bg[layer];
        if(!sub) camouflage_bg=screens[0];
        if ((p.screenEnabled[sub] & 16) && (!(p.screenWindowed[sub] & 16) || !window(&p, 4, x, view.extra)) &&
            objects[sx] > screens[sub]) screens[sub] = objects[sx];
      }
      bool color_window = window(&p, 5, x, view.extra);
      if (beam_count) {
        color_window = false;
        for (unsigned i = 0; i < beam_count; ++i)
          color_window |= x >= beams[i].left[y] && x <= beams[i].right[y];
      }
      out[y * view.width + sx] = colour(&p, r->palette, brightness, screens[0], screens[1], color_window, object_colors[sx], bg_colors,
                                       dialogue_backdrop && (x < 0 || x >= 256),camouflage_bg);
    }
  }
  for (unsigned i = 0; i < frame_debug_rect_count; ++i)
    draw_debug_rect(out, view, &frame_debug_rects[i]);
  if(stage) {
    MmxBossRushState draw_rush=frame_rush;
    for(unsigned i=0;i<2;++i) {
      draw_rush.bosses[i].death_x-=view_camera(frame.ram,0x1e4d)-frame_rush.camera_x;
      draw_rush.bosses[i].death_y-=view_camera(frame.ram,0x1e50)-frame_rush.camera_y;
    }
    MmxBossRushDraw(out,view.width,view.extra,&draw_rush);
  }
  if(rush_hud && frame_rush.mode==MMX_RUSH_PLAYING) {
    char count[32];snprintf(count,sizeof(count),"DEFEATED %u",(unsigned)frame_rush.defeated);
    rush_native_text(out,view,view.width/2-(int)strlen(count)*4,208,count,7);
    static const char *const labels[]={"CP","SM","AA","LO","BK","SC","SE","FM"};
    for(unsigned i=0;i<2;++i) if(frame_rush.bosses[i].phase)
      rush_native_text(out,view,view.width-40+(int)i*16,96,labels[frame_rush.bosses[i].id],7);
  }
  return true;
}

bool MmxRendererDraw(uint32_t *out, MmxRenderView view, bool hud) {
  return draw_frame(out, view, hud, 0);
}

bool MmxRendererDrawEye(uint32_t *out, MmxRenderView view, bool hud,
                        MmxStereoEye eye, unsigned amount) {
  return draw_frame(out, view, hud, frame_stereo_offset(eye, amount));
}

/* The reference compositor supports expanded views and private artwork.
 * Native vanilla output needs neither. Decode a whole 8-pixel tile row at
 * once: four bitplane bytes become eight 4-bit pixels in one 32-bit word
 * through 256-entry tables (nibble i = pixel i from the left). No per-pixel
 * divisions or bit extraction remain in this loop. */
static uint32_t planar_spread[2][256];
static void init_planar_spread(void) {
  if (planar_spread[0][128]) return;
  for (unsigned v = 0; v < 256; ++v) {
    uint32_t normal = 0, flipped = 0;
    for (unsigned i = 0; i < 8; ++i) {
      normal |= (uint32_t)((v >> (7 - i)) & 1) << (4 * i);
      flipped |= (uint32_t)((v >> i) & 1) << (4 * i);
    }
    planar_spread[0][v] = normal; planar_spread[1][v] = flipped;
  }
}
/* BG rows carry the largest disparity on both sides. */
enum { NATIVE_MARGIN = MMX_STEREO_MAX_SOURCE_OFFSET,
       NATIVE_ROW = 256 + 2 * MMX_STEREO_MAX_SOURCE_OFFSET };
static void native_bg_row(const Ppu *p, unsigned layer, int y, uint16_t row[NATIVE_ROW]) {
  static const unsigned low[] = {8, 7, 1}, high[] = {12, 11, 3};
  unsigned bpp = layer == 2 ? 2 : 4;
  unsigned tile_shift = PPU_bigTiles(p, layer) ? 4 : 3;
  unsigned mask = (1u << tile_shift) - 1;
  unsigned py = (y + p->vScroll[layer]) & 1023, ty = py >> tile_shift;
  unsigned sc = p->bgXsc[layer], tile_base = PPU_bgTileAdr(p, layer);
  unsigned map = (sc & 0xfc) * 256 + (ty & 31) * 32;
  if ((sc & 2) && (ty & 32)) map += (sc & 1) ? 2048 : 1024;
  unsigned high_priority = layer == 2 && (p->bgmode & 8) ? 15 : high[layer];
  uint16_t *out = row + NATIVE_MARGIN;
  for (unsigned x = 0; x < 256;) {
    unsigned px = (x + p->hScroll[layer]) & 1023, tx = px >> tile_shift;
    unsigned a = map + (tx & 31);
    if ((sc & 1) && (tx & 32)) a += 1024;
    unsigned tile = drawing_vram[a & 0x7fff];
    unsigned cx = px & mask, cy = py & mask;
    unsigned flip = (tile >> 14) & 1;
    if (flip) cx ^= mask;
    if (tile & 0x8000) cy ^= mask;
    unsigned number = ((tile & 1023) + (cx >> 3) + ((cy >> 3) << 4)) & 1023;
    unsigned adr = (tile_base + number * bpp * 4 + (cy & 7)) & 0x7fff;
    unsigned bits01 = drawing_vram[adr];
    const uint32_t *spread = planar_spread[flip];
    uint32_t pixels = spread[bits01 & 255] | spread[bits01 >> 8] << 1;
    if (bpp == 4) {
      unsigned bits23 = drawing_vram[(adr + 8) & 0x7fff];
      pixels |= spread[bits23 & 255] << 2 | spread[bits23 >> 8] << 3;
    }
    unsigned first = px & 7, n = 8 - first;
    if (n > 256 - x) n = 256 - x;
    uint16_t *dst = out + x;
    x += n;
    pixels >>= first * 4;
    if (!pixels) { for (unsigned i = 0; i < n; ++i) dst[i] = 0; continue; }
    unsigned priority = tile & 0x2000 ? high_priority : low[layer];
    unsigned code = (priority << 12) | (layer << 8) | (((tile >> 10) & 7) << bpp);
    for (unsigned i = 0; i < n; ++i, pixels >>= 4) {
      unsigned pixel = pixels & 15;
      dst[i] = pixel ? (uint16_t)(code | pixel) : 0;
    }
  }
}

static unsigned fast_stereo_lines;
unsigned MmxRendererFastStereoLines(void) { return fast_stereo_lines; }

/* colour()'s arithmetic for a vanilla, unclipped pixel with math on. */
static inline uint32_t math_mix(unsigned rgb, unsigned other, bool subtract, bool half,
                                const uint8_t brightness[32]) {
  return MmxColourMixLanes(MmxColourLanes((uint16_t)rgb),
      MmxColourLanes((uint16_t)other), subtract, half, brightness);
}
static inline uint32_t native_window_colour(const Ppu *p, unsigned main, unsigned sub,
    bool inside, const uint8_t brightness[32], const uint32_t rgb[256],
    const uint32_t lanes[256], uint32_t fixed) {
  bool clipped = condition(p->cgwsel >> 6, inside);
  bool math = !condition((p->cgwsel >> 4) & 3, inside) &&
      (((p->cgadsub & 63) >> ((main >> 8) & 15)) & 1);
  if (!math) return clipped ? 0 : rgb[main & 255];
  bool half = (p->cgadsub & 64) && !clipped;
  unsigned other = fixed;
  if (p->cgwsel & 2) {
    if (sub & 255) other = lanes[sub & 255];
    else half = false;
  }
  return MmxColourMixLanes(clipped ? 0 : lanes[main & 255], other,
      (p->cgadsub & 128) != 0, half, brightness);
}
/* Stable OAM is the common case. Build its accepted row lists once rather
 * than rejecting the same ~120 off-row slots 224 times. Raster-changing OAM
 * and OBSEL retain the per-line evaluator below. */
typedef struct NativeObject { int x, y, size; uint16_t attr; } NativeObject;
static NativeObject native_objects[128];
static uint8_t native_object_count[224], native_object_rows[224][128];
static bool native_object_lists(void) {
  const Raster *first = &frame.lines[0];
  unsigned obsel = first->registers[offsetof(Ppu, obsel)];
  for (unsigned y = 1; y < 224; ++y) {
    const Raster *r = &frame.lines[y];
    if (r->registers[offsetof(Ppu, obsel)] != obsel ||
        memcmp(r->oam, first->oam, sizeof(r->oam)) ||
        memcmp(r->high_oam, first->high_oam, sizeof(r->high_oam))) return false;
  }
  static const unsigned sizes[8][2] = {{8,16},{8,32},{8,64},{16,32},{16,64},{32,64},{16,32},{16,32}};
  memset(native_object_count, 0, sizeof(native_object_count));
  for (int slot = 127; slot >= 0; --slot) {
    unsigned pos = first->oam[slot * 2];
    unsigned hi = first->high_oam[slot / 4] >> ((slot & 3) * 2);
    int x = (pos & 255) | ((hi & 1) << 8);
    if (x >= 256) x -= 512;
    int size = (int)sizes[obsel >> 5][(hi >> 1) & 1];
    if (x + size <= 0 || x >= 256) continue;
    native_objects[slot] = (NativeObject){x, pos >> 8, size, first->oam[slot * 2 + 1]};
    for (unsigned row = 0; row < (unsigned)size; ++row) {
      unsigned y = ((pos >> 8) + row) & 255;
      if (y < 224) native_object_rows[y][native_object_count[y]++] = (uint8_t)slot;
    }
  }
  return true;
}
static unsigned pair_reason;
/* Native-path window masks, indexed 0..2 BG, 4 OBJ, 5 colour window. */
static struct { uint32_t windowsel; uint8_t window1left, window1right, window2left, window2right;
  uint16_t wbgobjlog; } window_key;
static bool window_cache_valid;
static unsigned window_cached_layers;
static uint8_t window_inside[6][256];
/* Restrict masks to enabled screens/layers and colour modes that read
 * the colour window. Unused WSEL state has no cost and no depth effect. */
static unsigned native_window_layers(const Ppu *p) {
  unsigned layers = ((p->screenEnabled[0] & p->screenWindowed[0]) |
      (p->screenEnabled[1] & p->screenWindowed[1])) & 0x17;
  for (unsigned layer = 0; layer <= 4; ++layer)
    if (!((p->windowsel >> (layer * 4)) & 10)) layers &= ~(1u << layer);
  unsigned clip = p->cgwsel >> 6, prevent = (p->cgwsel >> 4) & 3;
  if (((p->windowsel >> 20) & 10) &&
      (clip == 1 || clip == 2 || prevent == 1 || prevent == 2)) layers |= 32;
  return layers;
}
static void native_windows(const Ppu *p, unsigned layers) {
  if (!window_cache_valid || window_key.windowsel != p->windowsel ||
      window_key.window1left != p->window1left || window_key.window1right != p->window1right ||
      window_key.window2left != p->window2left || window_key.window2right != p->window2right ||
      window_key.wbgobjlog != p->wbgobjlog) {
    window_key.windowsel = p->windowsel;
    window_key.window1left = p->window1left; window_key.window1right = p->window1right;
    window_key.window2left = p->window2left; window_key.window2right = p->window2right;
    window_key.wbgobjlog = p->wbgobjlog;
    window_cache_valid = true; window_cached_layers = 0;
  }
  unsigned missing = layers & ~window_cached_layers;
  for (unsigned layer = 0; layer < 6; ++layer) if (missing & (1u << layer))
    for (unsigned x = 0; x < 256; ++x) window_inside[layer][x] = window(p, layer, (int)x, 0);
  window_cached_layers |= layers;
}
unsigned MmxRendererStereoPairReason(void) { return pair_reason; }
bool MmxRendererStereoPairIsNative(MmxRenderView view) {
  if (!frame.valid) return false;
  bool vanilla = view.width == 256 && !view.extra && !g_mmx_render_asset_repairs &&
      !MmxZeroEnabled() && !frame_coop.initialized && !frame.expand &&
      frame_rush.mode == MMX_RUSH_OFF && !frame_rush.menu &&
      !frame_weapons.page && !frame_weapons.menu_page &&
      !frame_world_sprite_count && !frame_debug_rect_count &&
      !frame_player_overlay.active && !MmxKncBugfixActive(0) &&
      !MmxWeaponsTimePhase(&frame_weapon_combat);
  /* Masks/mosaic retain the already-tested reference path. They can change
   * on any scanline, so decide before producing either output. */
  pair_reason = vanilla ? 0 : 1;
  if (!vanilla) {
    /* Diagnostics: which feature kept this frame off the native path. */
    unsigned bits = 0;
    if (MmxZeroEnabled()) bits |= 1u << 8;
    if (frame_coop.initialized) bits |= 1u << 9;
    if (frame.expand) bits |= 1u << 10;
    if (frame_rush.mode != MMX_RUSH_OFF) bits |= 1u << 11;
    if (frame_rush.menu) bits |= 1u << 12;
    if (frame_weapons.page) bits |= 1u << 13;
    if (frame_weapons.menu_page) bits |= 1u << 14;
    if (frame_world_sprite_count) bits |= 1u << 15;
    if (frame_debug_rect_count) bits |= 1u << 16;
    if (frame_player_overlay.active) bits |= 1u << 17;
    if (MmxKncBugfixActive(0)) bits |= 1u << 18;
    if (MmxWeaponsTimePhase(&frame_weapon_combat)) bits |= 1u << 19;
    if (g_mmx_render_asset_repairs) bits |= 1u << 20;
    pair_reason |= bits;
  }
  for (unsigned y = 0; vanilla && y < 224; ++y) {
    const uint8_t *regs = frame.lines[y].registers;
    Ppu p; memcpy(&p, regs, PPU_SAVESTATE_REGS_SIZE);
    /* Window masks (dialogue panels) are handled natively since rev. 3. */
    if ((p.mosaic & 0xf0) && (p.mosaic & (p.screenEnabled[0] | p.screenEnabled[1]) & 7)) { vanilla = false; pair_reason |= 4; }
  }
  return vanilla;
}

bool MmxRendererDrawStereoPair(uint32_t *left, uint32_t *right,
    MmxRenderView view, bool hud, unsigned amount) {
  fast_stereo_lines = 0;
  if (!left || !right || !frame.valid || (left == right && amount)) return false;
  init_planar_spread();
  bool stage = MmxWidePolicy_IsStageScene(frame.ram);
  bool vanilla = MmxRendererStereoPairIsNative(view);
  if (!vanilla) {
    if (!draw_frame(left, view, hud, frame_stereo_offset(MMX_STEREO_LEFT, amount))) return false;
    /* draw_frame keeps a line flat under these conditions (its stereo plane
     * rule). When every line is flat both eyes are equal: draw once. */
    bool disparity = false;
    if (amount && (stage || stage_select_scene()) && frame_stereo_offset(MMX_STEREO_LEFT, amount) &&
        !(frame.ram[0x1f7a] == 6 && frame.ram[0x1e89] == 0x0c))
      for (unsigned y = 0; y < 224 && !disparity; ++y) {
        const uint8_t *regs = frame.lines[y].registers; Ppu q;
        memcpy(&q, regs, PPU_SAVESTATE_REGS_SIZE);
        disparity = stereo_plane_allowed(&q);
      }
    if (!disparity) amount = 0;
    if (!amount) { if (right != left) memcpy(right, left, (size_t)view.width * 224 * sizeof(*right)); return true; }
    return draw_frame(right, view, hud, frame_stereo_offset(MMX_STEREO_RIGHT, amount));
  }
  memset(&stats, 0, sizeof(stats)); stats.pieces = frame.piece_count;
  view_dx = view_dy = 0;
  door_cache_reset();
  if (stage) prepare_stage_planes();
  const bool select = !stage && stage_select_scene();
  const bool flat = !stage && !select && flat_native_scene();
  const int offset = flat ? 0 : frame_stereo_offset(MMX_STEREO_LEFT, amount);
  const unsigned stereo_layer = MmxRendererStereoLayer(), stereo_mask = 1u << stereo_layer;
  const unsigned flat_layer = stereo_layer ^ 1u;
  window_cache_valid = false;
  bool stable_objects = native_object_lists();
  uint32_t rgb[256], math_rgb[256], lanes[256]; uint8_t brightness[32];
  unsigned palette_serial = 0, math_palette_serial = 0; uint32_t math_key = 0; bool math_valid = false;
  const uint16_t *last_palette = NULL; unsigned last_brightness = UINT_MAX;
  for (int y = 0; y < 224; ++y) {
    const Raster *r = &frame.lines[y]; Ppu p;
    memcpy(&p, r->registers, PPU_SAVESTATE_REGS_SIZE);
    uint32_t *out_l = left + y * 256, *out_r = right + y * 256;
    if (!(stage || select || flat) || (p.bgmode & 7) != 1) {
      memcpy(out_l, frame.stock + y * 256, 256 * sizeof(*out_l));
      if (out_r != out_l) memcpy(out_r, out_l, 256 * sizeof(*out_r));
      ++stats.fallback_lines; continue;
    }
    ++stats.custom_lines; ++fast_stereo_lines;
    if (p.inidisp & 128) {
      memset(out_l, 0, 256 * sizeof(*out_l));
      if (out_r != out_l) memset(out_r, 0, 256 * sizeof(*out_r));
      continue;
    }
    drawing_vram = frame.lines[vram_owner[y]].vram;
    unsigned intensity = p.inidisp & 15;
    if (intensity != last_brightness || !last_palette ||
        memcmp(last_palette, r->palette, sizeof(r->palette))) {
      for (unsigned c = 0; c < 32; ++c)
        brightness[c] = (uint8_t)(((c << 3) | (c >> 2)) * intensity / 15);
      for (unsigned c = 0; c < 256; ++c) {
        unsigned v = r->palette[c];
        lanes[c] = MmxColourLanes((uint16_t)v);
        rgb[c] = (uint32_t)brightness[v & 31] << 16 |
            (uint32_t)brightness[(v >> 5) & 31] << 8 | brightness[(v >> 10) & 31];
      }
      last_brightness = intensity; last_palette = r->palette; ++palette_serial;
    }
    /* Every bg[] entry read below is written first: whole rows for enabled
     * layers, and the shifted BG2 margins when a disparity is active. */
    uint16_t bg[3][NATIVE_ROW], objects[256];
    int object_colors[256]; /* Written by sprite(), never read here. */
    unsigned enabled = p.screenEnabled[0] | p.screenEnabled[1];
    for (unsigned layer = 0; layer < 3; ++layer)
      if (enabled & (1u << layer)) native_bg_row(&p, layer, y + 1, bg[layer]);
    /* Same plane/actor/mosaic rule as the reference compositor. */
    int shift = stereo_plane_allowed(&p) ? offset : 0;
    uint32_t fixed_lanes = MmxColourLanes(p.fixedColor);
    const bool plane_rows = (enabled & stereo_mask) != 0;
    if (!plane_rows) {
      /* Revision 17: BG2 off on both screens, BG1 scenery may still move. */
      if (stage && shift > 0 && bg1_depth_active(&p, shift)) memset(bg[stereo_layer], 0, sizeof(bg[0]));
      else shift = 0;
    }
    /* The select screen's tilemaps wrap like the hardware: native_bg_row's
     * margins are already the true neighbours. */
    if (stage && plane_rows) for (int x = -shift; x < 0; ++x) {
      int private_color = -1;
      bg[stereo_layer][x + NATIVE_MARGIN] = background(&p, r, stereo_layer, x, y + 1, true, &private_color);
    }
    if (stage && plane_rows) for (int x = 256; x < 256 + shift; ++x) {
      int private_color = -1;
      bg[stereo_layer][x + NATIVE_MARGIN] = background(&p, r, stereo_layer, x, y + 1, true, &private_color);
    }
    /* Revision 16: BG1 scenery joins the per-eye scene plane. Samples stay
     * within the visible columns (an edge sample repeats, as for terrain). */
    uint16_t depth_l[256], depth_r[256];
    bool depth1 = shift > 0 && (select ? bg1_select_depth_active(&p) : bg1_depth_active(&p, shift));
    const bool merge = (p.screenEnabled[0] & 1) == ((p.screenEnabled[0] >> 1) & 1) &&
        (p.screenEnabled[1] & 1) == ((p.screenEnabled[1] >> 1) & 1);
    if (depth1 && select) {
      /* Portraits: the whole of BG1, a third of the picture's depth. */
      const int s1 = (shift + 2) / 3;
      const uint16_t *b1 = bg[0] + NATIVE_MARGIN;
      const uint16_t *b2l = bg[stereo_layer] + NATIVE_MARGIN + shift;
      const uint16_t *b2r = bg[stereo_layer] + NATIVE_MARGIN - shift;
      for (int x = 0; x < 256; ++x) {
        uint16_t vl = b1[x + s1], vr = b1[x - s1], pl = b2l[x], pr = b2r[x];
        depth_l[x] = vl > pl ? vl : pl; depth_r[x] = vr > pr ? vr : pr;
      }
      memset(bg[0], 0, sizeof(bg[0]));
    } else if (depth1) {
      const int s1 = bg1_depth_offset(shift);
      uint8_t back[256]; int16_t prev_back[256], next_back[256]; uint16_t row[256];
      uint16_t *b1 = bg[0] + NATIVE_MARGIN;
      memcpy(row, b1, sizeof row);
      bg1_back_row(&p, y + 1, 0, 256, back);
      /* bg1_priority_front, the scenery test and the first half of
       * bg1_remap's walk (nearest scenery column at or before each column)
       * in one pass; the second half (at or after) follows. */
      bool scenery = false;
      int last = -1;
      for (int x = 0; x < 256; ++x) {
        unsigned v = row[x];
        if ((v & 255) && (v & 0x4000)) back[x] = 0;
        if (back[x]) { last = x; scenery |= (v & 255) != 0; }
        prev_back[x] = (int16_t)last;
      }
      if (scenery) {
      last = 256;
      for (int x = 255; x >= 0; --x) { if (back[x]) last = x; next_back[x] = (int16_t)last; }
      /* The left eye's offset is never negative (frame_stereo_offset). */
      const uint16_t *b2l = bg[stereo_layer] + NATIVE_MARGIN + shift;
      const uint16_t *b2r = bg[stereo_layer] + NATIVE_MARGIN - shift;
      /* Unmerged (BG1/BG2 on different screens): the planes hold BG1
       * scenery alone and are combined per screen below. */
      static const uint16_t none[256];
      if (!merge) { b2l = none; b2r = none; }
      for (int x = 0; x < 256; ++x) {
        uint16_t pl = b2l[x], pr = b2r[x];
        if (!back[x] && (row[x] & 255)) { depth_l[x] = pl; depth_r[x] = pr; continue; }
        int cl = x + s1 > 255 ? 255 : x + s1, cr = x - s1 < 0 ? 0 : x - s1;
        int ml = prev_back[cl], mr = next_back[cr];
        if (ml < x) ml = x;
        if (mr > x) mr = x;
        uint16_t vl = row[ml], vr = row[mr];
        depth_l[x] = vl > pl ? vl : pl; depth_r[x] = vr > pr ? vr : pr;
        b1[x] = 0; /* the flat fold keeps terrain only */
      }
      }
      /* No opaque scenery on this line: the flat result is identical. */
      depth1 = scenery;
    }
    if (enabled & 16) {
      memset(objects, 0, sizeof(objects));
      if (flicker_count && stage) draw_flicker_objects(&p, y, view, objects, object_colors);
      if (stable_objects) {
        for (unsigned i = 0; i < native_object_count[y]; ++i) {
          const NativeObject *s = &native_objects[native_object_rows[y][i]];
          sprite(&p, r, s->x, s->y, s->attr, s->size, y, view, objects,
              false, NULL, 0, object_colors, false, false, false);
        }
      } else {
      static const unsigned sizes[8][2] = {{8,16},{8,32},{8,64},{16,32},{16,64},{32,64},{16,32},{16,32}};
      const unsigned *size_pair = sizes[p.obsel >> 5];
      for (int slot = 127; slot >= 0; --slot) {
        unsigned pos = r->oam[slot * 2];
        unsigned hi = r->high_oam[slot / 4] >> ((slot & 3) * 2);
        int size = (int)size_pair[(hi >> 1) & 1];
        /* Same row test sprite() performs, without the call for the
         * ~120 slots that miss each scanline. */
        if ((unsigned)((y - (int)(pos >> 8)) & 255) >= (unsigned)size) continue;
        int x = (pos & 255) | ((hi & 1) << 8);
        if (x >= 256) x -= 512;
        if (x + size <= 0 || x >= 256) continue;
        sprite(&p, r, x, pos >> 8, r->oam[slot * 2 + 1], size, y, view, objects,
            false, NULL, 0, object_colors, false, false, false);
      }
      }
    }
    unsigned active_windows = native_window_layers(&p);
    if (active_windows) {
      native_windows(&p, active_windows);
      uint16_t screen[2][256];
      bool need_sub = (p.cgwsel & 2) && (p.cgadsub & 63);
      for (unsigned sub = 0; sub < (need_sub ? 2u : 1u); ++sub) {
        unsigned mask = p.screenEnabled[sub], windowed = p.screenWindowed[sub] & active_windows;
        uint16_t *f = screen[sub];
        for (unsigned x = 0; x < 256; ++x) f[x] = 0x500;
        for (unsigned layer = 0; layer < 3; ++layer) if ((mask & (1u << layer)) && !(layer == stereo_layer && shift)) {
          const uint16_t *src = bg[layer] + NATIVE_MARGIN;
          if (windowed & (1u << layer)) { const uint8_t *in = window_inside[layer];
            for (unsigned x = 0; x < 256; ++x) if (!in[x] && src[x] > f[x]) f[x] = src[x]; }
          else for (unsigned x = 0; x < 256; ++x) if (src[x] > f[x]) f[x] = src[x];
        }
        if (mask & 16) {
          if (windowed & 16) { const uint8_t *in = window_inside[4];
            for (unsigned x = 0; x < 256; ++x) if (!in[x] && objects[x] > f[x]) f[x] = objects[x]; }
          else for (unsigned x = 0; x < 256; ++x) if (objects[x] > f[x]) f[x] = objects[x];
        }
      }
      const uint16_t *bl = bg[stereo_layer] + NATIVE_MARGIN + shift, *br = bg[stereo_layer] + NATIVE_MARGIN - shift;
      bool main_plane = shift && (p.screenEnabled[0] & stereo_mask), sub_plane = shift && (p.screenEnabled[1] & stereo_mask);
      bool main_masked = (p.screenWindowed[0] & active_windows & stereo_mask) != 0;
      bool sub_masked = (p.screenWindowed[1] & active_windows & stereo_mask) != 0;
      for (unsigned x = 0; x < 256; ++x) {
        bool in = (active_windows & 32) && window_inside[5][x];
        bool plane_hidden = (active_windows & stereo_mask) && window_inside[stereo_layer][x];
        unsigned m = screen[0][x], sub = need_sub ? screen[1][x] : 0x500;
        unsigned l0 = m, l1 = sub, r0 = m, r1 = sub;
        if (main_plane && !(main_masked && plane_hidden)) { if (bl[x] > l0) l0 = bl[x]; if (br[x] > r0) r0 = br[x]; }
        if (sub_plane && !(sub_masked && plane_hidden)) { if (bl[x] > l1) l1 = bl[x]; if (br[x] > r1) r1 = br[x]; }
        out_l[x] = native_window_colour(&p, l0, l1, in, brightness, rgb, lanes, fixed_lanes);
        if (shift) out_r[x] = l0 == r0 && l1 == r1 ? out_l[x] :
            native_window_colour(&p, r0, r1, in, brightness, rgb, lanes, fixed_lanes);
      }
      if (!shift && out_r != out_l) memcpy(out_r, out_l, 256 * sizeof(*out_r));
      continue;
    }
    /* Highest code wins (priority:4 | layer:4 | colour:8); 0 = transparent.
     * Fold the layers that never move into one plane per screen, then add
     * the selected scene-plane sample for each eye. */
    uint16_t front[2][256];
    unsigned clip_mode = p.cgwsel >> 6, prevent_mode = (p.cgwsel >> 4) & 3;
    bool clipped = condition(clip_mode, false);
    unsigned math_layers = condition(prevent_mode, false) ? 0 : (p.cgadsub & 63);
    /* The subscreen only matters when colour math reads it (CGWSEL bit 1)
     * or for the rare clip-to-black modes, which keep the general colour(). */
    bool sub_math = math_layers && (p.cgwsel & 2);
    bool need_sub = clipped || sub_math;
    if (!clipped && math_layers && !sub_math) {
      /* Fixed-colour math: the result depends only on the palette entry,
       * so it is a 256-entry table like rgb[] (same arithmetic as colour). */
      uint32_t key = (uint32_t)p.cgadsub | (uint32_t)p.fixedColor << 8;
      if (!math_valid || math_key != key || math_palette_serial != palette_serial) {
        bool subtract = p.cgadsub & 128, half = p.cgadsub & 64;
        for (unsigned c = 0; c < 256; ++c)
          math_rgb[c] = math_mix(r->palette[c], p.fixedColor, subtract, half, brightness);
        math_valid = true; math_key = key; math_palette_serial = palette_serial;
      }
    }
    for (unsigned sub = 0; sub < (need_sub ? 2u : 1u); ++sub) {
      unsigned mask = p.screenEnabled[sub];
      uint16_t *f = front[sub];
      for (unsigned x = 0; x < 256; ++x) f[x] = 0x500;
      if (mask & (1u << flat_layer)) { const uint16_t *s = bg[flat_layer] + NATIVE_MARGIN;
        for (unsigned x = 0; x < 256; ++x) if (s[x] > f[x]) f[x] = s[x]; }
      if (mask & 4) { const uint16_t *s = bg[2] + NATIVE_MARGIN;
        for (unsigned x = 0; x < 256; ++x) if (s[x] > f[x]) f[x] = s[x]; }
      if (mask & 16) {
        for (unsigned x = 0; x < 256; ++x) if (objects[x] > f[x]) f[x] = objects[x]; }
    }
    const uint16_t *bl = bg[stereo_layer] + NATIVE_MARGIN + shift, *br = bg[stereo_layer] + NATIVE_MARGIN - shift;
    bool main_plane = (p.screenEnabled[0] & stereo_mask) != 0, sub_plane = (p.screenEnabled[1] & stereo_mask) != 0;
    const uint16_t *sl = NULL, *sr = NULL;
    uint16_t mix[4][256];
    if (depth1 && (merge || select)) { bl = depth_l; br = depth_r; }
    else if (depth1) {
      /* Revision 17: per-screen planes, BG1 scenery under BG1's enables
       * and BG2 under its own (same union the reference draws). */
      const uint16_t *gl = bl, *gr = br;
      const bool b1s = (p.screenEnabled[1] & 1) != 0;
      if (main_plane) {
        for (unsigned x = 0; x < 256; ++x) {
          mix[0][x] = depth_l[x] > gl[x] ? depth_l[x] : gl[x];
          mix[1][x] = depth_r[x] > gr[x] ? depth_r[x] : gr[x];
        }
        bl = mix[0]; br = mix[1];
      } else { bl = depth_l; br = depth_r; }
      if (b1s && sub_plane) {
        for (unsigned x = 0; x < 256; ++x) {
          mix[2][x] = depth_l[x] > gl[x] ? depth_l[x] : gl[x];
          mix[3][x] = depth_r[x] > gr[x] ? depth_r[x] : gr[x];
        }
        sl = mix[2]; sr = mix[3];
      } else if (b1s) { sl = depth_l; sr = depth_r; }
      else { sl = gl; sr = gr; }
      main_plane = true; sub_plane = b1s || sub_plane;
    }
    if (!sl) { sl = bl; sr = br; }
    if (!need_sub && math_layers) {
      const uint16_t *f = front[0];
      for (unsigned x = 0; x < 256; ++x) {
        unsigned m = f[x], l = m, q = m;
        if (main_plane) { if (bl[x] > l) l = bl[x]; if (br[x] > q) q = br[x]; }
        out_l[x] = (math_layers >> ((l >> 8) & 15)) & 1 ? math_rgb[l & 255] : rgb[l & 255];
        if (shift) out_r[x] = q == l ? out_l[x] :
            (math_layers >> ((q >> 8) & 15)) & 1 ? math_rgb[q & 255] : rgb[q & 255];
      }
      if (!shift && out_r != out_l) memcpy(out_r, out_l, 256 * sizeof(*out_r));
      continue;
    }
    if (!need_sub) {
      /* No colour math can touch this line: the main screen alone decides. */
      const uint16_t *f = front[0];
      if (!main_plane) {
        for (unsigned x = 0; x < 256; ++x) out_l[x] = rgb[f[x] & 255];
        if (out_r != out_l) memcpy(out_r, out_l, 256 * sizeof(*out_r));
      } else {
        for (unsigned x = 0; x < 256; ++x) {
          unsigned m = f[x], l = bl[x] > m ? bl[x] : m;
          out_l[x] = rgb[l & 255];
          if (shift) { unsigned q = br[x] > m ? br[x] : m;
            out_r[x] = q == l ? out_l[x] : rgb[q & 255]; }
        }
        if (!shift && out_r != out_l) memcpy(out_r, out_l, 256 * sizeof(*out_r));
      }
      continue;
    }
    if (!clipped) {
      /* Subscreen math, inline: same rules as colour() for vanilla pixels
       * (no private colours): a transparent subscreen pixel uses the fixed
       * colour and cancels halving. */
      bool subtract = p.cgadsub & 128, halving = p.cgadsub & 64;
      for (unsigned x = 0; x < 256; ++x) {
        unsigned m0 = front[0][x], m1 = front[1][x];
        unsigned l0 = m0, l1 = m1, r0 = m0, r1 = m1;
        if (main_plane) { if (bl[x] > l0) l0 = bl[x]; if (br[x] > r0) r0 = br[x]; }
        if (sub_plane) { if (sl[x] > l1) l1 = sl[x]; if (sr[x] > r1) r1 = sr[x]; }
        out_l[x] = !((math_layers >> ((l0 >> 8) & 15)) & 1) ? rgb[l0 & 255] :
            MmxColourMixLanes(lanes[l0 & 255], l1 & 255 ? lanes[l1 & 255] : fixed_lanes,
                subtract, halving && (l1 & 255), brightness);
        if (!shift) continue;
        out_r[x] = l0 == r0 && l1 == r1 ? out_l[x] :
            !((math_layers >> ((r0 >> 8) & 15)) & 1) ? rgb[r0 & 255] :
            MmxColourMixLanes(lanes[r0 & 255], r1 & 255 ? lanes[r1 & 255] : fixed_lanes,
                subtract, halving && (r1 & 255), brightness);
      }
      if (!shift && out_r != out_l) memcpy(out_r, out_l, 256 * sizeof(*out_r));
      continue;
    }
    const int private_colors[3] = {-1, -1, -1};
    for (unsigned x = 0; x < 256; ++x) {
      unsigned m0 = front[0][x], m1 = front[1][x];
      unsigned l0 = m0, l1 = m1, r0 = m0, r1 = m1;
      if (main_plane) { if (bl[x] > l0) l0 = bl[x]; if (br[x] > r0) r0 = br[x]; }
      if (sub_plane) { if (sl[x] > l1) l1 = sl[x]; if (sr[x] > r1) r1 = sr[x]; }
      out_l[x] = !clipped && !((math_layers >> ((l0 >> 8) & 15)) & 1) ? rgb[l0 & 255] :
          colour(&p, r->palette, brightness, (uint16_t)l0, (uint16_t)l1, false, -1,
              private_colors, false, 0x500);
      if (!shift) continue;
      out_r[x] = l0 == r0 && l1 == r1 ? out_l[x] :
          !clipped && !((math_layers >> ((r0 >> 8) & 15)) & 1) ? rgb[r0 & 255] :
          colour(&p, r->palette, brightness, (uint16_t)r0, (uint16_t)r1, false, -1,
              private_colors, false, 0x500);
    }
    if (!shift && out_r != out_l) memcpy(out_r, out_l, 256 * sizeof(*out_r));
  }
  return true;
}
