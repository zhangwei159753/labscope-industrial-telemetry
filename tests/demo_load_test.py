import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from demo_load import make_sample, percentile_summary


class DemoLoadTest(unittest.TestCase):
    def test_sample_has_integer_sequence_timestamp_and_numeric_metrics(self):
        sample = make_sample(17, 1_760_000_000_123)
        self.assertEqual(sample["seq"], 17)
        self.assertEqual(sample["timestamp_ms"], 1_760_000_000_123)
        self.assertTrue(sample["metrics"])
        self.assertTrue(all(isinstance(value, (int, float))
                            for value in sample["metrics"].values()))

    def test_percentiles_report_observed_values(self):
        self.assertEqual(percentile_summary([1, 2, 3, 4, 5])["p50"], 3)
        self.assertEqual(percentile_summary([])["count"], 0)


if __name__ == "__main__":
    unittest.main()
