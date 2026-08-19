"""
KEYSTONE DSMIL Telemetry Event Processor (ctypes binding).
"""

import ctypes
import os
from dataclasses import dataclass
from typing import List, Optional
from .core import _lib


class _CTelemetryEvent(ctypes.Structure):
    _fields_ = [
        ("timestamp", ctypes.c_uint64),
        ("event_type", ctypes.c_uint32),
        ("device_id", ctypes.c_uint32),
        ("layer_id", ctypes.c_uint32),
        ("_pad", ctypes.c_uint32),
        ("event_data", ctypes.c_void_p),
        ("data_size", ctypes.c_size_t),
    ]


class _CTelemetryResult(ctypes.Structure):
    _fields_ = [
        ("event", ctypes.POINTER(_CTelemetryEvent)),
        ("index", ctypes.c_size_t),
        ("exact_match_time", ctypes.c_uint64),
        ("is_exact_match", ctypes.c_bool),
    ]


_lib.dsmil_telemetry_processor_create.argtypes = [ctypes.c_size_t]
_lib.dsmil_telemetry_processor_create.restype = ctypes.c_void_p

_lib.dsmil_telemetry_processor_destroy.argtypes = [ctypes.c_void_p]
_lib.dsmil_telemetry_processor_destroy.restype = None

_lib.dsmil_telemetry_processor_add_event.argtypes = [ctypes.c_void_p, ctypes.POINTER(_CTelemetryEvent)]
_lib.dsmil_telemetry_processor_add_event.restype = ctypes.c_int

_lib.dsmil_telemetry_processor_find_by_timestamp.argtypes = [
    ctypes.c_void_p,
    ctypes.c_uint64,
    ctypes.POINTER(_CTelemetryResult),
]
_lib.dsmil_telemetry_processor_find_by_timestamp.restype = ctypes.c_int

_lib.dsmil_telemetry_processor_find_in_time_range.argtypes = [
    ctypes.c_void_p,
    ctypes.c_uint64,
    ctypes.c_uint64,
    ctypes.POINTER(_CTelemetryResult),
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.dsmil_telemetry_processor_find_in_time_range.restype = ctypes.c_int


@dataclass
class TelemetryEvent:
    timestamp: int
    event_type: int
    device_id: int
    layer_id: int
    data: Optional[bytes] = None


class TelemetryProcessor:
    """
    KEYSTONE-accelerated DSMIL Telemetry Event Storage and Query Processor.
    """
    def __init__(self, max_events: int = 100000):
        self._ptr = _lib.dsmil_telemetry_processor_create(max_events)
        if not self._ptr:
            raise RuntimeError("Failed to create TelemetryProcessor")
        self._keep_alive = []

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    def close(self):
        if self._ptr:
            _lib.dsmil_telemetry_processor_destroy(self._ptr)
            self._ptr = None

    def __del__(self):
        self.close()

    def add_event(self, event: TelemetryEvent) -> bool:
        """Adds a telemetry event to the processor."""
        data_buf = None
        data_ptr = None
        data_len = 0
        if event.data:
            data_buf = ctypes.create_string_buffer(event.data)
            self._keep_alive.append(data_buf)
            data_ptr = ctypes.cast(data_buf, ctypes.c_void_p)
            data_len = len(event.data)

        c_ev = _CTelemetryEvent(
            timestamp=event.timestamp,
            event_type=event.event_type,
            device_id=event.device_id,
            layer_id=event.layer_id,
            _pad=0,
            event_data=data_ptr,
            data_size=data_len,
        )
        return _lib.dsmil_telemetry_processor_add_event(self._ptr, ctypes.byref(c_ev)) == 0

    def find_by_timestamp(self, timestamp: int) -> Optional[TelemetryEvent]:
        """Finds event matching exact timestamp using Keystone interpolation search."""
        c_res = _CTelemetryResult()
        rc = _lib.dsmil_telemetry_processor_find_by_timestamp(self._ptr, int(timestamp), ctypes.byref(c_res))
        if rc == 0 and c_res.is_exact_match and c_res.event:
            ev = c_res.event.contents
            return TelemetryEvent(
                timestamp=ev.timestamp,
                event_type=ev.event_type,
                device_id=ev.device_id,
                layer_id=ev.layer_id,
            )
        return None

    def find_in_range(self, start_ts: int, end_ts: int, max_results: int = 1000) -> List[TelemetryEvent]:
        """Finds events within [start_ts, end_ts] range."""
        c_res = (_CTelemetryResult * max_results)()
        count = ctypes.c_size_t(0)
        rc = _lib.dsmil_telemetry_processor_find_in_time_range(
            self._ptr,
            int(start_ts),
            int(end_ts),
            c_res,
            max_results,
            ctypes.byref(count),
        )
        results = []
        if rc == 0:
            for i in range(count.value):
                if c_res[i].event:
                    ev = c_res[i].event.contents
                    results.append(
                        TelemetryEvent(
                            timestamp=ev.timestamp,
                            event_type=ev.event_type,
                            device_id=ev.device_id,
                            layer_id=ev.layer_id,
                        )
                    )
        return results
