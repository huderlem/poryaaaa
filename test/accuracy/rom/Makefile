# poryaaaa accuracy test ROM.
#
# Builds a standalone GBA ROM from the pokeemerald tree's m4a engine
# (m4a.c, m4a_1.s, m4a_tables.c), its full sound data (voicegroups, samples,
# song table, every vanilla song compiled with mid2agb exactly as the game
# does) plus the micro-test voicegroup and songs under micro/.
#
#   make POKEEMERALD=~/pokeemerald [BUILD=build] [MIXER=vanilla|hq]
#
# MIXER=hq builds the same ROM with ipatix's HQ mixer installed the way the
# pret wiki describes (docs/HQ-MIXER-ACCURACY.md, section 4): hq/m4a_1.s in
# place of the tree's src/m4a_1.s, the two-line m4a.c change (hq/m4a.c.sed)
# and the atomic DmaSetUnchecked (hq/include/gba/macro.h first on the
# include path).  Everything else, AgbMain included, is the vanilla build.
#
# Requires: agbcc installed in <pokeemerald>/tools/agbcc, the pokeemerald tools
# built (mid2agb, gbafix) and libagbsyscall built, arm-none-eabi binutils.

SHELL := /bin/bash
.SHELLFLAGS := -o pipefail -ec

POKEEMERALD ?= $(HOME)/pokeemerald
BUILD ?= build
MIXER ?= vanilla

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
else ifeq ($(MIXER),vanilla)
HQ_CPPFLAGS :=
M4A_1_SRC   := $(POKEEMERALD)/src/m4a_1.s
M4A_C_SRC   := $(POKEEMERALD)/src/m4a.c
else
$(error MIXER must be vanilla or hq)
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
$(BUILD)/m4a.c: $(POKEEMERALD)/src/m4a.c hq/m4a.c.sed
	@mkdir -p $(@D)
	sed -f hq/m4a.c.sed $< > $@
$(BUILD)/m4a.o: $(M4A_C_SRC) $(if $(HQ_CPPFLAGS),hq/include/gba/macro.h)
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
	$(call PREPROC_AS,$<)
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
