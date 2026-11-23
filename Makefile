ifeq ($(strip $(DEVKITARM)),)
$(error Set DEVKITARM, e.g. export DEVKITARM=/opt/devkitpro/devkitARM)
endif

ifeq ($(strip $(DEVKITPRO)),)
$(error Set DEVKITPRO, e.g. export DEVKITPRO=/opt/devkitpro)
endif

export PATH := $(DEVKITARM)/bin:$(DEVKITPRO)/tools/bin:$(PATH)

TARGET  := chicago3ds
CTRULIB := $(DEVKITPRO)/libctru

ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS  := -g -Wall -O2 -mword-relocations -ffunction-sections $(ARCH) \
           -D__3DS__ -I$(CTRULIB)/include -Ibuild

LDFLAGS := -specs=3dsx.specs -g $(ARCH) -L$(CTRULIB)/lib

LIBS    := -lcitro2d -lcitro3d -lctru -lm

# Audio and game assets
ROMFS_FILES := $(wildcard romfs/*)

# Application metadata
APP_TITLE       := Chicago3DS
APP_DESCRIPTION := Low-poly open-city walking demo
APP_AUTHOR      := Asenix
ICON            := assets/icon.png

all: $(TARGET).3dsx

build:
	mkdir -p build

# Generate the Nintendo 3DS application metadata
$(TARGET).smdh: $(ICON)
	smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(ICON) $@

# Vertex shader
build/vshader.shbin: source/vshader.v.pica | build
	picasso -o $@ $<

build/vshader.o: build/vshader.shbin
	bin2s -a 4 -H build/vshader_shbin.h $< | arm-none-eabi-as -o $@

# Main source
build/main.o: source/main.c build/vshader.o
	arm-none-eabi-gcc $(CFLAGS) -c $< -o $@

# Link executable
$(TARGET).elf: build/main.o build/vshader.o
	arm-none-eabi-gcc $(LDFLAGS) $^ $(LIBS) -o $@

# Package the final 3DSX with metadata and ROMFS
$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh $(ROMFS_FILES)
	3dsxtool $< $@ --smdh=$(TARGET).smdh $(if $(wildcard romfs),--romfs=romfs)
	@ls -l $@

clean:
	rm -rf build $(TARGET).elf $(TARGET).3dsx $(TARGET).smdh

.PHONY: all clean
