#!/usr/bin/env python3
"""
scripts/consult_fable_trigram.py — Submit KEYSTONE 24-bit Trigram Implementation to Claude Fable 5.1
Requests concrete, functional performance & algorithmic improvements.
"""

import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
import requests

def get_bothub_token() -> str:
    token = os.environ.get("BOTHUB_TOKEN")
    if token:
        return token
    bashrc = Path.home() / ".bashrc"
    if bashrc.exists():
        m = re.search(r'export BOTHUB_TOKEN="([^"]+)"', bashrc.read_text())
        if m:
            return m.group(1)
    raise RuntimeError("BOTHUB_TOKEN not found in environment or ~/.bashrc")

def main():
    token = get_bothub_token()
    keystone_root = Path("/home/john/Documents/KEYSTONE")
    header_path = keystone_root / "include" / "keystone_trigram.h"
    source_path = keystone_root / "src" / "keystone_trigram.c"

    if not header_path.exists() or not source_path.exists():
        print(f"ERROR: Files not found in {keystone_root}")
        sys.exit(1)

    header_code = header_path.read_text(encoding="utf-8")
    source_code = source_path.read_text(encoding="utf-8")

    print(f"Loaded header: {header_path} ({len(header_code):,} chars, {len(header_code.splitlines()):,} lines)")
    print(f"Loaded source: {source_path} ({len(source_code):,} chars, {len(source_code.splitlines()):,} lines)")

    system_prompt = (
        "You are an elite systems programmer and high-performance indexing specialist with deep expertise "
        "in C11, SIMD vectorization (AVX2, AVX-512), modern cache-conscious data structures, and sub-linear "
        "search algorithms (tgrep, ripgrep, Lucene, Roaring Bitmaps).\n\n"
        "GUIDELINES FOR THIS REVIEW:\n"
        "1. FOCUS ON NOTICABLE FUNCTIONAL PERFORMANCE AND EFFICIENCY FIRST. Hardening and safety edge cases last.\n"
        "2. Do NOT propose enterprise bloat, distributed consensus, or defensive liability shielding. "
        "This is a native local indexing engine (libkeystone.so) running on single-host systems.\n"
        "3. Provide CONCRETE, production-ready C code implementations for hot paths, not abstract suggestions.\n"
        "4. Be direct, rigorous, and technically specific."
    )

    user_prompt = f"""# Architectural & Performance Review: KEYSTONE 24-bit Trigram Index Engine

Below is the complete C11 codebase for KEYSTONE's 24-bit trigram content index engine (`include/keystone_trigram.h` and `src/keystone_trigram.c`).

### Key Existing Mechanisms in the Codebase:
1. **24-bit Trigram Key Space**: Trigrams are 24-bit packed integers (`(b0 << 16) | (b1 << 8) | b2`).
2. **Hash Table**: Split table (`bucket_keys` + `bucket_lists`), Fibonacci hashing (`hash_trigram_key`), linear probing, power-of-two capacity.
3. **Document Ingestion**: 2MB bitmap (`doc_seen` + `doc_seen_touched`) to deduplicate trigrams per-document in O(unique) clear time.
4. **Posting Lists**: Dynamic array `uint32_t* doc_ids` per trigram with geometric growth.
5. **Search / Candidate Selection**: Multi-way intersection of posting lists sorted by increasing frequency.
6. **SIMD**: Basic SSE4.2 substring verification (`_mm_cmpestri`) if enabled.

---

### File 1: `include/keystone_trigram.h`
```c
{header_code}
```

---

### File 2: `src/keystone_trigram.c`
```c
{source_code}
```

---

### Request for Improvements & Actionable Refactoring:

Please provide a comprehensive, deep technical review focusing on **concrete functional speedups and memory efficiency**:

1. **Vectorized Posting List Intersection (AVX2 / AVX-512)**:
   - Current intersection (`intersect_candidates_exact` / `intersect_two_postings`) is a standard scalar two-pointer scan.
   - How can we implement branchless AVX2 (`__m256i`) or AVX-512 vector set intersection (e.g. SIMD galloping, 8-way parallel comparison, or VPTESTMB)?
   - Provide concrete C implementation for vectorized two-way and multi-way intersection.

2. **SIMD Trigram Extraction During Document Ingestion**:
   - Current trigram extraction scans byte-by-byte (`doc_text[i]`, `doc_text[i+1]`, `doc_text[i+2]`).
   - How can we vectorize trigram extraction and packing using AVX2 or NEON?

3. **Posting List Compression & Memory Footprint**:
   - Currently, posting lists are uncompressed `uint32_t[]` arrays. For large text corpora, dense trigrams (e.g. `the`, `ing`, `http`, `://`) consume significant memory and cache bandwidth.
   - What lightweight compression format fits best (e.g. SIMD-BP128, Elias-Fano, Roaring Bitmaps, or hybrid bitsets for dense lists)? Provide a drop-in C design.

4. **Multi-Threaded Document Ingestion (OpenMP / Task Queue)**:
   - Ingestion is currently single-threaded. What is the optimal parallelization strategy for indexing hundreds of files / large logs without thread contention on hash buckets?

5. **Cache Locality & Hash Table Layout**:
   - Critique the split hash table and Fibonacci hashing vs. alternative cache-line friendly layouts (e.g. flat Swiss Tables / Robin Hood).

6. **Query Optimization & Trigram Selection Heuristics**:
   - When a search pattern produces multiple trigrams, how can we improve query planning, rare-trigram pruning, and early termination?

Provide concrete code implementations and measurable optimization paths.
"""

    model_name = "claude-fable-5.1"
    print(f"\nSubmitting trigram implementation to Bothub model: {model_name}...")
    t0 = time.time()

    url = "https://openai.bothub.chat/v1/chat/completions"
    headers = {
        "Authorization": f"Bearer {token}",
        "Content-Type": "application/json",
    }
    payload = {
        "model": model_name,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
        ],
        "stream": False,
    }

    payload_file = Path("/tmp/fable_payload.json")
    payload_file.write_text(json.dumps(payload), encoding="utf-8")
    print(f"Wrote payload to {payload_file} ({payload_file.stat().st_size:,} bytes)")

    out_json = Path("/tmp/fable_response.json")
    if out_json.exists():
        out_json.unlink()

    curl_cmd = [
        "curl", "-s", "-X", "POST",
        "https://openai.bothub.chat/v1/chat/completions",
        "-H", f"Authorization: Bearer {token}",
        "-H", "Content-Type: application/json",
        "-d", f"@{payload_file}",
        "--max-time", "180",
        "-o", str(out_json),
    ]

    print("Running curl request to Bothub...")
    rc = subprocess.run(curl_cmd)
    elapsed = time.time() - t0

    if rc.returncode != 0 or not out_json.exists():
        print(f"curl failed with return code {rc.returncode}")
        sys.exit(1)

    try:
        resp_data = json.loads(out_json.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"Failed to parse response JSON: {e}")
        print("Raw output:", out_json.read_text(encoding="utf-8")[:500])
        sys.exit(1)

    if "error" in resp_data:
        print(f"API returned error: {resp_data['error']}")
        sys.exit(1)

    choices = resp_data.get("choices", [])
    if not choices:
        print("No choices in response:", resp_data)
        sys.exit(1)

    full_text = choices[0].get("message", {}).get("content", "")
    usage = resp_data.get("usage", {})
    print(f"\nCompleted in {elapsed:.2f}s | Tokens: {usage}")
    print(f"Generated text: {len(full_text):,} chars, {len(full_text.splitlines()):,} lines")

    # Save to KEYSTONE and parrot-sabot docs
    out_file_ks = keystone_root / "docs" / "fable_trigram_review.md"
    out_file_ks.write_text(full_text, encoding="utf-8")
    print(f"Saved review to: {out_file_ks}")

    out_file_ps = Path("/home/john/Documents/parrot-sabot/docs/fable_trigram_review.md")
    out_file_ps.parent.mkdir(parents=True, exist_ok=True)
    out_file_ps.write_text(full_text, encoding="utf-8")
    print(f"Saved copy to: {out_file_ps}")

if __name__ == "__main__":
    main()

