
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
APP_ICON        := $(CTRULIB)/default_icon.png

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

$(TARGET).smdh:
	bannertool makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" -i "$(APP_ICON)" -o $@

$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh
	3dsxtool $(TARGET).elf $@ --smdh=$(TARGET).smdh --romfs=romfs

clean:
	rm -rf build $(TARGET).elf $(TARGET).3dsx $(TARGET).smdh

.PHONY: all clean
