#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Run a command in a session of its own and exit with its status.

test_runner.py ends a --failfast run by sending SIGKILL to its whole
process group. On the macOS jobs the CI runs on the GitHub runner itself,
where that group can include the runner's own processes, so one failing
functional test killed the runner and the job's log was never uploaded.
In a new session the command's process group holds only its own tree.
"""

import os
import sys

pid = os.fork()
if pid == 0:
    os.setsid()
    os.execvp(sys.argv[1], sys.argv[1:])
_, status = os.waitpid(pid, 0)
if os.WIFEXITED(status):
    sys.exit(os.WEXITSTATUS(status))
sys.exit(128 + os.WTERMSIG(status))
