#!/usr/bin/env python3
"""Run Antigravity CLI against the scripted Gemini API server in a disposable home.

Usage: agy_test.py TARBALL [--mutate]
       agy_test.py prepare WORK
       agy_test.py client BINARY WORK URL CA
       agy_test.py verify WORK RECORD
       agy_test.py --texts

TARBALL is Google's linux-x64 cli_linux_x64.tar.gz of Antigravity CLI 1.4.0,
and BINARY the `antigravity` program in it, which the installer names `agy`.
The prepare, client and verify commands are also the parts a guest suite
uses, with agy-session.json unchanged. WORK is an absolute path. The caller
of those commands removes it. The Linux proof removes its work on success or
a signal, and prints its evidence directory on failure.
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
import tarfile
import tempfile
import threading
import types
import unittest

import fakeapi
import proto_gemini

HERE = Path(__file__).resolve().parent
# The SHA-512 that Google's manifest gives for the tarball, and the SHA-256
# of the program in it.
TARBALL = "6c4b298c2a69b6bd0f58567031cc56d8097a32be54c4b633aa80ad395871b389618ab3860d4ba0a12599831b8226b51b03e258ee2ec7fae105a45b8e0fce2188"
DIGEST = "60e72242f07c131f75afcd9a9e425d6ee097a1a1b3190ea98ae78fa4180bbfa8"
FAULTS = (
    ("proto_gemini.py",
     'if actual is None or any(text not in actual for text in expected.get("contains", [])) or \\\n                ("equals" in expected and actual != expected["equals"]):',
     'if False:'),
    ("fakeapi.py",
     'return ordered[:1] + [i for i in left if self.steps[i].get("unordered")]',
     'return ordered[:1]'))
PROMPT = ("Read fixture.txt, check it with the shell, change answer=before "
          "to answer=after, and commit the change.")


def command(args, cwd, env=None):
    """Run a bounded command and return its output, failing on nonzero status."""
    return subprocess.check_output(args, cwd=cwd, env=env, stderr=subprocess.STDOUT,
                                   text=True, timeout=30)


def prepare(work):
    """Make a home with the API key settings, a fixture repository and the script.

    The settings are the vendor's documented ones for a Gemini API key, with
    telemetry off: https://antigravity.google/docs/install?tab=cli and
    https://antigravity.google/docs/cli/reference/
    """
    work = work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    settings = work / "home" / ".gemini" / "antigravity-cli"
    settings.mkdir(parents=True)
    (settings / "settings.json").write_text(json.dumps(
        {"modelProvider": "gemini", "enableTelemetry": False}))
    repo = work / "repo"
    repo.mkdir()
    (repo / "fixture.txt").write_text("rach2 fixture 493\nanswer=before\n")
    env = {"PATH": os.defpath, "HOME": str(work / "home"), "LC_ALL": "C"}
    command(["git", "init", "-q", "--initial-branch=master"], repo, env)
    for name, value in (("user.name", "Rach2 test"), ("user.email", "test@invalid"),
                        ("commit.gpgsign", "false")):
        command(["git", "config", name, value], repo, env)
    command(["git", "add", "fixture.txt"], repo, env)
    command(["git", "commit", "-qm", "Create the offline fixture"], repo, env)
    script = json.loads((HERE / "agy-session.json").read_text().replace(
        "@REPO@", json.dumps(str(repo))[1:-1]))
    (work / "script.json").write_text(json.dumps(script))


def client(binary, work, url, ca):
    """Run the pinned client headless with a fresh home and only a dummy key.

    No inherited credential, proxy or preload enters the child. The key, the
    server and the update setting are Google's documented ones; the
    certificate authority is the Go runtime's documented SSL_CERT_FILE, since
    the program is Go and Google documents no setting of its own for it:
    https://antigravity.google/docs/install?tab=cli
    https://antigravity.google/docs/cli/headless/
    https://antigravity.google/docs/cli/troubleshooting/
    https://pkg.go.dev/crypto/x509#SystemCertPool
    """
    binary = str(Path(binary).resolve())
    with open(binary, "rb") as f:
        if hashlib.file_digest(f, "sha256").hexdigest() != DIGEST:
            raise ValueError("the binary is not Google's linux-x64 Antigravity CLI 1.4.0")
    env = {"PATH": os.defpath, "HOME": str(work / "home"), "LC_ALL": "C", "TERM": "dumb",
           "TMPDIR": str(work), "GEMINI_API_KEY": proto_gemini.KEY,
           "GOOGLE_GEMINI_BASE_URL": url, "SSL_CERT_FILE": str(ca),
           "AGY_CLI_DISABLE_AUTO_UPDATE": "true"}
    args = [binary, "-p", PROMPT, "--output-format", "json",
            "--dangerously-skip-permissions", "--print-timeout", "2m"]
    with (work / "client.log").open("w") as output:
        child = subprocess.Popen(args, cwd=work / "repo", env=env, stdout=output,
                                 stderr=subprocess.STDOUT, start_new_session=True)
        try:
            return child.wait(timeout=180)
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
    """Judge the file, git tree, commit and recorded function responses."""
    repo = work / "repo"
    expected = "rach2 fixture 493\nanswer=after\n"
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
    script = json.loads((work / "script.json").read_text())
    if sorted(e.get("step", 0) for e in entries) != list(range(1, len(script["steps"]) + 1)):
        raise ValueError("the record is not one accepted request for each step")
    for entry in entries:
        request = fakeapi.Request(entry["method"], entry["path"] +
                                  ("?" + entry["query"] if entry["query"] else ""),
                                  "HTTP/1.1", entry["headers"], entry["body"].encode())
        reason = proto_gemini.check(script["steps"][entry["step"] - 1], request)
        if reason:
            raise ValueError("the recorded exchange failed: " + reason)


def exchange(binary, work, wrong=False, module=None):
    """Play one real client session; require a wrong result to be refused."""
    prepare(work)
    if wrong:
        script = json.loads((work / "script.json").read_text())
        script["steps"][3]["results"][0]["contains"] = ["WRONG_SHELL_RESULT_493"]
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
        if not verdict or server.played != 3 or not any(
                "wrong function response: call_shell" in f for f in server.failures) or \
                "answer=before" not in (work / "repo" / "fixture.txt").read_text():
            raise ValueError("a wrong function response was accepted")
        print("agy-test: result: wrong function response refused", flush=True)
    else:
        if status or verdict:
            raise ValueError(f"client status {status}, server status {verdict}: " +
                             (work / "client.log").read_text())
        verify(work, record)
        print("agy-test: result: file, commit and all function responses passed", flush=True)


def texts():
    """Check that each mutation still matches its source once."""
    for name, fault, _ in FAULTS:
        if (HERE / name).read_text().count(fault) != 1:
            raise ValueError(f"a mutation is not once in {name}")


def mutated(name, module_name):
    """Return a private copy of the module in NAME with its fault put in."""
    _, fault, replacement = next(f for f in FAULTS if f[0] == name)
    module = types.ModuleType(module_name)
    module.__file__ = str(HERE / name)
    exec(compile((HERE / name).read_text().replace(fault, replacement),
                 str(HERE / name), "exec"), module.__dict__)
    return module


def unit(attribute, module):
    """Run gemini_test with its module ATTRIBUTE replaced by MODULE."""
    import gemini_test
    original = getattr(gemini_test, attribute)
    try:
        setattr(gemini_test, attribute, module)
        suite = unittest.defaultTestLoader.loadTestsFromModule(gemini_test)
        return unittest.TextTestRunner(stream=io.StringIO()).run(suite)
    finally:
        setattr(gemini_test, attribute, original)


def caught(attribute, module):
    """Whether one test of gemini_test fails, and no other, with MODULE."""
    result = unit(attribute, module)
    return len(result.failures) == 1 and not result.errors


def unpack(tarball, work):
    """Check the vendor's tarball by the manifest's SHA-512 and unpack its program."""
    with open(tarball, "rb") as f:
        if hashlib.file_digest(f, "sha512").hexdigest() != TARBALL:
            raise ValueError("the tarball is not Google's linux-x64 Antigravity CLI 1.4.0")
    work.mkdir()
    with tarfile.open(tarball) as archive:
        archive.extract("antigravity", work, filter="data")
    return work / "antigravity"


def proof(tarball, mutate):
    """Prove the exchange, its refusal check and optionally its mutations."""
    work = Path(tempfile.mkdtemp(prefix="rach2-agy."))
    keep = False
    try:
        binary = unpack(tarball, work / "bin")
        exchange(binary, work / "fixed")
        exchange(binary, work / "wrong", wrong=True)
        if mutate:
            texts()
            module = mutated("proto_gemini.py", "mutated_gemini")
            if not caught("protocol", module):
                raise ValueError("the protocol test did not catch the result mutation")
            print("agy-test: result: protocol test caught the result mutation", flush=True)
            try:
                exchange(binary, work / "mutated", wrong=True, module=module)
            except ValueError as error:
                if str(error) != "a wrong function response was accepted":
                    raise
                print("agy-test: result: wrong-result mutation caught", flush=True)
            else:
                raise ValueError("the mutation did not fail its check")
            if not caught("fakeapi", mutated("fakeapi.py", "mutated_fakeapi")):
                raise ValueError("the protocol test did not catch the unordered mutation")
            print("agy-test: result: protocol test caught the unordered mutation", flush=True)
            if not unit("protocol", proto_gemini).wasSuccessful():
                raise ValueError("the restored protocol test did not pass")
            exchange(binary, work / "restored")
    except Exception:
        keep = True
        print(f"agy-test: evidence: {work}", file=sys.stderr)
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
    except (ValueError, OSError, subprocess.SubprocessError, tarfile.TarError) as error:
        print(f"agy-test: result: fail: {error}", file=sys.stderr)
        sys.exit(1)
