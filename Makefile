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
# still producing a bare MANGO.elf.
# ⚠️ VERSION_TAG is baked in via -D (CFLAGS/LINUX_CFLAGS). make does NOT track
# flag changes, so a plain bump only forces a re-LINK: the stale objects keep
# reporting the old version (v1.0.1 leaked into the v1.0.2 build once — the
# hosttest "api/status reports the Makefile's VERSION_TAG" check caught it).
# The VSTAMP prerequisite on the src/ object rules below forces those objects
# to recompile on every version change.
VERSION_TAG := v1.1
# ⚠️ TITLE_ID MUST match "titleId" in assets/param.json. app_installer.c uses
# TITLE_ID to create /user/app/<id>, but the PS5 launcher reads the title id out
# of the param.json we drop beside it; if they disagree the app is installed
# under a directory the launcher does not recognise. param.json says MANG88888.
TITLE_ID    := MANG88888
PYTHON      ?= python3
MINIFY      ?= 1
STRIP       ?= $(PS5_PAYLOAD_SDK)/bin/prospero-strip
PKG_CONFIG  ?= $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config
HOST_CC     ?= cc
HOST_STRIP  ?= strip
HOST_PKG_CONFIG ?= pkg-config
# Set NAS=0 to drop the vendored SMB/NFS backends (see below).
NAS         ?= 1
# WebDAV is self-contained (plain HTTP/1.1 written in src/transfer.c): no
# vendored library, no TLS. Set WEBDAV=0 to compile the scheme out.
WEBDAV      ?= 1

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
BIN        := PS5-Mango-$(VERSION_TAG).elf
LINUX_BIN  := PS5-Mango-linux-$(VERSION_TAG)
COMMON_SRCS := src/main.c src/websrv.c src/filemgr.c src/file_response.c src/task.c \
  src/upload.c src/download.c src/text.c src/list.c src/nas_addr.c src/space.c \
  src/fs_util.c src/json_util.c src/path_util.c src/asset.c src/mime.c \
  src/notify.c \
  src/mango_log.c src/pkg_installer.c src/pkg_info.c src/mango_port.c \
  src/readsrc.c \
  src/pkg_stream.c src/save_domain.c src/transfer.c src/archive_extract.c \
  src/elfldr.c src/savemgr.c src/zip_write.c
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
#
# ⚠️ They deliberately live OUTSIDE this directory (under MANGO_BUILD_ROOT), not
# in ./ps5-obj and ./linux-obj. Reason (2026-10-16): the project sits on the
# Windows Desktop, and one `WFM_CLEAN=1` build deletes+recreates ~460 objects.
# File COUNT is the problem, not bytes (both trees together are only ~11 MB, but
# 458 files): every one of those writes crosses 9p onto the Desktop, which is
# exactly what explorer, the WSearch indexer and the 11 third-party shell icon
# overlay handlers (Baidu Netdisk owns 7 of them) re-scan. The measured fallout
# was shell-session restarts while a build was running.
#
# ⚠️ This default MUST stay byte-identical to the one in .build/build-root.sh --
# that file is the shell-side copy of the same fact (make and sh cannot share one
# syntax). Drift between the two is what .build/chk_build_root.py guards, with
# .build/chk-build-root-counterproof.py proving the guard actually goes red.
# Override with the environment: MANGO_BUILD_ROOT=/somewhere make all
MANGO_BUILD_ROOT ?= $(HOME)/.cache/ps5-mango-build
PS5_OBJDIR     := $(MANGO_BUILD_ROOT)/ps5-obj
LINUX_OBJDIR   := $(MANGO_BUILD_ROOT)/linux-obj

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
# ── vendor TU section granularity ────────────────────────────────────────────
# ⚠️ MEASURED, NOT ASSUMED (v1.0, 2026-10-24): adding -ffunction-sections /
# -fdata-sections to the vendor TUs did NOT shrink .text (960 bytes total).
# sqlite3.o's 691 KB survives --gc-sections almost intact because sqlite's
# internal call graph is dense -- every function is reachable from the entry
# points we do use. The flags stay because they are free and they make future
# trimming possible, but do not expect .text to drop from them.
#
# The real win is -fno-asynchronous-unwind-tables -fno-unwind-tables: nothing
# in this payload unwinds through a C frame, so the vendor C TUs' .eh_frame and
# its index were pure dead weight. Measured on the whole payload:
#   .eh_frame      147,268 -> 39,524   (-107,744)
#   .eh_frame_hdr   31,196 ->  8,012   ( -23,184)
#   ELF total    2,588,208 -> 2,457,136 (-131,072, -5.1%)
# NOTE: unrar is C++ and is handled separately below -- its unwind tables are
# part of the ABI it was compiled against, so it does NOT take this flag.
TP_SECTION_FLAGS := -ffunction-sections -fdata-sections \
  -fno-asynchronous-unwind-tables -fno-unwind-tables

SMB_TP_FLAGS := -Os -w -Ithird_party/libsmb2 -Ithird_party/libsmb2/include \
  -Ithird_party/libsmb2/include/smb2 -DHAVE_CONFIG_H \
  "-D_U_=__attribute__((unused))" $(TP_SECTION_FLAGS)
# libnfs's generated rpcgen headers (libnfs-raw-*.h) sit next to their TUs in the
# mount/nfs/nfs4/nlm/nsm/portmap/rquota dirs, not in include/ -- upstream's
# lib/Makefile.am adds one -I per dir, and so do we.
NFS_TP_FLAGS := -Os -w -Ithird_party/libnfs -Ithird_party/libnfs/include \
  -Ithird_party/libnfs/include/nfsc -Ithird_party/libnfs/mount \
  -Ithird_party/libnfs/nfs -Ithird_party/libnfs/nfs4 -Ithird_party/libnfs/nlm \
  -Ithird_party/libnfs/nsm -Ithird_party/libnfs/portmap \
  -Ithird_party/libnfs/rquota -DHAVE_CONFIG_H \
  "-D_U_=__attribute__((unused))" $(TP_SECTION_FLAGS)

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
  -DSQLITE_OMIT_AUTHORIZATION -DSQLITE_OMIT_UTF16 $(TP_SECTION_FLAGS)
PS5_SQLITE_OBJS   := $(patsubst %.c,$(PS5_OBJDIR)/%.o,$(SQLITE_SRCS))
LINUX_SQLITE_OBJS := $(patsubst %.c,$(LINUX_OBJDIR)/%.o,$(SQLITE_SRCS))

# ------------------------------------------------ cJSON (vendored, MIT, 1.7.19)
# JSON 解析器：云盘驱动（P3）解析四家网盘 API 响应。项目自带的
# src/json_util.c 只有 strbuf + json_escape（序列化侧），无解析能力。
# 单文件零依赖，两平台同源编译；选取记录见 third_party/cjson/VENDORED.md
# （2026-10-25，用户规矩「遇问题先查 GitHub」后选定）。上游代码未改，
# -w 放松告警（与 sqlite3/smb2 同款 vendor 纪律）。
CJSON_SRCS := third_party/cjson/cJSON.c
CJSON_TP_FLAGS := -Os -w -Ithird_party/cjson $(TP_SECTION_FLAGS)
PS5_CJSON_OBJS   := $(patsubst %.c,$(PS5_OBJDIR)/%.o,$(CJSON_SRCS))
LINUX_CJSON_OBJS := $(patsubst %.c,$(LINUX_OBJDIR)/%.o,$(CJSON_SRCS))

# AesOpt.c hard-codes x86 AES-NI / AVX / VAES intrinsics (PS5 is Zen 2: all
# present). HAVE_WZAES / HAVE_PKCRYPT switch on minizip-ng's two ZIP
# encryption paths; their crypto backend is the in-tree mz_crypt_wfm.c.
ARCH_C_FLAGS := -Os -w -Isrc -Ithird_party/unrar7 -Ithird_party/zlib/include \
  -Ithird_party/minizip-ng/include -Ithird_party/7z \
  -DHAVE_ZLIB -DZLIB_COMPAT -DHAVE_UNISTD_H=1 -D_FILE_OFFSET_BITS=64 \
  -D_LARGEFILE64_SOURCE -DHAVE_FSEEKO -DZ7_PPMD_SUPPORT -DHAVE_WZAES -DHAVE_PKCRYPT \
  $(TP_SECTION_FLAGS)
ARCH_C_FLAGS_7Z := $(ARCH_C_FLAGS) -maes -mavx2 -mvaes
# unrar is C++, so it keeps its unwind tables (exceptions are part of the ABI it
# was written against); only the section granularity applies, so gc-sections can
# drop the unrar entry points we never call.
UNRAR7_CXX_FLAGS      := -Os -w -std=c++17 -DRARDLL -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE \
  -ffunction-sections -fdata-sections
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
# --icf=all folds identical code. Measured -32 KB on the pre-TP_SECTION_FLAGS
# payload; once the vendor TUs also get -ffunction-sections that gain is gone
# (gc-sections reaches the same duplicates first, 0 bytes measured on 2026-10-24).
# Kept because it costs nothing and re-applies if the section layout changes.
# --icf=safe is NOT usable here: ld.lld ignores it for crt1.o (no SHT_LLVM_ADDRSIG).
LDFLAGS := -Wl,--gc-sections -Wl,--icf=all
LDADD  := `$(PKG_CONFIG) libmicrohttpd --libs`
# ⚠️ NO -lSceIpmi / -lSceAppInstUtil here: AppInstUtil must be dlopen'd at
# runtime (Ipmi first) — statically linking it killed the payload on install
# (proven against Loopayeh/pkg-sender, 2026-10-05). pkg_installer.c owns the
# runtime loading; -lSceUserService stays (foreground-user lookup).
LDADD  += -lSceUserService
LINUX_CFLAGS := -O2 -flto -Wall -Werror -Isrc -Ithird_party/sqlite3 -Ithird_party/minizip-ng/include -Ithird_party/zlib/include -DVERSION_TAG=\"$(VERSION_TAG)\" -DTITLE_ID=\"$(TITLE_ID)\"
LINUX_CFLAGS += `$(HOST_PKG_CONFIG) libmicrohttpd --cflags`
LINUX_LDADD := `$(HOST_PKG_CONFIG) libmicrohttpd --libs` -pthread

# ⚠️ MANGO_HAVE_* for OUR OWN TUs (src/transfer.c) MUST sit AFTER the base
# CFLAGS / LINUX_CFLAGS ':=' assignments above. A later ':=' clobbers an earlier
# '+=', so appending them next to the vendor flags would leave the SMB/NFS glue
# #if'd out while the libraries still get linked (and then dropped by
# --gc-sections) -- i.e. exactly the silent "NAS never connects" failure.
ifeq ($(NAS),1)
CFLAGS       += -DMANGO_HAVE_LIBSMB2 -Ithird_party/libsmb2/include
CFLAGS       += -DMANGO_HAVE_LIBNFS -Ithird_party/libnfs/include -Ithird_party/libnfs/include/nfsc
LINUX_CFLAGS += -DMANGO_HAVE_LIBSMB2 -Ithird_party/libsmb2/include
LINUX_CFLAGS += -DMANGO_HAVE_LIBNFS -Ithird_party/libnfs/include -Ithird_party/libnfs/include/nfsc
endif

# WebDAV: the backend lives in its own TU (src/backend_webdav.c) since the
# 2026-10-27 backend split; the macro still has to sit AFTER the base ':='
# assignments for the same reason as above.
ifeq ($(WEBDAV),1)
CFLAGS       += -DMANGO_HAVE_WEBDAV
LINUX_CFLAGS += -DMANGO_HAVE_WEBDAV
PS5_SRCS    += src/backend_webdav.c
LINUX_SRCS  += src/backend_webdav.c
endif

# ------------------------------------------- mbedTLS（只服务 WebDAV 的 https）
# v1.0.8（2026-10-27）：云盘（阿里/百度/夸克）整条链移除后，mbedTLS 在链上
# 的唯一用途是 `webdavs://`（WebDAV over TLS）。它原先挂在 CLOUD_NET 下
# （云盘网络底座 cloud_net.c），一并拆开、改名 MBTLS：
#   · 云盘那套 DNS/TLS/HTTP 底座（cloud_net.c）与 121 证 CA bundle
#     （cloud_cacert.c，cacert.pem 182,140 B）一起下链 —— WebDAV 的 TLS
#     策略是 VERIFY_NONE（要能连自签的飞牛/群晖），本就不需要信任链，
#     这两个 TU 是纯负担，删掉直接省约 180 KB。
#   · 宏改名 -DMANGO_HAVE_MBEDTLS：backend_webdav.c 的 WD_HAVE_TLS 由
#     「MANGO_HAVE_WEBDAV && MANGO_HAVE_CLOUD」改为与它组合——原写法会把
#     webdavs 跟云盘绑死（砍云盘即砍 https），那正是这次要避免的连带损伤。
# third_party/mbedtls 由 tools/build-mbedtls.sh 交叉构建（PS5LM 配方：
# prospero-cmake -G Ninja + MBEDTLS_THREADING_C/PTHREAD），5 个静态库。
# ⚠️ 静态库链接顺序 mbedtls -> x509 -> crypto -> p256m -> everest（符号依赖序）。
# 宿主机 cc 可直接链这套 prospero 产物（2026-10-25 实测：x86-64 调用约定
# 一致，libc 符号由 glibc 侧解析），故 LINUX 与 PS5 共用同一套 .a。
# Set MBTLS=0 to compile TLS out (webdavs:// then fails with a clear error).
MBTLS ?= 1
MBT_LIB   := third_party/mbedtls/lib
MBT_LIBS  := $(MBT_LIB)/libmbedtls.a $(MBT_LIB)/libmbedx509.a \
             $(MBT_LIB)/libmbedcrypto.a $(MBT_LIB)/libp256m.a \
             $(MBT_LIB)/libeverest.a
# 宿主机 cc 无法直接链 PS5（newlib）产物：prospero-clang 展开出的
# __error / __isthreaded / __inet_pton / sysctl（entropy 的 BSD 分支）
# 在 glibc 侧没有对应物，defsym 补不干净。故 LINUX 侧用
# third_party/mbedtls/lib-host（MBEDTLS_HOST=1 tools/build-mbedtls.sh
# 构建：__linux__ 分支走 getrandom，符号干净）。
ifeq ($(MBTLS),1)
# MANGO_HAVE_MBEDTLS：告诉 backend_webdav.c「TLS 可用」。宏在这里而不是让
# 它自己猜——它只需要「mbedtls 在不在链上」这一位信息。
CFLAGS       += -DMANGO_HAVE_MBEDTLS -Ithird_party/mbedtls/include
LINUX_CFLAGS += -DMANGO_HAVE_MBEDTLS -Ithird_party/mbedtls/include
LDADD        += $(MBT_LIBS)
LINUX_MBT_LIBS := $(patsubst $(MBT_LIB)/%,$(MBT_LIB)-host/%,$(MBT_LIBS))
ifeq ($(wildcard $(MBT_LIB)-host/libmbedtls.a),)
$(warning mbedtls: third_party/mbedtls/lib-host missing; run \
  MBEDTLS_HOST=1 tools/build-mbedtls.sh (WSL) before 'make linux')
endif
LINUX_LDADD  += $(LINUX_MBT_LIBS)
endif

.PHONY: all linux deps linux-deps clean test test-guards test-host test-frontend

all: deps $(BIN)

linux: linux-deps $(LINUX_BIN)

# `make test` 只跑跨平台那半边。行为套件绑平台（hosttest 要宿主构建 + WSL，
# 前端断言要 Windows Chromium），硬塞进默认目标只会让它在另一半平台上假红。
# ⚠️ 关键是：**守卫必须在构建里有个入口**。此前它们只活在
# .build/build-verify-wsl.sh 里，而那个脚本要手工跑 ⇒ chk_signal_disposition.py
# 在被取代时漏注册，停摆至今无人察觉（守卫腐烂是静默的：没人跑，就没红过）。
test: test-guards
	@echo
	@echo "  static guards + counter-proofs: OK"
	@echo "  行为套件不在这里跑，按平台选："
	@echo "    make test-host      hosttest.py (需宿主构建，WSL)"
	@echo "    make test-frontend  Chromium 断言 + 前端反证 (需 Windows)"

# 静态守卫：只读源码 + python3，**不需要构建产物**。这是故意的 —— 守卫守的是
# 形状（挂载次数、参数所有权、SIGPIPE disposition…），不是这次的产物。
# run-guards.sh 还会自检清单与主链注册集合是否一致，漏注册当场红。
test-guards:
	@bash .build/run-guards.sh

test-host: linux
	@$(PYTHON) hosttest.py

test-frontend:
	@bash .build/frontend-verify-windows.sh

deps:
	@$(PKG_CONFIG) --exists libmicrohttpd || ./install-libmicrohttpd.sh

linux-deps:
	@$(HOST_PKG_CONFIG) --exists libmicrohttpd || \
	  (echo "libmicrohttpd development package is required for make linux" >&2; exit 1)

gen:
	mkdir gen

# ⚠️ Compiler flags are NOT in any .o's dependency graph -- make only compares
# source mtimes. After changing a vendor flag (TP_SECTION_FLAGS, SQLITE_TP_FLAGS,
# ARCH_C_FLAGS ...) you MUST run this, or the link silently reuses objects built
# with the old flags and the size win disappears with no error to tell you.
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

# Version stamp: the file NAME encodes VERSION_TAG, so a bump changes this
# prerequisite path for every src/ object and forces recompilation without a
# manual objdir wipe. Old stamps are pruned so returning to an earlier version
# also recompiles instead of trusting a stale mtime. gen/ (asset blobs) and
# arch/third_party objects are excluded: they compile without -DVERSION_TAG
# and never read it, and regenerating/minifying every asset on each bump
# would cost a full slow rebuild for nothing.
# ⚠️ The stamp lives under MANGO_BUILD_ROOT too, not in .build/: everything a
# build writes must stay off the Desktop (see the object-tree note above). With
# this, the only things a build creates inside the project are the two
# deliverables and gen/.
VSTAMP := $(MANGO_BUILD_ROOT)/vstamp-$(VERSION_TAG)
$(VSTAMP):
	@mkdir -p $(dir $@)
	@rm -f $(dir $@)vstamp-*
	@touch $@

$(PS5_OBJDIR)/src/%.o: src/%.c $(VSTAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
$(PS5_OBJDIR)/gen/%.o: gen/%.c | gen
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
$(LINUX_OBJDIR)/src/%.o: src/%.c $(VSTAMP)
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

$(PS5_OBJDIR)/third_party/cjson/%.o: third_party/cjson/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CJSON_TP_FLAGS) -c -o $@ $<
$(LINUX_OBJDIR)/third_party/cjson/%.o: third_party/cjson/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CJSON_TP_FLAGS) -c -o $@ $<

# Link with the C++ driver so libc++ (PS5) / libstdc++ (host) comes in for the
# unrar objects. Every TU is pre-built into its objdir with its OWN flag set --
# nothing is compiled inside the link command any more, so the driver language
# mixups that -x c/-x none used to paper over cannot happen.
# cJSON 无调用方时同样被 --gc-sections 整对象裁掉（与 mbedtls
# 同策略：先接基建，P3 驱动开工时零 Makefile 改动）。
$(BIN): $(PS5_MAIN_OBJS) $(PS5_TP_OBJS) $(PS5_ARCH_OBJS) $(PS5_SQLITE_OBJS) $(PS5_CJSON_OBJS)
	$(CXX) $(CFLAGS) $(LDFLAGS) -o $@ $(PS5_MAIN_OBJS) $(PS5_TP_OBJS) $(PS5_ARCH_OBJS) $(PS5_SQLITE_OBJS) $(PS5_CJSON_OBJS) $(LDADD) -lSceFsInternalForVsh -lSceRegMgr
	$(STRIP) $@

$(LINUX_BIN): $(LINUX_MAIN_OBJS) $(LINUX_TP_OBJS) $(LINUX_ARCH_OBJS) $(LINUX_SQLITE_OBJS) $(LINUX_CJSON_OBJS)
	$(HOST_CXX) $(LINUX_CFLAGS) -o $@ $(LINUX_MAIN_OBJS) $(LINUX_TP_OBJS) $(LINUX_ARCH_OBJS) $(LINUX_SQLITE_OBJS) $(LINUX_CJSON_OBJS) $(LINUX_LDADD)
	$(HOST_STRIP) $@
