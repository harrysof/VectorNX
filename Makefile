#---------------------------------------------------------------------------------
# VECTOR (Nekki) -- Nintendo Switch homebrew loader
#
# Unity 2022.3.62f2 (7670c08855a9) / IL2CPP / arm64-v8a.
# Forked from the MIT-licensed SoLoader lineage via badpiggies_nx
# (colorsheep_nx / laytonbmr_nx / vln_nx).
#
# Requires devkitA64 + devkitPro packages:
#   pacman -S switch-dev
#   pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-zlib switch-libpng
#
# All .c files in source/ are compiled automatically. This project ships NO
# game code and NO game assets; the user supplies their own APK files.
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "Set DEVKITPRO in your environment. (export DEVKITPRO=/opt/devkitpro)")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := vector_nx
APP_TITLE   := Vector
APP_AUTHOR  := VectorNX
APP_VERSION := 1.5.0
APP_ICON    := $(TOPDIR)/icon.png
export APP_TITLE APP_AUTHOR APP_VERSION APP_ICON
BUILD    := build
SOURCES  := source
INCLUDES := source

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS  := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES) \
           -DCRASH_LOG_PRINTF=debugPrintf \
           $(INCLUDE) -D__SWITCH__
# Reserved region where the game .so files are loaded (keep in sync with so_util.c).
CFLAGS  += -DLOAD_ADDRESS=0xC0000000
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# mesa GLES3 + EGL + nouveau (force the GLES path); SDL2 for window/HID/audio; zlib.
LIBS := -lSDL2 -lGLESv2 -lEGL -lglapi -ldrm_nouveau -lpng -lz -lnx -lm

LIBDIRS := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD := $(CXX)
export OFILES  := $(SFILES:.s=.o) $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(PORTLIBS)/include/SDL2 -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all clean
all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
$(BUILD):
	@mkdir -p $@
clean:
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
DEPENDS := $(OFILES:.o=.d)
NROFLAGS := --icon=$(APP_ICON) --nacp=$(OUTPUT).nacp
NRO_DEPS := $(OUTPUT).elf $(OUTPUT).nacp
ifneq ($(wildcard $(APP_ICON)),)
NRO_DEPS += $(APP_ICON)
endif
all : $(OUTPUT).nro
$(OUTPUT).nro : $(NRO_DEPS)
$(OUTPUT).elf : $(OFILES)
-include $(DEPENDS)
endif
