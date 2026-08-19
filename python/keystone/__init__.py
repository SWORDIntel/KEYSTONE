"""
KEYSTONE Python SDK
===================
Target-silicon-tuned search library and telemetry engine.

Usage:
    import keystone
    
    # Single interpolation search
    idx = keystone.KeystoneSearch.search(sorted_arr, key)
    
    # Auto-calibrated batch search
    indices = keystone.KeystoneSearch.search_batch(sorted_arr, keys)
    decision = keystone.KeystoneSearch.get_last_decision()
    print(f"Backend: {decision.backend} (latency: {decision.estimated_ns_per_key:.2f} ns/key)")
    
    # Telemetry Processor
    with keystone.TelemetryProcessor() as tp:
        tp.add_event(keystone.TelemetryEvent(timestamp=1600000000, device_id=42, metric_type=1, facility_id=10, value=24.5))
        ev = tp.find_by_timestamp(1600000000)
    
    # Cluster Slot Router (16,384 slots)
    slot = keystone.ClusterRouter.get_slot("device:alpha:42")
    
    # Neural Context Classification
    clf = keystone.NeuralClassifier()
    cls, name, conf = clf.classify("auth_failure admin@pentagon.af.mil")
"""

from .core import KeystoneSearch, AnchorTable, KeystoneBackend, WorkloadType, BackendDecision
from .telemetry import TelemetryProcessor, TelemetryEvent
from .cluster import ClusterRouter, crc16_keystone
from .neural import NeuralClassifier, SemanticClass

__all__ = [
    "KeystoneSearch", "AnchorTable", "KeystoneBackend", "WorkloadType", "BackendDecision",
    "TelemetryProcessor", "TelemetryEvent",
    "ClusterRouter", "crc16_keystone",
    "NeuralClassifier", "SemanticClass",
]
__version__ = "1.0.0"
