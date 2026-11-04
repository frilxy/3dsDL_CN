#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
TARGET          := 3dsDL
BUILD           := build
SOURCES         := source/app source/core source/net source/quirc source/stb \
                   source/qrcodegen
INCLUDES        := $(SOURCES)
ROMFS           := romfs

APP_VERSION     := $(strip $(shell cat $(TOPDIR)/VERSION))
APP_TITLE       := 3dsDL
APP_DESCRIPTION := Download files straight onto your 3DS
APP_AUTHOR      := ankushKun
ICON            := resources/icon-3dsdl.png

#---------------------------------------------------------------------------------
ARCH      := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS    := -g -Wall -Wextra -O2 -mword-relocations -ffunction-sections $(ARCH)
CFLAGS    += $(INCLUDE) -D__3DS__ -DAPP_VERSION=\"$(APP_VERSION)\" -Wno-psabi
# Test builds add defines here, e.g. make EXTRA_DEFINES=-DTHREEDSDL_FAULTS.
CFLAGS    += $(EXTRA_DEFINES)
CXXFLAGS  := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS   := -g $(ARCH)
LDFLAGS   := -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
# Every poll(), libcurl's included, goes through net::pollInSteps so no
# thread waits inside the network service (source/net/socket_wait.hpp), and
# every abort() writes crash.txt first (source/app/crash.cpp).
LDFLAGS   += -Wl,--wrap=poll -Wl,--wrap=abort

LIBS      := -lcitro2d -lcitro3d -lcurl -lmbedtls -lmbedx509 \
             -lmbedcrypto -lturbojpeg -larchive -lzstd -llz4 -llzma -lbz2 \
             -lz -lctru -lm
LIBDIRS   := $(CTRULIB) $(PORTLIBS)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

ifeq ($(strip $(CPPFILES)),)
export LD := $(CC)
else
export LD := $(CXX)
endif

export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export _3DSXDEPS := $(OUTPUT).smdh
export APP_ICON := $(TOPDIR)/$(ICON)
export _3DSXFLAGS += --smdh=$(OUTPUT).smdh --romfs=$(CURDIR)/$(ROMFS)

.PHONY: all clean version

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf $(TARGET).cia

version:
	@echo $(APP_VERSION)

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

$(OUTPUT).3dsx: $(OUTPUT).elf $(OUTPUT).smdh
$(OUTPUT).elf: $(OFILES)

# quirc is vendored unmodified; keep its upstream warnings out of the build log.
QUIRC_OFILES := $(patsubst %.c,%.o,$(notdir $(wildcard $(TOPDIR)/source/quirc/*.c)))
$(QUIRC_OFILES): CFLAGS += -Wno-sign-compare -Wno-unused-parameter -Wno-type-limits

# qrcodegen is vendored unmodified, so its warnings are not ours to fix.
qrcodegen.o: CFLAGS += -Wno-type-limits

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
