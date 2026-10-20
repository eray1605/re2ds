#---------------------------------------------------------------------------------
# re2ds - Resident Evil 2 (PS1) for the Nintendo DS (devkitARM + libnds)
# Based on the devkitPro arm9 template. Game data comes from nitrofiles/,
# which tools/build_assets.sh fills from your own PS1 disc.
#---------------------------------------------------------------------------------
.SUFFIXES:

# devkitPro's default place when DEVKITARM is not set
ifeq ($(strip $(DEVKITARM)),)
ifneq ($(wildcard /opt/devkitpro/devkitARM/ds_rules),)
export DEVKITPRO := /opt/devkitpro
export DEVKITARM := /opt/devkitpro/devkitARM
endif
endif

ifeq ($(strip $(DEVKITARM)),)
#---------------------------------------------------------------------------------
# No devkitPro here: build in devkitPro's Docker image (tools/setup_wsl.sh
# installs it when devkitpro.org is blocked). sudo only if docker needs it.
#---------------------------------------------------------------------------------
ifeq ($(shell command -v docker 2>/dev/null),)
$(error "devkitPro not found: run tools/setup_wsl.sh, or set DEVKITARM=<path to>devkitARM")
endif
DOCKER := $(shell docker info >/dev/null 2>&1 && echo docker || echo sudo docker)

.PHONY: docker-make clean
.DEFAULT_GOAL := docker-make
docker-make:
	$(DOCKER) run --rm -v "$(CURDIR)":/re2ds -w /re2ds -u "$$(id -u):$$(id -g)" devkitpro/devkitarm make $(MAKECMDGOALS)
clean: docker-make
else
include $(DEVKITARM)/ds_rules

TARGET     := re2ds
BUILD      := build_nds
SOURCES    := source
INCLUDES   := source
DATA       :=
NITRO      := nitrofiles
GAME_TITLE := RE2 DS Prototype
GAME_SUBTITLE1 := Disc 1 (Leon)
GAME_SUBTITLE2 := re2ds
# the ROM's icon, made from the disc by tools/icon2ds.py (build_assets.sh); without it
# the default libnds icon is used
_ICON := $(dir $(abspath $(firstword $(MAKEFILE_LIST))))build/icon.bmp
ifneq ($(wildcard $(_ICON)),)
GAME_ICON := $(_ICON)
endif

ARCH     := -marm -mthumb-interwork -march=armv5te -mtune=arm946e-s
CFLAGS   := -g -Wall -O2 -ffunction-sections -fdata-sections $(ARCH) $(INCLUDE) -DARM9
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=ds_arm9.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) -Wl,--gc-sections

LIBS    := -lfilesystem -lfat -lnds9 -lm
LIBDIRS := $(LIBNDS) $(PORTLIBS)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT  := $(CURDIR)/$(TARGET)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

ifneq ($(strip $(NITRO)),)
export NITRO_FILES := $(CURDIR)/$(NITRO)
endif

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export LD := $(CC)
export OFILES_BIN     := $(addsuffix .o,$(BINFILES))
export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES         := $(OFILES_BIN) $(OFILES_SOURCES)
export HFILES         := $(addsuffix .h,$(subst .,_,$(BINFILES)))
export INCLUDE := $(foreach dir,$(INCLUDES),-iquote $(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean

$(BUILD):
	@mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).nds

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

# every file and folder in nitrofiles/, so the ROM is rebuilt when assets are
# reconverted (the folder itself only changes when entries are added/removed)
NITRO_DEPS := $(if $(NITRO_FILES),$(shell find $(NITRO_FILES)))

$(OUTPUT).nds: $(OUTPUT).elf $(NITRO_DEPS)
$(OUTPUT).elf: $(OFILES)
$(OFILES_SOURCES) : $(HFILES)

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
endif # devkitPro found

