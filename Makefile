CC      := gcc
AS      := gcc          # assembling the .S through gcc, not `as` directly,
                         # so it goes through the same include/preprocessor
                         # path as everything else without a second toolchain
                         # entry to keep in sync.

WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wcast-qual \
    -Wwrite-strings -Wformat=2 -Wundef -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition \
    -Wimplicit-fallthrough -Wlogical-op -Wcast-align -Wvla -Wnull-dereference -Wdouble-promotion \
	-Wformat-overflow=2 -Wformat-truncation=2 -Walloc-zero -Warray-bounds=2 -Wstringop-overflow=4 \
	-Wstrict-overflow=5 -Wswitch-enum -Wpointer-arith -Winit-self -Wbad-function-cast

CPPFLAGS := -Iinclude -Isrc
CFLAGS   := -std=c11 $(WARNINGS)

AR       := ar
ARFLAGS  := rcs

# ------------------------------------------------------------------------------

DEBUG_CFLAGS := -O1 -g3 -fsanitize=address,undefined -fsanitize-address-use-after-scope -fno-omit-frame-pointer

RELEASE_CFLAGS := -O3 -DNDEBUG -flto

# Debug is the default.
BUILD ?= debug

ifeq ($(BUILD),release)
    BUILD_CFLAGS := $(RELEASE_CFLAGS)
else
    BUILD_CFLAGS := $(DEBUG_CFLAGS)
endif

# ------------------------------------------------------------------------------

BUILD_DIR := build
LIB       := $(BUILD_DIR)/libcr.a

SRCS_C := $(shell find src -name '*.c')
SRCS_S := $(shell find src -name '*.S')

OBJS := $(SRCS_C:%.c=$(BUILD_DIR)/%.o) \
        $(SRCS_S:%.S=$(BUILD_DIR)/%.o)

DEPFLAGS := -MMD -MP
DEPS := $(OBJS:.o=.d)

.PHONY: all clean debug release

all: debug

debug:
	$(MAKE) BUILD=debug $(LIB)

release:
	$(MAKE) BUILD=release $(LIB)

$(LIB): $(OBJS)
	@mkdir -p $(dir $@)
	$(AR) $(ARFLAGS) $@ $^

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BUILD_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(AS) $(CPPFLAGS) $(BUILD_CFLAGS) $(DEPFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
