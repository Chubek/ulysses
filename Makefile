MULIB ?= third_party/mulib
ON1XC ?= $(MULIB)/build/on1x/on1xc
PYTHON ?= python3
CC ?= cc
BUILD := build
DRIVER := $(MULIB)/on1x/tools/on1x-build.py
STDLIB := $(MULIB)/on1x/stdlib
SOURCES := $(wildcard src/*/*.on1x) src/foundation.on1x
PLATFORM := platform/linux.c $(wildcard $(MULIB)/include/*.h)
ON1X = $(PYTHON) "$(DRIVER)" --on1xc "$(ON1XC)" --stdlib "$(STDLIB)" --cc "$(CC)"
HOST = --extra-c platform/linux.c --cflag=-I"$(abspath $(MULIB))/include"

.PHONY: all test check-tools
all: $(addprefix $(BUILD)/,dublind telemachaid-worker bloomc bloomrun ulysses-check)

check-tools:
	@test -f "$(DRIVER)" || { echo "Initialize third_party/mulib or set MULIB to a MuLIB checkout." >&2; exit 1; }
	@test -x "$(ON1XC)" || { echo "Build On1x or pass ON1XC=/path/to/on1xc." >&2; exit 1; }

$(BUILD):
	mkdir -p "$@"

$(BUILD)/dublind: src/dublin/dublind.on1x $(SOURCES) $(PLATFORM) | check-tools $(BUILD)
	$(ON1X) $(HOST) "$<" -o "$@"

$(BUILD)/telemachaid-worker: src/telemachaid/worker.on1x $(SOURCES) $(PLATFORM) | check-tools $(BUILD)
	$(ON1X) $(HOST) "$<" -o "$@"

$(BUILD)/bloomc: src/bloomscript/bloomc.on1x $(SOURCES) | check-tools $(BUILD)
	$(ON1X) "$<" -o "$@"

$(BUILD)/bloomrun: src/bloomscript/bloomrun.on1x $(SOURCES) | check-tools $(BUILD)
	$(ON1X) "$<" -o "$@"

$(BUILD)/ulysses-check: src/foundation.on1x $(SOURCES) | check-tools $(BUILD)
	$(ON1X) "$<" -o "$@"

test: all
	"$(BUILD)/ulysses-check"
	$(PYTHON) tests/integration.py "$(BUILD)"
