// Guest-facing DirectStorage objects for real titles.
//
// Titles drive DirectStorage through COM-style factory, queue, file and status
// objects. Requests are read from host files on one worker thread per queue.
//
// Structure layouts follow the public GDK reference for dstorage_xs.h and
// match what a title builds. A vtable slot has a name here only if a title's
// call confirmed it; the others report themselves when called.
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

namespace {

constexpr int32_t kOk = 0;
constexpr int32_t kInvalidArg = static_cast<int32_t>(0x80070057u);
constexpr int32_t kNotImpl = static_cast<int32_t>(0x80004001u);
constexpr int32_t kPointer = static_cast<int32_t>(0x80004003u);
constexpr int32_t kPending = static_cast<int32_t>(0x8000000Au);
constexpr int32_t kOutOfMemory = static_cast<int32_t>(0x8007000Eu);
constexpr int32_t kFail = static_cast<int32_t>(0x80004005u);

constexpr uint16_t kMinQueueCapacity = 0x80;
constexpr uint16_t kMaxQueueCapacity = 0x2000;

// DSTORAGE_REQUEST_OPTIONS bits.
constexpr uint64_t kOptZlib = 1ull << 0;
constexpr uint64_t kOptBcpack = 0xFull << 1;
constexpr uint64_t kOptSwizzle = 0xFFFFull << 5;
constexpr uint64_t kOptDestinationPhysical = 1ull << 21;
constexpr uint64_t kOptSourceMemory = 1ull << 22;
constexpr uint64_t kOptSourcePhysical = 1ull << 23;
constexpr uint64_t kOptUnsupported =
    kOptZlib | kOptBcpack | kOptSwizzle | kOptDestinationPhysical | kOptSourcePhysical;

bool TraceEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("SCARLETTX_TRACE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return on;
}

#define DS_LOG(...)                                                \
    do {                                                           \
        std::fprintf(stderr, "[ScarlettX][dstorage] " __VA_ARGS__); \
        std::fputc('\n', stderr);                                  \
        std::fflush(stderr);                                       \
    } while (0)

// Traces the first calls of a kind only; requests arrive by the thousand.
#define DS_TRACE(limit, ...)                                             \
    do {                                                                 \
        static std::atomic<int> ds_trace_count{0};                       \
        if (TraceEnabled() && ds_trace_count.fetch_add(1) < (limit)) {   \
            DS_LOG(__VA_ARGS__);                                         \
        }                                                                \
    } while (0)

int32_t HresultFromWin32(DWORD error) {
    return error == 0 ? kOk : static_cast<int32_t>((error & 0xFFFFu) | 0x80070000u);
}

struct GuestDStorageConfiguration {
    uint32_t NumSubmitThreads;
    int32_t NumBuiltInCpuDecompressionThreads;
    uint32_t ForceMappingLayer;
    uint32_t DisableBypassIO;
    uint32_t DisableTelemetry;
    uint32_t DisableGpuDecompressionMetacommand;
    uint32_t DisableGpuDecompression;
};
static_assert(sizeof(GuestDStorageConfiguration) == 28);

std::mutex g_configuration_mutex;
GuestDStorageConfiguration g_configuration{};
bool g_has_configuration = false;

// DSTORAGE_QUEUE_DESC
struct QueueDesc {
    uint64_t source_type;  // 0 = file, 1 = memory
    uint16_t capacity;
    int8_t priority;
    const char* name;
};
static_assert(sizeof(QueueDesc) == 0x18 && offsetof(QueueDesc, name) == 0x10, "DSTORAGE_QUEUE_DESC layout");

// DSTORAGE_REQUEST
struct Request {
    uint64_t options;
    void* destination;
    uint64_t destination_page_offset;  // UINT16 in the SDK, padded
    uint32_t destination_size;
    uint32_t reserved;
    void* source;                      // IDStorageFileX* or the source memory
    uint64_t file_offset;
    uint32_t source_size;
    uint32_t intermediate_size;
    uint64_t cancellation_tag;
    const char* name;
};
static_assert(sizeof(Request) == 0x48, "DSTORAGE_REQUEST size");
static_assert(offsetof(Request, destination_size) == 0x18 && offsetof(Request, source) == 0x20 &&
                  offsetof(Request, source_size) == 0x30 && offsetof(Request, name) == 0x40,
              "DSTORAGE_REQUEST layout");

// What the title holds: a pointer to the vtable pointer.
struct Object {
    void* const* vtbl = nullptr;
    std::atomic<int32_t> refs{1};
    void (*destroy)(Object*) = nullptr;
};

uint32_t WINAPI ObjectAddRef(Object* self) {
    return static_cast<uint32_t>(self->refs.fetch_add(1) + 1);
}

uint32_t WINAPI ObjectRelease(Object* self) {
    const int32_t left = self->refs.fetch_sub(1) - 1;
    if (left == 0 && self->destroy) self->destroy(self);
    return static_cast<uint32_t>(left);
}

// Every object has one interface, so any interface id answers with it.
int32_t WINAPI ObjectQueryInterface(Object* self, const void* /*riid*/, void** out) {
    if (!out) return kPointer;
    ObjectAddRef(self);
    *out = self;
    return kOk;
}

const char* const kInterfaceNames[] = {"IDStorageFactoryX", "IDStorageQueueX", "IDStorageFileX",
                                       "IDStorageStatusArrayX"};

template <int Interface, int Slot>
intptr_t WINAPI UnnamedSlot(Object* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    static std::atomic<int> calls{0};
    if (calls.fetch_add(1) < 4) {
        DS_LOG("%s slot %d is not implemented (0x%llx, 0x%llx, 0x%llx)", kInterfaceNames[Interface], Slot,
               static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
               static_cast<unsigned long long>(a3));
    }
    return 0;
}

#define DS_SLOT(fn) reinterpret_cast<void*>(&fn)

// ---- status array -----------------------------------------------------------

struct StatusEntry {
    std::atomic<bool> complete{true};
    std::atomic<int32_t> hr{kOk};
};

struct StatusArray : Object {
    uint32_t capacity = 0;
    StatusEntry* entries = nullptr;
};

void DestroyStatusArray(Object* self) {
    auto* array = static_cast<StatusArray*>(self);
    delete[] array->entries;
    delete array;
}

int32_t WINAPI StatusIsComplete(Object* self, uint32_t index) {
    auto* array = static_cast<StatusArray*>(self);
    return index < array->capacity && array->entries[index].complete.load() ? 1 : 0;
}

int32_t WINAPI StatusGetHResult(Object* self, uint32_t index) {
    auto* array = static_cast<StatusArray*>(self);
    if (index >= array->capacity) return kInvalidArg;
    const StatusEntry& entry = array->entries[index];
    return entry.complete.load() ? entry.hr.load() : kPending;
}

void* const kStatusArrayVtbl[] = {
    DS_SLOT(ObjectQueryInterface), DS_SLOT(ObjectAddRef), DS_SLOT(ObjectRelease),
    DS_SLOT(StatusIsComplete),   // 3, confirmed
    DS_SLOT(StatusGetHResult),   // 4, confirmed
    DS_SLOT((UnnamedSlot<3, 5>)),
};

// ---- file -------------------------------------------------------------------

struct File : Object {
    std::mutex mu;
    HANDLE handle = INVALID_HANDLE_VALUE;
};

void CloseFileLocked(File* file) {
    if (file->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(file->handle);
        file->handle = INVALID_HANDLE_VALUE;
    }
}

void DestroyFile(Object* self) {
    auto* file = static_cast<File*>(self);
    CloseFileLocked(file);
    delete file;
}

void WINAPI FileClose(Object* self) {
    auto* file = static_cast<File*>(self);
    std::lock_guard<std::mutex> lock(file->mu);
    CloseFileLocked(file);
}

// The title owns the returned handle and closes it.
HANDLE WINAPI FileGetHandle(Object* self) {
    auto* file = static_cast<File*>(self);
    std::lock_guard<std::mutex> lock(file->mu);
    HANDLE copy = nullptr;
    if (file->handle == INVALID_HANDLE_VALUE ||
        !DuplicateHandle(GetCurrentProcess(), file->handle, GetCurrentProcess(), &copy, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        return INVALID_HANDLE_VALUE;
    }
    return copy;
}

void* const kFileVtbl[] = {
    DS_SLOT(ObjectQueryInterface), DS_SLOT(ObjectAddRef), DS_SLOT(ObjectRelease),
    DS_SLOT(FileClose),      // 3, confirmed
    DS_SLOT(FileGetHandle),  // 4, confirmed
    DS_SLOT((UnnamedSlot<2, 5>)),
};

// ---- queue ------------------------------------------------------------------

struct Entry {
    bool is_status = false;
    Request request{};
    File* file = nullptr;           // held until the request has run
    StatusArray* status = nullptr;  // held until the status is written
    uint32_t index = 0;
};

void ReleaseEntry(const Entry& entry) {
    if (entry.file) ObjectRelease(entry.file);
    if (entry.status) ObjectRelease(entry.status);
}

struct Queue : Object {
    uint64_t source_type = 0;
    uint16_t capacity = 0;
    std::mutex mu;
    std::condition_variable wake;
    std::vector<Entry> pending;    // enqueued, not yet submitted
    std::deque<Entry> submitted;
    std::thread worker;
    bool stop = false;
    int32_t first_failure = kOk;   // since the last status write; worker thread only
};

int32_t RunRequest(const Queue* queue, const Entry& entry) {
    const Request& r = entry.request;
    if (r.options & kOptUnsupported) return kNotImpl;
    const bool from_memory = (r.options & kOptSourceMemory) != 0;
    if (from_memory != (queue->source_type == 1)) return kInvalidArg;
    if (r.source_size == 0) return kOk;
    if (!r.destination || !r.source || r.destination_size < r.source_size) return kInvalidArg;
    if (from_memory) {
        std::memcpy(r.destination, r.source, r.source_size);
        return kOk;
    }
    std::lock_guard<std::mutex> lock(entry.file->mu);
    if (entry.file->handle == INVALID_HANDLE_VALUE) return HresultFromWin32(ERROR_INVALID_HANDLE);
    if (r.destination_size == 0 || r.destination_size < r.source_size) {
        return kInvalidArg;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) {
        return HresultFromWin32(GetLastError());
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(entry.file->handle, &size) ||
        r.file_offset > static_cast<uint64_t>(size.QuadPart) ||
        r.source_size > static_cast<uint64_t>(size.QuadPart) - r.file_offset) {
        CloseHandle(overlapped.hEvent);
        return HresultFromWin32(ERROR_HANDLE_EOF);
    }
    overlapped.Offset = static_cast<DWORD>(r.file_offset);
    overlapped.OffsetHigh = static_cast<DWORD>(r.file_offset >> 32);

    DWORD bytes = 0;
    const BOOL started =
        ReadFile(entry.file->handle, r.destination, r.source_size, &bytes, &overlapped);
    if (!started) {
        const DWORD read_error = GetLastError();
        if (read_error != ERROR_IO_PENDING) {
            CloseHandle(overlapped.hEvent);
            return HresultFromWin32(read_error);
        }

        const DWORD wait_result = WaitForSingleObject(overlapped.hEvent, 30000);
        if (wait_result != WAIT_OBJECT_0) {
            const DWORD wait_error =
                wait_result == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
            CancelIoEx(entry.file->handle, &overlapped);
            DWORD ignored_bytes = 0;
            GetOverlappedResult(entry.file->handle, &overlapped, &ignored_bytes, TRUE);
            CloseHandle(overlapped.hEvent);
            return HresultFromWin32(wait_error);
        }
        if (!GetOverlappedResult(
                entry.file->handle, &overlapped, &bytes, FALSE)) {
            const DWORD error = GetLastError();
            CloseHandle(overlapped.hEvent);
            return HresultFromWin32(error);
        }
    }
    CloseHandle(overlapped.hEvent);
    return bytes == r.source_size ? kOk : HresultFromWin32(ERROR_HANDLE_EOF);
}

void QueueRun(Queue* queue) {
    std::unique_lock<std::mutex> lock(queue->mu);
    for (;;) {
        queue->wake.wait(lock, [queue] { return queue->stop || !queue->submitted.empty(); });
        if (queue->stop && queue->submitted.empty()) return;
        if (queue->submitted.empty()) continue;
        const Entry entry = queue->submitted.front();
        queue->submitted.pop_front();
        lock.unlock();
        if (entry.is_status) {
            StatusEntry& slot = entry.status->entries[entry.index];
            slot.hr.store(queue->first_failure);
            slot.complete.store(true);
            queue->first_failure = kOk;
        } else {
            const int32_t hr = RunRequest(queue, entry);
            if (hr < 0) {
                if (queue->first_failure >= 0) queue->first_failure = hr;
                static std::atomic<int> reported{0};
                if (reported.fetch_add(1) < 16) {
                    DS_LOG("request failed: hr=0x%08X options=0x%llx offset=%llu size=%u", static_cast<unsigned>(hr),
                           static_cast<unsigned long long>(entry.request.options),
                           static_cast<unsigned long long>(entry.request.file_offset), entry.request.source_size);
                }
            }
        }
        ReleaseEntry(entry);
        lock.lock();
    }
}

// Caller holds queue->mu.
void SubmitLocked(Queue* queue) {
    if (queue->pending.empty()) return;
    queue->submitted.insert(queue->submitted.end(), queue->pending.begin(), queue->pending.end());
    queue->pending.clear();
    queue->wake.notify_one();
}

// Caller holds queue->mu. The console submits on its own once more than
// half of the queue's capacity is waiting.
void EnqueueLocked(Queue* queue, const Entry& entry) {
    queue->pending.push_back(entry);
    if (queue->pending.size() * 2 > queue->capacity) SubmitLocked(queue);
}

void DestroyQueue(Object* self) {
    auto* queue = static_cast<Queue*>(self);
    {
        std::lock_guard<std::mutex> lock(queue->mu);
        SubmitLocked(queue);
        queue->stop = true;
    }
    queue->wake.notify_all();
    if (queue->worker.joinable()) queue->worker.join();
    for (const Entry& entry : queue->pending) ReleaseEntry(entry);
    for (const Entry& entry : queue->submitted) ReleaseEntry(entry);
    delete queue;
}

void WINAPI QueueEnqueueRequest(Object* self, const Request* request) {
    auto* queue = static_cast<Queue*>(self);
    if (!request) return;
    DS_TRACE(8, "EnqueueRequest options=0x%llx file/source=%p offset=%llu size=%u -> %p (%u)",
             static_cast<unsigned long long>(request->options), request->source,
             static_cast<unsigned long long>(request->file_offset), request->source_size, request->destination,
             request->destination_size);
    Entry entry;
    entry.request = *request;
    if (!(request->options & kOptSourceMemory) && request->source) {
        entry.file = static_cast<File*>(static_cast<Object*>(request->source));
        ObjectAddRef(entry.file);
    }
    std::lock_guard<std::mutex> lock(queue->mu);
    EnqueueLocked(queue, entry);
}

void WINAPI QueueEnqueueStatus(Object* self, Object* status_array, uint32_t index) {
    auto* queue = static_cast<Queue*>(self);
    auto* array = static_cast<StatusArray*>(status_array);
    if (!array || index >= array->capacity) {
        DS_LOG("EnqueueStatus ignored: array=%p index=%u", static_cast<void*>(array), index);
        return;
    }
    array->entries[index].complete.store(false);
    Entry entry;
    entry.is_status = true;
    entry.status = array;
    entry.index = index;
    ObjectAddRef(array);
    std::lock_guard<std::mutex> lock(queue->mu);
    EnqueueLocked(queue, entry);
}

void WINAPI QueueSubmit(Object* self) {
    auto* queue = static_cast<Queue*>(self);
    std::lock_guard<std::mutex> lock(queue->mu);
    SubmitLocked(queue);
}

void* const kQueueVtbl[] = {
    DS_SLOT(ObjectQueryInterface), DS_SLOT(ObjectAddRef), DS_SLOT(ObjectRelease),
    DS_SLOT(QueueEnqueueRequest),  // 3, confirmed
    DS_SLOT(QueueEnqueueStatus),   // 4, confirmed
    DS_SLOT((UnnamedSlot<1, 5>)),
    DS_SLOT(QueueSubmit),          // 6, confirmed
    DS_SLOT((UnnamedSlot<1, 7>)),  DS_SLOT((UnnamedSlot<1, 8>)),  DS_SLOT((UnnamedSlot<1, 9>)),
    DS_SLOT((UnnamedSlot<1, 10>)), DS_SLOT((UnnamedSlot<1, 11>)),
};

// ---- factory ----------------------------------------------------------------

int32_t WINAPI FactoryCreateQueue(Object* /*self*/, const QueueDesc* desc, const void* /*riid*/, void** out) {
    if (!out) return kPointer;
    *out = nullptr;
    if (!desc || desc->source_type > 1 || desc->capacity < kMinQueueCapacity || desc->capacity > kMaxQueueCapacity) {
        return kInvalidArg;
    }
    auto* queue = new (std::nothrow) Queue();
    if (!queue) return kOutOfMemory;
    queue->vtbl = kQueueVtbl;
    queue->destroy = &DestroyQueue;
    queue->source_type = desc->source_type;
    queue->capacity = desc->capacity;
    try {
        queue->worker = std::thread(&QueueRun, queue);
    } catch (const std::system_error&) {
        delete queue;
        return kFail;
    } catch (const std::bad_alloc&) {
        delete queue;
        return kOutOfMemory;
    }
    DS_TRACE(8, "CreateQueue source=%llu capacity=%u name=%s -> %p", static_cast<unsigned long long>(desc->source_type),
             desc->capacity, desc->name ? desc->name : "", static_cast<void*>(queue));
    *out = static_cast<Object*>(queue);
    return kOk;
}

int32_t WINAPI FactoryOpenFile(Object* /*self*/, const wchar_t* path, const void* /*riid*/, void** out) {
    if (!out) return kPointer;
    *out = nullptr;
    if (!path) return kInvalidArg;
    const HANDLE handle =
        CreateFileW(path,
                    GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN,
                    nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const int32_t hr = HresultFromWin32(GetLastError());
        DS_LOG("OpenFile failed: %ls hr=0x%08X", path, static_cast<unsigned>(hr));
        return hr;
    }
    auto* file = new (std::nothrow) File();
    if (!file) {
        CloseHandle(handle);
        return kOutOfMemory;
    }
    file->vtbl = kFileVtbl;
    file->destroy = &DestroyFile;
    file->handle = handle;
    DS_TRACE(32, "OpenFile %ls -> %p", path, static_cast<void*>(file));
    *out = static_cast<Object*>(file);
    return kOk;
}

int32_t WINAPI FactoryCreateStatusArray(Object* /*self*/, uint32_t capacity, const char* name, const void* /*riid*/,
                                        void** out) {
    if (!out) return kPointer;
    *out = nullptr;
    if (capacity == 0) return kInvalidArg;
    auto* array = new (std::nothrow) StatusArray();
    StatusEntry* entries = new (std::nothrow) StatusEntry[capacity];
    if (!array || !entries) {
        delete array;
        delete[] entries;
        return kOutOfMemory;
    }
    array->vtbl = kStatusArrayVtbl;
    array->destroy = &DestroyStatusArray;
    array->capacity = capacity;
    array->entries = entries;
    DS_TRACE(8, "CreateStatusArray capacity=%u name=%s -> %p", capacity, name ? name : "", static_cast<void*>(array));
    *out = static_cast<Object*>(array);
    return kOk;
}

void* const kFactoryVtbl[] = {
    DS_SLOT(ObjectQueryInterface), DS_SLOT(ObjectAddRef), DS_SLOT(ObjectRelease),
    DS_SLOT(FactoryCreateQueue),        // 3, confirmed
    DS_SLOT(FactoryOpenFile),           // 4, confirmed
    DS_SLOT(FactoryCreateStatusArray),  // 5, confirmed
    DS_SLOT((UnnamedSlot<0, 6>)),  DS_SLOT((UnnamedSlot<0, 7>)),  DS_SLOT((UnnamedSlot<0, 8>)),
    DS_SLOT((UnnamedSlot<0, 9>)),  DS_SLOT((UnnamedSlot<0, 10>)), DS_SLOT((UnnamedSlot<0, 11>)),
};

// The factory is a process-wide singleton and is never destroyed.
Object g_factory{kFactoryVtbl};

}  // namespace

extern "C" __declspec(dllexport) int32_t WINAPI DStorageGetFactory(const void* riid, void** out) {
    if (!out) return kPointer;
    if (riid) {
        const auto* g = static_cast<const uint32_t*>(riid);
        DS_TRACE(4, "DStorageGetFactory riid=%08x-%08x-%08x-%08x", g[0], g[1], g[2], g[3]);
    }
    ObjectAddRef(&g_factory);
    *out = &g_factory;
    return kOk;
}

extern "C" __declspec(dllexport) int32_t WINAPI DStorageSetConfiguration(
    const void* config_pointer) {
    if (!config_pointer) return kInvalidArg;
    const auto* config =
        static_cast<const GuestDStorageConfiguration*>(config_pointer);
    {
        std::lock_guard<std::mutex> lock(g_configuration_mutex);
        g_configuration = *config;
        g_has_configuration = true;
    }
    DS_TRACE(1, "SetConfiguration submit_threads=%u cpu_decompression_threads=%d",
             config->NumSubmitThreads,
             config->NumBuiltInCpuDecompressionThreads);
    return kOk;
}

#else  // !_WIN32

extern "C" int32_t DStorageGetFactory(const void*, void** out) {
    if (out) *out = nullptr;
    return static_cast<int32_t>(0x80004001u);
}

extern "C" int32_t DStorageSetConfiguration(const void*) {
    return static_cast<int32_t>(0x80004001u);
}

#endif
