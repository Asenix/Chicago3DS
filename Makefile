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

# Sound effects and the title music live in romfs/ (unpacked from assets.zip by the build workflow).
# When the folder exists it is packed INSIDE the .3dsx, so the game is one self-contained file.
ROMFS_FILES := $(wildcard romfs/*)

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

$(TARGET).3dsx: $(TARGET).elf $(ROMFS_FILES)
	3dsxtool $< $@ $(if $(wildcard romfs),--romfs=romfs)
	@ls -l $@

clean:
	rm -rf build $(TARGET).elf $(TARGET).3dsx

.PHONY: all clean
