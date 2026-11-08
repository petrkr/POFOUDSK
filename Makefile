.PHONY: all ch375 sys upload clean

BUILD_DIR := build

CH375_SRC := src/ch375cli.c src/disk.c src/ch375disk.c src/ch375core.c src/port.c
CH375_HDR := src/disk.h src/ch375disk.h src/ch375.h src/port.h
CH375_BIN := $(BUILD_DIR)/CH375.COM

CH375SYS_SRC := src/ch375sys.c
CH375SYS_BIN := $(BUILD_DIR)/CH375.SYS

BCC ?= bcc
AS86 ?= as86
LD86 ?= ld86

# Bridge (or ESP smart-cable client) HTTP endpoint. Override HOST for a
# real ESP on the network, e.g.: make upload HOST=10.220.179.55
HOST ?= 127.0.0.1
PORT ?= 9000
# Empty by default - the bridge's own C:\ default then applies (see
# the upload target below). A trailing backslash in a Makefile
# variable is line continuation, not a literal character, so don't
# set one here; override as e.g. make upload DEST_DIR='D:\SOMEDIR\'.
DEST_DIR ?=

all: ch375 sys

ch375: $(CH375_BIN)

sys: $(CH375SYS_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(CH375_BIN): $(CH375_SRC) $(CH375_HDR) | $(BUILD_DIR)
	$(BCC) -Md -o $@ $(CH375_SRC)

# CH375.SYS - device header must land at file offset 0, so this uses
# the as86 -u -o (object) + ld86 -0 -d (tiny/impure link) pipeline
# instead of as86 -b, same reasoning as vdiskc3.c/sysdriver.md: as86
# -b always places .data before .text regardless of source order,
# which would push the hand-written header away from offset 0; ld86
# -d keeps .text/.data/.bss in source order instead. Links against
# ch375core.c and ch375disk.c (both driver-safe: no printf, no
# unsigned-long runtime helpers) - never disk.c, which pulls in printf
# and unsigned-long helpers a device driver can't link against.
$(CH375SYS_BIN): src/ch375sys.c src/ch375core.c src/ch375disk.c src/port.c src/ch375.h src/ch375disk.h src/port.h src/ch375sys_version.h | $(BUILD_DIR)
	$(BCC) -0 -Md -S -o $(BUILD_DIR)/ch375sys_bcc.s src/ch375sys.c
	$(BCC) -0 -Md -S -o $(BUILD_DIR)/ch375core_bcc.s src/ch375core.c
	$(BCC) -0 -Md -S -o $(BUILD_DIR)/ch375disk_bcc.s src/ch375disk.c
	$(BCC) -0 -Md -S -o $(BUILD_DIR)/port_bcc.s src/port.c
	$(AS86) -0 -u -o $(BUILD_DIR)/ch375sys.o $(BUILD_DIR)/ch375sys_bcc.s
	$(AS86) -0 -u -o $(BUILD_DIR)/ch375core.o $(BUILD_DIR)/ch375core_bcc.s
	$(AS86) -0 -u -o $(BUILD_DIR)/ch375disk.o $(BUILD_DIR)/ch375disk_bcc.s
	$(AS86) -0 -u -o $(BUILD_DIR)/port_lib.o $(BUILD_DIR)/port_bcc.s
	$(LD86) -0 -d -o $@ $(BUILD_DIR)/ch375sys.o $(BUILD_DIR)/ch375core.o $(BUILD_DIR)/ch375disk.o $(BUILD_DIR)/port_lib.o

# Assumes the bridge (or ESP smart-cable client) is already running at
# HOST:PORT and the Portfolio is sitting in File Transfer Server mode.
# Direct curl multipart POST to the bridge's /upload endpoint - destDir
# is a query param, left off entirely when DEST_DIR is empty so the
# bridge's own C:\ default applies. DEST_DIR_ENC substitutes %5C for \
# (DOS path separator) since it's going into a URL query string, not
# shell-quoted - Make's $(subst) does this without needing a subshell/
# sed call.
DEST_DIR_ENC = $(subst \,%5C,$(DEST_DIR))

upload: $(CH375_BIN)
	curl -sf -F "file=@$(CH375_BIN)" \
		"http://$(HOST):$(PORT)/upload?toAtari=1&overwrite=1$(if $(DEST_DIR),&destDir=$(DEST_DIR_ENC))"

clean:
	rm -rf $(BUILD_DIR)

