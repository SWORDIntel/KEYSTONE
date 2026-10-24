# KEYSTONE Makefile
# Standard make / make test workflow

CC      := gcc
MARCH   ?=
KEYSTONE_ARCH_FLAGS ?=
CFLAGS  := -O3 $(KEYSTONE_ARCH_FLAGS) $(if $(strip $(MARCH)),-march=$(MARCH)) -fPIC -Wall -Wextra -Werror=implicit-function-declaration -I./include
LDFLAGS := -lm -ldl

# Optional OpenMP (default: auto-enabled if the compiler supports it,
# since multi-core CPUs benefit from parallel batch search.  Set
# KEYSTONE_ENABLE_OPENMP=0 to disable.)
ifeq ($(KEYSTONE_ENABLE_OPENMP),1)
    CFLAGS  += -fopenmp
    LDFLAGS += -fopenmp
else ifneq ($(KEYSTONE_ENABLE_OPENMP),0)
    ifeq ($(shell echo | $(CC) -fopenmp -dM -E - 2>/dev/null | grep -q '_OPENMP' && echo yes),yes)
        CFLAGS  += -fopenmp
        LDFLAGS += -fopenmp
    endif
endif

# Optional tar.zst streaming support (default: enabled if libarchive + libzstd are available)
ifeq ($(KEYSTONE_ENABLE_TAR_ZST),1)
    TAR_ZST_CFLAGS := -DKEYSTONE_ENABLE_TAR_ZST
    TAR_ZST_LDFLAGS := -larchive -lzstd -lpthread
    CFLAGS  += $(TAR_ZST_CFLAGS)
    LDFLAGS += $(TAR_ZST_LDFLAGS)
else ifneq ($(KEYSTONE_ENABLE_TAR_ZST),0)
    ifeq ($(shell pkg-config --exists libarchive libzstd 2>/dev/null && echo yes),yes)
        KEYSTONE_ENABLE_TAR_ZST := 1
        TAR_ZST_CFLAGS := -DKEYSTONE_ENABLE_TAR_ZST
        TAR_ZST_LDFLAGS := $(shell pkg-config --libs libarchive libzstd) -lpthread
        CFLAGS  += $(TAR_ZST_CFLAGS)
        LDFLAGS += $(TAR_ZST_LDFLAGS)
    endif
endif

# Optional Fortran backend (default: enabled if gfortran is available)
# Compiled as an object file and linked directly into libkeystone.so
ifeq ($(KEYSTONE_ENABLE_FORTRAN),1)
    ifeq ($(shell command -v gfortran >/dev/null 2>&1 && echo yes),yes)
        FORTRAN_CFLAGS := -DKEYSTONE_ENABLE_FORTRAN
        CFLAGS  += $(FORTRAN_CFLAGS)
        FORTRAN_OBJ := fortran/keystone_batch.o
    else
        $(error KEYSTONE_ENABLE_FORTRAN=1 requires gfortran)
    endif
else ifneq ($(KEYSTONE_ENABLE_FORTRAN),0)
    ifeq ($(shell command -v gfortran >/dev/null 2>&1 && echo yes),yes)
        FORTRAN_CFLAGS := -DKEYSTONE_ENABLE_FORTRAN
        CFLAGS  += $(FORTRAN_CFLAGS)
        FORTRAN_OBJ := fortran/keystone_batch.o
    endif
endif

# Optional CUDA backend
NVCC ?= nvcc
CUDA_ARCH_FLAGS ?=
CUDA_HOME ?= $(firstword $(wildcard /usr/local/cuda /usr/local/cuda-13.3 /usr/local/cuda-12.8 /usr/local/cuda-12.4 /opt/cuda /usr))
CUDA_INCLUDE ?= $(firstword $(wildcard $(CUDA_HOME)/targets/x86_64-linux/include $(CUDA_HOME)/include))
CUDA_LIB ?= $(firstword $(wildcard $(CUDA_HOME)/targets/x86_64-linux/lib $(CUDA_HOME)/lib64 $(CUDA_HOME)/lib/x86_64-linux-gnu $(CUDA_HOME)/lib))
CUDA_NVCC_FOUND := $(shell command -v $(NVCC) >/dev/null 2>&1 && echo yes)

ifeq ($(KEYSTONE_ENABLE_CUDA),1)
    ifeq ($(CUDA_NVCC_FOUND),yes)
        CUDA_CFLAGS := -DKEYSTONE_ENABLE_CUDA $(if $(CUDA_INCLUDE),-I$(CUDA_INCLUDE))
        CUDA_LDFLAGS := -L./cuda -lkeystone_cuda -Wl,-rpath,'$$ORIGIN/cuda' -Wl,-rpath,'$$ORIGIN/../cuda'
        ifneq ($(CUDA_LIB),)
            CUDA_LDFLAGS += -L$(CUDA_LIB) -Wl,-rpath,'$(CUDA_LIB)'
        endif
        CFLAGS  += $(CUDA_CFLAGS)
        LDFLAGS += $(CUDA_LDFLAGS)
    else
        $(error KEYSTONE_ENABLE_CUDA=1 requires the selected NVCC compiler)
    endif
else ifneq ($(KEYSTONE_ENABLE_CUDA),0)
    ifeq ($(CUDA_NVCC_FOUND),yes)
        CUDA_CFLAGS := -DKEYSTONE_ENABLE_CUDA $(if $(CUDA_INCLUDE),-I$(CUDA_INCLUDE))
        CUDA_LDFLAGS := -L./cuda -lkeystone_cuda -Wl,-rpath,'$$ORIGIN/cuda' -Wl,-rpath,'$$ORIGIN/../cuda'
        ifneq ($(CUDA_LIB),)
            CUDA_LDFLAGS += -L$(CUDA_LIB) -Wl,-rpath,'$(CUDA_LIB)'
        endif
        CFLAGS  += $(CUDA_CFLAGS)
        LDFLAGS += $(CUDA_LDFLAGS)
    endif
endif

# Optional QIHSE Unified Wire Protocol Bridge. The bridge source is compiled in
# unconditionally below: when KEYSTONE_ENABLE_QIHSE_BRIDGE is absent it builds
# fail-closed stubs, and when enabled these flags link the real QIHSE backend.
ifeq ($(KEYSTONE_ENABLE_QIHSE_BRIDGE),1)
    ifndef QIHSE_ROOT
        $(error KEYSTONE_ENABLE_QIHSE_BRIDGE=1 requires QIHSE_ROOT to be set (e.g. QIHSE_ROOT=/path/to/QIHSE))
    endif
    QIHSE_CFLAGS := -DKEYSTONE_ENABLE_QIHSE_BRIDGE -I"$(QIHSE_ROOT)/include"
    QIHSE_LDFLAGS := -L"$(QIHSE_ROOT)" -L"$(QIHSE_ROOT)/lib" -lqihse -Wl,-rpath,"$(QIHSE_ROOT)" -Wl,-rpath,"$(QIHSE_ROOT)/lib"
    CFLAGS += $(QIHSE_CFLAGS)
    LDFLAGS += $(QIHSE_LDFLAGS)
endif

# CPU ISA feature flags. Auto builds target the build host; explicit profiles
# produce binaries for a chosen ISA level. Rebuild after changing profiles.
HOST_CPU_FLAGS ?= $(shell awk -F: '/^(flags|Features)[[:space:]]*:/{sub(/^ /, "", $$2); print $$2; exit}' /proc/cpuinfo 2>/dev/null)
KEYSTONE_TARGET_ARCH := $(firstword $(subst -, ,$(shell $(CC) -dumpmachine)))
KEYSTONE_TARGET_FAMILY := $(if $(filter x86_64 i386 i486 i586 i686,$(KEYSTONE_TARGET_ARCH)),x86,$(if $(filter aarch64,$(KEYSTONE_TARGET_ARCH)),arm,other))
cpu_has = $(if $(filter $(1),$(HOST_CPU_FLAGS)),1,0)
cpu_has_all = $(if $(strip $(foreach f,$(1),$(if $(filter $(f),$(HOST_CPU_FLAGS)),,$(f)))),0,1)
x86_has = $(if $(filter x86,$(KEYSTONE_TARGET_FAMILY)),$(call cpu_has,$(1)),0)
x86_has_all = $(if $(filter x86,$(KEYSTONE_TARGET_FAMILY)),$(call cpu_has_all,$(1)),0)
arm_has = $(if $(filter arm,$(KEYSTONE_TARGET_FAMILY)),$(call cpu_has,$(1)),0)
optional_x86_has = $(if $(filter auto native,$(KEYSTONE_ISA)),$(call x86_has_all,$(1)),0)
KEYSTONE_ISA ?= auto

ifeq ($(KEYSTONE_FORCE_SCALAR),1)
    override KEYSTONE_ISA := scalar
endif

ifeq ($(KEYSTONE_ISA),native)
    ifeq ($(strip $(MARCH)),)
        CFLAGS += -march=native
    endif
    KEYSTONE_ENABLE_SSE41  ?= $(call x86_has,sse4_1)
    KEYSTONE_ENABLE_SSE42  ?= $(call x86_has_all,sse4_1 sse4_2)
    KEYSTONE_ENABLE_AVX    ?= $(call x86_has_all,avx)
    KEYSTONE_ENABLE_AVX2   ?= $(call x86_has_all,avx2 fma)
    KEYSTONE_ENABLE_AVX512 ?= $(call x86_has_all,avx512f avx512dq avx512bw avx512vl fma)
else ifeq ($(KEYSTONE_ISA),auto)
    KEYSTONE_ENABLE_SSE41  ?= $(call x86_has,sse4_1)
    # The SSE4.2 trigram query path regresses on Ivy Bridge; leave it opt-in
    # until a faster implementation is demonstrated on this workload.
    KEYSTONE_ENABLE_SSE42  ?= 0
    KEYSTONE_ENABLE_AVX    ?= $(call x86_has_all,avx)
    KEYSTONE_ENABLE_AVX2   ?= $(call x86_has_all,avx2 fma)
    KEYSTONE_ENABLE_AVX512 ?= $(call x86_has_all,avx512f avx512dq avx512bw avx512vl fma)
else ifeq ($(KEYSTONE_ISA),scalar)
    override KEYSTONE_ENABLE_SSE41 := 0
    override KEYSTONE_ENABLE_SSE42 := 0
    override KEYSTONE_ENABLE_AVX := 0
    override KEYSTONE_ENABLE_AVX2 := 0
    override KEYSTONE_ENABLE_AVX512 := 0
else ifeq ($(KEYSTONE_ISA),avx)
    override KEYSTONE_ENABLE_SSE41 := 1
    KEYSTONE_ENABLE_SSE42 ?= 0
    override KEYSTONE_ENABLE_AVX := 1
    override KEYSTONE_ENABLE_AVX2 := 0
    override KEYSTONE_ENABLE_AVX512 := 0
else ifeq ($(KEYSTONE_ISA),avx2)
    override KEYSTONE_ENABLE_SSE41 := 1
    override KEYSTONE_ENABLE_SSE42 := 1
    override KEYSTONE_ENABLE_AVX := 1
    override KEYSTONE_ENABLE_AVX2 := 1
    override KEYSTONE_ENABLE_AVX512 := 0
else ifeq ($(KEYSTONE_ISA),avx512)
    override KEYSTONE_ENABLE_SSE41 := 1
    override KEYSTONE_ENABLE_SSE42 := 1
    override KEYSTONE_ENABLE_AVX := 1
    override KEYSTONE_ENABLE_AVX2 := 1
    override KEYSTONE_ENABLE_AVX512 := 1
else ifeq ($(KEYSTONE_ISA),avx_vnni)
    override KEYSTONE_ENABLE_SSE41 := 1
    override KEYSTONE_ENABLE_SSE42 := 1
    override KEYSTONE_ENABLE_AVX := 1
    override KEYSTONE_ENABLE_AVX2 := 1
    override KEYSTONE_ENABLE_AVX512 := 0
    override KEYSTONE_ENABLE_AVX_VNNI := 1
else ifeq ($(KEYSTONE_ISA),amx)
    override KEYSTONE_ENABLE_SSE41 := 1
    override KEYSTONE_ENABLE_SSE42 := 1
    override KEYSTONE_ENABLE_AVX := 1
    override KEYSTONE_ENABLE_AVX2 := 1
    override KEYSTONE_ENABLE_AVX512 := 1
    override KEYSTONE_ENABLE_AMX := 1
else ifeq ($(KEYSTONE_ISA),neon)
    override KEYSTONE_ENABLE_SSE41 := 0
    override KEYSTONE_ENABLE_SSE42 := 0
    override KEYSTONE_ENABLE_AVX := 0
    override KEYSTONE_ENABLE_AVX2 := 0
    override KEYSTONE_ENABLE_AVX512 := 0
    override KEYSTONE_ENABLE_NEON := 1
    override KEYSTONE_ENABLE_SVE := 0
else ifeq ($(KEYSTONE_ISA),sve)
    override KEYSTONE_ENABLE_SSE41 := 0
    override KEYSTONE_ENABLE_SSE42 := 0
    override KEYSTONE_ENABLE_AVX := 0
    override KEYSTONE_ENABLE_AVX2 := 0
    override KEYSTONE_ENABLE_AVX512 := 0
    override KEYSTONE_ENABLE_NEON := 1
    override KEYSTONE_ENABLE_SVE := 1
else
    $(error Unknown KEYSTONE_ISA '$(KEYSTONE_ISA)'; choose auto, scalar, avx, avx2, avx512, avx_vnni, amx, neon, sve, or native)
endif

# QIHSE's remaining x86 feature groups are independent opt-ins. In auto and
# native modes they follow the host; in a named profile they default off.
KEYSTONE_ENABLE_AVX_VNNI ?= $(if $(filter 1,$(KEYSTONE_ENABLE_AVX2)),$(call optional_x86_has,avx_vnni),0)
KEYSTONE_ENABLE_AMX      ?= $(call optional_x86_has,amx_tile amx_int8 amx_bf16)
KEYSTONE_ENABLE_F16C     ?= $(if $(filter 1,$(KEYSTONE_ENABLE_AVX)),$(call optional_x86_has,f16c),0)
KEYSTONE_ENABLE_AESNI    ?= $(call optional_x86_has,aes)
KEYSTONE_ENABLE_PCLMUL   ?= $(call optional_x86_has,pclmulqdq)
KEYSTONE_ENABLE_POPCNT   ?= $(call optional_x86_has,popcnt)
KEYSTONE_ENABLE_RDRAND   ?= $(call optional_x86_has,rdrand)
KEYSTONE_ENABLE_FSGSBASE ?= $(call optional_x86_has,fsgsbase)
KEYSTONE_ENABLE_ERMS     ?= $(call optional_x86_has,erms)
KEYSTONE_ENABLE_FMA      ?= $(call optional_x86_has,fma)
KEYSTONE_ENABLE_BMI1     ?= $(call optional_x86_has,bmi1)
KEYSTONE_ENABLE_BMI2     ?= $(call optional_x86_has,bmi2)
KEYSTONE_ENABLE_MOVBE    ?= $(call optional_x86_has,movbe)
KEYSTONE_ENABLE_RDSEED   ?= $(call optional_x86_has,rdseed)
KEYSTONE_ENABLE_ADX      ?= $(call optional_x86_has,adx)
KEYSTONE_ENABLE_RTM      ?= $(call optional_x86_has,rtm)
KEYSTONE_ENABLE_ABM      ?= $(call optional_x86_has,abm)
KEYSTONE_ENABLE_NEON     ?= $(if $(filter auto native,$(KEYSTONE_ISA)),$(call arm_has,asimd),0)
KEYSTONE_ENABLE_SVE      ?= $(if $(filter auto native,$(KEYSTONE_ISA)),$(call arm_has,sve),0)

KEYSTONE_X86_FEATURES := SSE41 SSE42 AVX AVX2 AVX512 AVX_VNNI AMX F16C AESNI PCLMUL POPCNT RDRAND FSGSBASE ERMS FMA BMI1 BMI2 MOVBE RDSEED ADX RTM ABM
ifeq ($(KEYSTONE_ISA),scalar)
    $(foreach feature,$(KEYSTONE_X86_FEATURES),$(eval override KEYSTONE_ENABLE_$(feature) := 0))
    override KEYSTONE_ENABLE_NEON := 0
    override KEYSTONE_ENABLE_SVE := 0
endif
KEYSTONE_ENABLED_X86 := $(strip $(foreach feature,$(KEYSTONE_X86_FEATURES),$(if $(filter 1,$(KEYSTONE_ENABLE_$(feature))),$(feature))))
ifneq ($(KEYSTONE_ENABLED_X86),)
    ifneq ($(KEYSTONE_TARGET_FAMILY),x86)
        $(error x86 ISA flags $(KEYSTONE_ENABLED_X86) require an x86 compiler)
    endif
endif
ifneq ($(filter 1,$(KEYSTONE_ENABLE_NEON) $(KEYSTONE_ENABLE_SVE)),)
    ifneq ($(KEYSTONE_TARGET_FAMILY),arm)
        $(error NEON/SVE flags require an AArch64 compiler)
    endif
endif
ifeq ($(KEYSTONE_ENABLE_AVX_VNNI),1)
    ifneq ($(KEYSTONE_ENABLE_AVX2),1)
        $(error KEYSTONE_ENABLE_AVX_VNNI=1 requires KEYSTONE_ENABLE_AVX2=1)
    endif
endif
ifeq ($(KEYSTONE_ENABLE_F16C),1)
    ifneq ($(KEYSTONE_ENABLE_AVX),1)
        $(error KEYSTONE_ENABLE_F16C=1 requires KEYSTONE_ENABLE_AVX=1)
    endif
endif
ifeq ($(KEYSTONE_ENABLE_SVE),1)
    ifneq ($(KEYSTONE_ENABLE_NEON),1)
        $(error KEYSTONE_ENABLE_SVE=1 requires KEYSTONE_ENABLE_NEON=1)
    endif
endif
ifeq ($(KEYSTONE_ENABLE_SSE42),1)
    ifneq ($(KEYSTONE_ENABLE_SSE41),1)
        $(error KEYSTONE_ENABLE_SSE42=1 requires KEYSTONE_ENABLE_SSE41=1)
    endif
endif

# Keep each group complete. AVX-512 and AMX instructions live in dedicated
# objects; common code only receives a build marker for guarded dispatch.
ifeq ($(KEYSTONE_ENABLE_SSE41),1)
    CFLAGS += -msse4.1
endif
ifeq ($(KEYSTONE_ENABLE_SSE42),1)
    CFLAGS += -msse4.2
endif
ifeq ($(KEYSTONE_ENABLE_AVX),1)
    CFLAGS += -mavx
endif
ifeq ($(KEYSTONE_ENABLE_AVX2),1)
    CFLAGS += -mavx2 -mfma
endif
ifeq ($(KEYSTONE_ENABLE_AVX512),1)
    CFLAGS += -DKEYSTONE_BUILD_AVX512=1
    AVX512_CFLAGS := -mavx512f -mavx512dq -mavx512bw -mavx512vl
endif
ifeq ($(KEYSTONE_ENABLE_AVX_VNNI),1)
    CFLAGS += -mavxvnni
endif
ifeq ($(KEYSTONE_ENABLE_AMX),1)
    AMX_CFLAGS := -mamx-tile -mamx-int8 -mamx-bf16
endif
ifeq ($(KEYSTONE_ENABLE_F16C),1)
    CFLAGS += -mf16c
endif
ifeq ($(KEYSTONE_ENABLE_AESNI),1)
    CFLAGS += -maes
endif
ifeq ($(KEYSTONE_ENABLE_PCLMUL),1)
    CFLAGS += -mpclmul
endif
ifeq ($(KEYSTONE_ENABLE_POPCNT),1)
    CFLAGS += -mpopcnt
endif
ifeq ($(KEYSTONE_ENABLE_RDRAND),1)
    CFLAGS += -mrdrnd
endif
ifeq ($(KEYSTONE_ENABLE_FSGSBASE),1)
    CFLAGS += -mfsgsbase
endif
ifeq ($(KEYSTONE_ENABLE_ERMS),1)
    CFLAGS += -DKEYSTONE_ENABLE_ERMS=1
endif
ifeq ($(KEYSTONE_ENABLE_FMA),1)
    ifneq ($(KEYSTONE_ENABLE_AVX2),1)
        CFLAGS += -mfma
    endif
endif
ifeq ($(KEYSTONE_ENABLE_BMI1),1)
    CFLAGS += -mbmi
endif
ifeq ($(KEYSTONE_ENABLE_BMI2),1)
    CFLAGS += -mbmi2
endif
ifeq ($(KEYSTONE_ENABLE_MOVBE),1)
    CFLAGS += -mmovbe
endif
ifeq ($(KEYSTONE_ENABLE_RDSEED),1)
    CFLAGS += -mrdseed
endif
ifeq ($(KEYSTONE_ENABLE_ADX),1)
    CFLAGS += -madx
endif
ifeq ($(KEYSTONE_ENABLE_RTM),1)
    CFLAGS += -mrtm
endif
ifeq ($(KEYSTONE_ENABLE_ABM),1)
    CFLAGS += -mlzcnt
endif
ifeq ($(KEYSTONE_ENABLE_SVE),1)
    CFLAGS += -march=armv8.2-a+sve
else ifeq ($(KEYSTONE_ENABLE_NEON),1)
    CFLAGS += -march=armv8-a+simd
endif
ifeq ($(KEYSTONE_TARGET_FAMILY),x86)
    # Even MARCH=native or KEYSTONE_ARCH_FLAGS must not spread these ISAs to
    # normal objects. Dedicated rules append their positive flags afterward.
    CFLAGS += -mno-avx512f -mno-avx512dq -mno-avx512bw -mno-avx512vl
    CFLAGS += -mno-amx-tile -mno-amx-int8 -mno-amx-bf16
endif
ifeq ($(KEYSTONE_ISA),scalar)
    ifeq ($(KEYSTONE_TARGET_FAMILY),x86)
        CFLAGS += -mno-avx -mno-avx2
    endif
endif

SRC     := src/keystone.c src/dsmil_keystone_wrapper.c src/dsmil_telemetry_processor.c \
           src/keystone_avx512_runtime.c src/qihse_keystone_bridge.c
ifeq ($(KEYSTONE_ENABLE_AVX512),1)
SRC     += src/keystone_avx512.c src/keystone_avx512_search.c
endif
OBJS    := $(SRC:.c=.o)

TEST_SRC := tests/test_core_native.c tests/test_auto_backend.c \
            tests/test_telemetry_processor_perf.c \
            tests/dsmil_integration_test.c tests/test_performance_fix.c \
            tests/test_trigram_index.c \
            tests/test_memory_ramp.c
TEST_BIN := bin/test_enhanced bin/test_auto_backend \
            bin/test_telemetry_processor_perf bin/test_performance_fix \
            bin/test_core_native bin/test_trigram_index \
            bin/test_memory_ramp

ifneq ($(FORTRAN_OBJ),)
TEST_SRC += tests/test_fortran_backend.c tests/test_fortran_workloads.c
TEST_BIN += bin/test_fortran_backend bin/test_fortran_workloads
endif

ifeq ($(KEYSTONE_ENABLE_TAR_ZST),1)
SRC     += src/keystone_tar_zst.c
TEST_SRC += tests/test_tar_zst.c
TEST_BIN += bin/test_tar_zst
endif

SRC     += src/dsmil_hash_indexer.c src/dsmil_dirty_parser.c src/dsmil_model_bridge.c src/dsmil_micro_model.c src/keystone_trigram.c \
           src/keystone_fabric.c src/federation/keystone_federation_ingest.c \
           src/federation/keystone_exact_index.c src/temporal/keystone_temporal_index.c \
           src/service/keystone_workflow_index.c src/service/keystoned_server.c src/service/keystoned_client.c \
           src/topology/keystone_topology_index.c src/query/keystone_hybrid_planner.c \
           src/telemetry/keystone_telemetry_engine.c src/incident/keystone_incident_engine.c \
           src/federation/keystone_federated_query.c src/federation/keystone_fed_transport.c src/federation/keystone_bootstrap.c src/federation/keystone_manifest.c \
           src/query/keystone_rag_engine.c \
           src/projector/keystone_workflow_projector.c src/projector/keystone_workflow_reader.c

TEST_SRC += tests/test_keystone_fabric.c tests/test_cuda_backend.c tests/test_federation_envelope.c \
            tests/persistence_fuzz/test_persistence_fuzz.c tests/test_federation_transport.c \
            tests/test_bootstrap.c tests/test_manifest.c \
            tests/test_exact_temporal_index.c tests/test_keystoned_service.c \
            tests/test_topology_hybrid_planner.c tests/test_telemetry_incident_engine.c \
            tests/test_federated_query_rag.c tests/test_workflow_index.c \
            tests/test_workflow_projector_reader.c \
            tests/test_workflow_adversarial_gates.c \
            tests/test_workflow_checkpoint_recovery.c
TEST_BIN += bin/test_keystone_fabric bin/test_cuda_backend bin/test_federation_envelope bin/test_persistence_fuzz bin/test_federation_transport bin/test_bootstrap bin/test_manifest \
            bin/test_exact_temporal_index bin/test_keystoned_service \
            bin/test_topology_hybrid_planner bin/test_telemetry_incident_engine \
            bin/test_federated_query_rag bin/test_workflow_index \
            bin/test_workflow_projector_reader \
            bin/test_workflow_adversarial_gates \
            bin/test_workflow_checkpoint_recovery

OBJS    := $(SRC:.c=.o)

BENCH_SRC := benchmarks/dsmil_benchmark.c benchmarks/performance_proof.c benchmarks/trigram_benchmark.c \
             benchmarks/bench_memory_ramp.c benchmarks/bench_federation.c \
             benchmarks/bench_workflow_latency.c
BENCH_BIN := benchmarks/dsmil_benchmark benchmarks/performance_proof benchmarks/trigram_benchmark \
             benchmarks/bench_memory_ramp benchmarks/bench_federation \
             benchmarks/bench_workflow_latency

.PHONY: all lib tests test check run-tests benchmarks clean tgrep keystoned projector-daemon asan valgrind install install-host

all: lib tests benchmarks bin/tgrep bin/keystoned bin/keystone_projector_daemon

install: install-host

install-host:
	@./scripts/install_keystone.sh $(INSTALL_ARGS)

tgrep: bin/tgrep

keystoned: bin/keystoned

projector-daemon: bin/keystone_projector_daemon

lib: libkeystone.so

libkeystone.so: $(OBJS) $(FORTRAN_OBJ)
	$(CC) -shared -fPIC -o $@ $(OBJS) $(FORTRAN_OBJ) $(LDFLAGS)

fortran/keystone_batch.o: fortran/keystone_batch.f90
	mkdir -p fortran
	gfortran -O3 -fPIC -fopenmp -Jfortran -c $< -o $@

tests: $(TEST_BIN)

test: check

check: tests
	@set -e; \
	for test_bin in $(TEST_BIN); do \
		echo "==> $$test_bin"; \
		LD_LIBRARY_PATH=./cuda:$$LD_LIBRARY_PATH ./$$test_bin; \
	done

run-tests: check

# --- Sanitizer matrix (isolated build; never touches in-place objects) ---
# Excluded: Fortran-backed tests (gfortran objects are not instrumented) and
# the CUDA hardware-in-the-loop test (needs the nvcc-built backend; its
# fallback contracts are covered by the native check run).
SAN_TEST_SRC := $(filter-out tests/test_fortran_%.c tests/test_cuda_backend.c,$(TEST_SRC))
SAN_LDFLAGS := -lm -ldl -lpthread $(TAR_ZST_LDFLAGS)

asan valgrind: | bin
	@./scripts/run_sanitizers.sh $(@) "$(CC)" "$(TAR_ZST_CFLAGS)" "$(SAN_LDFLAGS)" $(SRC) $(SAN_TEST_SRC)

benchmarks: $(BENCH_BIN)

bin:
	mkdir -p bin

# Pattern rules
# AVX-512 code is confined to these translation units. The baseline runtime
# wrapper stays in every build and provides checked dispatch/scalar fallback.
src/keystone_avx512.o: src/keystone_avx512.c
	$(CC) $(CFLAGS) $(AVX512_CFLAGS) -c $< -o $@

src/keystone_avx512_search.o: src/keystone_avx512_search.c
	$(CC) $(CFLAGS) $(AVX512_CFLAGS) -c $< -o $@

# The AMX incident kernel has its own runtime OS tile-permission check.
src/incident/keystone_incident_engine.o: CFLAGS += $(AMX_CFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

tests/%.o: tests/%.c
	$(CC) $(CFLAGS) -c $< -o $@

tests/persistence_fuzz/%.o: tests/persistence_fuzz/%.c
	$(CC) $(CFLAGS) -c $< -o $@

benchmarks/%.o: benchmarks/%.c
	$(CC) $(CFLAGS) -c $< -o $@

tools/%.o: tools/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Test binaries
bin/test_enhanced: $(OBJS) $(FORTRAN_OBJ) tests/dsmil_integration_test.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_core_native: $(OBJS) $(FORTRAN_OBJ) tests/test_core_native.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_fortran_backend: $(OBJS) $(FORTRAN_OBJ) tests/test_fortran_backend.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_auto_backend: $(OBJS) $(FORTRAN_OBJ) tests/test_auto_backend.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_telemetry_processor_perf: $(OBJS) $(FORTRAN_OBJ) tests/test_telemetry_processor_perf.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_performance_fix: $(OBJS) $(FORTRAN_OBJ) tests/test_performance_fix.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_trigram_index: $(OBJS) $(FORTRAN_OBJ) tests/test_trigram_index.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_tar_zst: $(OBJS) $(FORTRAN_OBJ) tests/test_tar_zst.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_keystone_fabric: $(OBJS) $(FORTRAN_OBJ) tests/test_keystone_fabric.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_fortran_workloads: $(OBJS) $(FORTRAN_OBJ) tests/test_fortran_workloads.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_memory_ramp: $(OBJS) $(FORTRAN_OBJ) tests/test_memory_ramp.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_cuda_backend: $(OBJS) $(FORTRAN_OBJ) tests/test_cuda_backend.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_federation_envelope: $(OBJS) $(FORTRAN_OBJ) tests/test_federation_envelope.o | bin
	$(CC) -o $@ $^ $(LDFLAGS) -pthread

bin/test_exact_temporal_index: $(OBJS) $(FORTRAN_OBJ) tests/test_exact_temporal_index.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_keystoned_service: $(OBJS) $(FORTRAN_OBJ) tests/test_keystoned_service.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_workflow_index: $(OBJS) $(FORTRAN_OBJ) tests/test_workflow_index.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_topology_hybrid_planner: $(OBJS) $(FORTRAN_OBJ) tests/test_topology_hybrid_planner.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_telemetry_incident_engine: $(OBJS) $(FORTRAN_OBJ) tests/test_telemetry_incident_engine.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_federated_query_rag: $(OBJS) $(FORTRAN_OBJ) tests/test_federated_query_rag.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_persistence_fuzz: $(OBJS) $(FORTRAN_OBJ) tests/persistence_fuzz/test_persistence_fuzz.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_federation_transport: $(OBJS) $(FORTRAN_OBJ) tests/test_federation_transport.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_bootstrap: $(OBJS) $(FORTRAN_OBJ) tests/test_bootstrap.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_manifest: $(OBJS) $(FORTRAN_OBJ) tests/test_manifest.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_workflow_projector_reader: $(OBJS) $(FORTRAN_OBJ) tests/test_workflow_projector_reader.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_workflow_adversarial_gates: $(OBJS) $(FORTRAN_OBJ) tests/test_workflow_adversarial_gates.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_workflow_checkpoint_recovery: $(OBJS) $(FORTRAN_OBJ) tests/test_workflow_checkpoint_recovery.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

# Benchmark binaries
benchmarks/dsmil_benchmark: $(OBJS) $(FORTRAN_OBJ) benchmarks/dsmil_benchmark.o benchmarks/benchmark_writer.o
	$(CC) -o $@ $^ $(LDFLAGS)

benchmarks/performance_proof: $(OBJS) $(FORTRAN_OBJ) benchmarks/performance_proof.o benchmarks/benchmark_writer.o
	$(CC) -o $@ $^ $(LDFLAGS)

benchmarks/trigram_benchmark: $(OBJS) $(FORTRAN_OBJ) benchmarks/trigram_benchmark.o
	$(CC) -o $@ $^ $(LDFLAGS)

benchmarks/bench_memory_ramp: $(OBJS) $(FORTRAN_OBJ) benchmarks/bench_memory_ramp.o benchmarks/benchmark_writer.o
	$(CC) -o $@ $^ $(LDFLAGS)

benchmarks/bench_federation: $(OBJS) $(FORTRAN_OBJ) benchmarks/bench_federation.o benchmarks/benchmark_writer.o
	$(CC) -o $@ $^ $(LDFLAGS)

benchmarks/bench_workflow_latency: $(OBJS) $(FORTRAN_OBJ) benchmarks/bench_workflow_latency.o benchmarks/benchmark_writer.o
	$(CC) -o $@ $^ $(LDFLAGS)

# Standalone CLI tools
bin/tgrep: $(OBJS) $(FORTRAN_OBJ) src/tgrep.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/keystoned: $(OBJS) $(FORTRAN_OBJ) src/service/keystoned_main.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/keystone_projector_daemon: $(OBJS) $(FORTRAN_OBJ) tools/keystone_projector_daemon.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

scripts/compare_search_auto: $(OBJS) $(FORTRAN_OBJ) scripts/compare_search.c
	$(CC) $(CFLAGS) -DKEYSTONE_BENCH_AUTO=1 -c scripts/compare_search.c -o scripts/compare_search_auto.o
	$(CC) -o $@ scripts/compare_search_auto.o $(OBJS) $(FORTRAN_OBJ) $(LDFLAGS)

# Fortran backend (optional) — compiled into libkeystone.so directly
FORTRAN_ENABLED := no
ifeq ($(KEYSTONE_ENABLE_FORTRAN),1)
FORTRAN_ENABLED := yes
else ifneq ($(KEYSTONE_ENABLE_FORTRAN),0)
ifeq ($(shell command -v gfortran >/dev/null 2>&1 && echo yes),yes)
FORTRAN_ENABLED := yes
endif
endif

# CUDA backend (optional)
CUDA_HOME ?= $(firstword $(wildcard /usr/local/cuda /usr/local/cuda-13.3 /usr/local/cuda-12.8 /usr/local/cuda-12.4 /opt/cuda /usr))
CUDA_INCLUDE ?= $(firstword $(wildcard $(CUDA_HOME)/targets/x86_64-linux/include $(CUDA_HOME)/include))
cuda/libkeystone_cuda.so: cuda/keystone_cuda.cu
	mkdir -p cuda
	$(NVCC) -O3 --std=c++17 -U_GNU_SOURCE $(CUDA_ARCH_FLAGS) $(if $(CUDA_INCLUDE),-I$(CUDA_INCLUDE)) --compiler-options '-fPIC' -shared -Xlinker -soname,libkeystone_cuda.so $< -o $@

CUDA_ENABLED := no
ifeq ($(KEYSTONE_ENABLE_CUDA),1)
CUDA_ENABLED := yes
else ifneq ($(KEYSTONE_ENABLE_CUDA),0)
ifeq ($(CUDA_NVCC_FOUND),yes)
CUDA_ENABLED := yes
endif
endif

ifeq ($(CUDA_ENABLED),yes)
all: cuda/libkeystone_cuda.so
libkeystone.so $(TEST_BIN) $(BENCH_BIN) bin/tgrep bin/keystone_projector_daemon: cuda/libkeystone_cuda.so | bin
endif

clean:
	rm -f $(OBJS) src/keystone_avx512.o src/keystone_avx512_search.o tests/*.o benchmarks/*.o tools/*.o src/projector/*.o libkeystone.so $(BENCH_BIN)
	rm -rf bin
	rm -f scripts/compare_search_auto
	rm -f fortran/keystone_batch.o fortran/*.mod fortran/libkeystone_batch.so
	rm -f cuda/libkeystone_cuda.so
