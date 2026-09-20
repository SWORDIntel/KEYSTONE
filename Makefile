# KEYSTONE Makefile
# Standard make / make test workflow

CC      := gcc
MARCH   ?= native
CFLAGS  := -O3 -march=$(MARCH) -fPIC -Wall -Wextra -Werror=implicit-function-declaration -I./include
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
CUDA_HOME ?= $(firstword $(wildcard /usr/local/cuda /usr/local/cuda-13.3 /usr/local/cuda-12.8 /usr/local/cuda-12.4 /opt/cuda))
CUDA_INCLUDE ?= $(firstword $(wildcard $(CUDA_HOME)/targets/x86_64-linux/include $(CUDA_HOME)/include))
CUDA_LIB ?= $(firstword $(wildcard $(CUDA_HOME)/targets/x86_64-linux/lib $(CUDA_HOME)/lib64 $(CUDA_HOME)/lib))

ifeq ($(KEYSTONE_ENABLE_CUDA),1)
    ifeq ($(shell command -v nvcc >/dev/null 2>&1 && echo yes),yes)
        CUDA_CFLAGS := -DKEYSTONE_ENABLE_CUDA $(if $(CUDA_INCLUDE),-I$(CUDA_INCLUDE))
        CUDA_LDFLAGS := -L./cuda -lkeystone_cuda -Wl,-rpath,'$$ORIGIN/cuda' -Wl,-rpath,'$$ORIGIN/../cuda'
        ifneq ($(CUDA_LIB),)
            CUDA_LDFLAGS += -L$(CUDA_LIB) -Wl,-rpath,'$(CUDA_LIB)'
        endif
        CFLAGS  += $(CUDA_CFLAGS)
        LDFLAGS += $(CUDA_LDFLAGS)
    else
        $(error KEYSTONE_ENABLE_CUDA=1 requires nvcc)
    endif
else ifneq ($(KEYSTONE_ENABLE_CUDA),0)
    ifeq ($(shell command -v nvcc >/dev/null 2>&1 && echo yes),yes)
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
    QIHSE_LDFLAGS := -L"$(QIHSE_ROOT)" -lqihse -Wl,-rpath,"$(QIHSE_ROOT)"
    CFLAGS += $(QIHSE_CFLAGS)
    LDFLAGS += $(QIHSE_LDFLAGS)
endif

# CPU ISA feature flags
HOST_CPU_FLAGS ?= $(shell awk -F: '/^flags/{sub(/^ /, "", $$2); print $$2; exit}' /proc/cpuinfo 2>/dev/null)
cpu_has = $(if $(filter $(1),$(HOST_CPU_FLAGS)),1,0)

KEYSTONE_ENABLE_AVX2   ?= $(call cpu_has,avx2)
KEYSTONE_ENABLE_AVX512 ?= $(if $(and $(filter avx512f,$(HOST_CPU_FLAGS)),$(filter avx512dq,$(HOST_CPU_FLAGS))),1,0)

# SIMD detection
ifeq ($(KEYSTONE_FORCE_SCALAR),1)
    CFLAGS += -mno-avx2 -mno-avx512f
else
    ifeq ($(KEYSTONE_ENABLE_AVX2),1)
        CFLAGS += -mavx2
    endif
    ifeq ($(KEYSTONE_ENABLE_AVX512),1)
        CFLAGS += -mavx512f -mavx512dq
    endif
endif

SRC     := src/keystone.c src/dsmil_keystone_wrapper.c src/dsmil_telemetry_processor.c \
           src/keystone_avx512.c \
           src/keystone_avx512_search.c src/qihse_keystone_bridge.c
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
           src/service/keystoned_server.c src/service/keystoned_client.c \
           src/topology/keystone_topology_index.c src/query/keystone_hybrid_planner.c \
           src/telemetry/keystone_telemetry_engine.c src/incident/keystone_incident_engine.c \
           src/federation/keystone_federated_query.c src/query/keystone_rag_engine.c

TEST_SRC += tests/test_keystone_fabric.c tests/test_cuda_backend.c tests/test_federation_envelope.c \
            tests/test_exact_temporal_index.c tests/test_keystoned_service.c \
            tests/test_topology_hybrid_planner.c tests/test_telemetry_incident_engine.c \
            tests/test_federated_query_rag.c
TEST_BIN += bin/test_keystone_fabric bin/test_cuda_backend bin/test_federation_envelope \
            bin/test_exact_temporal_index bin/test_keystoned_service \
            bin/test_topology_hybrid_planner bin/test_telemetry_incident_engine \
            bin/test_federated_query_rag

OBJS    := $(SRC:.c=.o)

BENCH_SRC := benchmarks/dsmil_benchmark.c benchmarks/performance_proof.c benchmarks/trigram_benchmark.c \
             benchmarks/bench_memory_ramp.c
BENCH_BIN := benchmarks/dsmil_benchmark benchmarks/performance_proof benchmarks/trigram_benchmark \
             benchmarks/bench_memory_ramp

.PHONY: all lib tests test check run-tests benchmarks clean tgrep keystoned

all: lib tests benchmarks bin/tgrep bin/keystoned

tgrep: bin/tgrep

keystoned: bin/keystoned

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

benchmarks: $(BENCH_BIN)

bin:
	mkdir -p bin

# Pattern rules
# Explicit rule for AVX-512 objects to isolate experimental code
src/keystone_avx512.o: src/keystone_avx512.c
	$(if $(filter 1,$(KEYSTONE_ENABLE_AVX512)),$(CC) $(CFLAGS) -mavx512f -mavx512dq -c $< -o $@,$(CC) $(CFLAGS) -mno-avx512f -c $< -o $@)

src/keystone_avx512_search.o: src/keystone_avx512_search.c
	$(if $(filter 1,$(KEYSTONE_ENABLE_AVX512)),$(CC) $(CFLAGS) -mavx512f -mavx512dq -c $< -o $@,$(CC) $(CFLAGS) -mno-avx512f -c $< -o $@)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

tests/%.o: tests/%.c
	$(CC) $(CFLAGS) -c $< -o $@

benchmarks/%.o: benchmarks/%.c
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
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_exact_temporal_index: $(OBJS) $(FORTRAN_OBJ) tests/test_exact_temporal_index.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_keystoned_service: $(OBJS) $(FORTRAN_OBJ) tests/test_keystoned_service.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_topology_hybrid_planner: $(OBJS) $(FORTRAN_OBJ) tests/test_topology_hybrid_planner.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_telemetry_incident_engine: $(OBJS) $(FORTRAN_OBJ) tests/test_telemetry_incident_engine.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_federated_query_rag: $(OBJS) $(FORTRAN_OBJ) tests/test_federated_query_rag.o | bin
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

# Standalone CLI tools
bin/tgrep: $(OBJS) $(FORTRAN_OBJ) src/tgrep.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/keystoned: $(OBJS) $(FORTRAN_OBJ) src/service/keystoned_main.o | bin
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
CUDA_HOME ?= $(firstword $(wildcard /usr/local/cuda /usr/local/cuda-13.3 /opt/cuda))
CUDA_INCLUDE ?= $(firstword $(wildcard $(CUDA_HOME)/targets/x86_64-linux/include $(CUDA_HOME)/include))
cuda/libkeystone_cuda.so: cuda/keystone_cuda.cu
	mkdir -p cuda
	nvcc -O3 --std=c++17 -U_GNU_SOURCE $(if $(CUDA_INCLUDE),-I$(CUDA_INCLUDE)) --compiler-options '-fPIC' -shared -Xlinker -soname,libkeystone_cuda.so $< -o $@

CUDA_ENABLED := no
ifeq ($(KEYSTONE_ENABLE_CUDA),1)
CUDA_ENABLED := yes
else ifneq ($(KEYSTONE_ENABLE_CUDA),0)
ifeq ($(shell command -v nvcc >/dev/null 2>&1 && echo yes),yes)
CUDA_ENABLED := yes
endif
endif

ifeq ($(CUDA_ENABLED),yes)
all: cuda/libkeystone_cuda.so
libkeystone.so $(TEST_BIN) $(BENCH_BIN) bin/tgrep: cuda/libkeystone_cuda.so | bin
endif

clean:
	rm -f $(OBJS) tests/*.o benchmarks/*.o libkeystone.so $(BENCH_BIN)
	rm -rf bin
	rm -f scripts/compare_search_auto
	rm -f fortran/keystone_batch.o fortran/*.mod fortran/libkeystone_batch.so
	rm -f cuda/libkeystone_cuda.so
