###############################################################################
## NAME            : Makefile
## PURPOSE         : Build ADprobe using g++ with C++14.
## AUTO-GENERATED  : Adaptive AUTOSAR Code Generation Pipeline
##
## Targets:
##   all     — build the executable (default)
##   clean   — remove object files and executable
##   rebuild — clean then build
##
## Usage:
##   make
##   make clean
##   make rebuild
##   make BUILD_TYPE=debug
##
## WSL / host (no Adaptive AUTOSAR libs installed):
##   make                    # default: HOST_BUILD=1 — links with -pthread + gedr_stub.cpp
##   make HOST_BUILD=0       # link against ara_* / someip / dlt (Yocto or SDK sysroot)
###############################################################################

# ── Toolchain ────────────────────────────────────────────────────────────── #
CXX      := g++
LD       := g++

# ── Target ───────────────────────────────────────────────────────────────── #
TARGET   := ADprobe

# ── Build type (default: release) ────────────────────────────────────────── #
BUILD_TYPE ?= release

ifeq ($(BUILD_TYPE),debug)
    OPT_FLAGS := -g -O0 -DDEBUG
else
    OPT_FLAGS := -O2 -DNDEBUG
endif

# ── Compiler flags ───────────────────────────────────────────────────────── #
CXXFLAGS := -std=c++14             \
            -pedantic              \
            -Wextra                \
            -Wsign-promo           \
            -Wnon-virtual-dtor     \
            -Wctor-dtor-privacy    \
            -Woverloaded-virtual   \
            -Wold-style-cast       \
            -Wpointer-arith        \
            -Wunused               \
            -Wuninitialized        \
            -Winit-self            \
            -Wdeprecated           \
            -Wfloat-equal          \
            -fno-strict-aliasing   \
            -DHAS_SOMEIP_BINDING   \
            -DHAS_IPC_BINDING      \
            -DIF_IAM_ENABLED       \
            $(OPT_FLAGS)

# ── Host vs. target link (default: host/WSL-friendly) ─────────────────────── #
HOST_BUILD ?= 1

ifeq ($(HOST_BUILD),1)
    LDFLAGS := -pthread
    GEDR_STUB_SRC := gedr_stub.cpp
else
    LDFLAGS := -pthread               \
               -lara_someipbinding    \
               -lsomeip               \
               -ldlt                  \
               -lara_ipcbinding       \
               -lara_exec             \
               -lara_com              \
               -lara_log              \
               -lara_per
    GEDR_STUB_SRC :=
endif

# ── Sources ──────────────────────────────────────────────────────────────── #
SRCS := main.cpp \
        ProbeComm/ProbeComm.cpp \
        ProbeCommVariant/ProbeCommVariant.cpp \
        CanConsumer/CanConsumer.cpp \
        ProbeApp/ProbeApp.cpp \
        $(GEDR_STUB_SRC)

# ── Include paths ────────────────────────────────────────────────────────── #
INCLUDES := -I. \
           -IProbeComm \
           -IProbeCommVariant \
           -ICanConsumer \
           -IProbeApp

# ── Object files ─────────────────────────────────────────────────────────── #
OBJS     := $(SRCS:.cpp=.o)

# ── Build directory ──────────────────────────────────────────────────────── #
BUILD_DIR := build

# ================================================================== #
# Targets
# ================================================================== #

.PHONY: all clean rebuild

all: $(TARGET)

## Link
$(TARGET): $(OBJS)
	@echo "[LINK] $@"
	$(LD) $(OBJS) -o $@ $(LDFLAGS)
	@echo "[DONE] $(TARGET) built successfully."

## Compile
%.o: %.cpp
	@echo "[CXX]  $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

## Clean
clean:
	@echo "[CLEAN] Removing object files and executable..."
	@find . -name "*.o" -delete
	@rm -f $(TARGET)
	@echo "[CLEAN] Done."

## Rebuild
rebuild: clean all
