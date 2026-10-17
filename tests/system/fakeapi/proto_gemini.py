"""Gemini API generateContent requests and streamed replies with function calls.

A step has `reply`, a list of Gemini content parts, each {"text": TEXT} or
{"functionCall": {"id", "name", "args"}}, and `results`, the function
responses it requires: each has `id` and `contains` (text fragments) or
`equals` (the whole text), compared with the response's `output`. `model`
defaults to gemini-3.1-pro-preview; `system` and `prompt` are fragments the
system instruction and the first user turn must contain. `stream: false` on a
step tests a whole generateContent response instead of server-sent events.
The result checks precede every reply, including the final text answer.

A function call is sent with a thought signature, and the request that
answers it must return the call with the same signature, as Gemini 3 models
require. Requests must carry the dummy key in `x-goog-api-key`.

The protocol follows the Gemini API's generateContent, function calling and
thought signature docs:
https://ai.google.dev/api/generate-content
https://ai.google.dev/gemini-api/docs/function-calling
https://ai.google.dev/gemini-api/docs/thought-signatures
"""

import base64
import json

MODEL = "gemini-3.1-pro-preview"
KEY = "rach2-offline-dummy-key"


def signature(ident):
    """Return the thought signature sent with the call IDENT."""
    return base64.b64encode(("rach2-signature-" + ident).encode()).decode()


def validate(step):
    """Check the script's parts and required function responses."""
    parts = step.get("reply")
    if not isinstance(parts, list) or not parts:
        return "reply must be a nonempty list of content parts"
    for part in parts:
        if set(part) == {"text"} and isinstance(part["text"], str):
            continue
        call = part.get("functionCall")
        if set(part) == {"functionCall"} and isinstance(call, dict) and all(
                isinstance(call.get(k), str) for k in ("id", "name")) and \
                isinstance(call.get("args"), dict):
            continue
        return "reply has an unsupported content part"
    for result in step.get("results", []):
        if not isinstance(result.get("id"), str) or not (
                "contains" in result or "equals" in result):
            return "a required result needs id and contains or equals"
    return None


def texts(content):
    """Return the text parts of a Content object, joined."""
    return "".join(p.get("text", "") for p in content.get("parts", [])
                   if isinstance(p, dict) and not p.get("thought"))


def result_text(response):
    """Return a function response's output or error as text."""
    value = response.get("output", response.get("error"))
    if isinstance(value, str):
        return value
    return None if value is None else json.dumps(value)


def check(step, request):
    """Accept a generateContent request only after every required result matches."""
    model = step.get("model", MODEL)
    method = "streamGenerateContent" if step.get("stream", True) else "generateContent"
    if request.method != "POST" or request.path != f"/v1beta/models/{model}:{method}":
        return f"expected POST /v1beta/models/{model}:{method}"
    if request.query != ("alt=sse" if step.get("stream", True) else ""):
        return "unexpected generateContent query"
    if request.header("x-goog-api-key") != KEY:
        return "unexpected x-goog-api-key header"
    if not (request.header("content-type") or "").startswith("application/json"):
        return "expected JSON content type"
    try:
        body = request.json()
    except ValueError:
        return "expected a JSON generateContent body"
    contents = body.get("contents")
    if not isinstance(contents, list) or not contents or \
            contents[0].get("role") != "user":
        return "expected contents that begin with a user turn"
    if "system" in step and step["system"] not in texts(body.get("systemInstruction", {})):
        return "the system instruction lacks the step's fragment"
    if "prompt" in step and step["prompt"] not in texts(contents[0]):
        return "the first user turn lacks the step's prompt"
    declared = {f.get("name") for tool in body.get("tools", [])
                for f in tool.get("functionDeclarations", [])}
    for name in step.get("tools", []):
        if name not in declared:
            return f"missing function declaration: {name}"
    calls = {}
    results = {}
    for content in contents:
        for part in content.get("parts", []):
            call = part.get("functionCall")
            response = part.get("functionResponse")
            if content.get("role") == "model" and isinstance(call, dict):
                ident = call.get("id")
                if part.get("thoughtSignature") != signature(ident or ""):
                    return f"a call lacks its thought signature: {ident}"
                calls[ident] = call
            if isinstance(response, dict):
                ident = response.get("id")
                if ident not in calls or ident in results or \
                        response.get("name") != calls[ident].get("name"):
                    return "a function response has no preceding call or is duplicated"
                results[ident] = response.get("response", {})
    for expected in step.get("results", []):
        ident = expected["id"]
        response = results.get(ident)
        if response is None or "error" in response:
            return f"missing or failed function response: {ident}"
        actual = result_text(response)
        if actual is None or any(text not in actual for text in expected.get("contains", [])) or \
                ("equals" in expected and actual != expected["equals"]):
            return f"wrong function response: {ident}"
    return None


def chunks(step):
    """Return the step's reply as GenerateContentResponse chunks.

    Text is split across several chunks; a function call is sent whole, with
    its signature, in a chunk of its own, and the last chunk has the finish
    reason and the usage.
    """
    out = []
    for part in step["reply"]:
        if "text" in part:
            value = part["text"]
            out += [{"text": value[i:i + 7]} for i in range(0, len(value), 7)]
        else:
            ident = part["functionCall"]["id"]
            out.append(dict(part, thoughtSignature=signature(ident)))
    responses = [{"candidates": [{"content": {"role": "model", "parts": [p]},
                                  "index": 0}],
                  "modelVersion": step.get("model", MODEL),
                  "responseId": "rach2-" + str(len(out))} for p in out]
    responses[-1]["candidates"][0]["finishReason"] = "STOP"
    responses[-1]["usageMetadata"] = {"promptTokenCount": 128,
                                      "candidatesTokenCount": 32,
                                      "totalTokenCount": 160}
    return responses


def reply(step, request, peer):
    """Send the step's parts as streamed or whole generateContent responses."""
    responses = chunks(step)
    if step.get("stream", True):
        peer.events({"data": r} for r in responses)
        return
    parts = [p for r in responses for p in r["candidates"][0]["content"]["parts"]]
    whole = dict(responses[-1])
    whole["candidates"] = [dict(whole["candidates"][0],
                                content={"role": "model", "parts": parts})]
    peer.respond(200, [("Content-Type", "application/json")], json.dumps(whole))
