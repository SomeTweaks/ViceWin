#define SCARLETTX_BUILDING_BINK2 1
#include "scarlettx/middleware/bink2_abi.h"

#include "core/logger.h"
#include "scarlettx/core/hle_trace.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

struct BinkStreamState {
    BINK pub{};
    std::string path;
    uint32_t played_frames = 0;
    std::vector<uint8_t> frame_rgba;  // Width * Height * 4
};

std::mutex g_mutex;
uint64_t g_next_id = 1;
std::unordered_map<uint64_t, std::unique_ptr<BinkStreamState>> g_streams;
std::string g_last_error;

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void SynthesizeFrame(BinkStreamState& s) {
    const uint32_t w = s.pub.Width;
    const uint32_t h = s.pub.Height;
    s.frame_rgba.resize(static_cast<size_t>(w) * h * 4ull);
    const uint32_t f = s.pub.FrameNum;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t idx = (static_cast<size_t>(y) * w + x) * 4ull;
            s.frame_rgba[idx + 0] = static_cast<uint8_t>((x * 4u + f * 17u) & 0xFFu);
            s.frame_rgba[idx + 1] = static_cast<uint8_t>((y * 4u + f * 9u) & 0xFFu);
            s.frame_rgba[idx + 2] = static_cast<uint8_t>(((x ^ y) + f * 23u) & 0xFFu);
            s.frame_rgba[idx + 3] = 255u;
        }
    }
}

std::mutex& LogMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_set<std::string>& LoggedEntryPoints() {
    static std::unordered_set<std::string> logged;
    return logged;
}

std::string Hex(uint64_t value) {
    char buffer[32] = {};
#if defined(_WIN32)
    std::snprintf(buffer, sizeof(buffer), "0x%llX",
                  static_cast<unsigned long long>(value));
#else
    std::snprintf(buffer, sizeof(buffer), "0x%llx",
                  static_cast<unsigned long long>(value));
#endif
    return buffer;
}

// Logs the raw arguments of an entry point the first time it is called. Later
// calls stay silent so a per-frame import cannot flood the log.
void LogFirstCall(const char* entry, uint64_t a0, uint64_t a1, uint64_t a2,
                  uint64_t a3) {
    {
        std::lock_guard<std::mutex> lock(LogMutex());
        if (!LoggedEntryPoints().insert(entry).second) {
            return;
        }
    }
    scarlettx::LogMessage(scarlettx::LogLevel::Info, "bink2",
                          std::string(entry) + " first call: a0=" + Hex(a0) +
                              " a1=" + Hex(a1) + " a2=" + Hex(a2) +
                              " a3=" + Hex(a3) +
                              " (Bink decode is stubbed; reporting success)");
}

// One warning per entry point, keyed separately from the info line above.
void LogWarningOnce(const char* entry, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(LogMutex());
        if (!LoggedEntryPoints().insert(std::string(entry) + ":warning").second) {
            return;
        }
    }
    scarlettx::LogMessage(scarlettx::LogLevel::Warning, "bink2", message);
}

// True only when [address, address+bytes) is committed and writable in this
// process. Used before writing through an out-parameter whose exact meaning is
// not known, so a wrong guess cannot corrupt the guest's heap.
bool WritablePointer(const void* address, size_t bytes) {
#if defined(_WIN32)
    if (!address || bytes == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) == 0) {
        return false;
    }
    if (info.State != MEM_COMMIT) {
        return false;
    }
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((info.Protect & writable) == 0) {
        return false;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t region_end =
        reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return start + bytes <= region_end;
#else
    (void)address;
    (void)bytes;
    return false;
#endif
}

// BinkSetMemory callbacks. Stored atomically; nothing ever calls them because
// this shim never allocates Bink-owned memory.
std::atomic<void*>& BinkAllocFn() {
    static std::atomic<void*> fn{nullptr};
    return fn;
}

std::atomic<void*>& BinkFreeFn() {
    static std::atomic<void*> fn{nullptr};
    return fn;
}

}  // namespace

extern "C" {

BINK* BinkOpen(const char* name, uint32_t flags) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!name || name[0] == '\0') {
        g_last_error = "BinkOpen: empty filename";
        return nullptr;
    }

    auto state = std::make_unique<BinkStreamState>();
    state->path = name;
    state->pub.Width = 320;
    state->pub.Height = 180;
    state->pub.Frames = 60;
    state->pub.FrameNum = 1;
    state->pub.LastFrameNum = 0;
    state->pub.FrameRate = 30;
    state->pub.FrameRateDiv = 1;
    state->pub.OpenFlags = flags;
    state->pub.Paused = 0;
    state->pub.SoundOn = 1;
    state->pub.Volume = 32768;

    // Parse real BIK2 / KB2 / BIKi header if a file exists on disk
    std::ifstream file(name, std::ios::binary);
    if (file.is_open()) {
        uint8_t hdr[44] = {};
        file.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
        if (file.gcount() >= 40) {
            const bool is_bink1 = (hdr[0] == 'B' && hdr[1] == 'I' && hdr[2] == 'K');
            const bool is_bink2 = (hdr[0] == 'K' && hdr[1] == 'B' && hdr[2] == '2');
            if (is_bink1 || is_bink2) {
                const uint32_t frames = ReadLe32(hdr + 8);
                const uint32_t width = ReadLe32(hdr + 20);
                const uint32_t height = ReadLe32(hdr + 24);
                const uint32_t fps = ReadLe32(hdr + 28);
                const uint32_t fps_div = ReadLe32(hdr + 32);
                if (width > 0 && width <= 7680 && height > 0 && height <= 4320) {
                    state->pub.Width = width;
                    state->pub.Height = height;
                }
                if (frames > 0 && frames <= 1000000) {
                    state->pub.Frames = frames;
                }
                if (fps > 0 && fps_div > 0) {
                    state->pub.FrameRate = fps;
                    state->pub.FrameRateDiv = fps_div;
                }
            }
        }
    }

    state->pub.InternalId = g_next_id++;
    SynthesizeFrame(*state);
    BINK* ret = &state->pub;
    g_streams[state->pub.InternalId] = std::move(state);
    g_last_error.clear();
    return ret;
}

void BinkClose(BINK* bnk) {
    if (!bnk) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_streams.erase(bnk->InternalId);
}

int32_t BinkDoFrame(BINK* bnk) {
    if (!bnk) return -1;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return -1;
    auto& s = *it->second;
    if (!s.pub.Paused) {
        SynthesizeFrame(s);
        s.pub.LastFrameNum = s.pub.FrameNum;
        ++s.played_frames;
    }
    return 0;  // 0 = frame decoded (no skip)
}

void BinkNextFrame(BINK* bnk) {
    if (!bnk) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return;
    auto& s = *it->second;
    if (!s.pub.Paused) {
        s.pub.FrameNum = (s.pub.FrameNum % std::max(1u, s.pub.Frames)) + 1u;
    }
}

int32_t BinkWait(BINK* bnk) {
    if (!bnk) return 0;
    return bnk->Paused ? 1 : 0;  // 0 = ready for next frame immediately
}

int32_t BinkCopyToBuffer(
    BINK* bnk,
    void* dest,
    int32_t dest_pitch,
    uint32_t dest_height,
    uint32_t dest_x,
    uint32_t dest_y,
    uint32_t flags) {
    if (!bnk || !dest || dest_pitch <= 0 || dest_height == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return -1;
    const auto& s = *it->second;

    const uint32_t fmt = flags & 0x0Fu;
    const bool is_bgra = (fmt == BINKSURFACE32BGRA);
    const uint32_t copy_h = std::min(s.pub.Height, dest_height > dest_y ? dest_height - dest_y : 0u);
    auto* dst_base = static_cast<uint8_t*>(dest);

    for (uint32_t y = 0; y < copy_h; ++y) {
        uint8_t* dst_row = dst_base + static_cast<size_t>(dest_y + y) * static_cast<size_t>(dest_pitch) +
                           static_cast<size_t>(dest_x) * 4ull;
        const uint8_t* src_row = s.frame_rgba.data() + static_cast<size_t>(y) * s.pub.Width * 4ull;
        if (!is_bgra) {
            std::memcpy(dst_row, src_row, static_cast<size_t>(s.pub.Width) * 4ull);
        } else {
            for (uint32_t x = 0; x < s.pub.Width; ++x) {
                dst_row[x * 4 + 0] = src_row[x * 4 + 2];
                dst_row[x * 4 + 1] = src_row[x * 4 + 1];
                dst_row[x * 4 + 2] = src_row[x * 4 + 0];
                dst_row[x * 4 + 3] = src_row[x * 4 + 3];
            }
        }
    }
    return 0;
}

int32_t BinkPause(BINK* bnk, int32_t pause) {
    if (!bnk) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return 0;
    it->second->pub.Paused = pause ? 1u : 0u;
    return static_cast<int32_t>(it->second->pub.Paused);
}

void BinkGoto(BINK* bnk, uint32_t frame, int32_t flags) {
    (void)flags;
    if (!bnk) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return;
    auto& s = *it->second;
    const uint32_t clamped = std::clamp(frame, 1u, std::max(1u, s.pub.Frames));
    s.pub.FrameNum = clamped;
    SynthesizeFrame(s);
}

void BinkSetSoundOnOff(BINK* bnk, int32_t onoff) {
    if (!bnk) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it != g_streams.end()) {
        it->second->pub.SoundOn = onoff ? 1u : 0u;
    }
}

void BinkSetVolume(BINK* bnk, uint32_t track_id, int32_t volume) {
    (void)track_id;
    if (!bnk) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it != g_streams.end()) {
        it->second->pub.Volume = std::clamp(volume, 0, 65536);
    }
}

void BinkGetSummary(BINK* bnk, BINKSUMMARY* sum) {
    if (!bnk || !sum) return;
    std::memset(sum, 0, sizeof(*sum));
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return;
    const auto& s = *it->second;
    sum->Width = s.pub.Width;
    sum->Height = s.pub.Height;
    sum->FileFrameRate = s.pub.FrameRate;
    sum->FileFrameRateDiv = s.pub.FrameRateDiv;
    sum->FrameRate = s.pub.FrameRate;
    sum->FrameRateDiv = s.pub.FrameRateDiv;
    sum->TotalFrames = s.pub.Frames;
    sum->TotalPlayedFrames = s.played_frames;
    sum->TotalTime = (s.pub.Frames * 1000u * std::max(1u, s.pub.FrameRateDiv)) /
                     std::max(1u, s.pub.FrameRate);
}

const char* BinkGetError(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_last_error.c_str();
}

}  // extern "C"

// ---------------------------------------------------------------------------
// Bink 2 entry points the retail title imports but whose parameter shapes are
// not public. Every one of them logs its raw arguments on first call and then
// reports the "success" that lets the title continue without a decoder. No
// argument is dereferenced for writing unless it passes WritablePointer.
// ---------------------------------------------------------------------------

extern "C" {

// BINK* BinkOpenWithOptions(const char* name, uint32_t flags, BINK** out_bink)
// There is no decoder, so the stream cannot be opened: report failure and let
// the title skip the movie.
SCARLETTX_BINK2_API BINK* __stdcall BinkOpenWithOptions(const char* name,
                                                        uint32_t flags,
                                                        BINK** out_bink) {
    LogFirstCall("BinkOpenWithOptions", reinterpret_cast<uint64_t>(name), flags,
                 reinterpret_cast<uint64_t>(out_bink), 0);
    LogWarningOnce("BinkOpenWithOptions",
                   "BinkOpenWithOptions: video decode is not implemented; the "
                   "title's Bink stream will be skipped");
    // A valid non-null handle is what the call site asserts on. BinkOpen already
    // synthesises a stream even when the file is missing, so the intro is opened
    // and skipped by BinkShouldSkip instead of trapping the loader.
    BINK* opened = BinkOpen(name, flags);
    if (WritablePointer(out_bink, sizeof(BINK*))) {
        *out_bink = opened;
    }
    scarlettx::HleTrace("BinkOpenWithOptions",
                        reinterpret_cast<uint64_t>(opened),
                        reinterpret_cast<uint64_t>(name), flags);
    return opened;
}

// void BinkSetMemory(void* alloc_fn, void* free_fn)
SCARLETTX_BINK2_API void __stdcall BinkSetMemory(void* alloc_fn,
                                                 void* free_fn) {
    BinkAllocFn().store(alloc_fn, std::memory_order_relaxed);
    BinkFreeFn().store(free_fn, std::memory_order_relaxed);
    LogFirstCall("BinkSetMemory", reinterpret_cast<uint64_t>(alloc_fn),
                 reinterpret_cast<uint64_t>(free_fn), 0, 0);
}

// int32_t BinkSetSoundSystem(void* system, void* param)
SCARLETTX_BINK2_API int32_t __stdcall BinkSetSoundSystem(void* system,
                                                         void* param) {
    LogFirstCall("BinkSetSoundSystem", reinterpret_cast<uint64_t>(system),
                 reinterpret_cast<uint64_t>(param), 0, 0);
    return 0;  // accepted; the stub has no audio path of its own
}

// int32_t BinkSetOSFileCallbacks(void* open, void* close, void* read, void* seek)
SCARLETTX_BINK2_API int32_t __stdcall BinkSetOSFileCallbacks(void* open_fn,
                                                             void* close_fn,
                                                             void* read_fn,
                                                             void* seek_fn) {
    LogFirstCall("BinkSetOSFileCallbacks", reinterpret_cast<uint64_t>(open_fn),
                 reinterpret_cast<uint64_t>(close_fn),
                 reinterpret_cast<uint64_t>(read_fn),
                 reinterpret_cast<uint64_t>(seek_fn));
    return 0;
}

// int32_t BinkGetPlatformInfo(BINK* bnk, void* platform_info)  (64 bytes out)
SCARLETTX_BINK2_API int32_t __stdcall BinkGetPlatformInfo(BINK* bnk,
                                                          void* platform_info) {
    LogFirstCall("BinkGetPlatformInfo", reinterpret_cast<uint64_t>(bnk),
                 reinterpret_cast<uint64_t>(platform_info), 0, 0);
    if (WritablePointer(platform_info, 64)) {
        std::memset(platform_info, 0, 64);
    }
    return 0;
}

// void BinkGetFrameBuffersInfo(BINK* bnk, void* info)  (128 bytes out)
SCARLETTX_BINK2_API void __stdcall BinkGetFrameBuffersInfo(BINK* bnk,
                                                           void* info) {
    LogFirstCall("BinkGetFrameBuffersInfo", reinterpret_cast<uint64_t>(bnk),
                 reinterpret_cast<uint64_t>(info), 0, 0);
    if (WritablePointer(info, 128)) {
        std::memset(info, 0, 128);
    }
}

// void BinkRegisterFrameBuffers(BINK* bnk, void* frame_buffer)
SCARLETTX_BINK2_API void __stdcall BinkRegisterFrameBuffers(BINK* bnk,
                                                            void* frame_buffer) {
    LogFirstCall("BinkRegisterFrameBuffers", reinterpret_cast<uint64_t>(bnk),
                 reinterpret_cast<uint64_t>(frame_buffer), 0, 0);
}

// void BinkGetKeyFrame(BINK* bnk, uint32_t frame, int32_t flags)
SCARLETTX_BINK2_API void __stdcall BinkGetKeyFrame(BINK* bnk, uint32_t frame,
                                                   int32_t flags) {
    LogFirstCall("BinkGetKeyFrame", reinterpret_cast<uint64_t>(bnk), frame,
                 static_cast<uint64_t>(static_cast<uint32_t>(flags)), 0);
}

// void BinkSetWillLoop(BINK* bnk, int32_t loop_count)
SCARLETTX_BINK2_API void __stdcall BinkSetWillLoop(BINK* bnk,
                                                   int32_t loop_count) {
    LogFirstCall("BinkSetWillLoop", reinterpret_cast<uint64_t>(bnk),
                 static_cast<uint64_t>(static_cast<uint32_t>(loop_count)), 0,
                 0);
}

// int32_t BinkShouldSkip(BINK* bnk) -- BOOL in EAX. Without a decoder the
// caller must skip the video rather than display uninitialised frame buffers.
SCARLETTX_BINK2_API int32_t __stdcall BinkShouldSkip(BINK* bnk) {
    LogFirstCall("BinkShouldSkip", reinterpret_cast<uint64_t>(bnk), 0, 0, 0);
    return 1;
}

// void BinkStartAsyncThread(BINK* bnk, void* param)
SCARLETTX_BINK2_API void __stdcall BinkStartAsyncThread(BINK* bnk,
                                                        void* param) {
    LogFirstCall("BinkStartAsyncThread", reinterpret_cast<uint64_t>(bnk),
                 reinterpret_cast<uint64_t>(param), 0, 0);
}

// void BinkRequestStopAsyncThreadsMulti(BINK* bnk, uint32_t count)
SCARLETTX_BINK2_API void __stdcall BinkRequestStopAsyncThreadsMulti(
    BINK* bnk, uint32_t count) {
    LogFirstCall("BinkRequestStopAsyncThreadsMulti",
                 reinterpret_cast<uint64_t>(bnk), count, 0, 0);
}

// void BinkWaitStopAsyncThreadsMulti(BINK* bnk, uint32_t count)
SCARLETTX_BINK2_API void __stdcall BinkWaitStopAsyncThreadsMulti(
    BINK* bnk, uint32_t count) {
    LogFirstCall("BinkWaitStopAsyncThreadsMulti",
                 reinterpret_cast<uint64_t>(bnk), count, 0, 0);
}

// int32_t BinkDoFrameAsyncMulti(BINK* bnk, uint32_t count, void* param)
SCARLETTX_BINK2_API int32_t __stdcall BinkDoFrameAsyncMulti(BINK* bnk,
                                                            uint32_t count,
                                                            void* param) {
    LogFirstCall("BinkDoFrameAsyncMulti", reinterpret_cast<uint64_t>(bnk),
                 count, reinterpret_cast<uint64_t>(param), 0);
    return 0;
}

// int32_t BinkDoFrameAsyncWait(BINK* bnk, int32_t timeout_ms) -- 0 means the
// (non-existent) async decode is already finished.
SCARLETTX_BINK2_API int32_t __stdcall BinkDoFrameAsyncWait(BINK* bnk,
                                                           int32_t timeout_ms) {
    LogFirstCall("BinkDoFrameAsyncWait", reinterpret_cast<uint64_t>(bnk),
                 static_cast<uint64_t>(static_cast<uint32_t>(timeout_ms)), 0,
                 0);
    return 0;
}

// void BinkFreeGlobals(void)
SCARLETTX_BINK2_API void __stdcall BinkFreeGlobals(void) {
    LogFirstCall("BinkFreeGlobals", 0, 0, 0, 0);
}

}  // extern "C"
