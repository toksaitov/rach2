"""proto_plain.py: the fake API's literal protocol, each request and reply
written out in the step.

A step is an object with a "request" the client must send and the "reply" the
server sends back:

    {"request": {"method": "POST", "path": "/v1/echo",
                 "headers": {"Content-Type": "application/json"},
                 "json": {"say": "ping"}},
     "reply": {"status": 200, "body": "pong\\n"}}

The request's members, each but "path" optional:

    method      the method, GET when left out.
    path        the path, without the query.
    query       the query, after the `?`, compared as written.
    headers     an object of headers the request must have, each with the
                value given; a name is compared in any case, a value exactly.
    json        a value the body, parsed as JSON, must match as
                fakeapi.subset says: an object may have members the step
                leaves out.
    body        the whole body as text.

The reply has a "status", 200 when left out, and "headers", and one of:

    body        a text body, as text/plain unless the headers say otherwise.
    json        a JSON body, as application/json.
    events      server-sent events, each an object with any of "event", "id"
                and "data", sent as fakeapi.Peer.events says.
    websocket   a WebSocket exchange the request must ask to upgrade to, a
                list of actions played in order: {"send": MESSAGE}, a string
                sent as text and any other value as JSON text; {"expect":
                MESSAGE}, the client's next message, which must equal a
                string, or match any other value as fakeapi.subset says once
                parsed; and {"close": CODE}. Without a close at the end, the
                server closes with 1000 after the last action. A message the
                exchange does not expect fails the run, and the server closes
                with 1008.

With no body, json, events or websocket the reply has an empty body.
"""

import json

import fakeapi

REPLY_KINDS = ("body", "json", "events", "websocket")


def validate(step):
    """Return None when STEP has a request and a reply this module can play."""
    request = step.get("request")
    reply = step.get("reply")
    if not isinstance(request, dict) or not isinstance(request.get("path"), str):
        return "the step has no request with a path"
    if not isinstance(reply, dict):
        return "the step has no reply"
    kinds = [k for k in REPLY_KINDS if k in reply]
    if len(kinds) > 1:
        return f"the reply has both {kinds[0]} and {kinds[1]}"
    for action in reply.get("websocket", []):
        if not isinstance(action, dict) or len(action) != 1 or \
                next(iter(action)) not in ("send", "expect", "close"):
            return f"{json.dumps(action)} is not a WebSocket action"
    return None


def check(step, request):
    """Return None when REQUEST is the one STEP describes, else the reason."""
    expected = step["request"]
    method = expected.get("method", "GET")
    if request.method != method:
        return f"the method is {request.method}, not {method}"
    if request.path != expected["path"]:
        return f"the path is {request.path}, not {expected['path']}"
    if "query" in expected and request.query != expected["query"]:
        return f"the query is {request.query!r}, not {expected['query']!r}"
    for name, value in expected.get("headers", {}).items():
        if request.header(name) != value:
            return f"the header {name} is {request.header(name)!r}, not {value!r}"
    if "websocket" in step["reply"] and \
            (request.header("upgrade") or "").lower() != "websocket":
        return "the request does not ask for a WebSocket"
    if "body" in expected and request.text() != expected["body"]:
        return f"the body is {request.text()[:80]!r}, not {expected['body'][:80]!r}"
    if "json" in expected:
        try:
            body = request.json()
        except ValueError:
            return f"the body is not JSON: {request.text()[:80]!r}"
        reason = fakeapi.subset(expected["json"], body)
        if reason:
            return f"in the body, {reason}"
    return None


def exchange(actions, socket, peer):
    """Play the WebSocket ACTIONS; return False when the client strayed."""
    for action in actions:
        if "send" in action:
            message = action["send"]
            socket.send(message if isinstance(message, str) else json.dumps(message))
        elif "close" in action:
            socket.close(action["close"])
            return True
        else:
            expected = action["expect"]
            message = socket.receive()
            if message is None:
                peer.fail(f"the WebSocket closed where the script expects {json.dumps(expected)}")
                return False
            if isinstance(expected, str):
                reason = None if message == expected else \
                    f"the message is {str(message)[:80]!r}, not {expected[:80]!r}"
            else:
                try:
                    reason = fakeapi.subset(expected, json.loads(message))
                except ValueError:
                    reason = f"the message is not JSON: {str(message)[:80]!r}"
            if reason:
                peer.fail(f"a WebSocket message the script does not expect: {reason}")
                socket.close(1008)
                return False
    socket.close(1000)
    return True


def reply(step, request, peer):
    """Answer REQUEST with STEP's reply."""
    answer = step["reply"]
    status = answer.get("status", 200)
    headers = fakeapi.pairs(answer.get("headers", {}))
    if "websocket" in answer:
        exchange(answer["websocket"], peer.websocket(), peer)
    elif "events" in answer:
        peer.events(answer["events"], status, headers)
    elif "json" in answer:
        if not fakeapi.has(headers, "content-type"):
            headers.append(("Content-Type", "application/json"))
        peer.respond(status, headers, json.dumps(answer["json"]))
    else:
        if "body" in answer and not fakeapi.has(headers, "content-type"):
            headers.append(("Content-Type", "text/plain; charset=utf-8"))
        peer.respond(status, headers, answer.get("body", ""))
