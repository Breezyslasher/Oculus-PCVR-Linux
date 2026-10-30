# Cross-compiles the Windows-side pieces with mingw-w64.
#   make            build everything into build/
#   make CROSS=...  use a different toolchain prefix

CROSS   ?= x86_64-w64-mingw32-
CC      := $(CROSS)gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Werror -D_WIN32_WINNT=0x0601 -DUNICODE -D_UNICODE
LDFLAGS += -static-libgcc -Wl,--enable-stdcall-fixup

BUILD   := build
COMMON  := src/common/log.c

PLATFORM_DLL := $(BUILD)/LibOVRPlatform64_1.dll
LIBOVR_DLL   := $(BUILD)/LibOVRRT64_1.dll
INJECTOR     := $(BUILD)/ovrinject.exe

all: $(PLATFORM_DLL) $(LIBOVR_DLL) $(INJECTOR)

TESTS := $(BUILD)/test_platform.exe $(BUILD)/test_libovr_loader.exe

$(BUILD):
	mkdir -p $@

$(PLATFORM_DLL): src/platform/ovr_platform_stub.c src/platform/platform_types.h $(COMMON) | $(BUILD)
	$(CC) $(CFLAGS) -shared -o $@ src/platform/ovr_platform_stub.c $(COMMON) $(LDFLAGS)

LIBOVR_SRC := src/libovr/libovr_stub.c src/libovr/iat_hook.c src/libovr/sigcheck.c \
              src/libovr/vr_backend.c src/libovr/swapchain_d3d.c
LIBOVR_INC := -Ithird_party/openvr

$(LIBOVR_DLL): $(LIBOVR_SRC) src/libovr/*.h $(COMMON) | $(BUILD)
	$(CC) $(CFLAGS) $(LIBOVR_INC) -shared -o $@ $(LIBOVR_SRC) $(COMMON) $(LDFLAGS) -lwintrust

$(INJECTOR): src/injector/ovrinject.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lshell32

$(BUILD)/test_%.exe: tests/test_%.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Smoke tests; need Wine (WINE=... to pick a binary).
WINE ?= wine
check: all $(TESTS)
	cd $(BUILD) && OVRSTUB_USER_NAME=TestUser WINEDEBUG=-all $(WINE) test_platform.exe
	cd $(BUILD) && WINEDEBUG=-all $(WINE) ovrinject.exe --wait LibOVRRT64_1.dll -- test_libovr_loader.exe

# List each DLL's exports, to compare against a real runtime's (see tools/).
exports: all
	PYTHONPATH=launcher python3 -m ovrl.pe $(PLATFORM_DLL) $(LIBOVR_DLL)

clean:
	rm -rf $(BUILD)

.PHONY: all check clean exports
