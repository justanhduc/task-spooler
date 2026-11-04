"""Exercise queue ordering and server survival through the public CLI."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest


BINARY = str(Path(os.environ.get("TS_BINARY", "./build/ts")).resolve())


class UrgentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ts-urgent-", dir="/tmp")
        self.directory = Path(self.temp.name)
        self.order = self.directory / "order"
        self.gates = []
        self.env = dict(os.environ, TS_SOCKET=str(self.directory / "queue"),
                        TMPDIR=self.temp.name, TS_MAXCONN="64", TS_SLOTS="1")
        for variable in ("TS_FEISHU_WEBHOOK", "TS_ENV", "TS_ONFINISH"):
            self.env.pop(variable, None)

    def tearDown(self):
        for gate in self.gates:
            gate.touch()
        if Path(self.env["TS_SOCKET"]).exists():
            self.ts("-S", "32")
            self.wait_until(lambda: all(job["State"] in ("finished", "skipped")
                                       for job in self.jobs()))
            self.ts("-K")
            self.wait_until(lambda: not Path(self.env["TS_SOCKET"]).exists())
        self.temp.cleanup()

    def ts(self, *args):
        return subprocess.run([BINARY, *map(str, args)], env=self.env, text=True,
                              capture_output=True, timeout=10)

    def jobs(self):
        result = self.ts("-M", "json")
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def active_ids(self):
        return [job["ID"] for job in self.jobs()
                if job["State"] not in ("finished", "skipped")]

    def wait_until(self, condition):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if condition():
                return
            time.sleep(0.02)
        self.fail("timed out waiting for the queue")

    def submit(self, name, slots=1, gated=False):
        gate = self.directory / (name + "-release") if gated else None
        if gate:
            self.gates.append(gate)
        script = ('if [ -n "$2" ]; then '
                  'while [ ! -f "$2" ]; do sleep 0.02; done; fi; '
                  'printf "%s\\n" "$3" >> "$1"')
        result = self.ts("-N", slots, "-L", name, "sh", "-c", script,
                         "test-job", self.order, str(gate) if gate else "", name)
        self.assertEqual(result.returncode, 0, result.stderr)
        return int(result.stdout.strip()), gate

    def promote(self, jobid=None):
        result = self.ts("-u", *([] if jobid is None else [jobid]))
        self.assertEqual(result.returncode, 0, result.stderr)

    def assert_order(self, names):
        self.wait_until(lambda: self.order.exists() and
                        len(self.order.read_text().splitlines()) == len(names))
        self.assertEqual(self.order.read_text().splitlines(), names)

    def test_no_running_job_promotes_ahead_of_waiting_head(self):
        a, _ = self.submit("A", slots=2)
        b, _ = self.submit("B", slots=2)
        c, _ = self.submit("C", slots=2)
        self.promote(c)
        self.assertEqual(self.active_ids(), [c, a, b])
        self.ts("-S", "2")
        self.assertEqual(self.ts("-w", b).returncode, 0)
        self.assert_order(["C", "A", "B"])

    def test_head_promotion_is_safe_and_repeatable(self):
        a, _ = self.submit("A", slots=2)
        b, _ = self.submit("B", slots=2)
        for _ in range(3):
            self.promote(a)
            self.assertEqual(self.active_ids(), [a, b])

    def test_single_waiting_job_can_be_promoted(self):
        a, _ = self.submit("A", slots=2)
        self.promote(a)
        self.assertEqual(self.active_ids(), [a])

    def test_running_job_is_not_preempted(self):
        running, gate = self.submit("running", gated=True)
        self.wait_until(lambda: self.ts("-s", running).stdout.strip() == "running")
        a, _ = self.submit("A")
        b, _ = self.submit("B")
        self.promote(b)
        self.assertEqual(self.active_ids(), [running, b, a])
        self.assertEqual(self.ts("-s", running).stdout.strip(), "running")
        self.assertFalse(self.order.exists())
        gate.touch()
        self.assertEqual(self.ts("-w", a).returncode, 0)
        self.assert_order(["running", "B", "A"])

    def test_mixed_running_and_waiting_jobs_keep_stable_order(self):
        self.ts("-S", "2")
        blocked, _ = self.submit("blocked", slots=3)
        first, first_gate = self.submit("first", gated=True)
        second, second_gate = self.submit("second", gated=True)
        self.wait_until(lambda: self.ts("-s", second).stdout.strip() == "running")
        a, _ = self.submit("A")
        b, _ = self.submit("B")
        self.promote(b)
        self.assertEqual(self.active_ids(), [first, second, b, blocked, a])
        first_gate.touch()
        self.assertEqual(self.ts("-w", a).returncode, 0)
        self.assertEqual(self.order.read_text().splitlines(), ["first", "B", "A"])
        self.assertEqual(self.ts("-s", second).stdout.strip(), "running")
        second_gate.touch()

    def test_default_selects_last_added_after_reordering(self):
        a, _ = self.submit("A", slots=2)
        b, _ = self.submit("B", slots=2)
        c, _ = self.submit("C", slots=2)
        self.promote(c)
        self.promote()
        self.assertEqual(self.active_ids(), [c, a, b])

    def test_invalid_finished_and_running_jobs_leave_queue_intact(self):
        self.assertNotEqual(self.ts("-u").returncode, 0)
        self.assertEqual(self.jobs(), [])
        running, gate = self.submit("running", gated=True)
        self.wait_until(lambda: self.ts("-s", running).stdout.strip() == "running")
        waiting, _ = self.submit("waiting")
        before = self.active_ids()
        for jobid in (running, 999999):
            self.assertNotEqual(self.ts("-u", jobid).returncode, 0)
            self.assertEqual(self.active_ids(), before)
        gate.touch()
        self.assertEqual(self.ts("-w", waiting).returncode, 0)
        self.assertNotEqual(self.ts("-u", running).returncode, 0)
        self.assertEqual(self.active_ids(), [])


if __name__ == "__main__":
    unittest.main()
