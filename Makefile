
ifeq ($(strip $(DEVKITARM)),)
$(error DEVKITARM is not set)
endif
ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set)
endif

export PATH := $(DEVKITARM)/bin:$(DEVKITPRO)/tools/bin:$(PATH)

TARGET  := chicago3ds
CTRULIB := $(DEVKITPRO)/libctru
ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS  := -g -Wall -O2 -mword-relocations -ffunction-sections $(ARCH) \
           -D__3DS__ -I$(CTRULIB)/include -Ibuild
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -L$(CTRULIB)/lib
LIBS    := -lcitro2d -lcitro3d -lctru -lm

APP_TITLE       := Chicago3DS
APP_DESCRIPTION := Open city game
APP_AUTHOR      := Asenix
# a 48x48 PNG; assets/icon.png is ours, the devkitPro default is the fallback if it is ever missing
APP_ICON        := $(firstword $(wildcard assets/icon.png) $(CTRULIB)/default_icon.png)

all: $(TARGET).3dsx

build:
	mkdir -p build

build/vshader.shbin: source/vshader.v.pica | build
	picasso -o $@ $<

build/vshader.o: build/vshader.shbin
	bin2s -a 4 -H build/vshader_shbin.h $< | arm-none-eabi-as -o $@

build/main.o: source/main.c build/vshader.o
	arm-none-eabi-gcc $(CFLAGS) -c $< -o $@

$(TARGET).elf: build/main.o build/vshader.o
	arm-none-eabi-gcc $(LDFLAGS) $^ $(LIBS) -o $@

$(TARGET).smdh: $(APP_ICON)
	smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" "$(APP_ICON)" $@

# 3dsxtool only embeds a RomFS together with an SMDH: "3dsxtool in.elf out.3dsx --romfs=romfs" on its own stops with
# "Cannot open SMDH file!", so the SMDH is always passed. If packaging the RomFS fails for any reason, the game is still
# packaged without it (a warning is printed): the title music is then read from sdmc:/3ds/Chicago3DS/title.pcm, or the
# built-in theme plays. romfs/ holds title.pcm, converted from assets/title.mp3 by tools/convert_audio.sh.
$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh
	@rm -f $@
	3dsxtool $(TARGET).elf $@ --smdh=$(TARGET).smdh --romfs=romfs || { \
	  echo "WARNING: could not embed romfs/ - packaging WITHOUT it (title music will use sdmc:/3ds/Chicago3DS/title.pcm or the built-in theme)"; \
	  rm -f $@; 3dsxtool $(TARGET).elf $@ --smdh=$(TARGET).smdh; }
	@ls -l $@

clean:
	rm -rf build $(TARGET).elf $(TARGET).3dsx $(TARGET).smdh

.PHONY: all clean
