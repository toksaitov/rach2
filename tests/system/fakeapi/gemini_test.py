#!/usr/bin/env python3
"""Check Gemini function results, signatures, replies and unordered steps."""

import base64
import copy
import json
import os
import tempfile
import unittest

import fakeapi
import proto_gemini as protocol


class GeminiTest(unittest.TestCase):
    """Exercise protocol errors without downloading a client."""

    def setUp(self):
        """Make a call and its matching function response."""
        self.step = {"results": [{"id": "call_1", "equals": "expected\n"}],
                     "reply": [{"text": "done"}]}
        self.call = {"functionCall": {"id": "call_1", "name": "run_command",
                                      "args": {"CommandLine": "printf expected"}},
                     "thoughtSignature": protocol.signature("call_1")}
        self.response = {"functionResponse": {"id": "call_1", "name": "run_command",
                                              "response": {"output": "expected\n"}}}
        self.body = {"contents": [{"role": "user", "parts": [{"text": "go"}]},
                                  {"role": "model", "parts": [self.call]},
                                  {"role": "model", "parts": [self.response]}]}

    def check(self, step=None, path=None):
        """Check the current request against STEP, the setUp step by default."""
        path = path or f"/v1beta/models/{protocol.MODEL}:streamGenerateContent?alt=sse"
        request = fakeapi.Request("POST", path, "HTTP/1.1",
                                  [("x-goog-api-key", protocol.KEY),
                                   ("content-type", "application/json")],
                                  json.dumps(self.body).encode())
        return protocol.check(step or self.step, request)

    def test_results(self):
        """Only a successful response to a preceding call may match."""
        self.assertIsNone(self.check())
        output = self.response["functionResponse"]["response"]
        output["output"] = "wrong"
        self.assertEqual(self.check(), "wrong function response: call_1")
        output["output"] = "expected\n"
        output["error"] = "failed"
        self.assertIn("failed function response", self.check())
        output.pop("error")
        self.body["contents"][1]["parts"] = []
        self.assertIn("no preceding call", self.check())
        self.body["contents"][1]["parts"] = [self.call]
        self.body["contents"].append(copy.deepcopy(self.body["contents"][2]))
        self.assertIn("duplicated", self.check())

    def test_signature_and_request(self):
        """A call must come back signed, to the step's model, with the key."""
        self.call["thoughtSignature"] = base64.b64encode(b"other").decode()
        self.assertIn("thought signature", self.check())
        self.call["thoughtSignature"] = protocol.signature("call_1")
        self.assertIn("expected POST", self.check(path="/v1beta/models/other:"
                                                  "streamGenerateContent?alt=sse"))
        self.assertIn("prompt", self.check(dict(self.step, prompt="absent")))
        self.assertIn("system", self.check(dict(self.step, system="absent")))
        self.assertIn("declaration", self.check(dict(self.step, tools=["view_file"])))

    def test_stream_and_json(self):
        """Both response forms preserve the text and the whole call."""
        for stream in (True, False):
            step = {"stream": stream, "reply": [
                {"text": "Checking the fixture."}, {"functionCall": self.call["functionCall"]}]}
            self.assertIsNone(protocol.validate(step))
            captured = []

            class Peer:
                """Collect one response for reconstruction."""

                def events(self, events):
                    """Collect the complete SSE sequence."""
                    captured.extend(e["data"] for e in events)

                def respond(self, status, headers, body):
                    """Collect one whole JSON response."""
                    captured.append(json.loads(body))

            protocol.reply(step, None, Peer())
            parts = [p for r in captured for p in r["candidates"][0]["content"]["parts"]]
            self.assertEqual("".join(p.get("text", "") for p in parts), "Checking the fixture.")
            calls = [p for p in parts if "functionCall" in p]
            self.assertEqual(calls, [self.call])
            self.assertEqual(captured[-1]["candidates"][0]["finishReason"], "STOP")
            self.assertEqual(len(captured), 4 if stream else 1)

    def test_unordered(self):
        """An unordered step plays when its request comes, before or after."""
        title = {"unordered": True, "model": "lite", "reply": [{"text": "Title"}]}
        main = {"model": "main", "reply": [{"text": "done"}]}
        record = tempfile.NamedTemporaryFile(delete=False)
        record.close()
        try:
            for order in (("main", "lite"), ("lite", "main")):
                server = fakeapi.Server({"protocol": "gemini",
                                         "steps": [title, main]}, record.name, None)
                played = []
                for model in order + ("main",):
                    request = fakeapi.Request(
                        "POST", f"/v1beta/models/{model}:streamGenerateContent?alt=sse",
                        "HTTP/1.1", [("x-goog-api-key", protocol.KEY),
                                     ("content-type", "application/json")],
                        json.dumps({"contents": [{"role": "user", "parts": []}]}).encode())
                    played.append(server.take(request)[2])
                server.record_file.close()
                self.assertEqual(played, [2, 1, None] if order[0] == "main" else [1, 2, None])
                self.assertEqual(server.played, 2)
        finally:
            os.unlink(record.name)


if __name__ == "__main__":
    unittest.main()
