"""Exercise the real client/server scheduler against CPU-only fake NVML."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = os.environ.get("TS_GPU_TEST_BINARY")


@unittest.skipUnless(BINARY, "set TS_GPU_TEST_BINARY to the fake-NVML binary")
class GpuAffinityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ts-gpu-", dir="/tmp")
        self.root = Path(self.temp.name)
        self.env = dict(os.environ, TS_SOCKET=str(self.root / "queue"),
                        TMPDIR=str(self.root), TS_SLOTS="2", TS_MAXCONN="64",
                        TS_TEST_GPU_STATE=str(self.root / "gpu-state"))
        for key in ("TS_FEISHU_WEBHOOK", "TS_ENV", "TS_ONFINISH", "TS_VISIBLE_DEVICES", "TS_SAVELIST"):
            self.env.pop(key, None)
        self.gates = []
        self.memory(100, 100, 100)

    def ts(self, *args):
        return subprocess.run([BINARY, *map(str, args)], env=self.env, text=True,
                              capture_output=True, timeout=10)

    def memory(self, *values):
        temp = self.root / "gpu-state.tmp"
        temp.write_text(" ".join(map(str, values)))
        temp.replace(self.env["TS_TEST_GPU_STATE"])

    def wait(self, predicate):
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            if predicate(): return
            time.sleep(.02)
        self.fail("scheduler condition did not become true")

    def state(self, job):
        return self.ts("-s", job).stdout.strip()

    def submit(self, name, *flags, gated=False):
        output = self.root / (name + ".gpu")
        gate = self.root / (name + ".release")
        if gated: self.gates.append(gate)
        else: gate.touch()
        script = ("import os,sys,time; from pathlib import Path; "
                  "Path(sys.argv[1]).write_text(os.environ['CUDA_VISIBLE_DEVICES']); "
                  "exec('while not Path(sys.argv[2]).exists(): time.sleep(.02)')")
        result = self.ts(*flags, "-L", "明-test-" + name, sys.executable, "-c", script, output, gate)
        self.assertEqual(result.returncode, 0, result.stderr)
        return int(result.stdout.strip()), output, gate

    def tearDown(self):
        for gate in self.gates: gate.touch()
        if Path(self.env["TS_SOCKET"]).exists():
            result = self.ts("-M", "json")
            for job in json.loads(result.stdout):
                if job["State"] in ("allocating", "queued", "holding client"):
                    self.ts("-r", job["ID"])
            self.wait(lambda: self.ts("-R").stdout.strip() == "0")
            self.ts("-K")
            self.wait(lambda: not Path(self.env["TS_SOCKET"]).exists())
        self.temp.cleanup()

    def test_waits_for_requested_gpu_despite_other_free_cards(self):
        self.memory(0, 100, 100)
        job, output, _ = self.submit("pin", "-G", 1, "--gpu-ids", "0")
        self.assertEqual(self.state(job), "allocating")
        self.assertFalse(output.exists())
        info = self.ts("-i", job).stdout
        self.assertIn("Allowed GPU IDs: 0", info)
        self.assertEqual(json.loads(self.ts("-M", "json").stdout)[0]["Allowed GPU IDs"], [0])
        self.memory(100, 100, 100)
        self.wait(lambda: self.state(job) == "finished")
        self.assertEqual(output.read_text(), "0")

    def test_reservation_blocks_same_card_but_not_other_cards(self):
        first, _, gate = self.submit("first", "-G", 1, "--gpu-ids", "0", gated=True)
        self.wait(lambda: self.state(first) == "running")
        second, second_output, _ = self.submit("second", "-G", 1, "--gpu-ids", "0")
        other, other_output, _ = self.submit("other", "-G", 1, "--gpu-ids", "1")
        self.wait(lambda: self.state(other) == "finished")
        self.assertEqual(other_output.read_text(), "1")
        self.assertEqual(self.state(second), "allocating")
        self.assertFalse(second_output.exists())
        gate.touch()
        self.wait(lambda: self.state(second) == "finished")
        self.assertEqual(second_output.read_text(), "0")

    def test_selects_only_available_members_of_candidate_set(self):
        self.memory(0, 100, 100)
        job, output, _ = self.submit("pool", "--gpu-ids", "0,2")
        self.wait(lambda: self.state(job) == "finished")
        self.assertEqual(output.read_text(), "2")

    def test_multiple_gpus_must_be_available_together(self):
        self.memory(100, 100, 0)
        job, output, _ = self.submit("pair", "--gpu-ids", "0,2", "-G", 2)
        self.assertEqual(self.state(job), "allocating")
        self.memory(100, 100, 100)
        self.wait(lambda: self.state(job) == "finished")
        self.assertEqual(set(output.read_text().split(",")), {"0", "2"})

    def test_server_visibility_still_restricts_affinity(self):
        self.env["TS_VISIBLE_DEVICES"] = "1"
        job, output, _ = self.submit("hidden", "--gpu-ids", "0")
        self.assertEqual(self.state(job), "allocating")
        other, other_output, _ = self.submit("visible", "-G", 1)
        self.wait(lambda: self.state(other) == "finished")
        self.assertEqual(other_output.read_text(), "1")
        self.assertFalse(output.exists())

    def test_cancel_does_not_release_a_running_jobs_gpu(self):
        first, _, gate = self.submit("held", "--gpu-ids", "0", gated=True)
        self.wait(lambda: self.state(first) == "running")
        cancelled, _, _ = self.submit("cancelled", "--gpu-ids", "0")
        self.assertEqual(self.ts("-r", cancelled).returncode, 0)
        waiting, output, _ = self.submit("waiting", "--gpu-ids", "0")
        self.assertEqual(self.state(waiting), "allocating")
        self.assertFalse(output.exists())
        gate.touch()
        self.wait(lambda: self.state(waiting) == "finished")

    def test_dependencies_and_slots_are_preserved(self):
        parent, _, gate = self.submit("parent", "--gpu-ids", "0", gated=True)
        child, output, _ = self.submit("child", "--gpu-ids", "1", "-D", parent)
        self.assertEqual(self.state(child), "allocating")
        gate.touch()
        self.wait(lambda: self.state(child) == "finished")
        self.assertEqual(output.read_text(), "1")
        self.ts("-S", 1)
        first, _, gate = self.submit("slot-held", "--gpu-ids", "0", gated=True)
        self.wait(lambda: self.state(first) == "running")
        second, _, _ = self.submit("slot-wait", "--gpu-ids", "1")
        self.assertEqual(self.state(second), "allocating")
        gate.touch()
        self.wait(lambda: self.state(second) == "finished")

    def test_legacy_forced_gpu_behavior_is_unchanged(self):
        self.memory(0, 100, 100)
        job, output, _ = self.submit("legacy", "-g", "0")
        self.wait(lambda: self.state(job) == "finished")
        self.assertEqual(output.read_text(), "0")

    def test_large_gpu_ids_are_reported_safely_and_never_allocated(self):
        candidates = ",".join(map(str, [2147483647, *range(100,140)]))
        job, output, _ = self.submit("unavailable", "--gpu-ids", candidates)
        self.assertEqual(self.state(job), "allocating")
        self.assertIn("Allowed GPU IDs: " + candidates, self.ts("-i", job).stdout)
        self.assertFalse(output.exists())
        other, _, _ = self.submit("usable", "--gpu-ids", "0")
        self.wait(lambda: self.state(other) == "finished")

    def test_invalid_requests_fail_before_creating_a_queue(self):
        invalid = [["--gpu-ids", value] for value in ("", "-1", "1,", ",1", "1,,2", "1,1", "one", "1x", "1 2", "99999999999999999999")]
        invalid += [["-G", 0, "--gpu-ids", "0"], ["-G", 2, "--gpu-ids", "0"],
                    ["--gpu-ids", "0", "-G", "1x"], ["--gpu-ids", "0", "-g", "1"],
                    ["-g", "1", "--gpu-ids", "0"], ["--gpu-ids", "0", "--gpu-ids", "1"]]
        for flags in invalid:
            with self.subTest(flags=flags):
                self.assertNotEqual(self.ts(*flags, "true").returncode, 0)
                self.assertFalse(Path(self.env["TS_SOCKET"]).exists())


if __name__ == "__main__": unittest.main()
