import unittest
import numpy as np
import keystone

class TestKeystoneSDK(unittest.TestCase):

    def test_cpu_detection(self):
        features = keystone.KeystoneSearch.detect_cpu_features()
        self.assertIsInstance(features, int)
        print(f"\n[KEYSTONE CPU Features] Bitmask: 0x{features:08x}")

    def test_search_single(self):
        arr = np.array([10, 20, 30, 40, 50, 60, 70, 80, 90, 100], dtype=np.int64)

        # Hit
        idx = keystone.KeystoneSearch.search(arr, 50)
        self.assertEqual(idx, 4)

        # Miss
        idx_miss = keystone.KeystoneSearch.search(arr, 55)
        self.assertEqual(idx_miss, -1)

    def test_search_batch_and_auto_decision(self):
        arr = np.arange(0, 100000, 7, dtype=np.int64)
        queries = np.array([arr[10], arr[500], arr[1200], 99999999], dtype=np.int64)

        results = keystone.KeystoneSearch.search_batch(arr, queries)
        self.assertEqual(results[0], 10)
        self.assertEqual(results[1], 500)
        self.assertEqual(results[2], 1200)
        self.assertEqual(results[3], -1)

        dec = keystone.KeystoneSearch.get_last_decision()
        if dec:
            print(f"[Auto Decision] Backend: {dec.backend} | Source: {dec.decision_source} | Est Latency: {dec.estimated_ns_per_key:.2f} ns/key")

    def test_anchor_table(self):
        with keystone.AnchorTable() as table:
            self.assertIsNotNone(table.handle)
            arr = np.arange(0, 10000, dtype=np.int64)
            idx = keystone.KeystoneSearch.search(arr, 5000, table=table)
            self.assertEqual(idx, 5000)

    def test_telemetry_processor(self):
        with keystone.TelemetryProcessor(max_events=1000) as tp:
            tp.add_event(keystone.TelemetryEvent(
                timestamp=1000, event_type=10, device_id=1, layer_id=2
            ))
            tp.add_event(keystone.TelemetryEvent(
                timestamp=2000, event_type=10, device_id=1, layer_id=2
            ))
            tp.add_event(keystone.TelemetryEvent(
                timestamp=3000, event_type=11, device_id=2, layer_id=3
            ))

            # Point lookup
            ev = tp.find_by_timestamp(2000)
            self.assertIsNotNone(ev)
            self.assertEqual(ev.timestamp, 2000)
            self.assertEqual(ev.device_id, 1)

            # Range lookup
            range_evs = tp.find_in_range(1000, 2500)
            self.assertEqual(len(range_evs), 2)
            self.assertEqual(range_evs[0].timestamp, 1000)
            self.assertEqual(range_evs[1].timestamp, 2000)

    def test_cluster_router(self):
        slot = keystone.ClusterRouter.get_slot("telemetry:device:42:sensor:temp")
        self.assertGreaterEqual(slot, 0)
        self.assertLess(slot, 16384)

        router = keystone.ClusterRouter(num_nodes=4)
        node_idx, slot_id = router.get_node("telemetry:device:42:sensor:temp")
        self.assertIn(node_idx, [0, 1, 2, 3])
        self.assertEqual(slot_id, slot)

    def test_neural_classifier(self):
        clf = keystone.NeuralClassifier()
        cls, name, conf = clf.classify("auth_failure admin@pentagon.af.mil topsecret_token=998822")
        self.assertIsInstance(cls, keystone.SemanticClass)
        self.assertGreater(conf, 0.0)
        self.assertLessEqual(conf, 1.0)


if __name__ == "__main__":
    unittest.main()
