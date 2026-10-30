# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from __future__ import annotations

import os
import shutil
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
# The harness is a short program; one that spins in a poll loop of the code it runs (vm_file.c's) would
# hang the suite without this.
RUN_TIMEOUT_S = 60


def _cc_command() -> list[str]:
    cmd = shlex.split(os.environ.get("CC", "cc"))
    if not cmd or shutil.which(cmd[0]) is None:
        raise unittest.SkipTest("C compiler not available")
    return cmd


def _sanitizer_flags(cc: list[str], tmp: Path) -> tuple[list[str], bool]:
    """(flags, leak checks work): ASan and UBSan if this compiler can build and run them, and then whether
    LeakSanitizer can too. The leak check is a second question, as in apps_host: macOS aborts on
    detect_leaks=1, which used to turn the sanitizers off here altogether."""
    probe = tmp / "asan_probe"
    flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer"]
    proc = subprocess.run(
        [
            *cc,
            "-x",
            "c",
            "-std=c11",
            *flags,
            "-o",
            str(probe),
            "-",
        ],
        input=b"int main(void) { return 0; }\n",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if proc.returncode != 0:
        return [], False

    def runs(options: str) -> bool:
        done = subprocess.run(
            [str(probe)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={**os.environ, "ASAN_OPTIONS": options},
        )
        return done.returncode == 0

    if not runs("detect_leaks=0:abort_on_error=1"):
        return [], False
    return flags, runs("detect_leaks=1:abort_on_error=1")


class LinkImageFetchHarnessTest(unittest.TestCase):
    def run_harness(self, spiram: bool) -> None:
        cc = _cc_command()
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            binary = tmp / "link_image_fetch_harness"
            wrapped_source = tmp / "image_fetch.c"
            wrapped_source.write_text(
                '#include "image_fetch_alloc.h"\n'
                f'#include "{ROOT / "main" / "image_fetch.c"}"\n'
            )
            # The reader of files on the VM, built the same way. Its writer is
            # never called here; these are all it needs that the harness
            # doesn't have.
            (tmp / "esp_random.h").write_text(
                "#pragma once\n#include <stdint.h>\nuint32_t esp_random(void);\n"
            )
            wrapped_vm_file = tmp / "vm_file.c"
            wrapped_vm_file.write_text(
                '#include "image_fetch_alloc.h"\n'
                f'#include "{ROOT / "main" / "vm_file.c"}"\n'
                "bool noise_ctrl_req_send(int64_t id, const void *data, size_t len,\n"
                "                         bool end_body, int wait_ms) {\n"
                "    (void)id; (void)data; (void)len; (void)end_body; (void)wait_ms;\n"
                "    return false;\n"
                "}\n"
                "bool noise_ctrl_req_cancel_wait(int64_t id, int wait_ms) {\n"
                "    (void)id; (void)wait_ms;\n"
                "    return true;\n"
                "}\n"
                "uint32_t esp_random(void) { return 0; }\n"
            )
            sanitizer_flags, leak_checks = _sanitizer_flags(cc, tmp)
            compile_cmd = [
                *cc,
                "-std=c11",
                "-D_GNU_SOURCE",
                "-Wall",
                "-Wextra",
                "-Werror",
                # The fake ESP_LOG macros drop their arguments.
                "-Wno-unused-parameter",
                "-Wno-unused-variable",
                "-g",
                "-O1",
                *sanitizer_flags,
                "-DLINK_FAKE_CUSTOM_TASKS",
                "-DLINK_FAKE_CUSTOM_HEAP_CAPS",
                "-DCONFIG_HOMEHUB_DAEMON_REQUESTS=1",
                f"-DCONFIG_SPIRAM={int(spiram)}",
                "-I",
                str(tmp),
                "-I",
                str(ROOT / "tests" / "image_fetch_fakes"),
                "-I",
                str(ROOT / "tests" / "link_fakes"),
                "-I",
                str(ROOT / "main"),
                str(ROOT / "tests" / "link_image_fetch_harness.c"),
                str(wrapped_source),
                str(wrapped_vm_file),
                "-o",
                str(binary),
            ]
            compile_proc = subprocess.run(
                compile_cmd,
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(
                compile_proc.returncode,
                0,
                msg=compile_proc.stdout + compile_proc.stderr,
            )

            env = os.environ.copy()
            if sanitizer_flags:
                env["ASAN_OPTIONS"] = f"detect_leaks={int(leak_checks)}:abort_on_error=1"
                env["UBSAN_OPTIONS"] = "halt_on_error=1"
            state = ("OFF (this compiler can't run ASan and UBSan)" if not sanitizer_flags
                     else "ASan+UBSan, leak checks on" if leak_checks
                     else "ASan+UBSan, no leak checks (LeakSanitizer isn't available here)")
            print(f"link_image_fetch: spiram={int(spiram)}: sanitizers {state}", file=sys.stderr, flush=True)
            try:
                run_proc = subprocess.run(
                    [str(binary)],
                    cwd=ROOT,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    env=env,
                    timeout=RUN_TIMEOUT_S,
                )
            except subprocess.TimeoutExpired:
                self.fail(f"the harness ran for more than {RUN_TIMEOUT_S} s: a loop that never ends")
            self.assertEqual(
                run_proc.returncode,
                0,
                msg=run_proc.stdout + run_proc.stderr,
            )

    def test_downloads_and_memory_without_psram(self) -> None:
        self.run_harness(False)

    def test_downloads_and_memory_with_psram(self) -> None:
        self.run_harness(True)


if __name__ == "__main__":
    unittest.main()
