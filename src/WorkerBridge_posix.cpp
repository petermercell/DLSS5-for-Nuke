// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
// POSIX implementation of WorkerBridge.
//
// Wire protocol is byte-identical to the Windows implementation: the same
// VideoHeader / FrameHeader / FrameResponse structs travel over the child's
// stdin and stdout. Only the process and pipe plumbing differs.
//
// Worker selection:
//   * a path ending in ".exe" is launched through Wine (the DLSS-NR runtime is
//     a Windows PE and there is no Linux build of it), i.e.
//         $NUKE_DLSS5_WINE (default "wine")  <worker.exe> --video
//   * any other path is exec'd directly, so a future native Linux worker, or a
//     wrapper script that sets up a Wine prefix, drops in without a code change.
//   * $NUKE_DLSS5_WORKER_LAUNCHER, if set, overrides both: it is exec'd with the
//     worker path and "--video" appended.
// -----------------------------------------------------------------------------

#ifndef _WIN32

#include "WorkerBridge.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

class ScopedExclusiveLock {
public:
    explicit ScopedExclusiveLock(SRWLOCK& lock) : m_lock(&lock), m_acquired(true) {
        AcquireSRWLockExclusive(m_lock);
    }
    ~ScopedExclusiveLock() {
        if (m_acquired && m_lock) ReleaseSRWLockExclusive(m_lock);
    }
    void unlock() {
        if (m_acquired && m_lock) { ReleaseSRWLockExclusive(m_lock); m_acquired = false; }
    }
    void relock() {
        if (!m_acquired && m_lock) { AcquireSRWLockExclusive(m_lock); m_acquired = true; }
    }
private:
    SRWLOCK* m_lock;
    bool     m_acquired;
};

// A dead worker turns the next write() into SIGPIPE, whose default disposition
// would take the whole Nuke process down. Ignore it once, and only if the host
// has not already installed its own handler.
void ensureSigPipeIgnored() {
    static bool done = false;
    if (done) return;
    done = true;
    struct sigaction cur;
    if (sigaction(SIGPIPE, nullptr, &cur) == 0 && cur.sa_handler == SIG_DFL) {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_IGN;
        sigaction(SIGPIPE, &sa, nullptr);
    }
}

bool endsWithNoCase(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    return ::strcasecmp(s.c_str() + (s.size() - n), suffix) == 0;
}

bool isRegularFile(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

} // namespace

WorkerBridge::WorkerBridge() {}

WorkerBridge::~WorkerBridge() { stop(); }

bool WorkerBridge::isRunning() {
    AcquireSRWLockShared(&m_lock);
    bool r = is_running;
    ReleaseSRWLockShared(&m_lock);
    return r;
}

bool WorkerBridge::start(const std::string& nvngx_path, const VideoHeader& header,
                         SetupResponse& out_setup) {
    ScopedExclusiveLock lock(m_lock);
    if (is_running) return false;
    if (nvngx_path.empty() || !isRegularFile(nvngx_path)) return false;

    ensureSigPipeIgnored();

    // The child chdir()s into the runtime folder before exec, so a path that
    // was relative to the *caller's* working directory would no longer resolve
    // and execvp would fail with ENOENT -- silently, as an immediate EOF on the
    // pipe. Resolve to an absolute path up front. (Windows does not have this
    // problem: CreateProcess resolves the image independently of its lpCurrentDirectory.)
    std::string worker_path = nvngx_path;
    {
        char resolved[PATH_MAX];
        if (::realpath(nvngx_path.c_str(), resolved)) worker_path = resolved;
    }

    // ---- Build argv ---------------------------------------------------------
    std::vector<std::string> args;
    if (const char* launcher = std::getenv("NUKE_DLSS5_WORKER_LAUNCHER")) {
        if (*launcher) args.push_back(launcher);
    }
    if (args.empty() && endsWithNoCase(worker_path, ".exe")) {
        const char* wine = std::getenv("NUKE_DLSS5_WINE");
        args.push_back((wine && *wine) ? wine : "wine");
    }
    args.push_back(worker_path);
    args.push_back("--video");

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    // Working directory = the runtime folder, so the worker finds _nvngx.dll,
    // nvngx_dlssnr.dll and the caller shim beside itself.
    std::string workingDir;
    size_t lastSlash = worker_path.find_last_of('/');
    if (lastSlash != std::string::npos) workingDir = worker_path.substr(0, lastSlash);

    // ---- Pipes --------------------------------------------------------------
    int inPipe[2]  = {-1, -1};   // parent writes inPipe[1]  -> child stdin  inPipe[0]
    int outPipe[2] = {-1, -1};   // child writes outPipe[1] -> parent reads outPipe[0]
    if (::pipe(inPipe) != 0) return false;
    if (::pipe(outPipe) != 0) {
        ::close(inPipe[0]); ::close(inPipe[1]);
        return false;
    }

    // Keep the parent-side ends out of any process the host forks later.
    ::fcntl(inPipe[1],  F_SETFD, FD_CLOEXEC);
    ::fcntl(outPipe[0], F_SETFD, FD_CLOEXEC);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(inPipe[0]); ::close(inPipe[1]);
        ::close(outPipe[0]); ::close(outPipe[1]);
        return false;
    }

    if (pid == 0) {
        // ---- Child: async-signal-safe calls only until execvp ---------------
        ::dup2(inPipe[0],  STDIN_FILENO);
        ::dup2(outPipe[1], STDOUT_FILENO);

        // Worker (and Wine) log chatter must never reach the binary stdout.
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) ::close(devnull);
        }

        ::close(inPipe[0]);  ::close(inPipe[1]);
        ::close(outPipe[0]); ::close(outPipe[1]);

        if (!workingDir.empty()) {
            if (::chdir(workingDir.c_str()) != 0) { _exit(127); }
        }

        // Quiet Wine unless the user deliberately asked for its output.
        if (!std::getenv("WINEDEBUG")) ::setenv("WINEDEBUG", "-all", 1);

        // Detach from Nuke's process group so a Ctrl-C in a terminal-launched
        // Nuke does not race the worker.
        ::setsid();

        ::execvp(argv[0], argv.data());
        _exit(127);
    }

    // ---- Parent -------------------------------------------------------------
    ::close(inPipe[0]);
    ::close(outPipe[1]);

    m_pid        = pid;
    m_fdChildIn  = inPipe[1];
    m_fdChildOut = outPipe[0];

    m_header = header;

    auto fail = [&]() -> bool {
        lock.unlock();
        stop();
        lock.relock();
        return false;
    };

    if (!writeExact(&header, sizeof(VideoHeader)))      return fail();
    if (!readExact(&out_setup, sizeof(SetupResponse)))  return fail();
    if (out_setup.magic != 0x34505553 || out_setup.setup_ok != 1) return fail();

    is_running = true;
    return true;
}

void WorkerBridge::stop() {
    ScopedExclusiveLock lock(m_lock);

    if (m_fdChildIn  >= 0) { ::close(m_fdChildIn);  m_fdChildIn  = -1; }
    if (m_fdChildOut >= 0) { ::close(m_fdChildOut); m_fdChildOut = -1; }

    if (m_pid > 0) {
        // Closing stdin is the worker's normal shutdown signal; give it a brief
        // grace period before killing, then always reap so we leave no zombie.
        int status = 0;
        bool exited = false;
        for (int i = 0; i < 100; ++i) {           // up to ~200 ms
            pid_t r = ::waitpid(m_pid, &status, WNOHANG);
            if (r == m_pid) { exited = true; break; }
            if (r < 0)      { exited = true; break; }
            ::usleep(2000);
        }
        if (!exited) {
            ::kill(m_pid, SIGKILL);
            while (::waitpid(m_pid, &status, 0) < 0 && errno == EINTR) {}
        }
        m_pid = -1;
    }

    is_running = false;
}

bool WorkerBridge::processFrame(
    uint32_t index, bool reset, int64_t pts,
    const uint8_t* rgba_data, size_t rgba_size,
    const uint8_t* motion_data, size_t motion_size,
    const uint8_t* depth_data, size_t depth_size,
    const uint8_t* control_mask_data, size_t control_mask_size,
    std::vector<uint8_t>& out_rgba)
{
    ScopedExclusiveLock lock(m_lock);
    if (!is_running) return false;

    FrameHeader header;
    header.index = index;
    header.reset = reset ? 1 : 0;
    header.pts   = pts;

    const size_t guide_size =
        (size_t)m_header.input_width * m_header.input_height * sizeof(float);
    if (depth_data && depth_size == guide_size)               header.guide_flags |= GUIDE_DEPTH;
    if (control_mask_data && control_mask_size == guide_size) header.guide_flags |= GUIDE_CONTROL_MASK;

    // Any transport failure means the worker is gone: tear down and report.
    auto die = [&]() -> bool {
        is_running = false;
        lock.unlock();
        stop();
        lock.relock();
        return false;
    };

    if (!writeExact(&header, sizeof(FrameHeader))) return die();
    if (!writeExact(rgba_data, rgba_size))         return die();

    if (m_header.mv_mode == 1) {
        if (motion_size > 0 && motion_data != nullptr) {
            if (!writeExact(motion_data, motion_size)) return die();
        } else {
            const size_t motion_bytes =
                (size_t)m_header.input_width * m_header.input_height * 2 * sizeof(uint16_t);
            std::vector<uint8_t> zeroMotion(motion_bytes, 0);
            if (!writeExact(zeroMotion.data(), zeroMotion.size())) return die();
        }
    }

    if ((header.guide_flags & GUIDE_DEPTH) && !writeExact(depth_data, depth_size))
        return die();
    if ((header.guide_flags & GUIDE_CONTROL_MASK) && !writeExact(control_mask_data, control_mask_size))
        return die();

    FrameResponse resp;
    if (!readExact(&resp, sizeof(FrameResponse))) return die();

    // A refused frame is not a dead worker: keep the process alive, as Windows does.
    if (resp.magic != 0x3154554F || resp.ok != 1 || resp.out_index != index) return false;

    out_rgba.resize(resp.byte_count);
    if (resp.byte_count && !readExact(out_rgba.data(), resp.byte_count)) return die();

    return true;
}

bool WorkerBridge::readExact(void* buffer, size_t size) {
    if (m_fdChildOut < 0) return false;
    char*  ptr   = static_cast<char*>(buffer);
    size_t total = 0;
    while (total < size) {
        ssize_t n = ::read(m_fdChildOut, ptr + total, size - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;   // EOF: worker exited
        total += (size_t)n;
    }
    return true;
}

bool WorkerBridge::writeExact(const void* buffer, size_t size) {
    if (m_fdChildIn < 0) return false;
    const char* ptr = static_cast<const char*>(buffer);
    size_t total = 0;
    while (total < size) {
        ssize_t n = ::write(m_fdChildIn, ptr + total, size - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;               // EPIPE included: worker is gone
        }
        total += (size_t)n;
    }
    return true;
}

#endif // !_WIN32
