// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
// worker_probe -- drive the real DLSS worker from Linux without Nuke.
//
// Uses the same WorkerBridge the plug-in uses, so a pass here means the whole
// Wine + vkd3d-proton + NGX chain works and only the Nuke side remains. A
// failure here is isolated from anything to do with Nuke, the .so, or the NDK.
//
//   ./worker_probe ~/.nuke/DLSS5Live/runtime/dlss5-worker.sh
//   ./worker_probe ~/.nuke/DLSS5Live/runtime/DLSS_Nuke_Worker.exe --scale 2.0
//
// Reports which stage failed, and prints the worker's setup_result, which is
// the NGX code -- 0xBAD00002 there means the runtime rejected the calling
// module (the caller-shim return-address check), which is a different problem
// from D3D12 device creation failing.
// -----------------------------------------------------------------------------

#include "../src/WorkerBridge.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static uint16_t floatToHalf(float f) {
    uint32_t x; std::memcpy(&x, &f, sizeof(x));
    uint32_t s = (x >> 16) & 0x8000u;
    int32_t  e = (int32_t)((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (e <= 0)  return (uint16_t)s;
    if (e >= 31) return (uint16_t)(s | 0x7c00u);
    return (uint16_t)(s | (uint32_t)(e << 10) | (m >> 13));
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <worker path> [--width N] [--height N] [--scale F] [--frames N]\n"
            "  worker path: dlss5-worker.sh (recommended) or DLSS_Nuke_Worker.exe\n", argv[0]);
        return 2;
    }

    std::string worker = argv[1];
    uint32_t W = 512, H = 288, frames = 3;
    float scale = 1.0f;   // 1.0 = DLAA, single-pass DLSS-NR only

    for (int i = 2; i < argc - 1; ++i) {
        if (!std::strcmp(argv[i], "--width"))  W = (uint32_t)std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--height")) H = (uint32_t)std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--scale"))  scale = (float)std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--frames")) frames = (uint32_t)std::atoi(argv[++i]);
    }

    const uint32_t OW = (uint32_t)std::lround(W * scale);
    const uint32_t OH = (uint32_t)std::lround(H * scale);

    VideoHeader hdr{};
    hdr.input_width   = W;   hdr.input_height  = H;
    hdr.output_width  = OW;  hdr.output_height = OH;
    hdr.warmup_frames = 0;
    hdr.frame_count   = 100000;
    hdr.perf_quality  = (scale <= 1.001f) ? 5u : 2u;   // DLAA : MaxQuality
    hdr.dlss_model_preset = 0;
    hdr.intensity = 1.0f; hdr.local_tone = 1.0f;
    hdr.local_structure = 1.0f; hdr.skin_structure = -1.0f;
    hdr.mv_mode = 0;                                   // no motion vectors
    hdr.dis_preset = 1; hdr.dis_flow_width = 640; hdr.dis_iterations = 25;

    std::printf("worker : %s\n", worker.c_str());
    std::printf("request: %ux%u -> %ux%u (scale %.2f), %u frame(s)\n\n",
                W, H, OW, OH, scale, frames);

    WorkerBridge bridge;
    SetupResponse setup{};

    std::printf("[1/3] starting worker and handshaking...\n");
    if (!bridge.start(worker, hdr, setup)) {
        std::printf("      FAILED.\n\n");
        if (setup.magic != 0x34505553) {
            // Do not report setup_result here: without a valid 'SUP4' magic the
            // struct holds whatever bytes did arrive, and printing them as an
            // NGX code sends you chasing an error that was never returned.
            std::printf("      No valid SetupResponse came back: the worker died before or\n"
                        "      during init, or something non-protocol was written to stdout.\n");
            if (setup.magic != 0) {
                std::printf("      First 4 bytes were 0x%08X", setup.magic);
                const char* b = (const char*)&setup.magic;
                bool printable = true;
                for (int i = 0; i < 4; ++i) if (b[i] < 0x20 || b[i] > 0x7e) printable = false;
                if (printable) std::printf(" (\"%c%c%c%c\" -- that is text, not protocol:\n"
                                           "      something is printing to stdout ahead of the worker)",
                                           b[0], b[1], b[2], b[3]);
                std::printf("\n");
            }
            std::printf("      Read runtime/dlss5-worker.log; the worker names its failing stage there.\n");
            return 1;
        }
        std::printf("      setup_result = 0x%08X\n", setup.setup_result);
        if (setup.setup_result == 0xBAD00002u) {
            std::printf("      0xBAD00002: the NR runtime rejected the calling module. The caller\n"
                        "      shim's return address did not land inside nvngx.dll. Do not build\n"
                        "      the shim with -flto.\n");
        }
        std::printf("\n      The worker handshaked, so it is running -- it just could not\n"
                    "      initialise. It logged the exact stage; find it with:\n"
                    "          grep '\\[DLSS5 worker\\]' runtime/dlss5-worker.log\n");
        return 1;
    }
    std::printf("      ok. render %ux%u, out %ux%u, limits %ux%u..%ux%u, preset %u, ngx=0x%08X\n\n",
                setup.render_width, setup.render_height,
                setup.output_width, setup.output_height,
                setup.min_width, setup.min_height,
                setup.max_width, setup.max_height,
                setup.applied_model_preset, setup.setup_result);

    // A recognisable test image: vertical gradient with a bright highlight band,
    // so a black or constant result is obvious.
    const size_t px = (size_t)W * H;
    std::vector<uint16_t> rgba(px * 4);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const float u = (float)x / (float)W;
            const float v = (float)y / (float)H;
            const float hi = (y > H / 2 && y < H / 2 + 8) ? 4.0f : 0.0f;  // HDR band
            const size_t i = ((size_t)y * W + x) * 4;
            rgba[i + 0] = floatToHalf(u + hi);
            rgba[i + 1] = floatToHalf(v + hi);
            rgba[i + 2] = floatToHalf(0.5f * (u + v) + hi);
            rgba[i + 3] = floatToHalf(1.0f);
        }
    }

    std::printf("[2/3] processing %u frame(s)...\n", frames);
    std::vector<uint8_t> out;
    for (uint32_t f = 0; f < frames; ++f) {
        if (!bridge.processFrame(f, f == 0, (int64_t)f,
                                 (const uint8_t*)rgba.data(), rgba.size() * sizeof(uint16_t),
                                 nullptr, 0, nullptr, 0, nullptr, 0, out)) {
            std::printf("      FAILED on frame %u.\n", f);
            std::printf("      Init succeeded but evaluation did not: this is an Evaluate-stage\n"
                        "      problem, not a load problem. Check dlss5-worker.log.\n");
            return 1;
        }
        std::printf("      frame %u ok, %zu bytes back\n", f, out.size());
    }

    const size_t expect = (size_t)OW * OH * 4 * sizeof(uint16_t);
    std::printf("\n[3/3] checking the returned buffer...\n");
    if (out.size() != expect) {
        std::printf("      FAILED: got %zu bytes, expected %zu.\n", out.size(), expect);
        return 1;
    }

    // Reject an all-zero or all-constant result: those come back "successful"
    // from a runtime that silently did nothing.
    const uint16_t* h = (const uint16_t*)out.data();
    bool all_zero = true, all_same = true;
    for (size_t i = 0; i < out.size() / 2; ++i) {
        if (h[i] != 0)     all_zero = false;
        if (h[i] != h[0])  all_same = false;
        if (!all_zero && !all_same) break;
    }
    if (all_zero) { std::printf("      FAILED: buffer is entirely zero -- the worker produced nothing.\n"); return 1; }
    if (all_same) { std::printf("      FAILED: buffer is a single constant value.\n"); return 1; }

    std::printf("      ok: %zu bytes, non-trivial content.\n", out.size());
    std::printf("\nPASS -- the full DLSS-NR chain works on this machine.\n");
    return 0;
}
