LLVM_BIN ?= $(if $(wildcard $(HOME)/llvm/build/bin/llvm-mc),$(HOME)/llvm/build/bin,$(HOME)/llvm/bin)
BUILD_DIR ?= build

LLVM_MC := $(LLVM_BIN)/llvm-mc
LLVM_OBJDUMP := $(LLVM_BIN)/llvm-objdump
LLVM_READELF := $(LLVM_BIN)/llvm-readelf
SOURCE := src/vfncvtmxfp.s
OBJECT := $(BUILD_DIR)/vfncvtmxfp.o
DUMP := $(BUILD_DIR)/vfncvtmxfp.dump
ATTR := $(BUILD_DIR)/vfncvtmxfp.attr

.PHONY: all check check-tools runtime clean

all: check

check: check-tools $(OBJECT) $(DUMP) $(ATTR)
	@grep -Eq '09757057[[:space:]]+vsetvli[[:space:]]+zero, a0, e32, mf2, tu, ma' $(DUMP)
	@grep -Eq '3aa4a45b[[:space:]]+vfncvtmxfp8\.f\.f\.w[[:space:]]+v8, v9, v10' $(DUMP)
	@grep -Eq '3aa4945b[[:space:]]+vfncvtmxfp4\.f\.f\.w[[:space:]]+v8, v9, v10' $(DUMP)
	@grep -Eq '38a4a45b[[:space:]]+vfncvtmxfp8\.f\.f\.w[[:space:]]+v8, v9, v10, v0\.t' $(DUMP)
	@grep -Eq 'xsaivfncvtmxfp0p1' $(ATTR)
	@echo "vfncvtmxfp assembler tests passed"

check-tools:
	@test -x "$(LLVM_MC)" || { echo "missing $(LLVM_MC); set LLVM_BIN to an LLVM build with xsaivfncvtmxfp support" >&2; exit 1; }
	@test -x "$(LLVM_OBJDUMP)" || { echo "missing $(LLVM_OBJDUMP)" >&2; exit 1; }
	@test -x "$(LLVM_READELF)" || { echo "missing $(LLVM_READELF)" >&2; exit 1; }

runtime:
	@test -n "$(AM_HOME)" || { echo "set AM_HOME before building the runtime test" >&2; exit 1; }
	@$(MAKE) -C runtime ARCH=$(if $(ARCH),$(ARCH),riscv64-xs) AM_HOME=$(AM_HOME)

$(BUILD_DIR):
	@mkdir -p $@

$(OBJECT): $(SOURCE) | $(BUILD_DIR)
	@$(LLVM_MC) -triple=riscv64 -mattr=+v,+xsaivfncvtmxfp -filetype=obj $< -o $@

$(DUMP): $(OBJECT)
	@$(LLVM_OBJDUMP) -d --mattr=+v,+xsaivfncvtmxfp $< > $@

$(ATTR): $(OBJECT)
	@$(LLVM_READELF) -A $< > $@

clean:
	@rm -rf $(BUILD_DIR)
	@if test -n "$(AM_HOME)"; then $(MAKE) -s -C runtime clean AM_HOME=$(AM_HOME); fi
