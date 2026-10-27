# poryaaaa accuracy test ROM.
#
# Builds a standalone GBA ROM from the pokeemerald tree's m4a engine
# (m4a.c, m4a_1.s, m4a_tables.c), its full sound data (voicegroups, samples,
# song table, every vanilla song compiled with mid2agb exactly as the game
# does) plus the micro-test voicegroup and songs under micro/.
#
#   make POKEEMERALD=~/pokeemerald [BUILD=build] [MIXER=vanilla|hq] [RATE=<hz>]
#
# MIXER=hq builds the same ROM with ipatix's HQ mixer installed the way the
# pret wiki describes (docs/HQ-MIXER-ACCURACY.md, section 4): hq/m4a_1.s in
# place of the tree's src/m4a_1.s, the two-line m4a.c change (hq/m4a.c.sed)
# and the atomic DmaSetUnchecked (hq/include/gba/macro.h first on the
# include path).  Everything else, AgbMain included, is the vanilla build.
#
# RATE=<hz> (default 13379, the games' rate) builds the ROM at another
# sound-mode rate (5734 10512 13379 15768 18157 21024 26758 31536 36314 40137
# 42048; 7884 only with MIXER=vanilla): m4aSoundInit's m4aSoundMode word gets
# SOUND_MODE_FREQ_<hz> (m4a.c line 79, the wiki's "engine rate" line; SoundInit's
# own SampleFreqSet(SOUND_MODE_FREQ_13379) is untouched, as in the game).  With
# MIXER=hq the wiki's two rate-dependent sizes follow: hq_buffer_ptr[] is the
# rate's frame length and PCM_DMA_BUF_SIZE becomes 7 frames (the wiki's table,
# docs/HQ-MIXER-ACCURACY.md appendix C) in both include/gba/m4a_internal.h and
# constants/m4a_constants.inc (generated copies under $(BUILD), first on the
# include paths).  A vanilla ROM at another rate keeps the define at 1584
# (the game's pcmDmaPeriod = 1584 / frame rule).
#
# Requires: agbcc installed in <pokeemerald>/tools/agbcc, the pokeemerald tools
# built (mid2agb, gbafix) and libagbsyscall built, arm-none-eabi binutils.

SHELL := /bin/bash
.SHELLFLAGS := -o pipefail -ec

POKEEMERALD ?= $(HOME)/pokeemerald
BUILD ?= build
MIXER ?= vanilla
RATE ?= 13379

# gPcmSamplesPerVBlankTable: the frame length (samples per VBlank) of each
# sound-mode rate, as the HQ mixer's FRAME_LENGTH_* table and the wiki give it.
FRAME_5734  := 0x60
FRAME_7884  := 0x84
FRAME_10512 := 0xB0
FRAME_13379 := 0xE0
FRAME_15768 := 0x108
FRAME_18157 := 0x130
FRAME_21024 := 0x160
FRAME_26758 := 0x1C0
FRAME_31536 := 0x210
FRAME_36314 := 0x260
FRAME_40137 := 0x2A0
FRAME_42048 := 0x2C0
FRAME := $(FRAME_$(RATE))
ifeq ($(FRAME),)
$(error RATE must be one of the m4a sound-mode rates (5734 7884 10512 13379 15768 18157 21024 26758 31536 36314 40137 42048))
endif
ifeq ($(MIXER)_$(RATE),hq_7884)
$(error the HQ mixer does not support 7884 Hz (frame 0x84 is not a multiple of 8))
endif
# The wiki's PCM_DMA_BUF_SIZE for the HQ mixer with reverb: 7 frames at a raised
# (or lowered) rate; the game's 1584 (7 * 224 + 16) stays at the default rate.
ifeq ($(RATE),13379)
HQ_PCM_DMA_BUF_SIZE := 1584
else
HQ_PCM_DMA_BUF_SIZE := $(shell printf '%d' $$((7 * $(FRAME))))
endif

AGBCC    := $(POKEEMERALD)/tools/agbcc
PREFIX   ?= arm-none-eabi-
CPP      := $(PREFIX)cpp
AS       := $(PREFIX)as
LD       := $(PREFIX)ld
OBJCOPY  := $(PREFIX)objcopy
CC1      := $(AGBCC)/bin/agbcc
CC1_OLD  := $(AGBCC)/bin/old_agbcc
MID      := $(POKEEMERALD)/tools/mid2agb/mid2agb
GBAFIX   := $(POKEEMERALD)/tools/gbafix/gbafix
PREPROC  := $(POKEEMERALD)/tools/preproc/preproc
CHARMAP  := $(POKEEMERALD)/charmap.txt
ABS_ROMDIR := $(abspath .)
ABS_BUILD  := $(abspath $(BUILD))

ASFLAGS  := -mcpu=arm7tdmi --defsym MODERN=0 -I $(POKEEMERALD) -I $(POKEEMERALD)/sound -I $(ABS_ROMDIR) -I $(ABS_BUILD)
ifeq ($(MIXER),hq)
HQ_CPPFLAGS := -iquote $(ABS_ROMDIR)/hq/include
M4A_1_SRC   := $(ABS_ROMDIR)/hq/m4a_1.s
M4A_C_SRC   := $(BUILD)/m4a.c
M4A_C_SED   := -f hq/m4a.c.sed -e 's/hq_buffer_ptr\[0xE0\]/hq_buffer_ptr[$(FRAME)]/'
ifneq ($(RATE),13379)
# The resized ring: generated m4a_internal.h (C) and m4a_constants.inc (the
# mixer's .include, redirected to the copy) with the wiki's value.
HQ_CPPFLAGS := -iquote $(ABS_BUILD)/include $(HQ_CPPFLAGS)
M4A_1_SRC   := $(ABS_BUILD)/m4a_1.s
M4A_HDR_DEP := $(BUILD)/include/gba/m4a_internal.h
endif
else ifeq ($(MIXER),vanilla)
HQ_CPPFLAGS :=
M4A_1_SRC   := $(POKEEMERALD)/src/m4a_1.s
M4A_C_SRC   := $(POKEEMERALD)/src/m4a.c
M4A_C_SED   :=
ifneq ($(RATE),13379)
M4A_C_SRC   := $(BUILD)/m4a.c
endif
else
$(error MIXER must be vanilla or hq)
endif
ifneq ($(RATE),13379)
# m4aSoundInit's mode word only (the `| SOUND_MODE_FREQ_13379` line).
M4A_C_SED += -e 's/| SOUND_MODE_FREQ_13379$$/| SOUND_MODE_FREQ_$(RATE)/'
endif

CPPFLAGS := $(HQ_CPPFLAGS) -iquote $(POKEEMERALD)/include -Wno-trigraphs -DMODERN=0 \
            -I $(AGBCC)/include -I $(AGBCC) -nostdinc -undef -std=gnu89
CFLAGS   := -mthumb-interwork -Wimplicit -Wparentheses -Werror -O2 -fhex-asm -g
LIBS     := -L $(AGBCC)/lib -lgcc -lc -L $(POKEEMERALD)/libagbsyscall -lagbsyscall

MIDI_SRC_DIR := $(POKEEMERALD)/sound/songs/midi
MIDI_CFG     := $(MIDI_SRC_DIR)/midi.cfg
MICRO_DIR    := micro
MICRO_CFG    := $(MICRO_DIR)/micro.cfg

VANILLA_NAMES := $(shell sed -e 's/\.mid:.*//' -e 's/:.*//' $(MIDI_CFG))
MICRO_NAMES   := $(shell sed -e 's/\.mid:.*//' -e 's/:.*//' $(MICRO_CFG))
VANILLA_OBJS  := $(VANILLA_NAMES:%=$(BUILD)/songs/%.o)
MICRO_OBJS    := $(MICRO_NAMES:%=$(BUILD)/micro/%.o)

OBJS := $(BUILD)/crt0.o $(BUILD)/main.o $(BUILD)/m4a.o $(BUILD)/m4a_1.o \
        $(BUILD)/m4a_tables.o $(BUILD)/sound_data.o $(VANILLA_OBJS) $(MICRO_OBJS)

ROM := $(BUILD)/accuracy_test.gba
ELF := $(ROM:.gba=.elf)
MAP := $(ROM:.gba=.map)

.PHONY: all clean
all: $(ROM)

$(ROM): $(ELF)
	$(OBJCOPY) -O binary $< $@
	$(GBAFIX) $@ --silent

$(ELF): rom.ld $(OBJS)
	@echo "LD $@"
	@$(LD) -T rom.ld -Map $(MAP) -o $@ $(OBJS) $(LIBS)

# ---- C (agbcc) ----
define COMPILE_C
	@mkdir -p $(@D)
	$(CPP) $(CPPFLAGS) $< | $(1) $(CFLAGS) -o - - > $(@:.o=.s)
	@printf '\t.text\n\t.align\t2, 0\n' >> $(@:.o=.s)
	$(AS) $(ASFLAGS) -o $@ $(@:.o=.s)
endef

$(BUILD)/main.o: main.c
	$(call COMPILE_C,$(CC1))
$(BUILD)/m4a.c: $(POKEEMERALD)/src/m4a.c hq/m4a.c.sed Makefile
	@mkdir -p $(@D)
	sed $(M4A_C_SED) $< > $@
	@grep -q 'SOUND_MODE_FREQ_$(RATE)$$' $@ || { echo "rate line not patched in $@" >&2; exit 1; }
$(BUILD)/include/gba/m4a_internal.h: $(POKEEMERALD)/include/gba/m4a_internal.h Makefile
	@mkdir -p $(@D)
	sed -e 's/^#define PCM_DMA_BUF_SIZE 1584\b/#define PCM_DMA_BUF_SIZE $(HQ_PCM_DMA_BUF_SIZE)/' $< > $@
	@grep -q '^#define PCM_DMA_BUF_SIZE $(HQ_PCM_DMA_BUF_SIZE)\b' $@ || { echo "PCM_DMA_BUF_SIZE not patched in $@" >&2; exit 1; }
$(BUILD)/m4a_constants.inc: $(POKEEMERALD)/constants/m4a_constants.inc Makefile
	@mkdir -p $(@D)
	sed -e 's/^\(\s*\.equiv PCM_DMA_BUF_SIZE,\s*\)1584\b/\1$(HQ_PCM_DMA_BUF_SIZE)/' $< > $@
	@grep -q 'PCM_DMA_BUF_SIZE, $(HQ_PCM_DMA_BUF_SIZE)\b' $@ || { echo "PCM_DMA_BUF_SIZE not patched in $@" >&2; exit 1; }
$(BUILD)/m4a_1.s: hq/m4a_1.s $(BUILD)/m4a_constants.inc Makefile
	@mkdir -p $(@D)
	sed -e 's|^\(\s*\.include "\)constants/m4a_constants.inc"|\1$(ABS_BUILD)/m4a_constants.inc"|' $< > $@
	@grep -q '$(ABS_BUILD)/m4a_constants.inc' $@ || { echo "include not redirected in $@" >&2; exit 1; }
$(BUILD)/m4a.o: $(M4A_C_SRC) $(if $(HQ_CPPFLAGS),hq/include/gba/macro.h) $(M4A_HDR_DEP)
	$(call COMPILE_C,$(CC1_OLD))
$(BUILD)/m4a_tables.o: $(POKEEMERALD)/src/m4a_tables.c
	$(call COMPILE_C,$(CC1))

# ---- Assembly ----
# pokeemerald .s/.inc sources use `label::` and rely on the game's preproc tool,
# which also expands .include itself relative to the pokeemerald root, so these
# run through the same pipeline the game uses, from that directory.
define PREPROC_AS
	@mkdir -p $(@D)
	cd $(POKEEMERALD) && $(PREPROC) $(1) $(CHARMAP) | $(CPP) -I include - \
	  | $(PREPROC) -ie $(1) $(CHARMAP) | $(AS) $(ASFLAGS) -I include -o $(abspath $@)
endef

$(BUILD)/crt0.o: crt0.s
	@mkdir -p $(@D)
	$(AS) $(ASFLAGS) -o $@ $<
$(BUILD)/m4a_1.o: $(M4A_1_SRC)
	$(call PREPROC_AS,$(abspath $<))
$(BUILD)/sound_data.s: sound_data.s accuracy_micro.inc accuracy_micro_synth.inc $(BUILD)/micro_song_table.inc
	@mkdir -p $(@D)
	sed -e 's|@ROMDIR@|$(ABS_ROMDIR)|g' -e 's|@BUILD@|$(ABS_BUILD)|g' $< > $@
$(BUILD)/sound_data.o: $(BUILD)/sound_data.s
	$(call PREPROC_AS,$(abspath $<))

$(BUILD)/micro_song_table.inc: $(MICRO_CFG)
	@mkdir -p $(@D)
	sed -e 's/\.mid:.*//' -e 's/.*/\t.4byte &/' $< > $@

# ---- Songs: .mid -> .s (mid2agb, options from the .cfg) -> .o ----
# $1 = build subdir, $2 = source dir, $3 = cfg file, $4 = name, $5 = options
define MID_RULE
$(BUILD)/$(1)/$(4).s: $(2)/$(4).mid $(3)
	@mkdir -p $$(@D)
	$(MID) $$< $$@ $(5)
endef
define MID_EXPANSION
$(eval $(call MID_RULE,$(1),$(2),$(3),$(basename $(patsubst %:,%,$(word 1,$(4)))),$(wordlist 2,999,$(4))))
endef
$(foreach line,$(shell sed 's/[[:space:]]\+/__SPACE__/g' $(MIDI_CFG)),\
  $(call MID_EXPANSION,songs,$(MIDI_SRC_DIR),$(MIDI_CFG),$(subst __SPACE__, ,$(line))))
$(foreach line,$(shell sed 's/[[:space:]]\+/__SPACE__/g' $(MICRO_CFG)),\
  $(call MID_EXPANSION,micro,$(MICRO_DIR),$(MICRO_CFG),$(subst __SPACE__, ,$(line))))

$(BUILD)/songs/%.o: $(BUILD)/songs/%.s
	$(AS) $(ASFLAGS) -o $@ $<
$(BUILD)/micro/%.o: $(BUILD)/micro/%.s
	$(AS) $(ASFLAGS) -o $@ $<

clean:
	rm -rf $(BUILD)
