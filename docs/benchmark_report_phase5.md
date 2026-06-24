DSMIL KEYSTONE Search Benchmark Suite
KEYSTONE Version: 1.1.0
Build: Tuned KEYSTONE search with runtime CPU detection and bounded anchor memory

Host Capabilities Detected:
   - AVX2 Support:    YES
   - AVX-512 Support: NO
   - AMX Support:     NO (detection only, not actively routed)

Warming up search paths...
🔬 Comprehensive Algorithm Comparison
=====================================
Algorithm                     | Time/op | Found | Speedup vs Baseline
------------------------------|---------|-------|--------------------
Baseline Binary Search        |  219.9 ns| 50000 | 1.00x
KEYSTONE Core Search        |   85.0 ns| 50000 | 2.59x
Tuned/Enhanced KEYSTONE     |  227.5 ns| 50000 | 0.97x

Tuned/Enhanced Search Details:
Total searches:       50000
Successful searches:  50000
Average search time:  137.32 ns
Reported speedup:     use measured table above

Tuned/Enhanced KEYSTONE Search Benchmark
==========================================
Tuned Search:      224.25 ns/op (50000/50000 found, 100.0% success)
Tuned Stats:       50000 searches, 135.49 ns avg

Auto Backend Batch Search Benchmark (random query profile)
========================================
Auto Batch Search: 100.75 ns/key (50000/50000 found)
Backend Decision:  backend=scalar source=fast_path shape=random threads=1
Calibration:       median=96.44 ns/key p95=96.44 ns/key runs=0 candidates=0
Decision Buckets:  cpu_features=0x00000001 array=131072 queries=65536
host,compiler,cpu_features,profile,n,queries,hit_rate,gap,jitter,stride,threads,backend,source,shape,calibration_runs,candidates,throughput_gib_s,median_ns,p95_ns,rss_kb,page_faults
730xd,gcc,0x00000001,random query profile,131072,65536,-1,-1,-1,-1,1,scalar,fast_path,random,0,0,0.000,96.44,96.44,5436,10

Auto Backend Batch Search Benchmark (dense sorted profile)
========================================
Auto Batch Search: 251.12 ns/key (8192/8192 found)
Backend Decision:  backend=fortran source=measured shape=dense_sorted threads=1
Calibration:       median=13.46 ns/key p95=26.09 ns/key runs=6 candidates=2
Decision Buckets:  cpu_features=0x00000001 array=131072 queries=8192
host,compiler,cpu_features,profile,n,queries,hit_rate,gap,jitter,stride,threads,backend,source,shape,calibration_runs,candidates,throughput_gib_s,median_ns,p95_ns,rss_kb,page_faults
730xd,gcc,0x00000001,dense sorted profile,131072,8192,-1,-1,-1,-1,1,fortran,measured,dense_sorted,6,2,0.000,13.46,26.09,5436,10

Auto Backend Batch Search Benchmark (sparse sorted profile)
========================================
Auto Batch Search: 319.67 ns/key (8192/8192 found)
Backend Decision:  backend=scalar source=fast_path shape=sparse_sorted threads=1
Calibration:       median=316.79 ns/key p95=316.79 ns/key runs=0 candidates=0
Decision Buckets:  cpu_features=0x00000001 array=131072 queries=8192
host,compiler,cpu_features,profile,n,queries,hit_rate,gap,jitter,stride,threads,backend,source,shape,calibration_runs,candidates,throughput_gib_s,median_ns,p95_ns,rss_kb,page_faults
730xd,gcc,0x00000001,sparse sorted profile,131072,8192,-1,-1,-1,-1,1,scalar,fast_path,sparse_sorted,0,0,0.000,316.79,316.79,5436,10

Auto Backend Batch Search Benchmark (strided profile)
========================================
Auto Batch Search: 240.89 ns/key (4096/8192 found)
Backend Decision:  backend=scalar source=fast_path shape=sparse_sorted threads=1
Calibration:       median=238.14 ns/key p95=238.14 ns/key runs=0 candidates=0
Decision Buckets:  cpu_features=0x00000001 array=131072 queries=8192
host,compiler,cpu_features,profile,n,queries,hit_rate,gap,jitter,stride,threads,backend,source,shape,calibration_runs,candidates,throughput_gib_s,median_ns,p95_ns,rss_kb,page_faults
730xd,gcc,0x00000001,strided profile,131072,8192,-1,-1,-1,-1,1,scalar,fast_path,sparse_sorted,0,0,0.000,238.14,238.14,5436,10

Auto Backend Batch Search Benchmark (mixed hit-rate profile)
========================================
Auto Batch Search: 164.69 ns/key (24959/50000 found)
Backend Decision:  backend=scalar source=fast_path shape=random threads=1
Calibration:       median=161.85 ns/key p95=161.85 ns/key runs=0 candidates=0
Decision Buckets:  cpu_features=0x00000001 array=131072 queries=65536
host,compiler,cpu_features,profile,n,queries,hit_rate,gap,jitter,stride,threads,backend,source,shape,calibration_runs,candidates,throughput_gib_s,median_ns,p95_ns,rss_kb,page_faults
730xd,gcc,0x00000001,mixed hit-rate profile,131072,65536,-1,-1,-1,-1,1,scalar,fast_path,random,0,0,0.000,161.85,161.85,6044,10

KEYSTONE Search Technology
============================
✓ Baseline binary search comparison
✓ Anchor-guided core search
✓ Tuned/enhanced search configuration
✓ Auto backend calibration decision reporting
✓ SIMD-aware CPU feature detection
✓ Workload-optimized anchor management

KEYSTONE benchmark suite completed.
Realistic search paths compared against the binary-search baseline.
