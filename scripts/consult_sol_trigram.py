#!/usr/bin/env python3
"""
scripts/consult_sol_trigram.py — Submit KEYSTONE 24-bit Trigram Implementation to gpt-5.6-sol via Bothub.
Uses curl -N for reliable SSE streaming, parses output into clean Markdown.
"""

import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

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
        "You are a World-Class Systems Architect and SIMD Vectorization Engineer specializing in "
        "low-latency information retrieval, sub-linear text search engines (tgrep, ripgrep, Lucene), "
        "cache-conscious data structures, and C11/AVX2/AVX-512 optimization.\n\n"
        "STRICT REVIEW PRINCIPLES:\n"
        "1. IMPROVEMENTS NOTICEABLE TO FUNCTION AND PERFORMANCE FIRST. Hardening and defensive edge cases last.\n"
        "2. Do NOT add enterprise bloat, distributed consensus, Kubernetes concepts, or legalistic liability shielding.\n"
        "3. Provide concrete, production-ready C code implementations for hot paths that can be compiled directly into libkeystone.so.\n"
        "4. Focus on raw search throughput, candidate rejection speed, indexing rate, and memory footprint."
    )

    user_prompt = f"""# Systems Architecture & Optimization Review: KEYSTONE 24-bit Trigram Index Engine

Below is the complete C11 codebase for KEYSTONE's 24-bit trigram content index engine (`include/keystone_trigram.h` and `src/keystone_trigram.c`).

### Architecture Overview:
- **24-bit Trigram Packing**: `(b0 << 16) | (b1 << 8) | b2` covering a 16M key space.
- **Split Hash Table**: `uint32_t* bucket_keys` (dense 4-byte keys, Fibonacci hashed) + `keystone_trigram_posting_list_t* bucket_lists` (only dereferenced on key match).
- **Ingestion Deduplication**: 2MB bitmap (`doc_seen`) tracking 16M bits with a `doc_seen_touched` index array for O(unique) zeroing between documents.
- **Posting Lists**: Array of `uint32_t* doc_ids` with geometric capacity expansion.
- **Search / Candidate Selection**: Multi-way intersection of posting lists sorted by increasing frequency.
- **Verification**: Candidate document IDs are verified against raw bytes using SSE4.2 substring search (`_mm_cmpestri`) if enabled.

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

Provide an exhaustive, highly technical review organized into concrete, implementable sections:

1. **AVX2 / AVX-512 Vectorized Posting List Intersection**:
   - Current intersection (`intersect_candidates_exact` / `intersect_two_postings`) is a scalar two-pointer scan:
     `while (i_a < a->count && i_b < b->count) ...`
   - How can we vectorize this hot path using AVX2 (`__m256i`, 8x uint32 parallel compare) or AVX-512 (`__m512i`, 16x uint32 with `_mm512_cmpeq_epu32_mask` / VPTESTMB)?
   - When posting list lengths differ significantly (e.g. 100 docs vs 100,000 docs), implement SIMD Galloping (exponential search with vector comparison).
   - Provide complete, compilable C functions for vectorized 2-way and multi-way intersection.

2. **SIMD Rolling Trigram Extraction During Document Ingestion**:
   - Current extraction loops byte-by-byte:
     `uint32_t gram = ((uint32_t)(unsigned char)doc_text[i] << 16) | ...`
   - How can we vectorize trigram extraction across 32-byte chunks using AVX2 permute/shuffle (`_mm256_shuffle_epi8`) to compute multiple 24-bit trigrams in parallel?
   - Provide concrete C code for vectorized trigram ingestion.

3. **Compressed Posting Lists & Cache Efficiency**:
   - For massive document collections, uncompressed `uint32_t[]` arrays waste significant L3 cache bandwidth on dense trigrams.
   - Design a hybrid posting representation (e.g. dense bitset/bitmap for trigrams appearing in >1/64 of documents, vs. sorted uint32 for sparse trigrams).
   - Provide the exact data structures and C functions for hybrid posting storage and intersection.

4. **Multi-Threaded Document Ingestion (OpenMP / Lock-Free Buckets)**:
   - Ingestion is currently single-threaded. How should we architect concurrent multi-document indexing without lock contention on hash buckets (e.g. thread-local partial indexes with a parallel merge, or striped bucket locks)?

5. **Hash Table Optimization**:
   - Critique the split hash table and Fibonacci hashing. Would a flat 16-byte slot with 1-byte control tag (Swiss Table style) or direct 24-bit radix table improve lookup latency?

Provide production-grade C code implementations for the hot paths.
"""

    payload = {
        "model": "gpt-5.6-sol",
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
        ],
        "stream": True,
    }

    payload_file = Path("/tmp/sol_trigram_payload.json")
    payload_file.write_text(json.dumps(payload), encoding="utf-8")
    print(f"Wrote payload to {payload_file} ({payload_file.stat().st_size:,} bytes)")

    sse_file = Path("/tmp/sol_trigram_raw.sse")
    if sse_file.exists():
        sse_file.unlink()

    curl_cmd = (
        f'curl -N -s -X POST https://openai.bothub.chat/v1/chat/completions '
        f'-H "Authorization: Bearer {token}" '
        f'-H "Content-Type: application/json" '
        f'-d @{payload_file} '
        f'> {sse_file}'
    )

    print("\nLaunching curl -N SSE stream to Bothub (gpt-5.6-sol)...")
    t0 = time.time()
    proc = subprocess.Popen(curl_cmd, shell=True)

    # Monitor SSE stream file size in real-time
    last_size = 0
    idle_count = 0
    while True:
        ret = proc.poll()
        if sse_file.exists():
            cur_size = sse_file.stat().st_size
            if cur_size > last_size:
                elapsed = time.time() - t0
                print(f"[{elapsed:5.1f}s] Received {cur_size:,} bytes SSE data...")
                last_size = cur_size
                idle_count = 0
            else:
                idle_count += 1
        time.sleep(2.0)
        if ret is not None:
            break
        # Guard timeout (300s)
        if time.time() - t0 > 300:
            print("Timeout reached, terminating curl...")
            proc.terminate()
            break

    elapsed = time.time() - t0
    final_size = sse_file.stat().st_size if sse_file.exists() else 0
    print(f"\nStream complete in {elapsed:.2f}s (Total SSE bytes: {final_size:,})")

    # Parse SSE stream into Markdown
    print("Parsing SSE chunks into Markdown...")
    extracted_chunks = []
    if sse_file.exists():
        with open(sse_file, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if line.startswith("data: ") and line != "data: [DONE]":
                    try:
                        data = json.loads(line[6:])
                        delta = data["choices"][0].get("delta", {})
                        content = delta.get("content", "")
                        if content:
                            extracted_chunks.append(content)
                    except Exception:
                        pass

    full_markdown = "".join(extracted_chunks)
    print(f"Parsed {len(full_markdown):,} characters ({len(full_markdown.splitlines()):,} lines) of review text.")

    if not full_markdown.strip():
        print("ERROR: No markdown text parsed from SSE stream!")
        if sse_file.exists():
            print("Raw SSE head:", sse_file.read_text()[:500])
        sys.exit(1)

    # Save to KEYSTONE docs
    out_ks = keystone_root / "docs" / "sol_trigram_review.md"
    out_ks.write_text(full_markdown, encoding="utf-8")
    print(f"Saved KEYSTONE review to: {out_ks}")

    # Save to parrot-sabot docs
    out_ps = Path("/home/john/Documents/parrot-sabot/docs/sol_trigram_review.md")
    out_ps.parent.mkdir(parents=True, exist_ok=True)
    out_ps.write_text(full_markdown, encoding="utf-8")
    print(f"Saved parrot-sabot copy to: {out_ps}")

if __name__ == "__main__":
    main()
