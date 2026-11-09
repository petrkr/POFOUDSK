.PHONY: all ch375 sys upload clean

BUILD_DIR := build

# Shared Portfolio library (tinylib etc.). CH375.COM links tinylib.c,
# which #includes tinynum.c; CH375.SYS links tinynum.c alone (no libc,
# no varargs) - never both in one program.
POFOLIB := lib/pofo

CH375_SRC := src/ch375cli.c src/disk.c src/ch375det.c src/ch375part.c src/ch375disk.c src/ch375core.c src/port.c $(POFOLIB)/tinylib.c
CH375_HDR := src/disk.h src/ch375disk.h src/ch375.h src/port.h $(POFOLIB)/tinylib.h $(POFOLIB)/tinynum.h $(POFOLIB)/tinynum.c
CH375_BIN := $(BUILD_DIR)/CH375.COM

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
	$(BCC) -Md -I$(POFOLIB) -o $@ $(CH375_SRC)

# CH375.SYS - device header must land at file offset 0, so this uses
# the as86 -u -o (object) + ld86 -0 -d (tiny/impure link) pipeline
# instead of as86 -b, same reasoning as vdiskc3.c/sysdriver.md: as86
# -b always places .data before .text regardless of source order,
# which would push the hand-written header away from offset 0; ld86
# -d keeps .text/.data/.bss in source order instead. Links only the
# driver-safe units (no libc, no unsigned-long runtime helpers) - never
# disk.c/ch375det.c, which are CLI only. See src/sys.h.
SYS_UNITS := ch375sys sysfar sysmount sysvdisk ch375core ch375disk ch375part port
SYS_OBJS := $(patsubst %,$(BUILD_DIR)/%.o,$(SYS_UNITS)) $(BUILD_DIR)/tinynum.o
SYS_HDR := src/ch375.h src/ch375disk.h src/port.h $(POFOLIB)/tinynum.h src/sys.h src/ch375sys_version.h

$(BUILD_DIR)/%.o: src/%.c $(SYS_HDR) | $(BUILD_DIR)
	$(BCC) -0 -Md -I$(POFOLIB) -S -o $(BUILD_DIR)/$*_bcc.s $<
	$(AS86) -0 -u -o $@ $(BUILD_DIR)/$*_bcc.s

$(BUILD_DIR)/%.o: $(POFOLIB)/%.c $(SYS_HDR) | $(BUILD_DIR)
	$(BCC) -0 -Md -I$(POFOLIB) -S -o $(BUILD_DIR)/$*_bcc.s $<
	$(AS86) -0 -u -o $@ $(BUILD_DIR)/$*_bcc.s

# ch375sys.o must stay first - its #asm device header has to land at
# file offset 0.
$(CH375SYS_BIN): $(SYS_OBJS)
	$(LD86) -0 -d -o $@ $(SYS_OBJS)

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
		"http://$(HOST):$(PORT)/uploadFile?toAtari=1&overwrite=1$(if $(DEST_DIR),&destDir=$(DEST_DIR_ENC))"

clean:
	rm -rf $(BUILD_DIR)

