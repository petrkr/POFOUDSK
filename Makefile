.PHONY: all ch375 upload clean

BUILD_DIR := build

CH375_SRC := src/ch375cli.c src/disk.c src/ch375.c src/port.c
CH375_HDR := src/disk.h src/ch375.h src/port.h
CH375_BIN := $(BUILD_DIR)/CH375.COM

BCC ?= bcc

# Bridge (or ESP smart-cable client) HTTP endpoint. Override HOST for a
# real ESP on the network, e.g.: make upload HOST=10.220.179.55
HOST ?= 127.0.0.1
PORT ?= 9000
# Empty by default - the bridge's own C:\ default then applies (see
# the upload target below). A trailing backslash in a Makefile
# variable is line continuation, not a literal character, so don't
# set one here; override as e.g. make upload DEST_DIR='D:\SOMEDIR\'.
DEST_DIR ?=

all: ch375

ch375: $(CH375_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(CH375_BIN): $(CH375_SRC) $(CH375_HDR) | $(BUILD_DIR)
	$(BCC) -Md -o $@ $(CH375_SRC)

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

