"""
KEYSTONE Cluster Slot Router & CRC16 Distribution (16,384 Slots).
"""

from typing import Tuple


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
