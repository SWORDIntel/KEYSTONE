# Optional Fortran Batch Backend

This directory contains an experimental Fortran batch-search backend. It is
wired into the C API as an optional build-time backend; the default build stays
C-only.

The integration roadmap is tracked in `../docs/FORTRAN_BACKEND_PLAN.md`.

The Fortran implementation exports one C ABI entry point:

```c
void keystone_batch_search_i64(
    const int64_t *data,
    size_t n,
    const int64_t *keys,
    size_t key_count,
    int64_t *out_indices);
```

Behavior:

- `data` must point to a sorted `int64_t` array.
- `keys` contains `key_count` lookup values.
- `out_indices` receives one zero-based index per key.
- A missing key is written as `-1`.
- Null input pointers or an empty dataset produce `-1` results where possible.

Example build:

```bash
gfortran -O3 -shared -fPIC -fopenmp -Jfortran \
  fortran/keystone_batch.f90 \
  -o fortran/libkeystone_batch.so
```

Build the full project with the Fortran adapter enabled:

```bash
KEYSTONE_ENABLE_FORTRAN=1 ./scripts/build_native.sh
./test_fortran_backend
```

The public adapter is:

```c
int keystone_fortran_backend_available(void);

size_t keystone_search_batch_fortran(
    const int64_t *arr,
    size_t n,
    keystone_batch_item_t *items,
    size_t num_items);
```

Omit `-fopenmp` for a single-threaded shared-library build; OpenMP directives
are comments when OpenMP is disabled. Benchmark integration is still pending.
