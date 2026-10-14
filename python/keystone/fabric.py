"""
KEYSTONE Fabric Node Capabilities (QIHSE cluster bus NODE_CAP frames).

Probes local hardware, serializes the 50-byte capability payload, and emits
the 66-byte UDP cluster-bus datagram (magic 0x51424E53). Restored from the
previously parked vector SDK onto the shared libkeystone.so loader in
``keystone.core``.
"""

import ctypes
import struct
from dataclasses import dataclass
from typing import Optional

from .core import _lib

KEYSTONE_FABRIC_NODE_ID_LEN = 40
KEYSTONE_FABRIC_NODE_CAP_PAYLOAD_SIZE = 50
KEYSTONE_FABRIC_BUS_HEADER_SIZE = 16
KEYSTONE_FABRIC_BUS_DATAGRAM_SIZE = 66
KEYSTONE_FABRIC_BUS_MAGIC = 0x51424E53
KEYSTONE_FABRIC_BUS_MSG_NODE_CAP = 8
KEYSTONE_FABRIC_BUS_DEFAULT_PORT = 16379


class _CNodeCap(ctypes.Structure):
    _fields_ = [
        ("node_id", ctypes.c_char * (KEYSTONE_FABRIC_NODE_ID_LEN + 1)),
        ("isa_tier", ctypes.c_uint8),
        ("npu", ctypes.c_uint8),
        ("gpu", ctypes.c_uint8),
        ("free_ram_mb", ctypes.c_uint32),
        ("load_pct", ctypes.c_uint16),
    ]


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


@dataclass
class NodeCapability:
    """Capability profile matching QIHSE AI Compute Fabric NODE_CAP (frame 8u)."""
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
    """Exports the 50-byte wire payload via the KEYSTONE C engine."""
    buf = (ctypes.c_uint8 * KEYSTONE_FABRIC_NODE_CAP_PAYLOAD_SIZE)()
    arg_id = node_id.encode("utf-8") if node_id else None
    rc = _lib.keystone_export_node_cap_frame(arg_id, buf, len(buf))
    if rc < 0:
        raise RuntimeError(f"keystone_export_node_cap_frame failed: {rc}")
    return bytes(buf)


def build_node_cap_datagram(node_id: Optional[str] = None, sender_index: int = 0) -> bytes:
    """Exports the full 66-byte cluster bus datagram (header + payload)."""
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
    """Broadcasts a NODE_CAP datagram to the QIHSE cluster bus via UDP."""
    c_host = host.encode("utf-8")
    arg_id = node_id.encode("utf-8") if node_id else None
    rc = _lib.keystone_broadcast_node_cap(c_host, port, arg_id, sender_index)
    return rc == 0


__all__ = [
    "NodeCapability",
    "probe_node_capability",
    "export_node_cap_frame",
    "build_node_cap_datagram",
    "broadcast_node_cap",
    "KEYSTONE_FABRIC_NODE_ID_LEN",
    "KEYSTONE_FABRIC_NODE_CAP_PAYLOAD_SIZE",
    "KEYSTONE_FABRIC_BUS_HEADER_SIZE",
    "KEYSTONE_FABRIC_BUS_DATAGRAM_SIZE",
    "KEYSTONE_FABRIC_BUS_MAGIC",
    "KEYSTONE_FABRIC_BUS_MSG_NODE_CAP",
    "KEYSTONE_FABRIC_BUS_DEFAULT_PORT",
]
