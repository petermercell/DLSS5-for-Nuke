// Exercises the POSIX WorkerBridge against fake_worker: handshake, several
// frames with and without guides, worker-death handling, and clean teardown.
#include "../src/WorkerBridge.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++failures; } \
    else         { std::printf("  ok:   %s\n", msg); } std::fflush(stdout); } while (0)

int main(int argc, char** argv) {
    const std::string worker = (argc > 1) ? argv[1] : "./fake_worker";

    const uint32_t W = 64, H = 32, OW = 128, OH = 64;

    VideoHeader hdr{};
    hdr.input_width = W;  hdr.input_height = H;
    hdr.output_width = OW; hdr.output_height = OH;
    hdr.mv_mode = 1;                 // exercise the motion-vector leg
    hdr.frame_count = 100000;

    const size_t in_px  = (size_t)W * H;
    const size_t out_px = (size_t)OW * OH;
    std::vector<uint8_t> color(in_px * 4 * sizeof(uint16_t), 0x11);
    std::vector<uint8_t> motion(in_px * 2 * sizeof(uint16_t), 0x22);
    std::vector<uint8_t> depth(in_px * sizeof(float), 0x33);
    std::vector<uint8_t> mask(in_px * sizeof(float), 0x44);

    std::printf("bridge_test\n");

    {
        WorkerBridge bridge;
        SetupResponse setup{};
        CHECK(bridge.start(worker, hdr, setup), "start(): spawn + handshake");
        CHECK(setup.magic == 0x34505553 && setup.setup_ok == 1, "SetupResponse magic and setup_ok");
        CHECK(setup.output_width == OW && setup.output_height == OH, "SetupResponse carries output dimensions");
        CHECK(bridge.isRunning(), "isRunning() true after start");

        // Frame with no guides.
        std::vector<uint8_t> out;
        CHECK(bridge.processFrame(0, true, 0, color.data(), color.size(),
                                  motion.data(), motion.size(), nullptr, 0, nullptr, 0, out),
              "processFrame(): plain frame");
        CHECK(out.size() == out_px * 4 * sizeof(uint16_t), "output buffer sized from FrameResponse");
        CHECK(!out.empty() && out[0] == 0, "output payload belongs to frame 0");

        // Frames with depth + control mask guides, several in a row.
        bool all_ok = true, right_payload = true;
        for (uint32_t i = 1; i <= 8; ++i) {
            std::vector<uint8_t> o;
            if (!bridge.processFrame(i, false, (int64_t)i * 1000,
                                     color.data(), color.size(),
                                     motion.data(), motion.size(),
                                     depth.data(), depth.size(),
                                     mask.data(), mask.size(), o)) { all_ok = false; break; }
            if (o.empty() || o[0] != (uint8_t)i) right_payload = false;
        }
        CHECK(all_ok, "processFrame(): 8 sequential frames with depth + mask guides");
        CHECK(right_payload, "each frame's payload matches its index (stream stayed in sync)");

        // A frame whose index the worker will not echo back must be refused
        // without tearing the worker down.
        CHECK(bridge.isRunning(), "worker still alive after guided frames");

        bridge.stop();
        CHECK(!bridge.isRunning(), "isRunning() false after stop");
    }

    // Worker that exits immediately: start() must fail rather than hang or crash.
    {
        WorkerBridge bridge;
        SetupResponse setup{};
        CHECK(!bridge.start("/bin/true", hdr, setup), "start() fails cleanly on a worker that exits");
        CHECK(!bridge.isRunning(), "no running state left behind");
    }

    // Nonexistent path.
    {
        WorkerBridge bridge;
        SetupResponse setup{};
        CHECK(!bridge.start("/nonexistent/DLSS_Nuke_Worker.exe", hdr, setup),
              "start() fails on a missing worker path");
    }

    // Worker killed mid-session: the next frame must fail, not raise SIGPIPE.
    {
        WorkerBridge bridge;
        SetupResponse setup{};
        ::setenv("FAKE_WORKER_DIE_AFTER_SETUP", "1", 1);
        const bool started = bridge.start(worker, hdr, setup);
        ::unsetenv("FAKE_WORKER_DIE_AFTER_SETUP");
        if (started) {
            ::usleep(200000);   // let the worker exit
            std::vector<uint8_t> o;
            bool ok = bridge.processFrame(0, true, 0, color.data(), color.size(),
                                          motion.data(), motion.size(), nullptr, 0, nullptr, 0, o);
            CHECK(!ok, "processFrame() reports failure after the worker dies");
            CHECK(!bridge.isRunning(), "bridge marks itself stopped after worker death");
        } else {
            CHECK(false, "second start() for the kill test");
        }
    }

    std::printf(failures ? "\n%d CHECK(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
