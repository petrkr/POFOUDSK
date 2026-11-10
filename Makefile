.PHONY: all ch375 sys dirfix upload upload-com upload-sys upload-dirfix clean

BUILD_DIR := build

# Portfolio library. The COM links tinylib.c (includes tinynum.c), the
# SYS only tinyhex.c - never both.
POFOLIB := lib/pofo

# CH375 chip library (driver-safe, no dependency on src/).
POFOCH375 := lib/pofoch375

# Bus transport unit (today: PID card). A new transport (e.g. tr_com,
# tr_lpt) is a one-variable change.
TRANSPORT := tr_pid

POFOUDSK_COM_SRC := src/ch375cli.c src/disk.c src/part.c src/partpick.c $(POFOCH375)/ch375det.c $(POFOCH375)/ch375disk.c $(POFOCH375)/ch375core.c $(POFOCH375)/$(TRANSPORT).c $(POFOLIB)/tinylib.c
POFOUDSK_COM_HDR := src/disk.h src/part.h src/pofoudsk.h $(POFOCH375)/ch375disk.h $(POFOCH375)/ch375.h $(POFOCH375)/transport.h $(POFOLIB)/tinylib.h $(POFOLIB)/tinynum.h $(POFOLIB)/tinynum.c $(POFOLIB)/tinyhex.c $(POFOLIB)/tinydec.c
POFOUDSK_COM_BIN := $(BUILD_DIR)/POFOUDSK.COM

POFOUDSK_SYS_BIN := $(BUILD_DIR)/POFOUDSK.SYS

BCC ?= bcc
AS86 ?= as86
LD86 ?= ld86

# File transfer HTTP endpoint, e.g. make upload HOST=10.220.179.55
HOST ?= 127.0.0.1
PORT ?= 9000
# Empty = C:\. Set on the command line, e.g. DEST_DIR='D:\DIR\' (a
# trailing backslash in the Makefile would be a line continuation).
DEST_DIR ?=

NASM ?= nasm
DIRFIX_BIN := $(BUILD_DIR)/DIRFIX.COM

all: ch375 sys dirfix

ch375: $(POFOUDSK_COM_BIN)

sys: $(POFOUDSK_SYS_BIN)

dirfix: $(DIRFIX_BIN)

# Standalone DIRFIX (also built into POFOUDSK.SYS).
$(DIRFIX_BIN): src/dirfix.asm src/dirfix.inc | $(BUILD_DIR)
	$(NASM) -f bin -I src/ -o $@ src/dirfix.asm

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(POFOUDSK_COM_BIN): $(POFOUDSK_COM_SRC) $(POFOUDSK_COM_HDR) | $(BUILD_DIR)
	$(BCC) -Md -I$(POFOLIB) -I$(POFOCH375) -Isrc -o $@ $(POFOUDSK_COM_SRC)

# POFOUDSK.SYS: as86 -u + ld86 -0 -d keeps .text/.data/.bss in link order
# (as86 -b would put .data first and move the device header off offset
# 0). Driver-safe units only - no libc, no long helpers.
SYS_UNITS := ch375sys sysfar sysmount sysvdisk ch375core ch375disk part $(TRANSPORT)
SYS_OBJS := $(patsubst %,$(BUILD_DIR)/%.o,$(SYS_UNITS)) $(BUILD_DIR)/tinyhex.o $(BUILD_DIR)/sysdirfix.o
SYS_HDR := $(POFOCH375)/ch375.h $(POFOCH375)/ch375disk.h $(POFOCH375)/transport.h src/part.h src/pofoudsk.h $(POFOLIB)/tinynum.h src/sys.h src/version.h src/build_id.h

$(BUILD_DIR)/%.o: src/%.c $(SYS_HDR) | $(BUILD_DIR)
	$(BCC) -0 -Md -I$(POFOLIB) -I$(POFOCH375) -S -o $(BUILD_DIR)/$*_bcc.s $<
	$(AS86) -0 -u -o $@ $(BUILD_DIR)/$*_bcc.s

$(BUILD_DIR)/%.o: $(POFOLIB)/%.c $(SYS_HDR) | $(BUILD_DIR)
	$(BCC) -0 -Md -I$(POFOLIB) -I$(POFOCH375) -S -o $(BUILD_DIR)/$*_bcc.s $<
	$(AS86) -0 -u -o $@ $(BUILD_DIR)/$*_bcc.s

$(BUILD_DIR)/%.o: $(POFOCH375)/%.c $(SYS_HDR) | $(BUILD_DIR)
	$(BCC) -0 -Md -I$(POFOLIB) -I$(POFOCH375) -S -o $(BUILD_DIR)/$*_bcc.s $<
	$(AS86) -0 -u -o $@ $(BUILD_DIR)/$*_bcc.s

# DIRFIX for the SYS: nasm emits an as86 object directly.
$(BUILD_DIR)/sysdirfix.o: src/sysdirfix.asm src/dirfix.inc | $(BUILD_DIR)
	$(NASM) -f as86 -I src/ -o $@ $<

# ch375sys.o must stay first - its #asm device header has to land at
# file offset 0.
$(POFOUDSK_SYS_BIN): $(SYS_OBJS)
	$(LD86) -0 -d -o $@ $(SYS_OBJS)

# Upload needs the Portfolio in File Transfer Server mode. Paths go into
# a URL query, so \ is sent as %5C.
DEST_DIR_ENC = $(subst \,%5C,$(DEST_DIR))

UPLOAD_BASE = http://$(HOST):$(PORT)/uploadFile?toAtari=1&overwrite=1
UPLOAD_URL = $(UPLOAD_BASE)$(if $(DEST_DIR),&destDir=$(DEST_DIR_ENC))

# The SYS goes to A: (CONFIG.SYS: DEVICE=A:\POFOUDSK.SYS).
SYS_DEST_DIR_ENC ?= A:%5C

# SYS and COM share the IOCTL layout - upload both together.
upload: upload-sys upload-com

upload-com: $(POFOUDSK_COM_BIN)
	curl -sf -F "file=@$(POFOUDSK_COM_BIN)" "$(UPLOAD_URL)"

upload-dirfix: $(DIRFIX_BIN)
	curl -sf -F "file=@$(DIRFIX_BIN)" "$(UPLOAD_URL)"

upload-sys: $(POFOUDSK_SYS_BIN)
	curl -sf -F "file=@$(POFOUDSK_SYS_BIN)" "$(UPLOAD_BASE)&destDir=$(SYS_DEST_DIR_ENC)"

clean:
	rm -rf $(BUILD_DIR)

