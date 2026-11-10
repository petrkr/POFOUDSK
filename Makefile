.PHONY: all ch375 sys dirfix upload upload-com upload-sys upload-dirfix clean

BUILD_DIR := build

# Shared Portfolio library (tinylib etc.). POFOUDSK.COM links
# tinylib.c, which #includes tinynum.c; POFOUDSK.SYS links only
# tinyhex.c (no libc, no varargs) - never both in one program.
POFOLIB := lib/pofo

# CH375 chip-level library (lib/pofoch375/README.md) - driver-safe,
# no src/ or DOS dependency, reusable as-is by a future ROM build.
POFOCH375 := lib/pofoch375

POFOUDSK_COM_SRC := src/ch375cli.c src/disk.c src/part.c src/partpick.c $(POFOCH375)/ch375det.c $(POFOCH375)/ch375disk.c $(POFOCH375)/ch375core.c $(POFOCH375)/port.c $(POFOLIB)/tinylib.c
POFOUDSK_COM_HDR := src/disk.h src/part.h src/pofoudsk.h $(POFOCH375)/ch375disk.h $(POFOCH375)/ch375.h $(POFOCH375)/port.h $(POFOLIB)/tinylib.h $(POFOLIB)/tinynum.h $(POFOLIB)/tinynum.c $(POFOLIB)/tinyhex.c $(POFOLIB)/tinydec.c
POFOUDSK_COM_BIN := $(BUILD_DIR)/POFOUDSK.COM

POFOUDSK_SYS_BIN := $(BUILD_DIR)/POFOUDSK.SYS

BCC ?= bcc
AS86 ?= as86
LD86 ?= ld86

# Bridge (or ESP smart-cable client) HTTP endpoint. Override HOST for a
# real ESP on the network, e.g.: make upload HOST=10.220.179.55
HOST ?= 127.0.0.1
PORT ?= 9000
# Empty by default - the bridge's own C:\ default then applies. A
# trailing backslash in a Makefile variable is line continuation, not
# a literal character, so don't set one here; override as e.g.
# make upload DEST_DIR='D:\SOMEDIR\'.
DEST_DIR ?=

NASM ?= nasm
DIRFIX_BIN := $(BUILD_DIR)/DIRFIX.COM

all: ch375 sys dirfix

ch375: $(POFOUDSK_COM_BIN)

sys: $(POFOUDSK_SYS_BIN)

dirfix: $(DIRFIX_BIN)

# Standalone MKDIR/directory-extension fix (see src/dirfix.inc,
# src/dirfix.asm) - also built into POFOUDSK.SYS, see sysdirfix below.
$(DIRFIX_BIN): src/dirfix.asm src/dirfix.inc | $(BUILD_DIR)
	$(NASM) -f bin -I src/ -o $@ src/dirfix.asm

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(POFOUDSK_COM_BIN): $(POFOUDSK_COM_SRC) $(POFOUDSK_COM_HDR) | $(BUILD_DIR)
	$(BCC) -Md -I$(POFOLIB) -I$(POFOCH375) -Isrc -o $@ $(POFOUDSK_COM_SRC)

# POFOUDSK.SYS - device header must land at file offset 0, so this uses
# the as86 -u -o (object) + ld86 -0 -d (tiny/impure link) pipeline
# instead of as86 -b: as86 -b always places .data before .text regardless of source order,
# which would push the hand-written header away from offset 0; ld86
# -d keeps .text/.data/.bss in source order instead. Links only the
# driver-safe units (no libc, no unsigned-long runtime helpers) - never
# disk.c/ch375det.c/partpick.c, which are CLI only. See src/sys.h.
SYS_UNITS := ch375sys sysfar sysmount sysvdisk ch375core ch375disk part port
SYS_OBJS := $(patsubst %,$(BUILD_DIR)/%.o,$(SYS_UNITS)) $(BUILD_DIR)/tinyhex.o $(BUILD_DIR)/sysdirfix.o
SYS_HDR := $(POFOCH375)/ch375.h $(POFOCH375)/ch375disk.h $(POFOCH375)/port.h src/part.h src/pofoudsk.h $(POFOLIB)/tinynum.h src/sys.h src/pofoudsk_version.h

$(BUILD_DIR)/%.o: src/%.c $(SYS_HDR) | $(BUILD_DIR)
	$(BCC) -0 -Md -I$(POFOLIB) -I$(POFOCH375) -S -o $(BUILD_DIR)/$*_bcc.s $<
	$(AS86) -0 -u -o $@ $(BUILD_DIR)/$*_bcc.s

$(BUILD_DIR)/%.o: $(POFOLIB)/%.c $(SYS_HDR) | $(BUILD_DIR)
	$(BCC) -0 -Md -I$(POFOLIB) -I$(POFOCH375) -S -o $(BUILD_DIR)/$*_bcc.s $<
	$(AS86) -0 -u -o $@ $(BUILD_DIR)/$*_bcc.s

$(BUILD_DIR)/%.o: $(POFOCH375)/%.c $(SYS_HDR) | $(BUILD_DIR)
	$(BCC) -0 -Md -I$(POFOLIB) -I$(POFOCH375) -S -o $(BUILD_DIR)/$*_bcc.s $<
	$(AS86) -0 -u -o $@ $(BUILD_DIR)/$*_bcc.s

# DIRFIX glue (see src/dirfix.inc, src/sysdirfix.asm) - nasm assembles
# directly to an as86-compatible object, no bcc step.
$(BUILD_DIR)/sysdirfix.o: src/sysdirfix.asm src/dirfix.inc | $(BUILD_DIR)
	$(NASM) -f as86 -I src/ -o $@ $<

# ch375sys.o must stay first - its #asm device header has to land at
# file offset 0.
$(POFOUDSK_SYS_BIN): $(SYS_OBJS)
	$(LD86) -0 -d -o $@ $(SYS_OBJS)

# Assumes the bridge is already running at HOST:PORT and the Portfolio
# is sitting in File Transfer Server mode. Direct curl multipart POST
# to the bridge's /upload endpoint. DEST_DIR_ENC substitutes %5C for \
# (DOS path separator) since it's going into a URL query string, not
# shell-quoted; already-encoded overrides (e.g. SYS_DEST_DIR_ENC) must
# use %5C directly for the same reason.
DEST_DIR_ENC = $(subst \,%5C,$(DEST_DIR))

UPLOAD_BASE = http://$(HOST):$(PORT)/uploadFile?toAtari=1&overwrite=1
UPLOAD_URL = $(UPLOAD_BASE)$(if $(DEST_DIR),&destDir=$(DEST_DIR_ENC))

# POFOUDSK.SYS goes to the A: memory card by default (C: is too small
# for both files) - CONFIG.SYS then needs DEVICE=A:\POFOUDSK.SYS.
SYS_DEST_DIR_ENC ?= A:%5C

# upload = both files (POFOUDSK.SYS/.COM must match - shared IOCTL
# reply layout), upload-com / upload-sys = just one.
upload: upload-sys upload-com

upload-com: $(POFOUDSK_COM_BIN)
	curl -sf -F "file=@$(POFOUDSK_COM_BIN)" "$(UPLOAD_URL)"

upload-dirfix: $(DIRFIX_BIN)
	curl -sf -F "file=@$(DIRFIX_BIN)" "$(UPLOAD_URL)"

upload-sys: $(POFOUDSK_SYS_BIN)
	curl -sf -F "file=@$(POFOUDSK_SYS_BIN)" "$(UPLOAD_BASE)&destDir=$(SYS_DEST_DIR_ENC)"

clean:
	rm -rf $(BUILD_DIR)

