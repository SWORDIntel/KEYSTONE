#!/bin/bash

echo "Building NOT_STISLA for native x86_64 architecture..."

# Clean up previous build artifacts
rm -f *.o test_enhanced benchmarks/dsmil_benchmark benchmarks/performance_proof

# Compile source files for test_enhanced
echo "Compiling src/not_stisla.c..."
gcc -O3 -march=native -mavx2 -mavx512f -mavx512dq -Wall -Wextra -Werror=implicit-function-declaration -I./include -c src/not_stisla.c || { echo "not_stisla.c compilation failed"; exit 1; }

echo "Compiling src/dsmil_not_stisla_wrapper.c..."
gcc -O3 -march=native -mavx2 -Wall -Wextra -I./include -c src/dsmil_not_stisla_wrapper.c || { echo "dsmil_not_stisla_wrapper.c compilation failed"; exit 1; }

echo "Compiling src/dsmil_telemetry_processor.c..."
gcc -O3 -march=native -mavx2 -Wall -Wextra -I./include -c src/dsmil_telemetry_processor.c || { echo "dsmil_telemetry_processor.c compilation failed"; exit 1; }

echo "Compiling tests/dsmil_integration_test.c..."
gcc -O3 -march=native -mavx2 -Wall -Wextra -I./include -c tests/dsmil_integration_test.c || { echo "dsmil_integration_test.c compilation failed"; exit 1; }

# Link test_enhanced
echo "Linking test_enhanced..."
gcc -o test_enhanced not_stisla.o dsmil_not_stisla_wrapper.o dsmil_telemetry_processor.o dsmil_integration_test.o -lm || { echo "test_enhanced linking failed"; exit 1; }

# Compile source files for benchmarks
echo "Compiling benchmarks/dsmil_benchmark.c..."
gcc -O3 -march=native -mavx2 -Wall -Wextra -I./include -c benchmarks/dsmil_benchmark.c || { echo "dsmil_benchmark.c compilation failed"; exit 1; }

echo "Compiling benchmarks/performance_proof.c..."
gcc -O3 -march=native -mavx2 -Wall -Wextra -I./include -c benchmarks/performance_proof.c || { echo "performance_proof.c compilation failed"; exit 1; }

# Link benchmarks
echo "Linking benchmarks/dsmil_benchmark..."
gcc -o benchmarks/dsmil_benchmark dsmil_benchmark.o not_stisla.o dsmil_not_stisla_wrapper.o dsmil_telemetry_processor.o -lm || { echo "dsmil_benchmark linking failed"; exit 1; }

echo "Linking benchmarks/performance_proof..."
gcc -o benchmarks/performance_proof performance_proof.o not_stisla.o dsmil_not_stisla_wrapper.o dsmil_telemetry_processor.o -lm || { echo "performance_proof linking failed"; exit 1; }

echo "Native x86_64 build completed successfully!"

# Clean up .o files (optional, but good practice for a clean build script)
rm -f *.o
