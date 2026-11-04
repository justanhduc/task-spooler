"""Black-box notification tests; no GPU or real Feishu account required.

Run: TS_BINARY=./build/ts python3 -m unittest discover -s tests -v
"""
import http.server
import json
import os
import re
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest


BINARY = str(Path(os.environ.get("TS_BINARY", "./build/ts")).resolve())


class FeishuTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ts-feishu-", dir="/tmp")
        self.env = dict(os.environ, TS_SOCKET=self.temp.name + "/queue",
                        TMPDIR=self.temp.name, TS_MAXCONN="64")
        self.env.pop("TS_FEISHU_WEBHOOK", None)
        self.env.pop("TS_ENV", None)
        self.env.pop("TS_ONFINISH", None)
        self.messages = []
        self.condition = threading.Condition()
        owner = self

        class Receiver(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                data = self.rfile.read(int(self.headers["Content-Length"]))
                payload = json.loads(data)
                with owner.condition:
                    owner.messages.append((self.path, payload))
                    owner.condition.notify_all()
                if self.path == "/slow":
                    time.sleep(0.5)
                self.send_response(503 if self.path == "/http-error" else 200)
                self.end_headers()
                if self.path == "/invalid-json":
                    self.wfile.write(b"not JSON")
                else:
                    self.wfile.write(json.dumps({
                        "code": 19024 if self.path == "/reject" else 0
                    }).encode())

            def log_message(self, *_):
                pass

        self.receiver = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Receiver)
        self.thread = threading.Thread(target=self.receiver.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.receiver.server_port}/hook?key=a=b"

    def tearDown(self):
        if Path(self.env["TS_SOCKET"]).exists():
            self.ts("-K")
            deadline = time.monotonic() + 15
            while Path(self.env["TS_SOCKET"]).exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertFalse(Path(self.env["TS_SOCKET"]).exists(), "server did not stop")
        self.receiver.shutdown()
        self.receiver.server_close()
        self.thread.join()
        self.temp.cleanup()

    def ts(self, *args):
        return subprocess.run([BINARY, *args], env=self.env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)

    def enable(self, url=None):
        self.assertEqual(self.ts("--set_feishu_webhook", url or self.url).returncode, 0)
        # Read back also synchronizes with the server processing the setter.
        self.assertEqual(self.ts("--get_feishu_webhook").stdout.strip(), url or self.url)

    def wait_messages(self, count):
        with self.condition:
            self.assertTrue(self.condition.wait_for(lambda: len(self.messages) >= count, 10),
                            f"expected {count} notifications, got {self.messages}")

    def events(self):
        statuses = {"任务已提交": "submitted", "任务开始运行": "running",
                    "任务执行成功": "succeeded", "任务执行失败": "failed", "任务已跳过": "skipped"}
        return [next(event for title, event in statuses.items()
                     if title in payload["card"]["header"]["title"]["content"])
                for _, payload in self.messages]

    def card_text(self, index):
        card = self.messages[index][1]["card"]
        return "\n".join(element["content"] for element in card["body"]["elements"]
                         if element["tag"] == "markdown")

    def test_success_and_failure_in_submission_order(self):
        self.enable()
        self.assertEqual(self.ts("-f", "-L", '中文 "label"', "sh", "-c", "exit 0").returncode, 0)
        self.assertNotEqual(self.ts("-f", "sh", "-c", "exit 7").returncode, 0)
        self.wait_messages(6)
        self.assertEqual(self.events(), ["submitted", "running", "succeeded",
                                        "submitted", "running", "failed"])
        self.assertEqual(len(self.messages), 6)
        self.assertIn('中文 "label"', self.card_text(0))
        self.assertIn("**退出码**　`0`", self.card_text(2))
        self.assertIn("**退出码**　`7`", self.card_text(5))
        self.assertRegex(self.card_text(1), r"进程 PID\*\*　`[1-9][0-9]*`")
        self.assertIn("**日志文件**", self.card_text(1))
        self.assertIn("```bash\n", self.card_text(0))
        self.assertEqual(self.messages[2][1]["card"]["header"]["template"], "green")
        self.assertEqual(self.messages[5][1]["card"]["header"]["template"], "red")
        for _, payload in self.messages:
            self.assertEqual(payload["msg_type"], "interactive")
            self.assertEqual(payload["card"]["schema"], "2.0")
        self.assertTrue(all(path.endswith("key=a=b") for path, _ in self.messages))

    def test_disabled_by_default_and_cli_disable(self):
        self.assertEqual(self.ts("--get_feishu_webhook").stdout.strip(), "")
        self.ts("-f", "true")
        self.enable()
        self.ts("--unset_feishu_webhook")
        self.assertEqual(self.ts("--get_feishu_webhook").stdout.strip(), "")
        self.ts("-f", "true")
        self.assertEqual(self.messages, [])

    def test_startup_environment(self):
        self.env["TS_FEISHU_WEBHOOK"] = self.url
        self.ts("-f", "true")
        self.wait_messages(3)
        self.assertEqual(self.events(), ["submitted", "running", "succeeded"])

    def test_slow_webhook_does_not_delay_jobs(self):
        self.enable(self.url.split("/hook")[0] + "/slow")
        start = time.monotonic()
        self.assertEqual(self.ts("-f", "true").returncode, 0)
        self.assertLess(time.monotonic() - start, 0.8)
        self.wait_messages(3)
        self.assertEqual(self.events(), ["submitted", "running", "succeeded"])

    def test_delivery_errors_are_logged_without_affecting_exit_code(self):
        for path, reason in [("reject", "19024"), ("http-error", "503"),
                             ("invalid-json", "invalid JSON")]:
            self.enable(self.url.split("/hook")[0] + "/" + path)
            self.assertEqual(self.ts("-f", "true").returncode, 0)
        self.wait_messages(9)
        # Graceful shutdown drains all messages, including their responses.
        self.ts("-K")
        deadline = time.monotonic() + 10
        while Path(self.env["TS_SOCKET"]).exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        log = Path(self.env["TS_SOCKET"] + ".error").read_text()
        for reason in ["19024", "503", "invalid JSON"]:
            self.assertIn(reason, log)
        self.assertNotIn(self.url, log)

    def test_signal_failure(self):
        self.enable()
        self.assertNotEqual(self.ts("-f", "sh", "-c", "kill -TERM $$").returncode, 0)
        self.wait_messages(3)
        self.assertEqual(self.events(), ["submitted", "running", "failed"])
        self.assertIn("信号 `15`", self.card_text(2))

    def test_dependency_skip_has_no_false_running_event(self):
        self.enable()
        parent = self.ts("sh", "-c", "exit 1").stdout.strip()
        self.ts("-f", "-W", parent, "true")
        self.wait_messages(5)
        child = [event for event, (_, payload) in zip(self.events(), self.messages)
                 if payload["card"]["header"]["title"]["content"].endswith("#1")]
        self.assertEqual(child, ["submitted", "skipped"])

    def test_invalid_url_rejected_before_starting_server(self):
        for url in ["file:///etc/passwd", "http://example.com/hook", "https://", "https://host/\n", "http://localhost:80@example.com/hook"]:
            result = self.ts("--set_feishu_webhook", url)
            self.assertNotEqual(result.returncode, 0)
        self.assertFalse(Path(self.env["TS_SOCKET"]).exists())

    def test_long_command_and_shell_metacharacters_are_literal(self):
        self.enable()
        marker = Path(self.temp.name) / "should-not-exist"
        label = f'$(touch {marker}) `touch {marker}` "quote" \\ 中文'
        self.ts("-f", "-L", label, "printf", "%s", "界" * 3000)
        self.wait_messages(3)
        self.assertIn("中文", self.card_text(0))
        self.assertIn('"quote"', self.card_text(0))
        self.assertFalse(marker.exists())
        command = re.search(r"```bash\n(.*?)\n```", self.card_text(0), re.S).group(1)
        self.assertLessEqual(len(command.encode()), 2048)
        self.assertIn("内容过长，已截断", self.card_text(0))

    def test_markdown_labels_and_embedded_code_fences(self):
        self.enable()
        self.ts("-f", "-L", "**title** <at id=all></at>", "printf", "%s", "```bash\necho test\n```")
        self.wait_messages(3)
        text = self.card_text(0)
        self.assertIn(r"\*\*title\*\*", text)
        self.assertIn("&lt;at id=all&gt;", text)
        self.assertNotIn("<at id=all>", text)
        self.assertIn("````bash\n", text)


if __name__ == "__main__":
    unittest.main()
