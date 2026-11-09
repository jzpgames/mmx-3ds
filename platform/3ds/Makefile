ifeq ($(strip $(DEVKITPRO)),)
$(error Set DEVKITPRO to your devkitPro installation)
endif
DEVKITARM ?= $(DEVKITPRO)/devkitARM
export DEVKITPRO DEVKITARM

APP_TITLE := MMX 3DS - Stereo probe
APP_DESCRIPTION := Synthetic scene - not the game
APP_AUTHOR := MMX 3DS experiment
include $(DEVKITARM)/3ds_rules

ROOT := ../..
BUILD := build
OUT := $(BUILD)/mmx-stereo-probe
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter \
          -ffunction-sections -fdata-sections -fno-strict-aliasing $(ARCH)
CPPFLAGS := -D__3DS__ -I$(ROOT)/src -I$(ROOT)/snesrecomp/runner/src \
            -I$(DEVKITPRO)/libctru/include
LDFLAGS := -specs=3dsx.specs $(ARCH) -Wl,--gc-sections,-Map,$(OUT).map
LIBS := -L$(DEVKITPRO)/libctru/lib -lctru -lm
RENDER_NAMES := mmx_renderer mmx_render_assets mmx_display mmx_wide_policy \
                mmx_zero mmx_weapons mmx_weapon_combat mmx_coop_view \
                mmx_boss_rush mmx_boss_rush_draw
OBJECTS := $(addprefix $(BUILD)/,$(addsuffix .o,$(RENDER_NAMES))) \
           $(BUILD)/probe_main.o $(BUILD)/probe_scene.o $(BUILD)/probe_mode_hooks.o

.DEFAULT_GOAL := all
.PHONY: all clean
all: $(OUT).3dsx

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: $(ROOT)/src/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(OUT).elf: $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) $(LIBS) -o $@

$(OUT).smdh: | $(BUILD)

$(OUT).3dsx: $(OUT).elf $(OUT).smdh
	3dsxtool $< $@ --smdh=$(OUT).smdh

clean:
	rm -rf $(BUILD)

-include $(OBJECTS:.o=.d)
