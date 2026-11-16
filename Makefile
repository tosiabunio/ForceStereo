BUILD   := build
DRIVER  := $(BUILD)/ForceStereo.driver
HELPER  := $(BUILD)/forcestereo
ARCHS   := -arch arm64 -arch x86_64
CFLAGS  := -O2 -Wall -Wextra -mmacosx-version-min=12.0 $(ARCHS)

all: $(DRIVER) $(HELPER)

$(DRIVER): driver/ForceStereoDriver.c driver/Info.plist shared/ForceStereoShared.h
	rm -rf $@
	mkdir -p $@/Contents/MacOS
	cp driver/Info.plist $@/Contents/Info.plist
	clang $(CFLAGS) -bundle -fvisibility=hidden -o $@/Contents/MacOS/ForceStereo driver/ForceStereoDriver.c \
		-framework CoreAudio -framework CoreFoundation
	codesign --force --sign - $@

$(HELPER): helper/forcestereo.c shared/ForceStereoShared.h
	mkdir -p $(BUILD)
	clang $(CFLAGS) -o $@ helper/forcestereo.c \
		-framework CoreAudio -framework AudioToolbox -framework CoreFoundation
	codesign --force --sign - $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
