ifneq ($(filter-out linux linux-deps clean,$(MAKECMDGOALS)),)
  ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
  else
    $(error PS5_PAYLOAD_SDK is undefined)
  endif
endif
ifeq ($(MAKECMDGOALS),)
  ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
  else
    $(error PS5_PAYLOAD_SDK is undefined)
  endif
endif

# ------------------------------------------------------------ version / identity
# ⚠️ VERSION_TAG MUST stay inside $(BIN) (see the program-name block below).
# The PS5 payload manager (and etaHEN's toolbox -> plugins/payload ELFs menu)
# lists payloads by FILE NAME, so a version that is not part of the file name is
# simply not visible on the console. That is exactly what the previous
# web-file-mgr-vX.Y.Z.elf naming bought, and what this build lost while it was
# still producing a bare NEXUS.elf.
# Side benefit: because the version is in the target name, bumping it also
# defeats make's "CFLAGS changed but nothing depends on it" staleness — a plain
# VERSION_TAG edit alone would silently reuse every stale object and link a
# binary that still reports the old version.
VERSION_TAG := v1.0.0
# ⚠️ TITLE_ID MUST match "titleId" in assets/param.json. app_installer.c uses
# TITLE_ID to create /user/app/<id>, but the PS5 launcher reads the title id out
# of the param.json we drop beside it; if they disagree the app is installed
# under a directory the launcher does not recognise. param.json says NEXS88888.
TITLE_ID    := NEXS88888
PYTHON      ?= python3
MINIFY      ?= 1
STRIP       ?= $(PS5_PAYLOAD_SDK)/bin/prospero-strip
PKG_CONFIG  ?= $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config
HOST_CC     ?= cc
HOST_STRIP  ?= strip
HOST_PKG_CONFIG ?= pkg-config
# Set NAS=0 to drop the vendored SMB/NFS backends (see below).
NAS         ?= 1

# ---------------------------------------------------------------- program name
# ⚠️ The version goes in $(BIN) and deliberately NOT in PROCESS_NAME (src/main.c).
# That split is the whole point, so do not "tidy" it up:
#   * the payload manager lists payloads by FILE NAME -> the version must be in
#     the file name or the console shows nothing;
#   * main.c replaces older instances by matching the THREAD name against
#     PROCESS_NAME. If PROCESS_NAME carried the version, loading v1.0.1 while
#     v1.0.0 was still running would match nothing -> two live instances, and the
#     new one would quietly fall through to port 2027 instead of taking over.
# PROCESS_NAME is therefore the version-free stem; BIN carries the version.
BIN        := PS5-Nexus-$(VERSION_TAG).elf
LINUX_BIN  := PS5-Nexus-linux-$(VERSION_TAG)
COMMON_SRCS := src/main.c src/websrv.c src/filemgr.c src/file_response.c src/task.c \
  src/upload.c src/download.c src/text.c src/list.c src/space.c src/fs_util.c \
  src/json_util.c src/path_util.c src/asset.c src/mime.c src/notify.c \
  src/pkg_installer.c src/pkg_stream.c src/pkg_info.c src/nexus_port.c \
  src/transfer.c src/archive_extract.c src/elfldr.c src/savemgr.c
PS5_SRCS    := $(COMMON_SRCS) src/app_installer.c src/cpu_support_stub.c
LINUX_SRCS  := $(COMMON_SRCS)
# assets/serve-pkg.py (the Windows helper offered as a download off the payload's
# own web root) is folded in with .incbin, not with the C-array generator — a
# raw script streamed straight to the browser is simpler and stays as one
# plaintext blob the ELF content assertion can grep. It is PS5-only and
# optional: without the file the rest of the payload still builds, and the
# download link 404s. Refresh it with tools/build-serve-pkg-exe.sh (it copies
# tools/serve-pkg.py -> assets/serve-pkg.py).
# ⚠️ serve-pkg.exe (the PyInstaller one-file build, ~9.2 MB of bundled CPython)
# is NO LONGER embedded — it would bloat the ELF to ~10.8 MB for no runtime
# gain. It ships via the GitHub Release instead; assets/index.html links to it
# externally. Both helpers are excluded from the gzip ASSETS group below so the
# .incbin copy is the only copy (no double-embedding).
ifneq ($(wildcard assets/serve-pkg.py),)
PS5_SRCS    += src/embed_serve_pkg.c
endif
BASE_ASSETS := $(filter-out %.dds,$(wildcard assets/*))
ifneq ($(filter linux,$(MAKECMDGOALS)),)
ASSETS      := $(filter-out assets/serve-pkg.exe assets/serve-pkg.py,$(BASE_ASSETS))
else
ASSETS      := $(filter-out assets/icon0.png assets/serve-pkg.exe assets/serve-pkg.py,$(BASE_ASSETS))
endif
GEN_SRCS    := $(patsubst assets/%,gen/%, $(ASSETS:=.c))

# Object trees. Two separate trees: PS5 (prospero-clang) vs host (cc/g++)
# objects differ in architecture and must never mix. Defined BEFORE the
# ARCH_OBJS patsubsts below -- those use := and need these already set.
PS5_OBJDIR     := ps5-obj
LINUX_OBJDIR   := linux-obj

# ------------------------------------------------------- NAS backends (vendor)
# Vendored libsmb2 (SMB2/3) and libnfs (NFSv3/v4) behind src/transfer.c. Without
# them transfer_open() returns NULL for smb:/nfs: and the UI's NAS bookmarks can
# never connect — that is the whole "NAS 连不上" symptom.
#
#⚠️ They MUST be compiled in their own invocations, never folded into the single
# $(CC) command that builds our own sources: upstream code does not survive
# -Wall -Werror, and its per-library -I chain (two different files are called
# config.h) cannot coexist with ours in one command line. Hence the two object
# trees + pattern rules below, and the "-w" in their flag sets.
#
# Each library gets its OWN flag set. Both do `#ifdef HAVE_CONFIG_H #include
# "config.h"`, and defining HAVE_CONFIG_H globally would make unrelated TUs pick
# up one of these config.h files from a shared -I chain; scoping the macro to
# exactly the TUs that own the config.h avoids that.
#
# libsmb2's lib/*.c do `#include "smb2.h"` / "libsmb2-private.h", which live in
# include/smb2 -- hence the second -I. _U_ is upstream's unused-parameter
# annotation, defined so the vendor code compiles without us editing it.
SMB_TP_FLAGS := -Os -w -Ithird_party/libsmb2 -Ithird_party/libsmb2/include \
  -Ithird_party/libsmb2/include/smb2 -DHAVE_CONFIG_H \
  "-D_U_=__attribute__((unused))"
# libnfs's generated rpcgen headers (libnfs-raw-*.h) sit next to their TUs in the
# mount/nfs/nfs4/nlm/nsm/portmap/rquota dirs, not in include/ -- upstream's
# lib/Makefile.am adds one -I per dir, and so do we.
NFS_TP_FLAGS := -Os -w -Ithird_party/libnfs -Ithird_party/libnfs/include \
  -Ithird_party/libnfs/include/nfsc -Ithird_party/libnfs/mount \
  -Ithird_party/libnfs/nfs -Ithird_party/libnfs/nfs4 -Ithird_party/libnfs/nlm \
  -Ithird_party/libnfs/nsm -Ithird_party/libnfs/portmap \
  -Ithird_party/libnfs/rquota -DHAVE_CONFIG_H \
  "-D_U_=__attribute__((unused))"

ifeq ($(NAS),1)
TP_SRCS := $(wildcard third_party/libsmb2/lib/*.c) \
  $(wildcard third_party/libnfs/lib/*.c) $(wildcard third_party/libnfs/mount/*.c) \
  $(wildcard third_party/libnfs/nfs/*.c) $(wildcard third_party/libnfs/nfs4/*.c) \
  $(wildcard third_party/libnfs/nlm/*.c) $(wildcard third_party/libnfs/nsm/*.c) \
  $(wildcard third_party/libnfs/portmap/*.c) $(wildcard third_party/libnfs/rquota/*.c)
else
TP_SRCS :=
endif

# ------------------------------------------------ archive engines (in-process)
# Extraction runs IN THIS PROCESS (ZIP / RAR / 7z, encrypted + volume sets) via
# src/extract_engine.c over vendored minizip-ng(+zlib), the 7z SDK and unrar
# 7.20.1 in RARDLL mode (C++). This replaces the external helper ELF: no
# /data/wfm/wfm-7zip-helper.elf to deploy, no elfldr round-trip at enqueue
# time, and the host build can exercise REAL unpacking (hosttest 14-series).
# The engine files are ours but must NOT take the main -Wall -Werror -Oz flags:
# they compile against the vendored headers and keep upstream-agnostic warnings
# relaxed, exactly like the vendor objects.
ARCH_SRCS := src/extract_engine.c src/zip_extract.c src/rar_extract.c \
  src/sevenz_extract.c src/sevenz_chain.c src/sevenz_header.c \
  src/sevenz_volstream.c src/sevenz_mt.c src/zipx_volume.c \
  src/zipx_volstream.c src/zipx_common.c

CXX      ?= $(dir $(CC))prospero-clang++
HOST_CXX ?= c++

# Source set mirrors UnRARDll.vcxproj's ClCompile list minus the Windows-only
# isnt.cpp / motw.cpp (the unrar UNIX makefile omits them too; PS5/linux both
# use the _UNIX branch where their symbols are #ifdef'd out).
UNRAR7_SRCS := \
  third_party/unrar7/archive.cpp third_party/unrar7/arcread.cpp third_party/unrar7/blake2s.cpp \
  third_party/unrar7/cmddata.cpp third_party/unrar7/consio.cpp third_party/unrar7/crc.cpp \
  third_party/unrar7/crypt.cpp third_party/unrar7/dll.cpp third_party/unrar7/encname.cpp \
  third_party/unrar7/errhnd.cpp third_party/unrar7/extinfo.cpp third_party/unrar7/extract.cpp \
  third_party/unrar7/filcreat.cpp third_party/unrar7/file.cpp third_party/unrar7/filefn.cpp \
  third_party/unrar7/filestr.cpp third_party/unrar7/find.cpp third_party/unrar7/getbits.cpp \
  third_party/unrar7/global.cpp third_party/unrar7/hash.cpp third_party/unrar7/headers.cpp \
  third_party/unrar7/largepage.cpp third_party/unrar7/match.cpp \
  third_party/unrar7/options.cpp third_party/unrar7/pathfn.cpp \
  third_party/unrar7/qopen.cpp third_party/unrar7/rar.cpp third_party/unrar7/rarpch.cpp \
  third_party/unrar7/rarvm.cpp third_party/unrar7/rawread.cpp third_party/unrar7/rdwrfn.cpp \
  third_party/unrar7/rijndael.cpp third_party/unrar7/rs.cpp third_party/unrar7/rs16.cpp \
  third_party/unrar7/scantree.cpp third_party/unrar7/secpassword.cpp third_party/unrar7/sha1.cpp \
  third_party/unrar7/sha256.cpp third_party/unrar7/smallfn.cpp third_party/unrar7/strfn.cpp \
  third_party/unrar7/strlist.cpp third_party/unrar7/system.cpp third_party/unrar7/threadpool.cpp \
  third_party/unrar7/timefn.cpp third_party/unrar7/ui.cpp third_party/unrar7/unicode.cpp \
  third_party/unrar7/unpack.cpp third_party/unrar7/volume.cpp

ARCH_TP_C_SRCS := $(wildcard third_party/zlib/src/*.c) \
  $(wildcard third_party/minizip-ng/src/*.c) $(wildcard third_party/7z/*.c)

# ------------------------------------------------ sqlite3 (vendored, public domain)
# Needed so imported saves get a savedata.db entry and the OS lists them.
# Compiled relaxed (-w) like the other vendor libs so -Werror stays ours.
SQLITE_SRCS := third_party/sqlite3/sqlite3.c
SQLITE_TP_FLAGS := -Os -w -Ithird_party/sqlite3 -DSQLITE_OMIT_LOAD_EXTENSION \
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_EXPLAIN \
  -DSQLITE_OMIT_AUTHORIZATION -DSQLITE_OMIT_UTF16
PS5_SQLITE_OBJS   := $(patsubst %.c,$(PS5_OBJDIR)/%.o,$(SQLITE_SRCS))
LINUX_SQLITE_OBJS := $(patsubst %.c,$(LINUX_OBJDIR)/%.o,$(SQLITE_SRCS))

# AesOpt.c hard-codes x86 AES-NI / AVX / VAES intrinsics (PS5 is Zen 2: all
# present). HAVE_WZAES / HAVE_PKCRYPT switch on minizip-ng's two ZIP
# encryption paths; their crypto backend is the in-tree mz_crypt_wfm.c.
ARCH_C_FLAGS := -Os -w -Isrc -Ithird_party/unrar7 -Ithird_party/zlib/include \
  -Ithird_party/minizip-ng/include -Ithird_party/7z \
  -DHAVE_ZLIB -DZLIB_COMPAT -DHAVE_UNISTD_H=1 -D_FILE_OFFSET_BITS=64 \
  -D_LARGEFILE64_SOURCE -DHAVE_FSEEKO -DZ7_PPMD_SUPPORT -DHAVE_WZAES -DHAVE_PKCRYPT
ARCH_C_FLAGS_7Z := $(ARCH_C_FLAGS) -maes -mavx2 -mvaes
UNRAR7_CXX_FLAGS      := -Os -w -std=c++17 -DRARDLL -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE
# prospero-clang++ defaults to -stdlib=libc++; state it explicitly for clarity.
UNRAR7_CXX_FLAGS_PS5  := $(UNRAR7_CXX_FLAGS) -stdlib=libc++
UNRAR7_CXX_FLAGS_HOST := $(UNRAR7_CXX_FLAGS)

PS5_ARCH_OBJS   := $(patsubst src/%.c,$(PS5_OBJDIR)/src/%.o,$(ARCH_SRCS)) \
  $(patsubst %.c,$(PS5_OBJDIR)/%.o,$(ARCH_TP_C_SRCS)) \
  $(patsubst %.cpp,$(PS5_OBJDIR)/%.o,$(UNRAR7_SRCS))
LINUX_ARCH_OBJS := $(patsubst src/%.c,$(LINUX_OBJDIR)/src/%.o,$(ARCH_SRCS)) \
  $(patsubst %.c,$(LINUX_OBJDIR)/%.o,$(ARCH_TP_C_SRCS)) \
  $(patsubst %.cpp,$(LINUX_OBJDIR)/%.o,$(UNRAR7_SRCS))

PS5_TP_OBJS    := $(patsubst %.c,$(PS5_OBJDIR)/%.o,$(TP_SRCS))
LINUX_TP_OBJS  := $(patsubst %.c,$(LINUX_OBJDIR)/%.o,$(TP_SRCS))

CFLAGS := -Oz -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Werror -ffunction-sections -fdata-sections -Isrc -Ithird_party/sqlite3 -Ithird_party/minizip-ng/include -Ithird_party/zlib/include -DVERSION_TAG=\"$(VERSION_TAG)\" -DTITLE_ID=\"$(TITLE_ID)\"
CFLAGS += `$(PKG_CONFIG) libmicrohttpd --cflags`
LDFLAGS := -Wl,--gc-sections
LDADD  := `$(PKG_CONFIG) libmicrohttpd --libs`
LDADD  += -lSceIpmi -lSceAppInstUtil -lSceUserService
LINUX_CFLAGS := -O2 -flto -Wall -Werror -Isrc -Ithird_party/sqlite3 -Ithird_party/minizip-ng/include -Ithird_party/zlib/include -DVERSION_TAG=\"$(VERSION_TAG)\" -DTITLE_ID=\"$(TITLE_ID)\"
LINUX_CFLAGS += `$(HOST_PKG_CONFIG) libmicrohttpd --cflags`
LINUX_LDADD := `$(HOST_PKG_CONFIG) libmicrohttpd --libs` -pthread

# ⚠️ NEXUS_HAVE_* for OUR OWN TUs (src/transfer.c) MUST sit AFTER the base
# CFLAGS / LINUX_CFLAGS ':=' assignments above. A later ':=' clobbers an earlier
# '+=', so appending them next to the vendor flags would leave the SMB/NFS glue
# #if'd out while the libraries still get linked (and then dropped by
# --gc-sections) -- i.e. exactly the silent "NAS never connects" failure.
ifeq ($(NAS),1)
CFLAGS       += -DNEXUS_HAVE_LIBSMB2 -Ithird_party/libsmb2/include
CFLAGS       += -DNEXUS_HAVE_LIBNFS -Ithird_party/libnfs/include -Ithird_party/libnfs/include/nfsc
LINUX_CFLAGS += -DNEXUS_HAVE_LIBSMB2 -Ithird_party/libsmb2/include
LINUX_CFLAGS += -DNEXUS_HAVE_LIBNFS -Ithird_party/libnfs/include -Ithird_party/libnfs/include/nfsc
endif

.PHONY: all linux deps linux-deps clean

all: deps $(BIN)

linux: linux-deps $(LINUX_BIN)

deps:
	@$(PKG_CONFIG) --exists libmicrohttpd || ./install-libmicrohttpd.sh

linux-deps:
	@$(HOST_PKG_CONFIG) --exists libmicrohttpd || \
	  (echo "libmicrohttpd development package is required for make linux" >&2; exit 1)

gen:
	mkdir gen

clean:
	rm -rf $(BIN) $(LINUX_BIN) gen $(PS5_OBJDIR) $(LINUX_OBJDIR)

gen/%.c: assets/% gen-asset-module.py | gen
	$(PYTHON) gen-asset-module.py --path $* $(if $(filter 1,$(MINIFY)),,--no-minify) $< > $@

# Vendor objects: PS5 tree (prospero-clang) and host tree (cc). Different
# architectures, so they must never share an object directory.
# ⚠️ config.h is an explicit prerequisite: it is hand-maintained (not
# autotools-generated) and we DO edit it, but make sees no dependency through
# `#include` -- without this line a config.h fix silently reuses the stale
# objects and the same link error comes back after an apparently clean edit.
$(PS5_OBJDIR)/third_party/libsmb2/%.o: third_party/libsmb2/%.c third_party/libsmb2/config.h
	@mkdir -p $(dir $@)
	$(CC) $(SMB_TP_FLAGS) -c -o $@ $<
$(PS5_OBJDIR)/third_party/libnfs/%.o: third_party/libnfs/%.c third_party/libnfs/config.h
	@mkdir -p $(dir $@)
	$(CC) $(NFS_TP_FLAGS) -c -o $@ $<
$(LINUX_OBJDIR)/third_party/libsmb2/%.o: third_party/libsmb2/%.c third_party/libsmb2/config.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(SMB_TP_FLAGS) -c -o $@ $<
$(LINUX_OBJDIR)/third_party/libnfs/%.o: third_party/libnfs/%.c third_party/libnfs/config.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(NFS_TP_FLAGS) -c -o $@ $<

# Archive engine + vendor objects. Layout inside each objdir:
#   <objdir>/src/...          our own non-engine sources (main flags, -Werror)
#   <objdir>/gen/...          generated asset modules (main flags)
#   <objdir>/arch/src/...     archive-engine TUs (ARCH_C_FLAGS, relaxed)
#   <objdir>/third_party/...  vendored libs (per-library flags)
# The split exists because the engine TUs compile against the vendored headers
# and keep warnings relaxed, like the vendors; -Werror is ours only.
# ⚠️ flag-stamp caveat: no stamp files here -- after changing ARCH_C_FLAGS or
# UNRAR7_CXX_FLAGS, rm -rf the objdir or `make clean`; make cannot see it.
PS5_MAIN_OBJS   := $(patsubst src/%.c,$(PS5_OBJDIR)/src/%.o,$(filter src/%,$(PS5_SRCS))) \
  $(patsubst gen/%.c,$(PS5_OBJDIR)/gen/%.o,$(GEN_SRCS))
LINUX_MAIN_OBJS := $(patsubst src/%.c,$(LINUX_OBJDIR)/src/%.o,$(filter src/%,$(LINUX_SRCS))) \
  $(patsubst gen/%.c,$(LINUX_OBJDIR)/gen/%.o,$(GEN_SRCS))

PS5_ARCH_OBJS   := $(patsubst src/%.c,$(PS5_OBJDIR)/arch/src/%.o,$(ARCH_SRCS)) \
  $(patsubst %.c,$(PS5_OBJDIR)/%.o,$(ARCH_TP_C_SRCS)) \
  $(patsubst %.cpp,$(PS5_OBJDIR)/%.o,$(UNRAR7_SRCS))
LINUX_ARCH_OBJS := $(patsubst src/%.c,$(LINUX_OBJDIR)/arch/src/%.o,$(ARCH_SRCS)) \
  $(patsubst %.c,$(LINUX_OBJDIR)/%.o,$(ARCH_TP_C_SRCS)) \
  $(patsubst %.cpp,$(LINUX_OBJDIR)/%.o,$(UNRAR7_SRCS))

$(PS5_OBJDIR)/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
$(PS5_OBJDIR)/gen/%.o: gen/%.c | gen
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
$(LINUX_OBJDIR)/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(LINUX_CFLAGS) -c -o $@ $<
$(LINUX_OBJDIR)/gen/%.o: gen/%.c | gen
	@mkdir -p $(dir $@)
	$(HOST_CC) $(LINUX_CFLAGS) -c -o $@ $<

$(PS5_OBJDIR)/arch/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ARCH_C_FLAGS) -c -o $@ $<
$(LINUX_OBJDIR)/arch/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(ARCH_C_FLAGS) -c -o $@ $<

$(PS5_OBJDIR)/third_party/zlib/%.o: third_party/zlib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ARCH_C_FLAGS) -c -o $@ $<
$(PS5_OBJDIR)/third_party/minizip-ng/%.o: third_party/minizip-ng/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ARCH_C_FLAGS) -c -o $@ $<
$(PS5_OBJDIR)/third_party/7z/%.o: third_party/7z/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ARCH_C_FLAGS_7Z) -c -o $@ $<
$(PS5_OBJDIR)/third_party/unrar7/%.o: third_party/unrar7/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(UNRAR7_CXX_FLAGS_PS5) -c -o $@ $<
$(LINUX_OBJDIR)/third_party/zlib/%.o: third_party/zlib/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(ARCH_C_FLAGS) -c -o $@ $<
$(LINUX_OBJDIR)/third_party/minizip-ng/%.o: third_party/minizip-ng/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(ARCH_C_FLAGS) -c -o $@ $<
$(LINUX_OBJDIR)/third_party/7z/%.o: third_party/7z/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(ARCH_C_FLAGS_7Z) -c -o $@ $<
$(LINUX_OBJDIR)/third_party/unrar7/%.o: third_party/unrar7/%.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(UNRAR7_CXX_FLAGS_HOST) -c -o $@ $<

$(PS5_OBJDIR)/third_party/sqlite3/%.o: third_party/sqlite3/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SQLITE_TP_FLAGS) -c -o $@ $<
$(LINUX_OBJDIR)/third_party/sqlite3/%.o: third_party/sqlite3/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(SQLITE_TP_FLAGS) -c -o $@ $<

# Link with the C++ driver so libc++ (PS5) / libstdc++ (host) comes in for the
# unrar objects. Every TU is pre-built into its objdir with its OWN flag set --
# nothing is compiled inside the link command any more, so the driver language
# mixups that -x c/-x none used to paper over cannot happen.
$(BIN): $(PS5_MAIN_OBJS) $(PS5_TP_OBJS) $(PS5_ARCH_OBJS) $(PS5_SQLITE_OBJS)
	$(CXX) $(CFLAGS) $(LDFLAGS) -o $@ $(PS5_MAIN_OBJS) $(PS5_TP_OBJS) $(PS5_ARCH_OBJS) $(PS5_SQLITE_OBJS) $(LDADD) -lSceFsInternalForVsh -lSceRegMgr
	$(STRIP) $@

$(LINUX_BIN): $(LINUX_MAIN_OBJS) $(LINUX_TP_OBJS) $(LINUX_ARCH_OBJS) $(LINUX_SQLITE_OBJS)
	$(HOST_CXX) $(LINUX_CFLAGS) -o $@ $(LINUX_MAIN_OBJS) $(LINUX_TP_OBJS) $(LINUX_ARCH_OBJS) $(LINUX_SQLITE_OBJS) $(LINUX_LDADD)
	$(HOST_STRIP) $@
