#!/usr/bin/env python3
"""Check Messages result rejection and streamed content reconstruction.

Each test that fails is named on a line of its own.
"""

import copy
import json
import sys
import unittest

import fakeapi
import proto_anthropic as protocol


class MessagesTest(unittest.TestCase):
    """Exercise protocol errors without downloading a client."""

    def setUp(self):
        """Make a call and its matching user result."""
        self.step = {"results": [{"id": "toolu_1", "equals": "expected\n"}],
                     "reply": [{"type": "text", "text": "done"}]}
        self.call = {"type": "tool_use", "id": "toolu_1", "name": "Bash",
                     "input": {"command": "printf expected"}}
        self.result = {"type": "tool_result", "tool_use_id": "toolu_1",
                       "content": "expected\n"}
        self.body = {"model": protocol.MODEL, "max_tokens": 512, "stream": True,
                     "messages": [{"role": "assistant", "content": [self.call]},
                                  {"role": "user", "content": [self.result]}]}

    def check(self):
        """Check the current request and step."""
        request = fakeapi.Request("POST", "/v1/messages?beta=true", "HTTP/1.1",
                                  [("x-api-key", protocol.KEY),
                                   ("anthropic-version", "2023-06-01"),
                                   ("content-type", "application/json")],
                                  json.dumps(self.body).encode())
        return protocol.check(self.step, request)

    def test_results(self):
        """Only a successful result of a preceding call may match."""
        self.assertIsNone(self.check())
        for content in ("wrong", [{"type": "text", "text": "wrong"}]):
            self.result["content"] = content
            self.assertEqual(self.check(), "wrong tool result: toolu_1")
        self.result["content"] = [{"type": "text", "text": "expected\n"}]
        self.assertIsNone(self.check())
        self.result["is_error"] = True
        self.assertIn("failed tool result", self.check())
        self.result.pop("is_error")
        self.body["messages"][0]["content"] = []
        self.assertIn("no preceding call", self.check())
        self.body["messages"][0]["content"] = [self.call]
        self.body["messages"][1]["content"].append(copy.deepcopy(self.result))
        self.assertIn("duplicated", self.check())

    def test_body(self):
        """A step's body must match the request's as fakeapi.subset says."""
        self.step["body"] = {"tools": [], "output_config": {"format": {"type": "json_schema"}}}
        self.assertIn("unexpected body: $ has no member", self.check())
        self.body["tools"] = [{"name": "Bash", "input_schema": {}}]
        self.body["output_config"] = {"effort": "high", "format": {"type": "json_schema"}}
        self.assertIn("unexpected body: $.tools is", self.check())
        self.body["tools"] = []
        self.assertIsNone(self.check())

    def test_stream_and_json(self):
        """Both response forms preserve text and complete tool arguments."""
        for stream in (True, False):
            step = {"stream": stream, "reply": [
                {"type": "text", "text": "Checking the fixture."}, self.call]}
            self.assertIsNone(protocol.validate(step))
            captured = []

            class Peer:
                """Collect one response for reconstruction."""

                def events(self, events):
                    """Collect the complete SSE sequence."""
                    captured.extend(events)

                def respond(self, status, headers, body):
                    """Collect one whole JSON message."""
                    captured.append(json.loads(body))

            protocol.reply(step, None, Peer())
            if not stream:
                self.assertEqual(captured[0]["content"], step["reply"])
                self.assertEqual(captured[0]["stop_reason"], "tool_use")
                continue
            self.assertEqual(captured[0]["event"], "message_start")
            self.assertEqual(captured[-1]["event"], "message_stop")
            self.assertEqual(captured[-2]["data"]["delta"]["stop_reason"], "tool_use")
            deltas = [e["data"] for e in captured if e["event"] == "content_block_delta"]
            text = "".join(d["delta"]["text"] for d in deltas if d["index"] == 0)
            args = "".join(d["delta"]["partial_json"] for d in deltas if d["index"] == 1)
            self.assertEqual(text, step["reply"][0]["text"])
            self.assertEqual(json.loads(args), self.call["input"])
            self.assertGreater(len(deltas), 2)


if __name__ == "__main__":
    result = unittest.main(exit=False).result
    for kind, cases in (("failed", result.failures), ("raised an error", result.errors)):
        for case, _ in cases:
            print(f"anthropic-test: fail: {case.id().rsplit('.', 1)[1]}: {kind}", file=sys.stderr)
    sys.exit(not result.wasSuccessful())
