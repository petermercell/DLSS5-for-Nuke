// Stand-in for DLSS_Nuke_Worker: speaks the wire protocol, does no DLSS.
// Used to verify the POSIX WorkerBridge without a GPU, Wine or Nuke.
#include "../src/WorkerBridge.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>

static bool readExact(void* buf, size_t n) {
    char* p = (char*)buf; size_t t = 0;
    while (t < n) { ssize_t r = ::read(0, p + t, n - t); if (r <= 0) return false; t += (size_t)r; }
    return true;
}
static bool writeExact(const void* buf, size_t n) {
    const char* p = (const char*)buf; size_t t = 0;
    while (t < n) { ssize_t r = ::write(1, p + t, n - t); if (r <= 0) return false; t += (size_t)r; }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "--video") != 0) {
        std::fprintf(stderr, "fake_worker: expected --video\n");
        return 2;
    }
    // Prove stderr noise cannot corrupt the binary stream.
    std::fprintf(stderr, "fake_worker: chatty log line that must never reach stdout\n");

    VideoHeader hdr{};
    if (!readExact(&hdr, sizeof(hdr))) return 1;

    // Test hook: simulate an init failure carrying a given NGX code.
    if (const char* fc = ::getenv("FAKE_WORKER_FAIL_SETUP")) {
        SetupResponse bad{};
        bad.magic = 0x34505553;
        bad.setup_ok = 0;
        bad.setup_result = (uint32_t)::strtoul(fc, nullptr, 0);
        writeExact(&bad, sizeof(bad));
        return 4;
    }

    SetupResponse setup{};
    setup.magic         = 0x34505553;
    setup.setup_ok      = 1;
    setup.render_width  = hdr.input_width;
    setup.render_height = hdr.input_height;
    setup.output_width  = hdr.output_width;
    setup.output_height = hdr.output_height;
    if (!writeExact(&setup, sizeof(setup))) return 1;

    // Test hook: simulate a worker that dies after the handshake.
    if (const char* d = ::getenv("FAKE_WORKER_DIE_AFTER_SETUP")) { if (*d == '1') return 3; }

    const size_t in_px    = (size_t)hdr.input_width * hdr.input_height;
    const size_t out_px   = (size_t)hdr.output_width * hdr.output_height;
    const size_t color_in = in_px * 4 * sizeof(uint16_t);   // RGBA16F
    const size_t mv_bytes = in_px * 2 * sizeof(uint16_t);   // RG16F
    const size_t guide    = in_px * sizeof(float);
    const size_t out_sz   = out_px * 4 * sizeof(uint16_t);

    std::vector<uint8_t> scratch;
    for (;;) {
        FrameHeader fh{};
        if (!readExact(&fh, sizeof(fh))) break;   // EOF = clean shutdown
        if (fh.magic != 0x314D5246) return 1;

        scratch.resize(color_in);
        if (!readExact(scratch.data(), color_in)) return 1;
        if (hdr.mv_mode == 1) { scratch.resize(mv_bytes); if (!readExact(scratch.data(), mv_bytes)) return 1; }
        if (fh.guide_flags & GUIDE_DEPTH)        { scratch.resize(guide); if (!readExact(scratch.data(), guide)) return 1; }
        if (fh.guide_flags & GUIDE_CONTROL_MASK) { scratch.resize(guide); if (!readExact(scratch.data(), guide)) return 1; }

        FrameResponse fr{};
        fr.magic      = 0x3154554F;
        fr.out_index  = fh.index;
        fr.ok         = 1;
        fr.byte_count = (uint32_t)out_sz;
        fr.ngx_result = 1;
        fr.out_pts    = fh.pts;
        if (!writeExact(&fr, sizeof(fr))) return 1;

        // Fill with a value derived from the frame index so the caller can
        // prove it read back the right frame's payload.
        std::vector<uint8_t> out(out_sz, (uint8_t)(fh.index & 0xFF));
        if (!writeExact(out.data(), out.size())) return 1;
    }
    return 0;
}
