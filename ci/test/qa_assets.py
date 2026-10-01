#!/usr/bin/env python3
# Copyright (c) The ConnectCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""Acquire the complete pinned QA assets checkout, including lazy Git blobs."""

import argparse
from contextlib import ExitStack
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import tempfile
import time


REMOTE = "https://github.com/bitcoin-core/qa-assets"
NETWORK_ATTEMPTS = 3
NETWORK_TIMEOUT = 600
LOCAL_TIMEOUT = 60


def stop_process_tree(process):
    """Git may be waiting for remote-https/index-pack children on timeout."""
    tree_error = None
    if os.name == "nt":
        try:
            result = subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                                    check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
            if result.returncode and process.poll() is None:
                tree_error = RuntimeError(f"taskkill exited with {result.returncode}")
        except (OSError, subprocess.TimeoutExpired) as failure:
            tree_error = failure
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    if process.poll() is None:
        process.kill()
    process.wait(timeout=15)
    if tree_error is not None:
        raise RuntimeError("Could not confirm termination of the QA assets Git process tree") from tree_error


def git(arguments, *, network=False, capture=False):
    command = ["git", "-c", "http.version=HTTP/1.1", *map(str, arguments)]
    environment = os.environ.copy()
    # These apply to lazy promisor fetches launched by checkout as well.
    environment.update(GIT_TERMINAL_PROMPT="0", GIT_HTTP_LOW_SPEED_LIMIT="1024", GIT_HTTP_LOW_SPEED_TIME="60")
    attempts = NETWORK_ATTEMPTS if network else 1
    timeout = NETWORK_TIMEOUT if network else LOCAL_TIMEOUT
    for attempt in range(1, attempts + 1):
        print(f"+ {shlex.join(command)} (attempt {attempt}/{attempts}, timeout {timeout}s)", flush=True)
        options = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt" else {"start_new_session": True}
        with ExitStack() as stack:
            # A descendant can inherit output handles. PIPE readers (including
            # Windows communicate threads) can then block indefinitely on close.
            # Temporary files keep process waiting independent of output EOF.
            output = stack.enter_context(tempfile.TemporaryFile(mode="w+t", encoding="utf8")) if capture else None
            errors = stack.enter_context(tempfile.TemporaryFile(mode="w+t", encoding="utf8")) if capture else None
            # Do not use Popen's context manager: its unbounded __exit__ wait
            # would defeat the cleanup deadline if process termination failed.
            process = subprocess.Popen(command, env=environment, stdout=output, stderr=errors, **options)
            try:
                process.wait(timeout=timeout)
                stdout, stderr = "", ""
                if capture:
                    output.seek(0)
                    errors.seek(0)
                    stdout, stderr = output.read(), errors.read()
                if process.returncode:
                    raise subprocess.CalledProcessError(process.returncode, command, stdout, stderr)
                return stdout.strip()
            except subprocess.TimeoutExpired as failure:
                stop_process_tree(process)
                # Forced termination can leave an index/shallow lock. Never
                # delete a lock of uncertain ownership or blindly retry it.
                raise RuntimeError(
                    f"QA assets Git operation exceeded {timeout}s: {shlex.join(command)}. "
                    "Forced termination may leave Git locks; inspect the preserved checkout and its lock files before retrying."
                ) from failure
            except subprocess.CalledProcessError as failure:
                error = failure
        if attempt == attempts:
            raise error
        print(f"QA assets Git operation failed: {error}; retrying in {attempt * 2}s", flush=True)
        time.sleep(attempt * 2)


def acquire(directory, commit, *, remote=REMOTE):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("QA assets pin must be a full lowercase commit hash")
    directory = Path(directory).absolute()
    current_head = ""
    if directory.is_symlink():
        raise ValueError(f"QA assets path must not be a symlink: {directory}")
    if directory.exists():
        if not (directory / ".git").is_dir() or (directory / ".git").is_symlink():
            raise ValueError(f"QA assets path exists but is not a Git checkout: {directory}")
        if Path(git(["-C", directory, "rev-parse", "--show-toplevel"], capture=True)).resolve() != directory.resolve():
            raise ValueError(f"QA assets path is not a checkout root: {directory}")
        if git(["-C", directory, "remote", "get-url", "origin"], capture=True) != remote:
            raise ValueError(f"QA assets checkout has an unexpected origin: {directory}")
        try:
            current_head = git(["-C", directory, "rev-parse", "--verify", "HEAD"], capture=True)
        except subprocess.CalledProcessError:
            # An interrupted initial fetch leaves an initialized, unborn repo.
            pass
    else:
        # Init then fetch makes failed downloads resumable without deleting a
        # partial clone. Fetch only the pin, never a moving branch or its history.
        git(["init", directory])
        git(["-C", directory, "remote", "add", "origin", remote])
    if current_head != commit:
        git(["-C", directory, "fetch", "--depth=1", "--no-tags", "origin", commit], network=True)
    # Even matching HEAD can have an unpopulated worktree after a no-checkout
    # clone. Existing partial clones may download promised blobs here.
    git(["-C", directory, "checkout", "--detach", commit], network=True)
    actual = git(["-C", directory, "rev-parse", "HEAD"], capture=True)
    if actual != commit:
        raise RuntimeError(f"QA assets checkout is {actual}, expected {commit}")
    # Checkout preserves local tracked edits/deletions, including when HEAD
    # already matches. Fail closed rather than silently replaying less corpus
    # or overwriting local data. Untracked generated seeds remain available.
    entries = git(["-C", directory, "ls-files", "-v", "-z"], capture=True).split("\0")
    if any(entry and (entry[0] == "S" or entry[0].islower()) for entry in entries):
        raise RuntimeError("QA assets index hides tracked files (sparse checkout or assume-unchanged)")
    if git(["-C", directory, "status", "--porcelain", "--untracked-files=no"], capture=True):
        raise RuntimeError("QA assets has tracked changes or missing files; preserving the checkout without running fuzz")
    if not (directory / "fuzz_corpora").is_dir():
        raise RuntimeError(f"QA assets checkout has no fuzz_corpora directory: {directory}")
    print(f"Using complete QA assets checkout at {commit}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    pin = Path(__file__).resolve().parents[1] / "qa-assets-commit.txt"
    acquire(args.directory, pin.read_text(encoding="utf8").strip())


if __name__ == "__main__":
    main()
