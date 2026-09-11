"""
KEYSTONE Trigram Index Python ctypes wrapper.

Provides 24-bit trigram content indexing for fast candidate filtering:
- Streaming document ingestion (begin/feed/end)
- Trigram extraction from patterns
- Posting-list intersection for candidate selection
- Frequency lookup for query planning
- Paginated candidate iteration for large result sets
- Memory usage reporting

The trigram index is an accelerator: posting-list intersection proposes
candidate documents, but the caller must perform final verification against
the authoritative data source.

Usage:
    from keystone.trigram import TrigramIndex

    idx = TrigramIndex(initial_doc_capacity=4096)
    idx.add_document("readme.md", b"hello world")
    idx.add_document_external("config.toml", b"some content")  # no content retained
    idx.finalize()

    candidates = idx.get_candidates(b"hello")
    freq = idx.frequency(0x68656C)  # "hel"
    mem = idx.memory_usage()
"""

import ctypes
import os
from typing import List, Optional, Tuple
from dataclasses import dataclass

# Reuse the shared libkeystone.so loader from core
from .core import _lib

# ---------------------------------------------------------------------------
# C function signatures
# ---------------------------------------------------------------------------

_lib.keystone_trigram_index_create.argtypes = [ctypes.c_size_t]
_lib.keystone_trigram_index_create.restype = ctypes.c_void_p

_lib.keystone_trigram_index_create_options.argtypes = [ctypes.c_size_t, ctypes.c_uint32]
_lib.keystone_trigram_index_create_options.restype = ctypes.c_void_p

_lib.keystone_trigram_index_get_flags.argtypes = [ctypes.c_void_p]
_lib.keystone_trigram_index_get_flags.restype = ctypes.c_uint32

_lib.keystone_trigram_index_save.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
_lib.keystone_trigram_index_save.restype = ctypes.c_int

_lib.keystone_trigram_index_load.argtypes = [ctypes.c_char_p]
_lib.keystone_trigram_index_load.restype = ctypes.c_void_p

_lib.keystone_trigram_index_destroy.argtypes = [ctypes.c_void_p]
_lib.keystone_trigram_index_destroy.restype = None

_lib.keystone_trigram_index_add_document.argtypes = [
    ctypes.c_void_p,
    ctypes.c_char_p,   # name (may be NULL)
    ctypes.c_char_p,   # text
    ctypes.c_size_t,   # text_len
    ctypes.POINTER(ctypes.c_uint32),  # out_doc_id
]
_lib.keystone_trigram_index_add_document.restype = ctypes.c_int

_lib.keystone_trigram_index_add_document_external.argtypes = [
    ctypes.c_void_p,
    ctypes.c_char_p,
    ctypes.c_char_p,
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_uint32),
]
_lib.keystone_trigram_index_add_document_external.restype = ctypes.c_int

_lib.keystone_trigram_index_finalize.argtypes = [ctypes.c_void_p]
_lib.keystone_trigram_index_finalize.restype = ctypes.c_int

_lib.keystone_trigram_index_document_count.argtypes = [ctypes.c_void_p]
_lib.keystone_trigram_index_document_count.restype = ctypes.c_size_t

_lib.keystone_trigram_index_search.argtypes = [
    ctypes.c_void_p,
    ctypes.c_char_p,
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_uint32),
    ctypes.c_size_t,
]
_lib.keystone_trigram_index_search.restype = ctypes.c_size_t

_lib.keystone_trigram_index_get_candidates.argtypes = [
    ctypes.c_void_p,
    ctypes.c_char_p,
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_uint32),
    ctypes.c_size_t,
]
_lib.keystone_trigram_index_get_candidates.restype = ctypes.c_size_t

_lib.keystone_trigram_extract.argtypes = [
    ctypes.c_char_p,
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_uint32),
    ctypes.c_size_t,
]
_lib.keystone_trigram_extract.restype = ctypes.c_size_t

_lib.keystone_trigram_index_frequency.argtypes = [
    ctypes.c_void_p,
    ctypes.c_uint32,
]
_lib.keystone_trigram_index_frequency.restype = ctypes.c_size_t

_lib.keystone_trigram_index_memory_usage.argtypes = [ctypes.c_void_p]
_lib.keystone_trigram_index_memory_usage.restype = ctypes.c_size_t

# Streaming ingestion
_lib.keystone_trigram_begin_document.argtypes = [
    ctypes.c_void_p,
    ctypes.c_char_p,
]
_lib.keystone_trigram_begin_document.restype = ctypes.c_void_p

_lib.keystone_trigram_begin_document_options.argtypes = [
    ctypes.c_void_p,
    ctypes.c_char_p,
    ctypes.c_bool,
]
_lib.keystone_trigram_begin_document_options.restype = ctypes.c_void_p

_lib.keystone_trigram_feed_bytes.argtypes = [
    ctypes.c_void_p,
    ctypes.c_char_p,
    ctypes.c_size_t,
]
_lib.keystone_trigram_feed_bytes.restype = ctypes.c_int

_lib.keystone_trigram_end_document.argtypes = [
    ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_uint32),
]
_lib.keystone_trigram_end_document.restype = ctypes.c_int

_lib.keystone_trigram_cancel_document.argtypes = [ctypes.c_void_p]
_lib.keystone_trigram_cancel_document.restype = None

# Posting visitor callback type
_KeystonePostingVisitor = ctypes.CFUNCTYPE(
    ctypes.c_int,          # return: 0=continue, nonzero=stop
    ctypes.c_uint32,       # gram
    ctypes.POINTER(ctypes.c_uint32),  # doc_ids
    ctypes.c_size_t,       # count
    ctypes.c_void_p,       # ctx
)

_lib.keystone_trigram_visit_postings.argtypes = [
    ctypes.c_void_p,
    ctypes.c_void_p,       # visitor function pointer
    ctypes.c_void_p,       # ctx
]
_lib.keystone_trigram_visit_postings.restype = ctypes.c_int

# Candidate iterator
_lib.keystone_trigram_candidates_begin.argtypes = [
    ctypes.c_void_p,
    ctypes.c_char_p,
    ctypes.c_size_t,
]
_lib.keystone_trigram_candidates_begin.restype = ctypes.c_void_p

_lib.keystone_trigram_candidates_next.argtypes = [
    ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_uint32),
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
    ctypes.POINTER(ctypes.c_int),
]
_lib.keystone_trigram_candidates_next.restype = ctypes.c_int

_lib.keystone_trigram_candidates_free.argtypes = [ctypes.c_void_p]
_lib.keystone_trigram_candidates_free.restype = None


# ---------------------------------------------------------------------------
# Stats struct
# ---------------------------------------------------------------------------

class _CTrigramStats(ctypes.Structure):
    _fields_ = [
        ("total_documents", ctypes.c_size_t),
        ("unique_trigrams", ctypes.c_size_t),
        ("total_postings", ctypes.c_size_t),
        ("bytes_indexed", ctypes.c_size_t),
        ("build_time_ns", ctypes.c_uint64),
        ("total_searches", ctypes.c_uint64),
        ("candidate_docs_evaluated", ctypes.c_uint64),
        ("candidate_docs_rejected", ctypes.c_uint64),
    ]

_lib.keystone_trigram_index_get_stats.argtypes = [
    ctypes.c_void_p,
    ctypes.POINTER(_CTrigramStats),
]
_lib.keystone_trigram_index_get_stats.restype = None


@dataclass
class TrigramStats:
    total_documents: int
    unique_trigrams: int
    total_postings: int
    bytes_indexed: int
    build_time_ns: int
    total_searches: int
    candidate_docs_evaluated: int
    candidate_docs_rejected: int


# ---------------------------------------------------------------------------
# High-level TrigramIndex
# ---------------------------------------------------------------------------

KEYSTONE_TRIGRAM_OPT_NONE = 0
KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE = 1


class TrigramIndex:
    """
    24-bit trigram content index for fast candidate filtering.

    Documents are added before finalize(). After finalize(), the index is
    immutable and supports candidate queries via posting-list intersection.

    For security-sensitive integrations, use add_document_external() which
    does not retain content. The index is an accelerator only — candidates
    must be verified against the authoritative data source.
    """

    def __init__(
        self,
        initial_doc_capacity: int = 0,
        case_insensitive: bool = False,
        flags: int = KEYSTONE_TRIGRAM_OPT_NONE,
    ):
        if case_insensitive:
            flags |= KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE
        self._ptr = _lib.keystone_trigram_index_create_options(initial_doc_capacity, flags)
        if not self._ptr:
            raise RuntimeError("Failed to create trigram index")

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    def __del__(self):
        self.close()

    def close(self):
        if self._ptr:
            _lib.keystone_trigram_index_destroy(self._ptr)
            self._ptr = None

    @property
    def flags(self) -> int:
        if not self._ptr:
            return 0
        return int(_lib.keystone_trigram_index_get_flags(self._ptr))

    def save(self, filepath: str) -> None:
        """Serialize a finalized trigram index to a binary file on disk."""
        if not self._ptr:
            raise RuntimeError("TrigramIndex is closed")
        c_path = filepath.encode("utf-8")
        rc = _lib.keystone_trigram_index_save(self._ptr, c_path)
        if rc != 0:
            raise RuntimeError(f"save failed with code {rc}")

    @classmethod
    def load(cls, filepath: str) -> "TrigramIndex":
        """Load and reconstruct a finalized trigram index from a binary file."""
        if not os.path.exists(filepath):
            raise FileNotFoundError(f"Index file not found: {filepath}")
        c_path = filepath.encode("utf-8")
        ptr = _lib.keystone_trigram_index_load(c_path)
        if not ptr:
            raise RuntimeError(f"Failed to load trigram index from {filepath}")
        inst = cls.__new__(cls)
        inst._ptr = ptr
        return inst

    @property
    def document_count(self) -> int:
        return int(_lib.keystone_trigram_index_document_count(self._ptr))

    def add_document(self, name: Optional[str], text: bytes) -> int:
        """Add a document with content retention (for exact search).

        Args:
            name: Optional document name/path (None or string).
            text: Document content bytes.

        Returns:
            Assigned document ID.
        """
        c_name = name.encode("utf-8") if name else None
        doc_id = ctypes.c_uint32(0)
        rc = _lib.keystone_trigram_index_add_document(
            self._ptr, c_name, text, len(text), ctypes.byref(doc_id)
        )
        if rc != 0:
            raise RuntimeError(f"add_document failed with code {rc}")
        return int(doc_id.value)

    def add_document_external(self, name: Optional[str], text: bytes) -> int:
        """Add a document without content retention (candidate-only indexing).

        Args:
            name: Optional document name/path (None or string).
            text: Document content bytes.

        Returns:
            Assigned document ID.
        """
        c_name = name.encode("utf-8") if name else None
        doc_id = ctypes.c_uint32(0)
        rc = _lib.keystone_trigram_index_add_document_external(
            self._ptr, c_name, text, len(text), ctypes.byref(doc_id)
        )
        if rc != 0:
            raise RuntimeError(f"add_document_external failed with code {rc}")
        return int(doc_id.value)

    def begin_document(self, name: Optional[str] = None, retain_content: bool = False) -> "TrigramStream":
        """Begin streaming document ingestion.

        Returns a TrigramStream that can be fed bytes incrementally.
        """
        c_name = name.encode("utf-8") if name else None
        stream_ptr = _lib.keystone_trigram_begin_document_options(self._ptr, c_name, retain_content)
        if not stream_ptr:
            raise RuntimeError("begin_document failed")
        return TrigramStream(stream_ptr)

    def finalize(self) -> None:
        """Finalize the index for querying. After this, no more documents."""
        rc = _lib.keystone_trigram_index_finalize(self._ptr)
        if rc != 0:
            raise RuntimeError(f"finalize failed with code {rc}")

    def search(self, pattern: bytes, max_matches: int = 4096) -> List[int]:
        """Trigram-accelerated exact substring search over owned docs.

        External-only documents are skipped (no content retained).
        Returns list of matching document IDs.
        """
        out = (ctypes.c_uint32 * max_matches)()
        n = _lib.keystone_trigram_index_search(
            self._ptr, pattern, len(pattern), out, max_matches
        )
        return [int(out[i]) for i in range(n)]

    def get_candidates(self, pattern: bytes, max_candidates: int = 4096) -> List[int]:
        """Intersect posting lists to get candidate document IDs.

        Candidates are hints only — the caller must verify them.
        For patterns shorter than 3 bytes, all docs are candidates.
        """
        out = (ctypes.c_uint32 * max_candidates)()
        n = _lib.keystone_trigram_index_get_candidates(
            self._ptr, pattern, len(pattern), out, max_candidates
        )
        return [int(out[i]) for i in range(n)]

    def get_candidates_iter(self, pattern: bytes, batch_size: int = 4096):
        """Paginated candidate iterator for large result sets.

        Yields lists of candidate document IDs.
        """
        iter_ptr = _lib.keystone_trigram_candidates_begin(
            self._ptr, pattern, len(pattern)
        )
        if not iter_ptr:
            return
        try:
            while True:
                buf = (ctypes.c_uint32 * batch_size)()
                count = ctypes.c_size_t(0)
                exhausted = ctypes.c_int(0)
                rc = _lib.keystone_trigram_candidates_next(
                    iter_ptr, buf, batch_size,
                    ctypes.byref(count), ctypes.byref(exhausted)
                )
                if rc != 0:
                    break
                if count.value > 0:
                    yield [int(buf[i]) for i in range(count.value)]
                if exhausted.value:
                    break
        finally:
            _lib.keystone_trigram_candidates_free(iter_ptr)

    def frequency(self, gram: int) -> int:
        """Document frequency of a 24-bit trigram key. 0 if not found."""
        return int(_lib.keystone_trigram_index_frequency(self._ptr, gram))

    def memory_usage(self) -> int:
        """Approximate memory usage in bytes."""
        return int(_lib.keystone_trigram_index_memory_usage(self._ptr))

    def get_stats(self) -> TrigramStats:
        """Get index statistics."""
        stats = _CTrigramStats()
        _lib.keystone_trigram_index_get_stats(self._ptr, ctypes.byref(stats))
        return TrigramStats(
            total_documents=int(stats.total_documents),
            unique_trigrams=int(stats.unique_trigrams),
            total_postings=int(stats.total_postings),
            bytes_indexed=int(stats.bytes_indexed),
            build_time_ns=int(stats.build_time_ns),
            total_searches=int(stats.total_searches),
            candidate_docs_evaluated=int(stats.candidate_docs_evaluated),
            candidate_docs_rejected=int(stats.candidate_docs_rejected),
        )

    def visit_postings(self):
        """Iterate all posting lists as (gram, doc_ids) tuples.

        Yields (gram: int, doc_ids: List[int]) for each trigram in the index.
        """
        results: List[Tuple[int, List[int]]] = []

        @ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_uint32,
                          ctypes.POINTER(ctypes.c_uint32),
                          ctypes.c_size_t, ctypes.c_void_p)
        def visitor(gram, doc_ids, count, ctx):
            ids = [int(doc_ids[i]) for i in range(count)]
            results.append((int(gram), ids))
            return 0  # continue

        _lib.keystone_trigram_visit_postings(
            self._ptr, visitor, None
        )
        return results


# ---------------------------------------------------------------------------
# Streaming document handle
# ---------------------------------------------------------------------------

class TrigramStream:
    """Streaming document ingestion handle.

    Feed bytes incrementally, then call end() to commit or cancel() to abort.
    """

    def __init__(self, ptr: int):
        self._ptr = ptr
        self._ended = False

    def feed(self, data: bytes) -> None:
        """Feed bytes to the streaming document."""
        if self._ended or not self._ptr:
            raise RuntimeError("stream is closed")
        rc = _lib.keystone_trigram_feed_bytes(self._ptr, data, len(data))
        if rc != 0:
            raise RuntimeError(f"feed_bytes failed with code {rc}")

    def end(self) -> int:
        """Finish the document and add to index. Returns assigned doc ID."""
        if self._ended or not self._ptr:
            raise RuntimeError("stream is already closed")
        doc_id = ctypes.c_uint32(0)
        rc = _lib.keystone_trigram_end_document(self._ptr, ctypes.byref(doc_id))
        self._ptr = None
        self._ended = True
        if rc != 0:
            raise RuntimeError(f"end_document failed with code {rc}")
        return int(doc_id.value)

    def cancel(self) -> None:
        """Abort the document without indexing."""
        if self._ptr and not self._ended:
            _lib.keystone_trigram_cancel_document(self._ptr)
        self._ptr = None
        self._ended = True

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        if exc_type is not None:
            self.cancel()
        elif not self._ended:
            self.end()


# ---------------------------------------------------------------------------
# Standalone trigram extraction
# ---------------------------------------------------------------------------

def extract_trigrams(pattern: bytes) -> List[int]:
    """Extract unique 24-bit trigram keys from a text pattern.

    Returns a list of trigram keys (integers).
    """
    max_grams = max(256, len(pattern) * 2)
    buf = (ctypes.c_uint32 * max_grams)()
    n = _lib.keystone_trigram_extract(pattern, len(pattern), buf, max_grams)
    return [int(buf[i]) for i in range(n)]


__all__ = [
    "TrigramIndex",
    "TrigramStream",
    "TrigramStats",
    "extract_trigrams",
    "KEYSTONE_TRIGRAM_OPT_NONE",
    "KEYSTONE_TRIGRAM_OPT_CASE_INSENSITIVE",
]
