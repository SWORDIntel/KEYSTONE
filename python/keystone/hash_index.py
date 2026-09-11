"""
KEYSTONE Hash Index Python ctypes wrapper.

Provides FNV-1a hash-based exact-match whole-token lookup:
- Add tokens with associated doc IDs
- Finalize and search (binary search on sorted hash array)
- Save/load to disk for persistence
- search_all returns all matching doc IDs for a token

Usage:
    from keystone.hash_index import HashIndex

    idx = HashIndex(initial_capacity=4096)
    idx.add(b"Terminal", doc_id=42)
    idx.add(b"Struct", doc_id=17)
    idx.finalize()

    results = idx.search_all(b"Terminal")  # [42]
    idx.save("/tmp/index.thi")

    loaded = HashIndex.load("/tmp/index.thi")
    assert loaded.search_all(b"Terminal") == [42]
"""

import ctypes
from typing import List, Optional

from .core import _lib

# ---------------------------------------------------------------------------
# C function signatures
# ---------------------------------------------------------------------------

_lib.dsmil_hash_index_create.argtypes = [ctypes.c_size_t]
_lib.dsmil_hash_index_create.restype = ctypes.c_void_p

_lib.dsmil_hash_index_destroy.argtypes = [ctypes.c_void_p]
_lib.dsmil_hash_index_destroy.restype = None

_lib.dsmil_hash_index_add.argtypes = [
    ctypes.c_void_p,
    ctypes.c_char_p,
    ctypes.c_size_t,
    ctypes.c_uint64,
]
_lib.dsmil_hash_index_add.restype = ctypes.c_int

_lib.dsmil_hash_index_finalize.argtypes = [ctypes.c_void_p]
_lib.dsmil_hash_index_finalize.restype = ctypes.c_int

_lib.dsmil_hash_index_search_all.argtypes = [
    ctypes.c_void_p,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_uint64),
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.dsmil_hash_index_search_all.restype = ctypes.c_size_t

_lib.dsmil_hash_index_save.argtypes = [
    ctypes.c_void_p,
    ctypes.c_char_p,
]
_lib.dsmil_hash_index_save.restype = ctypes.c_int

_lib.dsmil_hash_index_load.argtypes = [ctypes.c_char_p]
_lib.dsmil_hash_index_load.restype = ctypes.c_void_p


# ---------------------------------------------------------------------------
# High-level HashIndex
# ---------------------------------------------------------------------------

class HashIndex:
    """
    FNV-1a hash index for exact-match whole-token queries.

    Tokens are hashed with FNV-1a, sorted by unsigned hash value, and
    searched with binary search. String verification handles collisions.
    Persisted as .thi files.
    """

    def __init__(self, ptr: int, count: int = 0):
        self._ptr = ptr
        self._count = count

    @classmethod
    def create(cls, initial_capacity: int = 4096) -> "HashIndex":
        """Create a new hash index with the given initial capacity."""
        ptr = _lib.dsmil_hash_index_create(initial_capacity)
        if not ptr:
            raise RuntimeError("Failed to create hash index")
        return cls(ptr, 0)

    @classmethod
    def load(cls, path: str) -> "HashIndex":
        """Load a hash index from a .thi file."""
        c_path = path.encode("utf-8")
        ptr = _lib.dsmil_hash_index_load(c_path)
        if not ptr:
            raise RuntimeError(f"Failed to load hash index from {path}")
        # Count is unknown after load; set to -1 to indicate "unknown"
        return cls(ptr, -1)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    def __del__(self):
        self.close()

    def close(self):
        if self._ptr:
            _lib.dsmil_hash_index_destroy(self._ptr)
            self._ptr = None

    @property
    def count(self) -> int:
        """Number of tokens in the index.

        Note: requires the index to be finalized. The underlying C library
        does not export a count function, so we track it in Python.
        """
        return self._count

    def add(self, token: bytes, doc_id: int) -> None:
        """Add a token with its doc ID to the index."""
        token_buf = ctypes.create_string_buffer(token)
        rc = _lib.dsmil_hash_index_add(self._ptr, token_buf, len(token), doc_id)
        if rc != 0:
            raise RuntimeError(f"hash_index_add failed with code {rc}")
        self._count += 1

    def finalize(self) -> None:
        """Finalize the index (sorts by hash, enables search)."""
        rc = _lib.dsmil_hash_index_finalize(self._ptr)
        if rc != 0:
            raise RuntimeError(f"hash_index_finalize failed with code {rc}")

    def search_all(self, token: bytes, max_results: int = 65536) -> List[int]:
        """Search for all doc IDs matching the given token.

        Returns a list of doc IDs. Empty list if not found.
        """
        # NUL-terminate the token for the C API (uses strlen internally)
        token_buf = ctypes.create_string_buffer(token)
        out = (ctypes.c_uint64 * max_results)()
        count = ctypes.c_size_t(0)
        _lib.dsmil_hash_index_search_all(
            self._ptr, token_buf, out, max_results, ctypes.byref(count)
        )
        return [int(out[i]) for i in range(count.value)]

    def save(self, path: str) -> None:
        """Save the hash index to a .thi file."""
        c_path = path.encode("utf-8")
        rc = _lib.dsmil_hash_index_save(self._ptr, c_path)
        if rc != 0:
            raise RuntimeError(f"hash_index_save failed with code {rc}")
