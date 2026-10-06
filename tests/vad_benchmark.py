#!/usr/bin/env python3
"""Checks for benchmark scoring: unknown labels, overlap, boundaries and cache policy."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "vad_benchmark", Path(__file__).resolve().parents[1]/"tools/benchmark-vad.py")
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class Scoring(unittest.TestCase):
    def test_confusion_and_unknown_tail(self):
        # Known labels cover only 0..2 s. A false detection in the last
        # unannotated second must not count against the detector.
        counts = bench.confusion([[0, 1]], [[0.5, 1.5], [2, 3]], [[0, 2]], 3)
        self.assertEqual(counts, dict(tp=50, fp=50, fn=50, tn=50))
        self.assertEqual(bench.rates(counts)["f1"], 0.5)

    def test_grid_midpoints_and_tail(self):
        self.assertEqual(bench.mask([[0, .005]], .02), [False, False])
        self.assertEqual(bench.mask([[.005, .015]], .02), [True, False])
        # The midpoint beyond the actual recording is unknown, not scored.
        self.assertEqual(bench.confusion([], [[0, .02]], [[0, .011]], .011),
                         dict(tp=0, fp=1, fn=0, tn=0))

    def test_cleanup_merges_before_discarding(self):
        self.assertEqual(bench.merge_intervals([[1, 1.1], [1.3, 1.4], [2, 2.1]], .3, .2),
                         [[1, 1.4]])

    def test_scv_absolute_times_and_unknown_gaps(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/"test.scv"
            p.write_text("test,0.2,0.7,1,1.0,1.5,0\n")
            speech, valid = bench.annotations(p, "ten-scv", 2)
            self.assertEqual(speech, [[.2, .7]])
            self.assertEqual(valid, [[.2, .7], [1, 1.5]])
            p.write_text("test,0,1,1,.5,2,0\n")
            with self.assertRaises(ValueError):
                bench.annotations(p, "ten-scv", 2)

    def test_rttm_overlapping_speakers_are_unioned(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/"test.rttm"
            p.write_text("SPEAKER x 1 0.5 1.0 <NA> <NA> A <NA> <NA>\n"
                         "SPEAKER x 1 1.0 1.0 <NA> <NA> B <NA> <NA>\n")
            self.assertEqual(bench.annotations(p, "rttm", 3),
                             ([[.5, 2]], [[0, 3]]))

    def test_boundary_duplicates_not_matched_twice(self):
        result = bench.boundary_scores([[1, 2]], [[.9, 1.1], [1.2, 2.1]], [[0, 3]], 3)
        self.assertEqual(result["counts"], dict(tp=2, fp=2, fn=0, tn=0))
        self.assertAlmostEqual(bench.rates(result["counts"])["f1"], 2/3)

    def test_boundary_missing_and_unknown(self):
        result = bench.boundary_scores([[0, 1]], [[0, 1.5]], [[0, 2]], 2)
        self.assertEqual(result["counts"], dict(tp=0, fp=1, fn=1, tn=0))
        # A speech boundary exactly at the end of known annotation is excluded.
        result = bench.boundary_scores([[0, 1]], [], [[0, 1]], 2)
        self.assertEqual(result["counts"]["fn"], 0)

    def test_cache_cannot_enter_checkout_through_symlink(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/"checkout"
            p.symlink_to(bench.ROOT, target_is_directory=True)
            with self.assertRaises(ValueError):
                bench.outside_checkout(p/"corpus")
        with self.assertRaises(ValueError):
            bench.outside_checkout(bench.ROOT/"corpus")

    def test_ava_original_movie_times_clipped_and_shifted(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/"ava.csv"
            p.write_text("other,900,910,CLEAN_SPEECH\n"
                         "movie,899,901,NO_SPEECH\n"
                         "movie,901,903,CLEAN_SPEECH\n"
                         "movie,903,904,SPEECH_WITH_MUSIC\n"
                         "movie,904,907,SPEECH_WITH_NOISE\n")
            speech, valid, conditions = bench.ava_annotations(p, "movie", 900, 5)
            self.assertEqual(speech, [[1, 5]])
            self.assertEqual(valid, [[0, 5]])
            self.assertEqual(conditions["SPEECH_WITH_NOISE"], [[4, 5]])
            # Speech condition changes are not additional speech boundaries.
            self.assertEqual(bench.boundary_scores(speech, [[1, 5]], valid, 5)["counts"]["tp"], 1)

    def test_ava_gaps_unknown_and_invalid_labels_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/"ava.csv"
            p.write_text("movie,900,901,NO_SPEECH\nmovie,902,903,CLEAN_SPEECH\n")
            speech, valid, _ = bench.ava_annotations(p, "movie", 900, 4)
            self.assertEqual(valid, [[0, 1], [2, 3]])
            self.assertEqual(bench.confusion(speech, [[0, 4]], valid, 4),
                             dict(tp=100, fp=100, fn=0, tn=0))
            for text in ("movie,900,901,INVALID\n",
                         "movie,900,902,NO_SPEECH\nmovie,901,903,CLEAN_SPEECH\n"):
                p.write_text(text)
                with self.assertRaises(ValueError):
                    bench.ava_annotations(p, "movie", 900, 4)
            with self.assertRaises(ValueError):
                bench.ava_annotations(p, "missing", 900, 4)

    def test_condition_aggregation(self):
        clip = {"counts": dict(tp=50, fp=25, fn=50, tn=75), "duration": 2,
                "metrics": {"f1": 4/7}, "wall_seconds": .01, "cpu_seconds": .01,
                "kernel_seconds": .005,
                "boundaries": {"counts": dict(tp=0, fp=0, fn=0, tn=0), "matched_errors_ms": []},
                "condition_counts": {"CLEAN_SPEECH": dict(tp=50, fn=50, fp=0, tn=0),
                                     "NO_SPEECH": dict(tp=0, fn=0, fp=25, tn=75)}}
        result = bench.aggregate([clip, clip])["conditions"]
        self.assertEqual(result["CLEAN_SPEECH"]["recall"], .5)
        self.assertEqual(result["CLEAN_SPEECH"]["labelled_seconds"], 2)
        self.assertEqual(result["NO_SPEECH"]["false_positive_rate"], .25)


if __name__ == "__main__":
    unittest.main()
