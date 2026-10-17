"""proto_responses.py: OpenAI's Responses API, as a model provider serves
it, over HTTPS with server-sent events and in WebSocket mode.

A client creates a response with `POST /v1/responses` and a JSON body whose
`input` is the conversation so far; with `"stream": true` the answer is a
stream of server-sent events, `response.created` first and
`response.completed` last, with the response's output items between. In
WebSocket mode the client opens a WebSocket on `GET /v1/responses` and sends
each body as a text message of type `response.create`; the server answers
with the same events, each a text message. A message with `"generate":
false` is a warmup: it makes a response with no output, which a later one
continues. A body with `previous_response_id` continues that response, and
its `input` has only the items that came after it. A response made with
`"store": false`, as an agent makes them, is kept only in the memory of the
WebSocket that made it, so over HTTPS no response can be continued.

The protocol's steps play these exchanges, the model's side written out as
turns. Each step's request is on the path "/v1/responses" unless the step
has a "path":

    {"websocket": [TURN, ...]}
        A request for a WebSocket that plays each TURN in order, one for
        each `response.create` message that is not a warmup. A warmup is
        answered at any point. Once the last turn is played, the server
        closes the WebSocket with 1000, so a client with more to ask must
        connect again.
    {"refuse": STATUS}
        A request for a WebSocket answered with STATUS, such as 426, and a
        JSON error.
    {"turn": TURN}
        A `POST` of one response, answered with server-sent events.

A turn is an object:

    request   a value the body must match, as fakeapi.subset says, such as
              {"model": "gpt-5.5"}; its `input` is checked by "input".
    input     items the conversation must have, each one of:
                  {"role": ROLE, "text": TEXT}
                      a message of ROLE whose text has TEXT;
                  {"output_of": CALL_ID, "text": TEXT}
                      the output of the tool call CALL_ID, which has TEXT.
              TEXT is a string, or a list of strings that must each be in
              the text. The conversation is the request's input after the
              input of the response it continues and that response's output.
    output    the items the model answers with, each one of:
                  {"message": TEXT}
                      an assistant message;
                  {"call": NAME, "id": CALL_ID, "arguments": VALUE}
                      a call of the function tool NAME, VALUE sent as JSON;
                  {"custom": NAME, "id": CALL_ID, "input": TEXT}
                      a call of the custom tool NAME with the text TEXT.

Every request is checked as the API checks it: a `response.create` message
or a streaming POST, a model named, a continued response that exists, an
output for every tool call in the conversation, and a tool the request
offers for each call the turn makes. A request a step does not accept fails
the run; on a WebSocket the server sends an `error` event and closes with
1008.
"""

import itertools
import json
import threading

import fakeapi

PATH = "/v1/responses"
STEP_KINDS = ("websocket", "refuse", "turn")
OUTPUT_KINDS = ("message", "call", "custom")
CALL_TYPES = {"function_call": "function_call_output",
              "custom_tool_call": "custom_tool_call_output"}

numbers = itertools.count(1)
numbers_lock = threading.Lock()


def new_id(prefix):
    """Return an identifier no other object of this server has."""
    with numbers_lock:
        return f"{prefix}_{next(numbers):04d}"


def validate_turn(turn):
    """Return None when TURN is a turn this module can play."""
    if not isinstance(turn, dict) or not isinstance(turn.get("output"), list):
        return f"{json.dumps(turn)[:80]} is not a turn with an output"
    for item in turn.get("input", []):
        if not isinstance(item, dict) or "text" not in item or \
                ("role" in item) == ("output_of" in item):
            return f"{json.dumps(item)[:80]} is not an input to match"
    for item in turn["output"]:
        kinds = [k for k in OUTPUT_KINDS if k in item] if isinstance(item, dict) else []
        if len(kinds) != 1:
            return f"{json.dumps(item)[:80]} is not an output item"
        if kinds[0] != "message" and not isinstance(item.get("id"), str):
            return f"{json.dumps(item)[:80]} has no call id"
    return None


def validate(step):
    """Return None when STEP is a step this module can play."""
    kinds = [k for k in STEP_KINDS if k in step]
    if len(kinds) != 1:
        return f"the step has {' and '.join(kinds) or 'none'} of {', '.join(STEP_KINDS)}"
    if "refuse" in step:
        return None if isinstance(step["refuse"], int) else "the refusal has no status"
    turns = step["websocket"] if "websocket" in step else [step["turn"]]
    if not isinstance(turns, list) or not turns:
        return "the WebSocket has no turns"
    for turn in turns:
        reason = validate_turn(turn)
        if reason:
            return reason
    return None


def wants_websocket(request):
    """Whether REQUEST asks to be upgraded to a WebSocket."""
    return (request.header("upgrade") or "").lower() == "websocket"


def check(step, request):
    """Return None when REQUEST is the one STEP expects, else the reason."""
    path = step.get("path", PATH)
    if request.path != path:
        return f"the path is {request.path}, not {path}"
    if "turn" in step:
        if request.method != "POST" or wants_websocket(request):
            return f"{request.describe()} is not the POST of a response"
        try:
            body = request.json()
        except ValueError:
            return f"the body is not JSON: {request.text()[:80]!r}"
        if not isinstance(body, dict):
            return "the body is not an object"
        if body.get("stream") is not True:
            return "the request does not ask for a stream of events"
        if "generate" in body:
            return "a warmup over HTTPS, which only WebSocket mode has"
        return check_turn(step["turn"], body, conversation({}, body))
    if request.method != "GET" or not wants_websocket(request):
        return f"{request.describe()} does not ask for a WebSocket"
    return None


def conversation(responses, body):
    """Return the items the model sees for BODY, or a str for the error.

    RESPONSES maps each response that may be continued to the items it was
    made from and its output.
    """
    previous = body.get("previous_response_id")
    items = body.get("input", [])
    if isinstance(items, str):
        items = [{"type": "message", "role": "user",
                  "content": [{"type": "input_text", "text": items}]}]
    if not isinstance(items, list):
        return "the input is not a list of items"
    if previous is None:
        return items
    if previous not in responses:
        return f"the previous response {previous} was not found"
    return responses[previous] + items


def text_of(value):
    """Return the text of a message's content or a tool's output."""
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        return "\n".join(part.get("text", "") for part in value if isinstance(part, dict))
    return ""


def find(items, pattern):
    """Return None when an item of ITEMS matches PATTERN, else the reason."""
    wanted = pattern["text"] if isinstance(pattern["text"], list) else [pattern["text"]]
    if "role" in pattern:
        texts = [text_of(i.get("content")) for i in items
                 if i.get("type", "message") == "message" and i.get("role") == pattern["role"]]
        what = f"a {pattern['role']} message"
    else:
        texts = [text_of(i.get("output")) for i in items
                 if i.get("type") in CALL_TYPES.values() and
                 i.get("call_id") == pattern["output_of"]]
        what = f"the output of {pattern['output_of']}"
        if not texts:
            return f"the conversation has no output of {pattern['output_of']}"
    for text in texts:
        if all(w in text for w in wanted):
            return None
    shown = json.dumps(texts[-1][:300]) if texts else "none"
    return f"{what} with {json.dumps(wanted)} is not in the conversation; the last is {shown}"


def offered(body, item):
    """Whether the request BODY offers the tool that output ITEM calls."""
    kind, name = ("function", item["call"]) if "call" in item else ("custom", item["custom"])
    tools = body.get("tools") or []
    return any(isinstance(t, dict) and t.get("type") == kind and t.get("name") == name
               for t in tools)


def check_turn(turn, body, items):
    """Return None when BODY, with the conversation ITEMS, plays TURN."""
    if isinstance(items, str):
        return items
    if not isinstance(body.get("model"), str):
        return "the request names no model"
    request = dict(turn.get("request", {}))
    request.pop("input", None)
    reason = fakeapi.subset(request, body)
    if reason:
        return f"in the body, {reason}"
    calls = {i.get("call_id") for i in items if isinstance(i, dict) and i.get("type") in CALL_TYPES}
    answered = {i.get("call_id") for i in items
                if isinstance(i, dict) and i.get("type") in CALL_TYPES.values()}
    unanswered = sorted(c for c in calls - answered if c)
    if unanswered:
        return f"no tool output found for the call {unanswered[0]}"
    for pattern in turn.get("input", []):
        reason = find([i for i in items if isinstance(i, dict)], pattern)
        if reason:
            return reason
    for item in turn["output"]:
        if "message" not in item and not offered(body, item):
            return f"the request offers no tool {item.get('call') or item.get('custom')}"
    return None


def output_items(turn):
    """Return the Responses API items of TURN's output, as the model made them."""
    items = []
    for item in turn["output"]:
        if "message" in item:
            items.append({"type": "message", "id": new_id("msg"), "status": "completed",
                          "role": "assistant",
                          "content": [{"type": "output_text", "text": item["message"],
                                       "annotations": []}]})
        elif "call" in item:
            items.append({"type": "function_call", "id": new_id("fc"), "status": "completed",
                          "call_id": item["id"], "name": item["call"],
                          "arguments": json.dumps(item.get("arguments", {}))})
        else:
            items.append({"type": "custom_tool_call", "id": new_id("ctc"), "status": "completed",
                          "call_id": item["id"], "name": item["custom"], "input": item["input"]})
    return items


def events(body, output):
    """Return the stream of events that makes a response with OUTPUT for BODY.

    The response's ID is the `id` of each event's `response`.
    """
    response = {"id": new_id("resp"), "object": "response", "status": "in_progress",
                "model": body.get("model"), "output": []}
    stream = [{"type": "response.created", "response": dict(response)},
              {"type": "response.in_progress", "response": dict(response)}]
    for index, item in enumerate(output):
        started = dict(item, status="in_progress")
        if item["type"] == "message":
            started["content"] = []
        elif item["type"] == "function_call":
            started["arguments"] = ""
        else:
            started["input"] = ""
        stream.append({"type": "response.output_item.added", "output_index": index,
                       "item": started})
        where = {"item_id": item["id"], "output_index": index}
        if item["type"] == "message":
            text = item["content"][0]["text"]
            part = {"type": "output_text", "text": "", "annotations": []}
            stream += [
                dict(where, type="response.content_part.added", content_index=0, part=part),
                dict(where, type="response.output_text.delta", content_index=0, delta=text),
                dict(where, type="response.output_text.done", content_index=0, text=text),
                dict(where, type="response.content_part.done", content_index=0,
                     part=item["content"][0]),
            ]
        elif item["type"] == "function_call":
            stream += [
                dict(where, type="response.function_call_arguments.delta",
                     delta=item["arguments"]),
                dict(where, type="response.function_call_arguments.done",
                     arguments=item["arguments"]),
            ]
        else:
            stream += [
                dict(where, type="response.custom_tool_call_input.delta",
                     call_id=item["call_id"], delta=item["input"]),
                dict(where, type="response.custom_tool_call_input.done",
                     call_id=item["call_id"], input=item["input"]),
            ]
        stream.append({"type": "response.output_item.done", "output_index": index,
                       "item": item})
    stream.append({"type": "response.completed",
                   "response": dict(response, status="completed", output=output,
                                    usage={"input_tokens": 0,
                                           "input_tokens_details": {"cached_tokens": 0},
                                           "output_tokens": 0,
                                           "output_tokens_details": {"reasoning_tokens": 0},
                                           "total_tokens": 0})})
    for number, event in enumerate(stream):
        event["sequence_number"] = number
    return stream


def error(message):
    """Return the API's error object for a request it refuses."""
    return {"type": "invalid_request_error", "code": "fakeapi_unexpected",
            "message": f"fakeapi: {message}", "param": None}


def play(turns, socket, peer):
    """Play TURNS on SOCKET; return False when the client strayed."""
    responses = {}
    index = 0

    def refuse(reason):
        peer.fail(f"turn {index + 1}: {reason}")
        socket.send(json.dumps({"type": "error", "status": 400, "error": error(reason)}))
        socket.close(1008)
        return False

    while index < len(turns):
        message = socket.receive()
        if message is None:
            peer.fail(f"the WebSocket closed where the script has turn {index + 1}")
            return False
        try:
            body = json.loads(message)
        except ValueError:
            return refuse(f"a message that is not JSON: {str(message)[:80]!r}")
        if not isinstance(body, dict) or body.get("type") != "response.create":
            return refuse(f"a message that is not a response.create: {str(message)[:80]!r}")
        items = conversation(responses, body)
        if body.get("generate") is False:
            if isinstance(items, str):
                return refuse(items)
            output = []
        else:
            reason = check_turn(turns[index], body, items)
            if reason:
                return refuse(reason)
            output = output_items(turns[index])
            index += 1
        stream = events(body, output)
        for event in stream:
            socket.send(json.dumps(event))
        responses[stream[0]["response"]["id"]] = items + output
    socket.close(1000)
    return True


def reply(step, request, peer):
    """Answer REQUEST as STEP says."""
    if "refuse" in step:
        status = step["refuse"]
        peer.respond(status, [("Content-Type", "application/json")],
                     json.dumps({"error": error(f"this step answers the WebSocket with {status}")}))
    elif "websocket" in step:
        play(step["websocket"], peer.websocket(), peer)
    else:
        body = request.json()
        stream = events(body, output_items(step["turn"]))
        peer.events([{"event": e["type"], "data": e} for e in stream])
