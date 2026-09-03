// Crash test: kill the writer with SIGKILL at a random moment, reopen the
// database, and verify that every write the writer was TOLD had succeeded is
// still there.
//
// The contract under test is the one written in db.h: with sync_every_write,
// Put returning true means the write survives. This harness makes that promise
// auditable by having the child record every acknowledged key to a separate
// file before moving on.
//
// SCOPE: SIGKILL kills the process, not the kernel. Data already handed to the
// page cache survives regardless of fsync. So this proves crash-safe REPLAY
// and torn-tail handling. It does NOT prove fsync correctness; that needs
// power loss or a hard VM reset.
//
// Standalone binary rather than a gtest case: forking inside a test framework
// makes the child inherit reporting machinery it should not.

#include "db.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

const char* kDir = "/tmp/minidb_crash";
const char* kDbDir = "/tmp/minidb_crash/db";
const char* kAckedPath = "/tmp/minidb_crash/acked.log";

void ResetDir() {
    std::string cmd = std::string("rm -rf ") + kDir;
    if (system(cmd.c_str()) != 0) {
        std::cerr << "failed to reset " << kDir << "\n";
        std::exit(1);
    }
    if (::mkdir(kDir, 0755) != 0) {
        std::perror("mkdir");
        std::exit(1);
    }
}

// Runs in the forked child. Writes until killed. Never returns normally
// during a real run.
[[noreturn]] void RunWriter(unsigned seed) {
    minidb::Options opts;
    opts.sync_every_write = true;

    auto db = minidb::DB::Open(kDbDir, opts);
    if (!db) std::_Exit(2);

    // Opened separately from the DB. This file records what the database
    // PROMISED, so it must not share any buffering with it.
    int ack_fd = ::open(kAckedPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (ack_fd < 0) std::_Exit(3);

    std::mt19937 rng(seed);
    long i = 0;
    while (true) {
        std::string key = "key" + std::to_string(rng() % 100000);
        std::string value = "value" + std::to_string(i);

        if (!db->Put(key, value)) std::_Exit(4);

        // Put returned true, so the DB has promised this write survives.
        // Record the promise, then fsync so the record itself survives.
        std::string line = key + "=" + value + "\n";
        if (::write(ack_fd, line.data(), line.size()) !=
            static_cast<ssize_t>(line.size())) {
            std::_Exit(5);
        }
        if (::fsync(ack_fd) != 0) std::_Exit(6);

        ++i;
    }
}

// A trailing line with no newline means the child was killed mid-write to this
// file, so it never finished recording that acknowledgement. Dropping it is
// correct, not a fudge.
std::vector<std::pair<std::string, std::string>> ReadAcked() {
    std::vector<std::pair<std::string, std::string>> acked;
    std::ifstream in(kAckedPath, std::ios::binary);
    if (!in) return acked;

    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());

    size_t pos = 0;
    while (true) {
        size_t nl = content.find('\n', pos);
        if (nl == std::string::npos) break;  // torn trailing line: ignore
        std::string line = content.substr(pos, nl - pos);
        pos = nl + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        acked.push_back({line.substr(0, eq), line.substr(eq + 1)});
    }
    return acked;
}

// Values are "value<N>" with N increasing over the run, so a larger N means a
// later write.
long ValueIndex(const std::string& value) {
    if (value.rfind("value", 0) != 0) return -1;
    return std::atol(value.c_str() + 5);
}

struct Result {
    long acked_count = 0;
    long checked_keys = 0;
};

bool RunOnce(int iteration, unsigned seed, Result* out) {
    ResetDir();

    pid_t pid = ::fork();
    if (pid < 0) {
        std::perror("fork");
        return false;
    }
    if (pid == 0) {
        RunWriter(seed);
    }

    // Let the child get some writes in, then kill it at an unpredictable
    // point. SIGKILL cannot be caught, so no cleanup handler runs.
    std::mt19937 rng(seed);
    int delay_ms = 1 + (rng() % 50);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

    if (::kill(pid, SIGKILL) != 0) {
        std::perror("kill");
        return false;
    }
    int status = 0;
    ::waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        std::cerr << "iteration " << iteration << ": child exited with "
                  << WEXITSTATUS(status) << " instead of being killed\n";
        return false;
    }

    auto acked = ReadAcked();
    out->acked_count = static_cast<long>(acked.size());
    if (acked.empty()) return true;  // killed before the first write landed

    minidb::Options opts;
    opts.sync_every_write = true;
    auto db = minidb::DB::Open(kDbDir, opts);
    if (!db) {
        std::cerr << "iteration " << iteration
                  << ": FAILED to reopen the database after the kill\n";
        return false;
    }

    // A key can be written more than once, so only the LAST acknowledged value
    // for each key is what the database owes us.
    std::vector<std::pair<std::string, std::string>> expected;
    std::set<std::string> seen;
    for (auto it = acked.rbegin(); it != acked.rend(); ++it) {
        if (seen.insert(it->first).second) expected.push_back(*it);
    }
    out->checked_keys = static_cast<long>(expected.size());

    for (const auto& kv : expected) {
        auto got = db->Get(kv.first);
        if (!got.has_value()) {
            std::cerr << "iteration " << iteration << ": LOST acknowledged key "
                      << kv.first << "\n";
            return false;
        }
        // The database may legitimately hold a NEWER value than the last
        // acknowledged one: if the kill landed between Put returning true and
        // the ack line being written, that write is durable but unrecorded.
        // What must never happen is the database holding an OLDER value, which
        // would mean an acknowledged write was lost.
        long have = ValueIndex(*got);
        long owed = ValueIndex(kv.second);
        if (have < owed) {
            std::cerr << "iteration " << iteration << ": key " << kv.first
                      << " rolled back to '" << *got << "', expected at least '"
                      << kv.second << "'\n";
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    int iterations = (argc > 1) ? std::atoi(argv[1]) : 100;

    long total_acked = 0;
    long total_checked = 0;
    int empty_iterations = 0;

    for (int i = 0; i < iterations; ++i) {
        Result r;
        if (!RunOnce(i, static_cast<unsigned>(i * 7919 + 13), &r)) {
            std::cerr << "\nCRASH TEST FAILED at iteration " << i << "\n";
            return 1;
        }
        total_acked += r.acked_count;
        total_checked += r.checked_keys;
        if (r.acked_count == 0) ++empty_iterations;

        if ((i + 1) % 50 == 0) {
            std::cout << "  " << (i + 1) << "/" << iterations
                      << " iterations, " << total_acked
                      << " acknowledged writes verified so far\n";
        }
    }

    std::cout << "\nCRASH TEST PASSED\n"
              << "  iterations              : " << iterations << "\n"
              << "  acknowledged writes     : " << total_acked << "\n"
              << "  distinct keys verified  : " << total_checked << "\n"
              << "  writes lost             : 0\n";
    if (empty_iterations > 0) {
        std::cout << "  iterations killed before the first write: "
                  << empty_iterations << "\n";
    }
    return 0;
}