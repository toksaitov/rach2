"""Anthropic Messages requests and replies, including streamed tool inputs.

A step has `results`, the earlier tool results it requires, and `reply`, a
Messages content array. Each result has `id` and `contains` (text fragments),
or `equals` (the whole text). `model` defaults to claude-sonnet-4-6. Requests
must advertise the tools in `tools` (names), use the dummy key, and stream.
`stream: false` on a step tests a nonstreaming Messages exchange instead.
`body`, when a step has it, is a value the request's JSON body must match as
fakeapi.subset says, which tells apart a request the client sends for its own
use, such as the title of a session, from a turn of the conversation.
The result checks precede every reply, including the final text answer.

The protocol follows Anthropic's Messages, tool-result and streaming docs:
https://platform.claude.com/docs/en/api/messages
https://platform.claude.com/docs/en/agents-and-tools/tool-use/handle-tool-calls
https://platform.claude.com/docs/en/build-with-claude/streaming
"""

import json

import fakeapi

MODEL = "claude-sonnet-4-6"
KEY = "rach2-offline-dummy-key"


def validate(step):
    """Check the script's content blocks and required tool results."""
    blocks = step.get("reply")
    if not isinstance(blocks, list) or not blocks:
        return "reply must be a nonempty Messages content array"
    for block in blocks:
        if block.get("type") == "text" and isinstance(block.get("text"), str):
            continue
        if block.get("type") == "tool_use" and all(
                isinstance(block.get(k), str) for k in ("id", "name")) and \
                isinstance(block.get("input"), dict):
            continue
        return "reply has an unsupported content block"
    for result in step.get("results", []):
        if not isinstance(result.get("id"), str) or not (
                "contains" in result or "equals" in result):
            return "a required result needs id and contains or equals"
    return None


def result_text(block):
    """Return the text of a string or text-block tool result."""
    content = block.get("content", "")
    if isinstance(content, str):
        return content
    if isinstance(content, list) and all(b.get("type") == "text" for b in content):
        return "\n".join(b["text"] for b in content)
    return None


def check(step, request):
    """Accept a Messages request only after every required result matches."""
    if request.method != "POST" or request.path != "/v1/messages":
        return "expected POST /v1/messages"
    if request.query not in ("", "beta=true"):
        return "unexpected Messages query"
    for name, value in (("x-api-key", KEY), ("anthropic-version", "2023-06-01")):
        if request.header(name) != value:
            return f"unexpected {name} header"
    if not (request.header("content-type") or "").startswith("application/json"):
        return "expected JSON content type"
    try:
        body = request.json()
    except ValueError:
        return "expected a JSON Messages body"
    if body.get("model") != step.get("model", MODEL) or \
            body.get("stream", False) is not step.get("stream", True):
        return "unexpected model or stream setting"
    if not isinstance(body.get("max_tokens"), int) or body["max_tokens"] <= 0:
        return "expected a positive max_tokens"
    tools = {tool.get("name"): tool for tool in body.get("tools", [])}
    for name in step.get("tools", []):
        if name not in tools or not isinstance(tools[name].get("input_schema"), dict):
            return f"missing tool definition: {name}"
    if "body" in step:
        reason = fakeapi.subset(step["body"], body)
        if reason:
            return "unexpected body: " + reason
    results = {}
    calls = {}
    for message in body.get("messages", []):
        content = message.get("content", [])
        if not isinstance(content, list):
            continue
        for block in content:
            if message.get("role") == "assistant" and block.get("type") == "tool_use":
                calls[block["id"]] = block
            if message.get("role") == "user" and block.get("type") == "tool_result":
                ident = block.get("tool_use_id")
                if ident not in calls or ident in results:
                    return "a tool result has no preceding call or is duplicated"
                results[ident] = block
    for expected in step.get("results", []):
        ident = expected["id"]
        block = results.get(ident)
        if block is None or block.get("is_error", False):
            return f"missing or failed tool result: {ident}"
        actual = result_text(block)
        if actual is None or any(text not in actual for text in expected.get("contains", [])) or \
                ("equals" in expected and actual != expected["equals"]):
            return f"wrong tool result: {ident}"
    return None


def events(message):
    """Encode a Message as SSE, splitting text and tool JSON across deltas."""
    start = dict(message, content=[], stop_reason=None)
    yield {"event": "message_start", "data": {"type": "message_start", "message": start}}
    yield {"event": "ping", "data": {"type": "ping"}}
    for index, block in enumerate(message["content"]):
        tool = block["type"] == "tool_use"
        initial = dict(block, input={}) if tool else dict(block, text="")
        yield {"event": "content_block_start", "data": {
            "type": "content_block_start", "index": index, "content_block": initial}}
        value = json.dumps(block["input"]) if tool else block["text"]
        for offset in range(0, len(value), 7):
            delta = {"type": "input_json_delta", "partial_json": value[offset:offset + 7]} \
                if tool else {"type": "text_delta", "text": value[offset:offset + 7]}
            yield {"event": "content_block_delta", "data": {
                "type": "content_block_delta", "index": index, "delta": delta}}
        yield {"event": "content_block_stop", "data": {
            "type": "content_block_stop", "index": index}}
    yield {"event": "message_delta", "data": {"type": "message_delta", "delta": {
        "stop_reason": message["stop_reason"], "stop_sequence": None},
        "usage": {"output_tokens": 32}}}
    yield {"event": "message_stop", "data": {"type": "message_stop"}}


def reply(step, request, peer):
    """Send the step's content as a streamed or whole Messages response."""
    content = step["reply"]
    message = {"id": "msg_" + content[0].get("id", "done"), "type": "message",
               "role": "assistant", "model": step.get("model", MODEL),
               "content": content, "stop_sequence": None,
               "stop_reason": "tool_use" if any(b["type"] == "tool_use" for b in content)
               else "end_turn", "usage": {"input_tokens": 128, "output_tokens": 32}}
    if step.get("stream", True):
        peer.events(events(message))
    else:
        peer.respond(200, [("Content-Type", "application/json")], json.dumps(message))
