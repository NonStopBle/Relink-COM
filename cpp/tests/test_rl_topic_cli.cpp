// rl_topic CLI behavior tests -- covers what the shm_transport/
// topic_hash unit tests and the manual two-process smoke tests don't:
// the `--ipc` guard on `list`/`info` in rl_topic.cpp's main(). Unlike
// hz/bw/echo/pub, list/info have no discovery step to fall back on for
// a same-host-only topic (there is no central directory of shm-only
// topics -- rlcore/multicast never see them), so they must reject
// --ipc outright with a non-zero exit rather than hang waiting for a
// reply that will never come, or silently print nothing.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef RL_TOPIC_EXE
#error "RL_TOPIC_EXE must be defined to the built rl_topic binary's path"
#endif

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); ++g_failures; } \
    else { std::printf("ok: %s\n", #cond); } \
} while (0)

// Runs `RL_TOPIC_EXE <args>`, returning the exit code and capturing
// combined stdout+stderr into `output`.
static int run(const std::string& args, std::string& output) {
    std::string cmd = std::string(RL_TOPIC_EXE) + " " + args + " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { std::fprintf(stderr, "popen failed for: %s\n", cmd.c_str()); return -1; }
    std::array<char, 4096> buf{};
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0) output.append(buf.data(), n);
    int status = pclose(p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main() {
    {
        std::string out;
        int rc = run("list --ipc", out);
        CHECK(rc == 2);
        CHECK(out.find("not supported") != std::string::npos);
    }
    {
        std::string out;
        int rc = run("info /some/topic --ipc", out);
        CHECK(rc == 2);
        CHECK(out.find("not supported") != std::string::npos);
    }
    // list/info WITHOUT --ipc must fail (no rlcore/multicast peer in a
    // unit-test sandbox) for a DIFFERENT reason than the --ipc guard --
    // that guard's message must never appear on the non---ipc path.
    {
        std::string out;
        run("list --timeout 0.2 --no-cache", out);
        CHECK(out.find("--ipc is not supported") == std::string::npos);
    }
    // Sanity/regression guard: hz/bw/echo/pub must NOT hit the list/info
    // guard just because they also carry --ipc. pub --ipc is the one
    // subcommand that both accepts --ipc and returns immediately (no
    // peer-wait loop -- see cmd_pub()'s header comment), so it's the only
    // one of the four safe to run synchronously in a unit test.
    {
        std::string out;
        int rc = run("pub /relink/test_cli_ipc_guard --ipc --text hello", out);
        CHECK(rc == 0);
        CHECK(out.find("--ipc is not supported") == std::string::npos);
        CHECK(out.find("published") != std::string::npos);
    }

    if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}
