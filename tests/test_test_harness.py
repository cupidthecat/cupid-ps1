"""Exercise child isolation, output capture and result handling in the C runner."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


FIXTURE = r'''
#include "tests/test_harness.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static int changed;

static void success(int id)
{
  EXPECT_EQ(changed, 0);
  changed = id;
  FILE* resource = fopen("data/fixture.txt", "r");
  EXPECT_NOT_NULL(resource);
  char value[16] = {0};
  EXPECT_NOT_NULL(fgets(value, sizeof(value), resource));
  EXPECT_STREQ(value, "fixture-data\n");
  EXPECT_EQ(fclose(resource), 0);
  for (int i = 0; i < 300; i++)
    printf("fixture-row id=%d index=%d payload=abcdefghijklmnopqrstuvwxyz0123456789\n", id, i);
}

TEST(HarnessSuccess, first) { success(1); }
TEST(HarnessSuccess, second) { success(2); }

TEST(HarnessFailure, assertion)
{
  printf("before-assertion\n");
  EXPECT_TRUE(false);
}
TEST(HarnessFailure, following) { printf("following-case-ran\n"); }

TEST(HarnessSignal, crash)
{
  struct rlimit limit = {0, 0};
  EXPECT_EQ(setrlimit(RLIMIT_CORE, &limit), 0);
  printf("before-signal\n");
  raise(SIGSEGV);
}

static void barrier(const char* name, const char* peer, int id)
{
  const char* directory = getenv("HARNESS_PROBE_DIRECTORY");
  EXPECT_NOT_NULL(directory);
  FILE* cache = fopen("worker.cache", "w");
  EXPECT_NOT_NULL(cache);
  EXPECT_TRUE(fprintf(cache, "%d\n", id) > 0);
  EXPECT_EQ(fclose(cache), 0);
  char own[1024], other[1024];
  EXPECT_TRUE(snprintf(own, sizeof(own), "%s/%s", directory, name) < (int)sizeof(own));
  EXPECT_TRUE(snprintf(other, sizeof(other), "%s/%s", directory, peer) < (int)sizeof(other));
  int descriptor = open(own, O_WRONLY | O_CREAT | O_EXCL, 0600);
  EXPECT_TRUE(descriptor >= 0);
  EXPECT_EQ(close(descriptor), 0);
  alarm(5);
  const struct timespec pause = {.tv_nsec = 1000000};
  while (access(other, F_OK) != 0)
    nanosleep(&pause, NULL);
  alarm(0);
  cache = fopen("worker.cache", "r");
  EXPECT_NOT_NULL(cache);
  int value = 0;
  EXPECT_EQ(fscanf(cache, "%d", &value), 1);
  EXPECT_EQ(fclose(cache), 0);
  EXPECT_EQ(value, id);
  printf("barrier-passed id=%d\n", id);
}
TEST(HarnessBarrier, first) { barrier("first", "second", 1); }
TEST(HarnessBarrier, second) { barrier("second", "first", 2); }
'''


class TestHarnessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="cupid-harness-")
        cls.directory = Path(cls.temporary.name)
        cls.work = cls.directory / "work"
        (cls.work / "data").mkdir(parents=True)
        (cls.work / "data/fixture.txt").write_text("fixture-data\n")
        fixture = cls.directory / "fixture.c"
        fixture.write_text(FIXTURE)
        root = Path(__file__).resolve().parents[1]
        runner = root / "tests/test_main.c"
        cls.binary = cls.directory / "harness"
        compiler = shlex.split(os.environ.get("CC", "cc"))
        compiled = subprocess.run(compiler + ["-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                                              "-I" + str(root), "-I" + str(root / "src"), str(runner),
                                              str(fixture), "-o", str(cls.binary)],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if compiled.returncode:
            raise RuntimeError(compiled.stdout)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_harness(self, *args, environment=None):
        env = os.environ.copy()
        env["TMPDIR"] = str(self.directory)
        if environment:
            env.update(environment)
        return subprocess.run([str(self.binary), *args], cwd=self.work, env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, timeout=15)

    def check_success(self, jobs):
        result = self.run_harness("--jobs", str(jobs), "HarnessSuccess")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("2 tests ran: 2 passed, 0 failed (registered: 7)", result.stdout)
        rows = re.findall(r"^fixture-row id=(\d) index=(\d+) payload=abcdefghijklmnopqrstuvwxyz0123456789$",
                          result.stdout, re.MULTILINE)
        self.assertEqual(len(rows), 600)
        self.assertEqual(set(rows), {(str(id_), str(index)) for id_ in (1, 2) for index in range(300)})
        for id_ in (1, 2):
            indices = [int(index) for identity, index in rows if identity == str(id_)]
            self.assertEqual(indices, list(range(300)))

    def test_serial_isolation_and_output(self):
        self.check_success(1)

    def test_parallel_isolation_and_output(self):
        self.check_success(2)

    def test_workers_overlap_and_keep_cache_files_separate(self):
        with tempfile.TemporaryDirectory(dir=self.directory) as barrier:
            result = self.run_harness("--jobs", "2", "HarnessBarrier",
                                      environment={"HARNESS_PROBE_DIRECTORY": barrier})
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("2 tests ran: 2 passed, 0 failed (registered: 7)", result.stdout)
        self.assertEqual(result.stdout.count("barrier-passed id="), 2)

    def test_failure_keeps_output_and_runs_following_case(self):
        for jobs in (1, 2):
            with self.subTest(jobs=jobs):
                result = self.run_harness("--jobs", str(jobs), "HarnessFailure")
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn("2 tests ran: 1 passed, 1 failed (registered: 7)", result.stdout)
                self.assertIn("[ FAIL ] HarnessFailure.assertion (exit 2)", result.stdout)
                self.assertIn("before-assertion", result.stdout)
                self.assertIn("following-case-ran", result.stdout)

    def test_signal_is_reported_with_prior_output(self):
        result = self.run_harness("--jobs", "2", "HarnessSignal")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("[CRASH ] HarnessSignal.crash (signal 11)", result.stdout)
        self.assertIn("before-signal", result.stdout)
        self.assertIn("1 test ran: 0 passed, 1 failed (registered: 7)", result.stdout)

    def test_empty_filter_fails(self):
        result = self.run_harness("--jobs", "2", "NothingSelected")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("0 tests ran: 0 passed, 0 failed (registered: 7)", result.stdout)
        self.assertIn("No tests matched: NothingSelected.", result.stdout)

    def test_invalid_arguments_fail(self):
        for args in [("--jobs",), ("--jobs", "0"), ("--jobs", "-1"),
                     ("--jobs", "2x"), ("--jobs", "4294967296"),
                     ("--jobs", "2", "--jobs", "1"), ("--unknown",), ("one", "two")]:
            with self.subTest(args=args):
                result = self.run_harness(*args)
                self.assertEqual(result.returncode, 2, result.stdout)
                self.assertIn("Usage:", result.stdout)

    def test_help_does_not_execute_cases(self):
        result = self.run_harness("--help")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("Usage:", result.stdout)
        self.assertNotIn("[ RUN  ]", result.stdout)

    def test_invalid_output_directory_fails(self):
        result = self.run_harness("HarnessSuccess", environment={"TMPDIR": str(self.directory / "missing")})
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("2 tests ran: 0 passed, 2 failed (registered: 7)", result.stdout)


if __name__ == "__main__":
    unittest.main()
