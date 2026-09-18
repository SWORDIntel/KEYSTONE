"""
Unit tests for KEYSTONE TrigramIndex Python bindings, including parallel construction.
"""

import pytest
from keystone.trigram import TrigramIndex, TrigramStats


def test_trigram_basic():
    idx = TrigramIndex(initial_doc_capacity=10)
    idx.add_document("doc0.txt", "The quick brown fox jumps over the lazy dog")
    idx.add_document("doc1.txt", "Interpolation search engine for sorted int64")
    idx.finalize()

    assert idx.document_count == 2
    cands = idx.get_candidates("quick brown")
    assert cands == [0]

    matches = idx.search("Interpolation")
    assert matches == [1]


def test_trigram_build_parallel():
    docs = [
        ("file_0.log", f"Telemetry payload alpha signal doc {i} with unique marker {i * 10}")
        if i != 77 else ("file_77.log", "Special anomaly detected in sensor matrix 77")
        for i in range(200)
    ]

    idx = TrigramIndex(initial_doc_capacity=200, case_insensitive=True, direct_directory=True)
    idx.build_parallel(docs, thread_count=4)

    assert idx.document_count == 200

    # Search for the specific needle
    matches = idx.search("Special anomaly")
    assert matches == [77]

    # Search case-insensitively
    matches_ci = idx.search("special ANOMALY")
    assert matches_ci == [77]

    # Search for common tokens across threads
    cands = idx.get_candidates("Telemetry payload")
    assert len(cands) == 199

    # Verify finalized state - adding documents should raise RuntimeError
    with pytest.raises(RuntimeError):
        idx.add_document("extra.txt", "should fail")


def test_trigram_build_parallel_dict_format():
    docs = [
        {"name": "docA.txt", "text": "Content A for parallel test", "owns_content": True},
        {"name": "docB.txt", "text": "Content B for parallel test", "owns_content": True},
        {"name": "docC.txt", "text": "Content C external candidate only", "owns_content": False},
    ]

    idx = TrigramIndex(initial_doc_capacity=3)
    idx.build_parallel(docs, thread_count=2)

    assert idx.document_count == 3
    # Owned documents match on exact search
    assert idx.search("Content A") == [0]
    assert idx.search("Content B") == [1]
    # External document has candidates but no exact matches
    assert 2 in idx.get_candidates("external candidate")
    assert idx.search("external candidate") == []

    # Verify get_document
    name_0, content_0 = idx.get_document(0)
    assert name_0 == "docA.txt"
    assert content_0 == b"Content A for parallel test"

    name_2, content_2 = idx.get_document(2)
    assert name_2 == "docC.txt"
    assert content_2 is None  # external doc does not retain content

    with pytest.raises(IndexError):
        idx.get_document(99)
