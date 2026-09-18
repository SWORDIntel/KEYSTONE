"""
KEYSTONE Cluster Slot Router, CRC16 Distribution & Fabric Node Capabilities.
"""

import ctypes
import os
import struct
from dataclasses import dataclass
from typing import Optional, Tuple

KEYSTONE_FABRIC_NODE_ID_LEN = 40
KEYSTONE_FABRIC_NODE_CAP_PAYLOAD_SIZE = 50
KEYSTONE_FABRIC_BUS_HEADER_SIZE = 16
KEYSTONE_FABRIC_BUS_DATAGRAM_SIZE = 66
KEYSTONE_FABRIC_BUS_MAGIC = 0x51424E53
KEYSTONE_FABRIC_BUS_MSG_NODE_CAP = 8
KEYSTONE_FABRIC_BUS_DEFAULT_PORT = 16379

# Locate libkeystone.so
_LIB_PATHS = [
    os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "libkeystone.so"),
    "/usr/local/lib/libkeystone.so",
    "/usr/lib/libkeystone.so",
]

_lib = None
for p in _LIB_PATHS:
    if os.path.exists(p):
        try:
            _lib = ctypes.CDLL(p)
            break
        except Exception:
            pass


class _CNodeCap(ctypes.Structure):
    _fields_ = [
        ("node_id", ctypes.c_char * (KEYSTONE_FABRIC_NODE_ID_LEN + 1)),
        ("isa_tier", ctypes.c_uint8),
        ("npu", ctypes.c_uint8),
        ("gpu", ctypes.c_uint8),
        ("free_ram_mb", ctypes.c_uint32),
        ("load_pct", ctypes.c_uint16),
    ]


if _lib is not None:
    try:
        _lib.keystone_probe_node_cap.argtypes = [ctypes.c_char_p, ctypes.POINTER(_CNodeCap)]
        _lib.keystone_probe_node_cap.restype = ctypes.c_int

        _lib.keystone_export_node_cap_frame.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
        ]
        _lib.keystone_export_node_cap_frame.restype = ctypes.c_int

        _lib.keystone_build_node_cap_datagram.argtypes = [
            ctypes.c_char_p,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        _lib.keystone_build_node_cap_datagram.restype = ctypes.c_int

        _lib.keystone_broadcast_node_cap.argtypes = [
            ctypes.c_char_p,
            ctypes.c_uint16,
            ctypes.c_char_p,
            ctypes.c_uint32,
        ]
        _lib.keystone_broadcast_node_cap.restype = ctypes.c_int
    except AttributeError:
        pass


@dataclass
class NodeCapability:
    """
    Capability profile matching QIHSE AI Compute Fabric NODE_CAP (frame 8u).
    """
    node_id: str
    isa_tier: int
    npu: int
    gpu: int
    free_ram_mb: int
    load_pct: int

    def to_wire(self) -> bytes:
        """Serializes capability profile into exact 50-byte wire payload."""
        id_bytes = self.node_id.encode("utf-8")[:KEYSTONE_FABRIC_NODE_ID_LEN]
        id_padded = id_bytes + b"\x00" * (KEYSTONE_FABRIC_NODE_ID_LEN + 1 - len(id_bytes))
        return struct.pack(
            f"<{KEYSTONE_FABRIC_NODE_ID_LEN + 1}sBBBIH",
            id_padded,
            self.isa_tier,
            self.npu,
            self.gpu,
            self.free_ram_mb,
            self.load_pct,
        )

    @classmethod
    def from_wire(cls, data: bytes) -> "NodeCapability":
        """Deserializes from 50-byte wire payload."""
        if len(data) < KEYSTONE_FABRIC_NODE_CAP_PAYLOAD_SIZE:
            raise ValueError(f"Payload too short: {len(data)} < 50")
        raw_id = data[:KEYSTONE_FABRIC_NODE_ID_LEN + 1]
        node_id = raw_id.split(b"\x00", 1)[0].decode("utf-8", errors="replace")
        isa_tier, npu, gpu = struct.unpack("<BBB", data[41:44])
        free_ram_mb = struct.unpack("<I", data[44:48])[0]
        load_pct = struct.unpack("<H", data[48:50])[0]
        return cls(
            node_id=node_id,
            isa_tier=isa_tier,
            npu=npu,
            gpu=gpu,
            free_ram_mb=free_ram_mb,
            load_pct=load_pct,
        )


def probe_node_capability(node_id: Optional[str] = None) -> NodeCapability:
    """Probes local node hardware capabilities via KEYSTONE runtime."""
    if _lib is None or not hasattr(_lib, "keystone_probe_node_cap"):
        raise RuntimeError("libkeystone.so with fabric capability support not loaded")

    c_cap = _CNodeCap()
    arg_id = node_id.encode("utf-8") if node_id else None
    rc = _lib.keystone_probe_node_cap(arg_id, ctypes.byref(c_cap))
    if rc != 0:
        raise RuntimeError(f"keystone_probe_node_cap failed: {rc}")

    raw_id = bytes(c_cap.node_id).split(b"\x00", 1)[0].decode("utf-8", errors="replace")
    return NodeCapability(
        node_id=raw_id,
        isa_tier=c_cap.isa_tier,
        npu=c_cap.npu,
        gpu=c_cap.gpu,
        free_ram_mb=c_cap.free_ram_mb,
        load_pct=c_cap.load_pct,
    )


def export_node_cap_frame(node_id: Optional[str] = None) -> bytes:
    """Exports 50-byte wire payload via KEYSTONE C engine."""
    if _lib is None or not hasattr(_lib, "keystone_export_node_cap_frame"):
        raise RuntimeError("libkeystone.so with fabric capability support not loaded")

    buf = (ctypes.c_uint8 * KEYSTONE_FABRIC_NODE_CAP_PAYLOAD_SIZE)()
    arg_id = node_id.encode("utf-8") if node_id else None
    rc = _lib.keystone_export_node_cap_frame(arg_id, buf, len(buf))
    if rc < 0:
        raise RuntimeError(f"keystone_export_node_cap_frame failed: {rc}")
    return bytes(buf)


def build_node_cap_datagram(node_id: Optional[str] = None, sender_index: int = 0) -> bytes:
    """Exports full 66-byte cluster bus datagram (header + payload)."""
    if _lib is None or not hasattr(_lib, "keystone_build_node_cap_datagram"):
        raise RuntimeError("libkeystone.so with fabric capability support not loaded")

    buf = (ctypes.c_uint8 * KEYSTONE_FABRIC_BUS_DATAGRAM_SIZE)()
    out_len = ctypes.c_size_t(0)
    arg_id = node_id.encode("utf-8") if node_id else None
    rc = _lib.keystone_build_node_cap_datagram(arg_id, sender_index, buf, len(buf), ctypes.byref(out_len))
    if rc != 0:
        raise RuntimeError(f"keystone_build_node_cap_datagram failed: {rc}")
    return bytes(buf[:out_len.value])


def broadcast_node_cap(
    host: str = "127.0.0.1",
    port: int = KEYSTONE_FABRIC_BUS_DEFAULT_PORT,
    node_id: Optional[str] = None,
    sender_index: int = 0,
) -> bool:
    """Broadcasts NODE_CAP datagram directly to QIHSE cluster bus via UDP."""
    if _lib is None or not hasattr(_lib, "keystone_broadcast_node_cap"):
        raise RuntimeError("libkeystone.so with fabric capability support not loaded")

    c_host = host.encode("utf-8")
    arg_id = node_id.encode("utf-8") if node_id else None
    rc = _lib.keystone_broadcast_node_cap(c_host, port, arg_id, sender_index)
    return rc == 0


def crc16_keystone(data: bytes) -> int:
    """
    Standard CRC-16-CCITT implementation used across QIHSE and KEYSTONE cluster topologies.
    """
    crc = 0x0000
    for byte in data:
        crc ^= (byte << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


class ClusterRouter:
    """
    Routes keys and telemetry entities to one of 16,384 cluster hash slots and assigned cluster nodes.
    """
    TOTAL_SLOTS = 16384

    def __init__(self, num_nodes: int = 1):
        if num_nodes <= 0:
            raise ValueError("num_nodes must be >= 1")
        self.num_nodes = num_nodes

    @classmethod
    def get_slot(cls, key: str) -> int:
        """Computes the cluster slot in [0, 16383] for the given string key."""
        encoded = key.encode("utf-8")
        return crc16_keystone(encoded) % cls.TOTAL_SLOTS

    def get_node(self, key: str) -> Tuple[int, int]:
        """
        Returns (node_index, slot_id) for the given key.
        """
        slot = self.get_slot(key)
        slots_per_node = self.TOTAL_SLOTS // self.num_nodes
        node_idx = min(slot // slots_per_node, self.num_nodes - 1)
        return node_idx, slot
