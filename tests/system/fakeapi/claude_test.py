#!/usr/bin/env python3
"""Run Claude Code against the scripted Messages server in a disposable home.

Usage: claude_test.py BINARY [--mutate]
       claude_test.py prepare WORK
       claude_test.py client BINARY WORK URL CA
       claude_test.py verify WORK RECORD
       claude_test.py --texts

The prepare, client and verify commands are also the parts a guest suite
uses, with claude-session.json unchanged. WORK is an absolute path. The
caller of those commands removes it. The Linux proof removes its work on
success or a signal, and prints its evidence directory on failure.
"""

import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import signal
import ssl
import subprocess
import sys
import tempfile
import threading
import unittest

import fakeapi
import proto_anthropic

HERE = Path(__file__).resolve().parent
DIGEST = "24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de"
FAULT = 'if actual is None or any(text not in actual for text in expected.get("contains", [])) or \\\n                ("equals" in expected and actual != expected["equals"]):'
REPLACEMENT = 'if False:'


def command(args, cwd, env=None):
    """Run a bounded command and return its output, failing on nonzero status."""
    return subprocess.check_output(args, cwd=cwd, env=env, stderr=subprocess.STDOUT,
                                   text=True, timeout=30)


def prepare(work):
    """Make an empty home, a fixture repository and the rendered script."""
    work = work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    (work / "home").mkdir()
    repo = work / "repo"
    repo.mkdir()
    (repo / "fixture.txt").write_text("rach2 fixture 489\nanswer=before\n")
    env = {"PATH": os.defpath, "HOME": str(work / "home"), "LC_ALL": "C"}
    command(["git", "init", "-q", "--initial-branch=master"], repo, env)
    for name, value in (("user.name", "Rach2 test"), ("user.email", "test@invalid"),
                        ("commit.gpgsign", "false")):
        command(["git", "config", name, value], repo, env)
    command(["git", "add", "fixture.txt"], repo, env)
    command(["git", "commit", "-qm", "Create the offline fixture"], repo, env)
    script = json.loads((HERE / "claude-session.json").read_text().replace(
        "@REPO@", json.dumps(str(repo))[1:-1]))
    (work / "script.json").write_text(json.dumps(script))


def client(binary, work, url, ca):
    """Run the pinned client with fresh settings and only a dummy API key.

    No inherited credential, proxy, preload, plugin or agent configuration
    enters the child. These are the vendor's documented settings:
    https://code.claude.com/docs/en/llm-gateway
    https://code.claude.com/docs/en/network-config
    https://code.claude.com/docs/en/env-vars
    https://code.claude.com/docs/en/cli-reference
    """
    binary = str(Path(binary).resolve())
    if hashlib.sha256(Path(binary).read_bytes()).hexdigest() != DIGEST:
        raise ValueError("the binary is not Anthropic's linux-x64 Claude Code 2.1.296")
    env = {"PATH": os.defpath, "HOME": str(work / "home"), "LC_ALL": "C", "TERM": "dumb",
           "TMPDIR": str(work), "ANTHROPIC_BASE_URL": url,
           "ANTHROPIC_API_KEY": proto_anthropic.KEY, "NODE_EXTRA_CA_CERTS": str(ca),
           "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC": "1", "DISABLE_UPDATES": "1",
           "CLAUDE_CODE_DISABLE_OFFICIAL_MARKETPLACE_AUTOINSTALL": "1",
           "CLAUDE_CODE_ENABLE_PROMPT_SUGGESTION": "false"}
    args = [binary, "-p", "Read fixture.txt, check it with the shell, change answer=before "
            "to answer=after, and commit the change.", "--model", proto_anthropic.MODEL,
            "--permission-mode", "bypassPermissions", "--tools", "Read,Bash,Edit",
            "--strict-mcp-config", "--mcp-config", '{"mcpServers":{}}',
            "--setting-sources", "user", "--settings", '{"sandbox":{"enabled":false}}',
            "--no-session-persistence", "--max-turns", "8"]
    with (work / "client.log").open("w") as output:
        child = subprocess.Popen(args, cwd=work / "repo", env=env, stdout=output,
                                 stderr=subprocess.STDOUT, start_new_session=True)
        try:
            return child.wait(timeout=120)
        finally:
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


def verify(work, record):
    """Judge the file, git tree, commit and recorded tool results."""
    repo = work / "repo"
    expected = "rach2 fixture 489\nanswer=after\n"
    if (repo / "fixture.txt").read_text() != expected:
        raise ValueError("wrong edited fixture")
    env = {"PATH": os.defpath, "HOME": str(work / "home"), "LC_ALL": "C"}
    checks = [(["git", "status", "--porcelain"], ""),
              (["git", "rev-list", "--count", "HEAD"], "2\n"),
              (["git", "log", "-1", "--format=%s"], "Update the offline fixture\n"),
              (["git", "show", "HEAD:fixture.txt"], expected),
              (["git", "diff-tree", "--no-commit-id", "--name-only", "-r", "HEAD"],
               "fixture.txt\n")]
    for args, wanted in checks:
        if command(args, repo, env) != wanted:
            raise ValueError("wrong git result: " + " ".join(args))
    entries = [json.loads(line) for line in record.read_text().splitlines()]
    if len(entries) != 5 or [e.get("step") for e in entries] != list(range(1, 6)):
        raise ValueError("the record is not five accepted Messages requests")
    script = json.loads((work / "script.json").read_text())
    for step, entry in zip(script["steps"], entries):
        request = fakeapi.Request(entry["method"], entry["path"] +
                                  ("?" + entry["query"] if entry["query"] else ""),
                                  "HTTP/1.1", entry["headers"], entry["body"].encode())
        reason = proto_anthropic.check(step, request)
        if reason:
            raise ValueError("the recorded exchange failed: " + reason)


def exchange(binary, work, wrong=False, module=None):
    """Play one real client session; require wrong results to be refused."""
    prepare(work)
    if wrong:
        script = json.loads((work / "script.json").read_text())
        script["steps"][2]["results"][0]["contains"] = ["WRONG_SHELL_RESULT_489"]
        (work / "script.json").write_text(json.dumps(script))
    ca = work / "ca"
    if fakeapi.make_ca(str(ca), ["127.0.0.1"]) != 0:
        raise ValueError("could not make the test CA")
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(ca / "server.pem", ca / "server.key")
    record = work / "record.jsonl"
    server = fakeapi.Server(json.loads((work / "script.json").read_text()), str(record), context)
    if module:
        server.module = lambda step: module
    listener = fakeapi.Listener(("127.0.0.1", 0), fakeapi.Connection)
    listener.fake = server
    thread = threading.Thread(target=listener.serve_forever, daemon=True)
    thread.start()
    try:
        status = client(binary, work, f"https://127.0.0.1:{listener.server_address[1]}", ca / "ca.pem")
    finally:
        listener.shutdown()
        listener.server_close()
        thread.join()
        server.record_file.close()
    verdict = server.verdict()
    if wrong:
        if not verdict or server.played != 2 or not any(
                "wrong tool result: toolu_shell" in f for f in server.failures):
            raise ValueError("a wrong tool result was accepted")
        print("anthropic-test: result: wrong tool result refused", flush=True)
    else:
        if status or verdict:
            raise ValueError(f"client status {status}, server status {verdict}: " +
                             (work / "client.log").read_text())
        verify(work, record)
        print("anthropic-test: result: file, commit and all tool results passed", flush=True)


def texts():
    """Check that the mutation still matches its source once."""
    source = (HERE / "proto_anthropic.py").read_text()
    if source.count(FAULT) != 1:
        raise ValueError("the wrong-result mutation is not once in proto_anthropic.py")
    return source


def proof(binary, mutate):
    """Prove the exchange, its refusal check and optionally its mutation."""
    work = Path(tempfile.mkdtemp(prefix="rach2-anthropic."))
    keep = False
    try:
        exchange(binary, work / "fixed")
        exchange(binary, work / "wrong", wrong=True)
        if mutate:
            import types
            module = types.ModuleType("mutated_anthropic")
            exec(compile(texts().replace(FAULT, REPLACEMENT), str(HERE / "proto_anthropic.py"),
                         "exec"), module.__dict__)
            import anthropic_test
            original = anthropic_test.protocol
            try:
                anthropic_test.protocol = module
                suite = unittest.defaultTestLoader.loadTestsFromModule(anthropic_test)
                result = unittest.TextTestRunner(stream=io.StringIO()).run(suite)
                if result.wasSuccessful() or len(result.failures) != 1 or result.errors:
                    raise ValueError("the protocol test did not catch the result mutation")
            finally:
                anthropic_test.protocol = original
            print("anthropic-test: result: protocol test caught the mutation", flush=True)
            try:
                exchange(binary, work / "mutated", wrong=True, module=module)
            except ValueError as error:
                if str(error) != "a wrong tool result was accepted":
                    raise
                print("anthropic-test: result: wrong-result mutation caught", flush=True)
            else:
                raise ValueError("the mutation did not fail its check")
            exchange(binary, work / "restored")
    except Exception:
        keep = True
        print(f"anthropic-test: evidence: {work}", file=sys.stderr)
        raise
    finally:
        if not keep:
            shutil.rmtree(work)


def main(args):
    """Dispatch the Linux proof or a part shared with the guest suite."""
    if args == ["--texts"]:
        texts()
        return
    if args[0] == "prepare":
        prepare(Path(args[1]))
    elif args[0] == "client":
        sys.exit(client(args[1], Path(args[2]), args[3], Path(args[4])))
    elif args[0] == "verify":
        verify(Path(args[1]), Path(args[2]))
    else:
        if "--offline-child" not in args:
            os.execvp("unshare", [
                "unshare", "--user", "--map-current-user", "--keep-caps", "--net",
                "sh", "-ec", 'ip link set lo up; exec "$@"', "sh", sys.executable,
                "-B", str(Path(__file__).resolve()), *args, "--offline-child"])
        proof(args[0], "--mutate" in args[1:])


if __name__ == "__main__":
    def interrupted(signum, frame):
        """Unwind child and scratch cleanup when a signal ends the proof."""
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGHUP, interrupted)
    try:
        main(sys.argv[1:])
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"anthropic-test: result: fail: {error}", file=sys.stderr)
        sys.exit(1)
