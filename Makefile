ENABLE_ASAN ?= 0

CXX = g++

CXXFLAGS = -std=c++20 -Wall -O2 -g
CXXFLAGS += -fno-strict-aliasing # fix libev warnings
CXXFLAGS += -I .
CXXFLAGS += -I third-party/libev/build/include

LDFLAGS = -lpthread
LDFLAGS += -L third-party/libev/build/lib
LDFLAGS += -Wl,-Bstatic -lev -Wl,-Bdynamic

ifeq ($(ENABLE_ASAN), 1)
    CXXFLAGS += -fsanitize=address -fno-omit-frame-pointer
    LDFLAGS += -fsanitize=address
endif

SRC_DIR := examples
BUILD_DIR := build

SRCS = $(wildcard $(SRC_DIR)/*.cc)
BINS := $(patsubst $(SRC_DIR)/%.cc,$(BUILD_DIR)/%,$(SRCS))

all: prepare $(BINS)

prepare:
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%: $(SRC_DIR)/%.cc
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean prepare
