# ============================================================================
# Makefile — cpp-maker (projet multi-fichiers C++23)
# ============================================================================
# Cibles :
#   make              → build release (build/release/cpp-maker)
#   make debug        → build debug   (build/debug/cpp-maker)
#   make -j$(nproc)   → build parallèle
#   make test         → tests de fumée (new + build + run)
#   make install      → install dans $(DESTDIR)$(PREFIX)/bin
#   make clean        → supprime build/ et bin/
#   make distclean    → supprime aussi les binaires résiduels
#   make format       → clang-format sur sources et headers (si dispo)
#   make help         → rappel des cibles
#
# Variables :
#   CXX, BUILD, PREFIX, DESTDIR, V (V=1 → commandes affichées)
# ============================================================================

# ---------------------------------------------------------------------------
# Objectif par défaut
# ---------------------------------------------------------------------------
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

# ---------------------------------------------------------------------------
# Outils
# ---------------------------------------------------------------------------
CXX     ?= g++
INSTALL ?= install
RM      ?= rm -f
MKDIR   ?= mkdir -p

# Mode verbeux : `make V=1` affiche les commandes.
ifeq ($(V),1)
  Q :=
  Q_ECHO := @true
else
  Q := @
  Q_ECHO := @echo
endif

# ---------------------------------------------------------------------------
# Cible, répertoires, sources
# ---------------------------------------------------------------------------
TARGET      := cpp-maker
BUILD       ?= release
OBJDIR      := build/$(BUILD)
TARGET_PATH := $(OBJDIR)/$(TARGET)

SRCS := \
    src/util.cpp \
    src/proc.cpp \
    src/output.cpp \
    src/toml.cpp \
    src/deps.cpp \
    src/cfg.cpp \
    src/sanitize.cpp \
    src/toolchain.cpp \
    src/build.cpp \
    src/elf.cpp \
    src/cmd-helpers.cpp \
    src/cmd-build.cpp \
    src/cmd-project.cpp \
    src/cmd-deploy.cpp \
    src/cmd-config.cpp \
	src/cmd-backup.cpp \
    src/main.cpp

HDRS := src/cpp-maker.hpp src/cmd-helpers.hpp src/flags.hpp
OBJS := $(addprefix $(OBJDIR)/,$(SRCS:.cpp=.o))
DEPS := $(OBJS:.o=.d)

# ---------------------------------------------------------------------------
# Flags
# ---------------------------------------------------------------------------
CXXSTD    := -std=c++23
WARNFLAGS := -Wall -Wextra -Wpedantic -Wformat=2 \
             -Wshadow -Wconversion -Wsign-conversion -Wdouble-promotion \
             -Wundef -Wcast-align -Wcast-qual -Woverloaded-virtual \
             -Wnon-virtual-dtor

# Flags indépendants du mode
CPPFLAGS += -I. -MMD -MP
CXXFLAGS += $(CXXSTD) $(WARNFLAGS) -pthread
LDFLAGS  += -pthread

# Chaque mode a son propre répertoire build/<mode>/ : pas de mélange d'objets.
ifeq ($(BUILD),debug)
  CXXFLAGS += -O0 -g3 -DDEBUG -fno-omit-frame-pointer
else ifeq ($(BUILD),asan)
  CXXFLAGS += -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
  LDFLAGS  += -fsanitize=address,undefined
else ifeq ($(BUILD),tsan)
  CXXFLAGS += -O1 -g -fsanitize=thread -fno-omit-frame-pointer
  LDFLAGS  += -fsanitize=thread
else
  CXXFLAGS += -O2 -DNDEBUG
endif

# ---------------------------------------------------------------------------
# Règles
# ---------------------------------------------------------------------------
.PHONY: all debug release asan tsan clean distclean \
        test check test-asan test-tsan test-valgrind test-all \
        install uninstall format help

all: $(TARGET_PATH)

$(TARGET_PATH): $(OBJS)
	$(Q_ECHO) "  LD   $@"
	$(Q)$(CXX) $(OBJS) -o $@ $(LDFLAGS)

$(OBJDIR)/%.o: %.cpp $(HDRS)
	$(Q)test -d $(dir $@) || $(MKDIR) $(dir $@)
	$(Q_ECHO) "  CXX  $<"
	$(Q)$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

# Bascule debug/release sans mélanger les objets.
# (Les deux builds coexistent : build/debug et build/release.)
debug:
	$(Q)$(MAKE) --no-print-directory BUILD=debug
	$(Q_ECHO) "✓ build debug  : build/debug/$(TARGET)"

release:
	$(Q)$(MAKE) --no-print-directory BUILD=release
	$(Q_ECHO) "✓ build release: build/release/$(TARGET)"

asan:
	$(Q)$(MAKE) --no-print-directory BUILD=asan
	$(Q_ECHO) "✓ build asan   : build/asan/$(TARGET)"

tsan:
	$(Q)$(MAKE) --no-print-directory BUILD=tsan
	$(Q_ECHO) "✓ build tsan   : build/tsan/$(TARGET)"

test-asan:
	$(Q)$(MAKE) --no-print-directory BUILD=asan
	$(Q)$(MAKE) --no-print-directory test BUILD=asan

test-tsan:
	$(Q)$(MAKE) --no-print-directory BUILD=tsan
	$(Q)$(MAKE) --no-print-directory test BUILD=tsan

# Valgrind s'utilise sur un binaire non instrumenté (build debug conseillé).
test-valgrind: debug
	@set -e; \
	tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT INT TERM; \
	VG='valgrind --quiet --leak-check=full --errors-for-leak-kinds=definite,indirect --error-exitcode=1 --track-origins=yes'; \
	echo "── version (valgrind) ──"; \
	$$VG "$(CURDIR)/build/debug/$(TARGET)" --version; \
	echo "── help list (valgrind) ──"; \
	$$VG "$(CURDIR)/build/debug/$(TARGET)" help list > /dev/null && echo "  ok"; \
	echo "── new + build + run (valgrind) ──"; \
	cd "$$tmp"; \
	$$VG "$(CURDIR)/build/debug/$(TARGET)" new hello > /dev/null; \
	cd hello; \
	$$VG "$(CURDIR)/build/debug/$(TARGET)" build > /dev/null; \
	out=$$($$VG "$(CURDIR)/build/debug/$(TARGET)" run); \
	echo "$$out" | grep -q "Hello from hello"; \
	echo "  ok — sortie : $$out"; \
	echo "✓ valgrind passé"

test-all: test test-asan test-tsan test-valgrind

# ---------------------------------------------------------------------------
# Nettoyage
# ---------------------------------------------------------------------------
clean:
	$(Q_ECHO) "  RM   build/ bin/"
	$(Q)$(RM) -r build bin

distclean: clean
	$(Q)$(RM) $(TARGET)

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------
PREFIX  ?= /usr/local
DESTDIR ?=

install: $(TARGET_PATH)
	$(Q_ECHO) "  INST $(DESTDIR)$(PREFIX)/bin/$(TARGET)"
	$(Q)$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(Q)$(INSTALL) -m 755 $(TARGET_PATH) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

uninstall:
	$(Q_ECHO) "  RM   $(DESTDIR)$(PREFIX)/bin/$(TARGET)"
	$(Q)$(RM) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

# ---------------------------------------------------------------------------
# Tests de fumée : construit un projet temporaire, vérifie build + run.
# ---------------------------------------------------------------------------
test check: $(TARGET_PATH)
	@set -e; \
	tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT INT TERM; \
	echo "── version ──"; \
	"$(CURDIR)/$(TARGET_PATH)" --version; \
	echo "── help ──"; \
	"$(CURDIR)/$(TARGET_PATH)" help list > /dev/null && echo "  ok"; \
	echo "── new + build + run ──"; \
	cd "$$tmp"; \
	"$(CURDIR)/$(TARGET_PATH)" new hello > /dev/null; \
	cd hello; \
	"$(CURDIR)/$(TARGET_PATH)" build > /dev/null; \
	out=$$("$(CURDIR)/$(TARGET_PATH)" run); \
	echo "$$out" | grep -q "Hello from hello"; \
	echo "  ok — sortie : $$out"; \
	echo "✓ tests de fumée passés"

# ---------------------------------------------------------------------------
# Formatage (clang-format, optionnel)
# ---------------------------------------------------------------------------
format:
	@if command -v clang-format > /dev/null 2>&1; then \
	  clang-format -i $(SRCS) $(HDRS); \
	  echo "✓ formaté"; \
	else \
	  echo "clang-format introuvable" >&2; exit 1; \
	fi

# ---------------------------------------------------------------------------
# Aide
# ---------------------------------------------------------------------------
help:
	@echo "cpp-maker — Makefile"
	@echo ""
	@echo "  make                 build $(BUILD) (BUILD=$(BUILD))"
		@echo "  make debug           build -O0 -g3"
	@echo "  make release         build -O2"
	@echo "  make asan            build ASan+UBSan (build/asan)"
	@echo "  make tsan            build TSan (build/tsan)"
	@echo "  make test|check      tests de fumée"
	@echo "  make test-asan       tests sous ASan+UBSan"
	@echo "  make test-tsan       tests sous TSan"
	@echo "  make test-valgrind   tests sous valgrind (build debug)"
	@echo "  make test-all        tout ce qui précède"
	@echo "  make install         install dans \$$(DESTDIR)\$$(PREFIX)/bin"
	@echo "  make uninstall       retire le binaire"
	@echo "  make clean           supprime build/ et bin/"
	@echo "  make distclean       supprime aussi les binaires résiduels"
	@echo "  make format          clang-format"
	@echo ""
	@echo "  CXX=$(CXX)  BUILD=$(BUILD)  PREFIX=$(PREFIX)  V=1 (verbeux)"

# ---------------------------------------------------------------------------
# Dépendances générées par -MMD -MP
# ---------------------------------------------------------------------------
-include $(DEPS)
