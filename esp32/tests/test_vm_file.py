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

"""vm_file: the reader of files on the VM (GET /fs/raw) and the writer (POST /fs/upload).

tests/vm_file_harness.c runs main/vm_file.c against a fake daemon on the Noise session, a clock and a heap it
controls, and each case there is one test here. A case checks what it can in C (every byte the sink or a
read gets is compared with the file's pattern, every allocation is counted, every lock taken is given, and
the daemon reads what the writer sends as a form, as the VM does, and says what it wrote) and prints the
rest as @-lines, which these tests assert: the ranges asked for, what the sink was given, the head of the form
the writer sent byte for byte, the size of each send, and how each part read. vm_file.c is built the way
test_link_image_fetch.py builds image_fetch.c, with its allocations going through the harness's counting
allocator, once with PSRAM and once without. A third build adds image_fetch.c, for the cases where it and
the reader meet: who holds the reader, and what a download says when a later range doesn't come.
"""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

import apps_host

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"
TESTS = ROOT / "tests"
HARNESS = TESTS / "vm_file_harness.c"
# The firmware's options come as -D flags, as in test_link_image_fetch.py.
SDKCONFIG = "#pragma once\n"
# vm_file.c's log lines are checked for their format, which the real macros do too.
ESP_LOG = ("#pragma once\n"
           "void fake_esp_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));\n"
           "#define ESP_LOGE(tag, ...) fake_esp_log('E', tag, __VA_ARGS__)\n"
           "#define ESP_LOGW(tag, ...) fake_esp_log('W', tag, __VA_ARGS__)\n"
           "#define ESP_LOGI(tag, ...) fake_esp_log('I', tag, __VA_ARGS__)\n")
ESP_TIMER = "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n"
ESP_RANDOM = "#pragma once\n#include <stdint.h>\nuint32_t esp_random(void);\n"
# image_fetch.c logs the stack it has left, which its own harness never evaluates.
TASK = '#pragma once\n#include "{fake}"\nunsigned uxTaskGetStackHighWaterMark(TaskHandle_t task);\n'
# The allocator hooks of image_fetch's wrapper: malloc, free and the others go to the harness's counting ones.
WRAPPER = '#include "image_fetch_alloc.h"\n#include "{source}"\n'
PIECE = 4096
SEND_MAX = 8192     # what the session takes in one send


def pat(i: int) -> int:
    """The byte at offset i of the file the daemon serves, and of the data a case writes."""
    return (i * 167 + (i >> 8) * 13 + 11) & 0xFF


def unescape(text: str) -> str:
    """The inverse of the harness's put_escaped."""
    out = []
    i = 0
    while i < len(text):
        c = text[i]
        if c != "\\":
            out.append(c)
            i += 1
        elif text[i + 1] == "x":
            out.append(chr(int(text[i + 2:i + 4], 16)))
            i += 4
        else:
            out.append({"\\": "\\", "t": "\t", "r": "\r", "n": "\n"}[text[i + 1]])
            i += 2
    return "".join(out)


def form_head(boundary: str, path: str, filename: str) -> str:
    """The bytes before the file's: the text parts path, overwrite and create_parent, in this order, and the
    headers of the file part."""
    def field(name, value):
        return f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n'
    return (field("path", path) + field("overwrite", "true") + field("create_parent", "true")
            + f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="{filename}"\r\n'
              "Content-Type: application/octet-stream\r\n\r\n")


class Part:
    def __init__(self, name, filename, content_type, length, text):
        self.name, self.filename, self.content_type, self.length, self.text = name, filename, content_type, length, text


class Upload:
    """What the fake daemon saw of one POST /fs/upload: the @up lines a case printed under a label."""

    def __init__(self, out, label):
        self.parts = []
        self.malformed = None
        found = False
        for rest in out.all("up"):
            fields = rest.split("\t")
            if fields[0] != label:
                continue
            found = True
            kind = fields[1]
            if kind == "open":
                self.path, self.content_type = fields[2], fields[3]
            elif kind == "sends":
                self.end_at, self.late = int(fields[2]), int(fields[3])
                self.sends = [int(n) for n in fields[4].split(",") if n]
            elif kind == "head":
                self.head = unescape(fields[2])
            elif kind == "tail":
                self.tail = unescape(fields[2])
            elif kind == "part":
                self.parts.append(Part(fields[2], None if fields[3] == "-" else fields[3],
                                       None if fields[4] == "-" else fields[4], int(fields[5]), unescape(fields[6])))
            elif kind == "malformed":
                self.malformed = fields[2]
        assert found, f"no upload printed under {label!r}"

    @property
    def boundary(self):
        return self.content_type.partition("boundary=")[2]


class Output:
    """The @-lines a case printed: @tag rest."""

    def __init__(self, stdout: bytes):
        self.lines = []
        for raw in stdout.decode("utf-8").split("\n"):
            if raw.startswith("@"):
                tag, _, rest = raw[1:].partition(" ")
                self.lines.append((tag, rest))
            else:
                self.lines.append((None, raw))

    def all(self, tag):
        return [rest for t, rest in self.lines if t == tag]

    def one(self, tag):
        found = self.all(tag)
        assert len(found) == 1, f"@{tag}: {len(found)} lines: {self.lines}"
        return found[0]

    def upload(self, label):
        return Upload(self, label)


class VmFileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        root = Path(tmp.name)
        staged = root / "staged"
        staged.mkdir()
        (staged / "sdkconfig.h").write_text(SDKCONFIG)
        (staged / "esp_log.h").write_text(ESP_LOG)
        (staged / "esp_timer.h").write_text(ESP_TIMER)
        (staged / "esp_random.h").write_text(ESP_RANDOM)
        (staged / "freertos").mkdir()
        (staged / "freertos" / "task.h").write_text(TASK.format(fake=TESTS / "link_fakes/freertos/task.h"))
        wrapped = root / "vm_file.c"
        wrapped.write_text(WRAPPER.format(source=MAIN / "vm_file.c"))
        wrapped_image = root / "image_fetch.c"
        wrapped_image.write_text(WRAPPER.format(source=MAIN / "image_fetch.c"))
        cls.binaries = {}
        for name, spiram, image in (("psram", 1, 0), ("nopsram", 0, 0), ("image", 1, 1)):
            cls.binaries[name] = apps_host.build(
                f"vm_file_harness_{name}", [HARNESS, wrapped, *([wrapped_image] if image else [])], root,
                include_dirs=[staged, TESTS / "image_fetch_fakes", TESTS / "link_fakes", MAIN],
                defines=["CONFIG_HOMEHUB_DAEMON_REQUESTS=1", f"CONFIG_SPIRAM={spiram}", f"WITH_IMAGE_FETCH={image}",
                         *(["CONFIG_HOMEHUB_DISPLAY_COMMANDS=1"] if image else []),
                         "LINK_FAKE_CUSTOM_TASKS", "LINK_FAKE_CUSTOM_HEAP_CAPS"])

    def run_case(self, case, *args, board="psram"):
        done = apps_host.run(self.binaries[board], case, *args, text=False, timeout=60)
        stderr = done.stderr.decode("utf-8", errors="replace")
        self.assertEqual(done.returncode, 0, f"{case}: {stderr}{done.stdout.decode('utf-8', errors='replace')[-2000:]}")
        self.assertEqual(done.stdout.decode("utf-8").splitlines()[-1], "ok")
        return Output(done.stdout)

    # ---- The reader ----

    def test_read_all_streams_ranges_to_sink(self):  # a 40 KB file, a 16 KB range: three calls
        out = self.run_case("read_all_streams_ranges_to_sink")
        self.assertEqual(out.all("sink"), ["16384", "16384", "8192"])
        self.assertEqual(out.all("get"), ["bytes=0-16383", "bytes=16384-32767", "bytes=32768-49151"])
        self.assertEqual(out.one("bytes"), str(40 * 1024))   # and the harness compared every byte

    def test_sink_abort_cancels(self):  # the rest of the file is not asked for
        out = self.run_case("sink_abort_cancels")
        self.assertEqual(out.one("error"), "larger than 1048576 bytes")
        self.assertEqual(out.all("sink"), ["16384", "16384"])
        self.assertEqual(out.all("get"), ["bytes=0-16383", "bytes=16384-32767"])

    def test_max_exceeded(self):
        out = self.run_case("max_exceeded")
        self.assertEqual(out.one("error"), "larger than 20000 bytes")
        # No range that would take the file past max reaches the sink.
        self.assertEqual(out.all("sink"), ["16384"])
        self.assertEqual(out.all("get"), ["bytes=0-16383", "bytes=16384-32767"])
        # Exactly max is fine, a byte more is not; max 0 takes an empty file and refuses a byte.
        self.assertEqual(out.all("limit"), ["0 1 ", "1 0 larger than 40959 bytes"])
        self.assertEqual(out.all("zero"), ["0 1 0 ", "1 0 0 larger than 0 bytes"])

    def test_second_reader_is_busy(self):  # with each owner tag, the other's words
        self.run_case("second_reader_is_busy")

    def test_a_short_206_is_the_end_of_the_file(self):
        # A reply carries no length of the file, so a range shorter than asked for ends it.
        out = self.run_case("short_206_is_the_end")
        self.assertEqual(out.all("sink"), ["10000"])
        self.assertEqual(out.all("get"), ["bytes=0-16383"])
        self.assertEqual(out.one("bytes"), "10000")

    def test_416_at_the_end(self):
        out = self.run_case("416_at_the_end")
        self.assertEqual(out.all("sink"), ["16384", "16384"])
        self.assertEqual(out.all("get"), ["bytes=0-16383", "bytes=16384-32767", "bytes=32768-49151"])
        self.assertEqual(out.one("empty"), "0 1")   # no sink call, one request

    def test_a_200_to_a_ranged_get(self):
        out = self.run_case("200_to_a_ranged_get")
        # A daemon that ignores Range sends a file that fits one range whole, once.
        self.assertEqual(out.all("sink"), ["5000"])
        self.assertEqual(out.one("requests"), "1")
        # A larger one overflows the buffer: nothing is given to the sink, the rest is cancelled.
        self.assertEqual(out.one("big"), "0 1 the VM ignored Range and sent the whole file")
        # A 200 at an offset is not the rest of the file.
        self.assertEqual(out.one("late"), "1 VM answered HTTP 200")   # the first range had gone to the sink

    def test_session_drop_mid_read(self):
        out = self.run_case("session_drop_mid_read")
        self.assertEqual(out.all("sink"), ["16384"])
        self.assertEqual(out.one("error"), "the VM connection dropped")
        self.assertEqual(out.one("read"), "-1 the VM connection dropped")
        self.assertEqual(out.one("first"), "the VM connection dropped")
        self.assertEqual(out.one("large"), "the VM sent a frame too large for this board")
        self.assertEqual(out.one("down"), "could not send the request to the VM 0 0")   # nothing sent, nothing to cancel

    def test_an_http_error_is_named(self):
        out = self.run_case("http_error")
        self.assertEqual(out.all("error"), ["VM answered HTTP 404", "VM answered HTTP 404"])

    def test_a_read_times_out_at_its_deadline(self):
        out = self.run_case("read_timeout")
        self.assertEqual(out.one("timeout"), "1 download timed out 1000000")   # exactly the deadline, in µs
        self.assertEqual(out.one("cancels"), "1")   # the request still out is cancelled
        self.assertEqual(out.one("past"), "1 1")
        # Once it has timed out, a request that can't be sent says so, not that it timed out again.
        self.assertEqual(out.one("gone"), "1 could not send the request to the VM")

    def test_late_frames_are_dropped(self):
        self.run_case("late_frames_are_dropped")

    def test_read_in_small_pieces(self):
        out = self.run_case("read_in_small_pieces")
        self.assertEqual(out.one("read"), "40960 9 3")   # 8 reads of 5000, one of 960, over 3 ranges
        self.assertEqual(len(out.all("get")), 3)

    def test_read_then_read_all(self):  # read_all starts with what is left of the range read from
        out = self.run_case("read_then_read_all")
        self.assertEqual(out.all("sink"), ["16284", "16384", "8192"])
        self.assertEqual(len(out.all("get")), 3)

    # ---- Opening ----

    def test_open_refusals(self):
        self.run_case("open_refusals")

    def test_open_counts_keep(self):
        self.run_case("open_counts_keep")
        self.run_case("open_counts_keep", board="nopsram")

    def test_open_chunk_under_pressure(self):
        out = self.run_case("open_chunk_under_pressure")
        self.assertEqual(out.one("chunk"), "10240")   # PSRAM keeps the largest range
        out = self.run_case("open_chunk_under_pressure", board="nopsram")
        self.assertLess(int(out.one("chunk")), 10240)  # without it the range shrinks to fit

    def test_request_path_is_encoded(self):
        self.run_case("request_path_is_encoded")

    # ---- The writer ----

    def test_write_upload_multipart(self):
        # 10000 binary bytes, with a zero, line breaks and dashes in them, to a path with a space and a letter
        # that isn't ASCII: one POST /fs/upload, a form with the three text parts before the file.
        path = "workspace/apps/a b/ü.lua"
        out = self.run_case("write_upload_multipart", path)
        up = out.upload("up")
        self.assertEqual(up.path, "/fs/upload")
        self.assertRegex(up.boundary, r"^[0-9A-Za-z'()+_,./:=?-]{1,70}$")
        self.assertTrue(up.content_type.startswith("multipart/form-data; boundary="), up.content_type)
        self.assertEqual([part.name for part in up.parts], ["path", "overwrite", "create_parent", "file"])
        self.assertEqual([part.text for part in up.parts[:3]], [path, "true", "true"])
        self.assertEqual([part.filename for part in up.parts], [None, None, None, "ü.lua"])
        self.assertEqual(up.parts[3].content_type, "application/octet-stream")
        self.assertEqual(up.parts[3].length, 10000)          # its bytes are compared with the data in C
        self.assertIsNone(up.malformed)
        # What went out: the head of the form, the file in pieces of at most 4096 bytes, the closing boundary.
        self.assertEqual(up.head, form_head(up.boundary, path, "ü.lua"))
        self.assertEqual(up.sends[0], len(up.head.encode()))
        self.assertEqual(up.sends[1:-1], [4096, 4096, 1808])
        self.assertEqual(up.tail, f"\r\n--{up.boundary}--\r\n")
        self.assertEqual(up.sends[-1], len(up.tail))
        self.assertTrue(all(size <= SEND_MAX for size in up.sends), up.sends)
        self.assertEqual(up.end_at, len(up.sends) - 1)        # end_body on the last send, and no other
        self.assertEqual(up.late, 0)

    def test_write_boundary_collision(self):
        # The data holds the first boundary the writer makes: it makes another and the file still arrives. So
        # when the path does, and when the first two are in the data. When all four tries are, it says so.
        out = self.run_case("write_boundary_collision")
        self.assertEqual(out.one("second"), "1")
        self.assertEqual(out.one("exhausted"), "couldn't pick a boundary the data doesn't contain|0|16")

    def test_write_short_count(self):
        # The write is only done when the VM says it wrote every byte.
        out = self.run_case("write_short_count")
        self.assertEqual(out.one("short"), "the VM wrote 9999 of 10000 bytes|5|0|0")
        self.assertEqual(out.one("long"), "the VM wrote 10001 of 10000 bytes|5|0|0")
        self.assertEqual(out.one("short_busy"), "the VM wrote 9997 of 10000 bytes|5|0|0")
        self.assertEqual(out.one("none"), "the VM didn't say how many bytes it wrote|5|0|0")
        self.assertEqual(out.one("string"), "the VM didn't say how many bytes it wrote|5|0|0")
        self.assertEqual(out.one("counted"), "12")      # three wordings, each in frames of 0, 1, 3 and 7 bytes
        self.assertEqual(out.one("huge"), "the VM wrote 4294967295 of 10000 bytes|5|0|0")   # too many digits for a number

    def test_write_bad_path(self):
        out = self.run_case("write_bad_path")
        refused = dict(line.split("\t") for line in out.all("refused"))
        self.assertEqual(sorted(refused), sorted(["/etc/passwd", "/", "/workspace/a.txt", "..", "../x", "a/..",
                                                  "a/../b", "a/b/../../c", "x/y/.."]))
        self.assertEqual(set(refused.values()), {"vm_file_write: bad path"})
        nameless = dict(line.split("\t") for line in out.all("nameless"))
        self.assertEqual(sorted(nameless), ["a/b/", "dir/"])
        self.assertEqual(set(nameless.values()), {"path names no file"})
        # ".." inside a name is a name, and "." and an empty folder go to the VM as they are.
        self.assertEqual(out.all("allowed"), ["a..b", "..a", "a/..b/c", "a/b..", ".hidden", "a/./b", "a//b", "...",
                                              "a/.../b"])

    def test_write_filename_is_escaped(self):
        out = self.run_case("write_filename_is_escaped")
        up = out.upload("name")
        path = 'dir/a"b\r\nc d%41é.txt'
        name = 'a%22b%0D%0Ac d%41é.txt'
        self.assertEqual(up.parts[0].text, path)          # the path goes as it is
        self.assertEqual(up.parts[3].filename, name)
        self.assertEqual(up.head, form_head(up.boundary, path, name))
        self.assertIsNone(up.malformed)

    def test_write_zero_4096_and_4097_bytes(self):
        out = self.run_case("write_zero_4096_4097")
        for size in (0, 1, 4095, 4096, 4097, 8192, 8193):
            with self.subTest(size=size):
                up = out.upload(str(size))
                pieces = [PIECE] * (size // PIECE) + ([size % PIECE] if size % PIECE else [])
                # The head, the file's pieces (none for no bytes, and no empty one after a whole one), the end.
                self.assertEqual(up.sends[1:-1], pieces)
                self.assertEqual(len(up.sends), 2 + len(pieces))
                self.assertEqual(up.parts[3].length, size)
                self.assertEqual(up.end_at, len(up.sends) - 1)
                self.assertIsNone(up.malformed)

    def test_write_http_error(self):
        out = self.run_case("write_http_error")
        # The whole upload went out, and the VM said no; its words follow the status when they fit.
        self.assertEqual(out.one("e500"), "VM answered HTTP 500: boom|5|0|0")
        # It said no after the head: nothing more is sent, and there is nothing to cancel.
        self.assertEqual(out.one("e403"),
                         "VM answered HTTP 403: route not allowed for this credential's scopes|1|0|0")
        self.assertEqual(out.one("e404"), 'VM answered HTTP 404: {"ok":false,"error":{"code":"not_found",'
                                          '"message":"path not found"}}|5|0|0')
        self.assertEqual(out.one("e_long"), "VM answered HTTP 500|5|0|0")      # too long for the room there is
        self.assertEqual(out.one("e_kept"), "VM answered HTTP 500")            # more than the writer keeps
        self.assertEqual(out.one("e_clean"), "VM answered HTTP 500: bad path ? ??|5|0|0")   # only what prints, trimmed
        self.assertEqual(out.one("e_empty"), "VM answered HTTP 500|5|0|0")
        self.assertEqual(out.one("e302"), "VM answered HTTP 302|5|0|0")
        self.assertEqual(out.one("e204"), "the VM didn't say how many bytes it wrote|3|0|0")   # a 2xx is not enough

    def test_write_session_drop(self):
        out = self.run_case("write_session_drop")
        self.assertEqual(out.one("drop"), "the VM connection dropped|2|0|1")        # no send after it, and a cancel
        self.assertEqual(out.one("drop_late"), "the VM connection dropped|5|0|1")
        self.assertEqual(out.one("down"), "could not send the request to the VM 0 0")
        self.assertEqual(out.one("queue"), "could not send the request to the VM|2|0|1")        # the request is cancelled
        self.assertEqual(out.one("queue_first"), "could not send the request to the VM|0|0|1")
        self.assertEqual(out.one("large"), "the VM sent a frame too large for this board")

    def test_write_timeout(self):
        out = self.run_case("write_timeout")
        self.assertEqual(out.one("stall"), "write timed out 1 1 1000000")   # exactly timeout_ms, the request cancelled
        # The sends count: the third piece isn't sent, and each was told it could wait for the time left.
        self.assertEqual(out.one("slow"), "write timed out|3|0|1|1200000|1000,600,200")
        self.assertEqual(out.one("waited"), "write timed out|0|1")          # a send that waited out the time
        self.assertEqual(out.one("quick"), "could not send the request to the VM|0|1")   # one that failed at once
        self.assertEqual(out.one("total"), "write timed out 1 1 1000000")   # an answer that comes too late
        self.assertEqual(out.one("none"), "write timed out 0")
        self.assertEqual(out.one("negative"), "write timed out 0")

    def test_write_waits_for_the_whole_reply(self):
        out = self.run_case("write_waits_for_the_whole_reply")
        self.assertEqual(out.one("head"), "write timed out 1 1")   # a head of 200 is not the end, and is cancelled

    def test_write_is_one_at_a_time(self):
        out = self.run_case("write_one_at_a_time")
        self.assertEqual(out.one("second"), "0 the VM file writer is busy")
        self.assertEqual(out.one("first"), "1")

    def test_a_reader_and_a_writer_do_not_share_a_reply(self):
        out = self.run_case("reader_and_writer_are_independent")
        self.assertEqual(out.one("write"), "1 ")
        self.assertEqual(out.all("sink"), ["16384", "16384", "8192"])
        self.assertEqual(out.one("posts"), "1")

    def test_write_late_frames_are_dropped(self):
        out = self.run_case("write_late_frames_are_dropped")
        self.assertEqual(out.one("late"), "write timed out")

    def test_write_path_limits(self):
        out = self.run_case("write_path_limits")
        # The head of the form is one send, of 4096 bytes at most, which every board's session takes:
        # exactly that is written, and one byte more is refused.
        self.assertEqual(out.all("limit"), ["4096 1 - 1 4096", "4097 0 path is too long 0 0"])

    def test_write_arguments(self):
        out = self.run_case("write_arguments")
        self.assertEqual(out.one("null"), "path names no file")
        self.assertEqual(out.one("empty"), "path names no file")
        self.assertEqual(out.one("folder"), "path names no file")
        self.assertEqual(out.one("nodata"), "no data to write")
        self.assertEqual(out.one("cleared"), "[]")      # a write that works leaves err empty
        self.assertEqual(out.one("tiny"), "no da")      # the reason is cut to the room there is
        self.assertEqual(out.one("small"), "VM answ")   # and so is the VM's

    def test_write_memory(self):
        out = self.run_case("write_memory")
        self.assertEqual(out.one("lock"), "out of memory 0")
        self.assertEqual(out.one("alloc"), "out of memory 0")
        self.run_case("write_memory", board="nopsram")

    def test_the_writer_takes_both_kinds_of_board(self):
        # Nothing in the writer depends on PSRAM, except where its buffer goes, and no send is over what a
        # board without it takes (the harness refuses a send of more than 4096 bytes there).
        out = self.run_case("write_upload_multipart", "a.txt", board="nopsram")
        up = out.upload("up")
        self.assertEqual(up.sends[1:-1], [4096, 4096, 1808])
        self.assertTrue(all(size <= PIECE for size in up.sends), up.sends)

    # ---- With image_fetch.c ----

    def test_a_download_is_refused_while_an_install_reads(self):
        out = self.run_case("image_refused_while_an_install_reads", board="image")
        self.assertEqual(out.one("refused"), "busy | the VM file reader is busy (an app install)")
        self.assertEqual(out.one("drawn"), "5 80 1")   # then, the reader free, the same download draws 5 rows

    def test_an_install_is_refused_while_an_image_downloads(self):
        out = self.run_case("install_refused_while_an_image_downloads", board="image")
        self.assertEqual(out.one("install"), "0 busy | the VM file reader is busy (an image download)")
        # The download's own flag is still what stops a second download, VM file or URL.
        self.assertEqual(out.one("again"), "0 busy | another image is still downloading")
        self.assertEqual(out.one("http"), "0 busy | another image is still downloading")

    def test_a_download_that_stalls_in_a_later_range_timed_out(self):
        out = self.run_case("image_stalls_in_a_later_range", board="image")
        self.assertEqual(out.one("late"),
                         "download_failed | download timed out | rows 1024 | cancels 1 | bytes 16384 | done 1")
        # The first range is the reader's to explain, and a download that has nothing draws nothing.
        self.assertEqual(out.one("first"), "download_failed | VM answered HTTP 404 | rows 0 | done 0 | bytes 0")
        self.assertEqual(out.one("silent"), "download_failed | download timed out | cancels 1")

    def test_a_download_needs_a_block_for_its_stack(self):
        out = self.run_case("image_needs_a_block_for_its_stack", board="image")
        self.assertEqual(out.all("block"), ["6143 0 out_of_memory | failed to allocate | 0", "6144 1  |  | 1"])

    def test_a_download_needs_a_block_for_its_drawing(self):
        out = self.run_case("image_needs_a_block_for_its_drawing", board="image")
        # On a wide screen four rows (8000 bytes) are larger than the stack (6144): they decide.
        self.assertEqual(out.all("wide"), ["7999 0 out_of_memory | failed to allocate", "8000 1  | "])

    def test_a_download_whose_task_will_not_start_frees_the_reader(self):
        out = self.run_case("image_task_that_will_not_start", board="image")
        self.assertEqual(out.one("task"), "out_of_memory | failed to start the download task | 0")

    # ---- Wiring ----

    def test_the_firmware_builds_it(self):
        cmake = (MAIN / "CMakeLists.txt").read_text()
        sources = re.search(r"set\(GADGET_SRCS(.*?)\)", cmake, re.S).group(1)
        self.assertIn('"image_fetch.c"', sources)
        self.assertIn('"vm_file.c"', sources)
        source = (MAIN / "vm_file.c").read_text()
        self.assertRegex(source, r"#if CONFIG_HOMEHUB_DAEMON_REQUESTS\b")   # noise_ctrl_req_open needs it

    def test_image_fetch_reads_through_it(self):
        source = (MAIN / "image_fetch.c").read_text()
        self.assertIn('#include "vm_file.h"', source)
        # It keeps its own flag, which draw_url shares, and names itself to the reader.
        self.assertIn("atomic_compare_exchange_strong(&s_busy", source)
        self.assertRegex(source, r"vm_file_open\([^;]*VM_FILE_OWNER_IMAGE")
        # The reader counts the stack and the drawing memory as a sum; each needs a block of its own.
        self.assertRegex(source, r"heap_caps_get_largest_free_block\(BYTE_CAPS\) < block_need")
        # What moved is gone from it, not copied.
        for moved in ("s_vm", "vm_request_path", "vm_get_range", "vm_next_range", "vm_on_frame", "vm_allocate_body",
                      "noise_ctrl_req_open", "noise_ctrl_req_cancel"):
            self.assertNotRegex(source, rf"\b{moved}\b", f"{moved} was moved to vm_file.c, not copied")


if __name__ == "__main__":
    unittest.main()
