"""Card formatting boundaries without sleeping for long jobs or contacting Feishu."""
import json
import os
import re
import subprocess
import unittest

BINARY = os.environ.get("TS_CARD_TEST_BINARY")


@unittest.skipUnless(BINARY, "set TS_CARD_TEST_BINARY to the card fixture")
class CardFormatTests(unittest.TestCase):
    def text(self, seconds, signal=False):
        p = subprocess.run([BINARY, str(seconds), str(int(signal))], text=True, capture_output=True, check=True)
        payload = json.loads(p.stdout)
        return "\n".join(row["content"] for row in payload["card"]["body"]["elements"])

    def test_human_readable_duration_boundaries(self):
        for value, expected in [(0,"0分0秒"),(.4,"0分0秒"),(59.9,"0分59秒"),
                                (60,"1分0秒"),(125,"2分5秒"),(3599,"59分59秒"),
                                (3600,"1小时0分"),(86399,"23小时59分"),
                                (86400,"1天0小时0分"),(183845,"2天3小时4分")]:
            with self.subTest(seconds=value):
                self.assertIn("**运行耗时**　" + expected, self.text(value))
                self.assertIn("**运行耗时**　" + expected, self.text(value, signal=True))

    def test_queued_notification_shows_candidates_without_fake_allocation(self):
        p = subprocess.run([BINARY, "0", "0", "submitted"], text=True, capture_output=True, check=True)
        card = json.loads(p.stdout)["card"]
        text = "\n".join(row["content"] for row in card["body"]["elements"])
        self.assertIn("任务已提交", card["header"]["title"]["content"])
        self.assertIn("**候选 GPU**　0, 2", text)
        self.assertNotIn("**分配 GPU**", text)
        self.assertNotIn("**运行耗时**", text)

    def test_time_has_minute_precision_and_gpu_constraints_are_distinct(self):
        text = self.text(1)
        event_time = next(line.split("　",1)[1] for line in text.splitlines() if "**事件时间**" in line)
        self.assertRegex(event_time, r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}$")
        self.assertIn("**候选 GPU**　0, 2", text)
        self.assertIn("**分配 GPU**　2", text)


if __name__ == "__main__": unittest.main()
