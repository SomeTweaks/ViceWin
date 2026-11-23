#include "scarlettx/loader/pe_loader.h"

#include <algorithm>
#include <cctype>

namespace scarlettx {
namespace {

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value;
}

}  // namespace

DllRedirectionTable::DllRedirectionTable() {
    RegisterRedirect("kernelx.dll", "kernel32.dll", false);
    RegisterRedirect("mfplat.dll", "mfplat.dll", false);
    RegisterRedirect("mfreadwrite.dll", "mfreadwrite.dll", false);
    RegisterRedirect("d3d12_xs.dll", "d3d12_xs.dll", true);
    RegisterRedirect("xg_xs.dll", "xg_xs.dll", true);
    RegisterRedirect("acphal.dll", "AcpHal.dll", true);
    RegisterRedirect("xgameruntime.dll", "xgameruntime.dll", true);
    RegisterRedirect("xmem.dll", "xgameruntime.dll", true);
    RegisterRedirect("xcrdapi.dll", "xcrdapi.dll", true);
    RegisterRedirect("xboxbvhbuilder.dll", "XboxBvhBuilder.dll", true);
    RegisterRedirect("dstorage_xs.dll", "dstorage_xs.dll", true);
    // `dstorage.dll` (GDK desktop) and `dstorage_xs.dll` (console) are two
    // different ABI surfaces: the former exports DStorageEnqueueRequest and
    // friends, the latter exports the riid-taking DStorageGetFactory. Pointing
    // both at the console shim left every GDK-style entry point unresolvable,
    // so keep them on their own shim.
    RegisterRedirect("dstorage.dll", "dstorage.dll", true);
    RegisterRedirect("dstoragecore.dll", "dstorage_xs.dll", true);
    RegisterRedirect("xaudio2_9.dll", "xaudio2_9.dll", true);
    RegisterRedirect("xaudio2_9redist.dll", "xaudio2_9.dll", true);
    RegisterRedirect("gameinput.dll", "GameInput.dll", true);
    RegisterRedirect("xcurl.dll", "XCurl.dll", true);
    RegisterRedirect("libhttpclient.gdk.dll", "libHttpClient.GDK.dll", true);
    RegisterRedirect("bink2scarlett.dll", "Bink2Scarlett.dll", true);
    RegisterRedirect("amd_fidelityfx_loader_xbox.dll",
                     "amd_fidelityfx_loader_xbox.dll", true);
    RegisterRedirect("amd_fidelityfx_upscaler_xbox.dll",
                     "amd_fidelityfx_upscaler_xbox.dll", true);
    RegisterRedirect("pixevt.dll", "pixevt_stub.dll", true);
    RegisterRedirect("chad.dll", "chad.dll", true);
    // xapu.dll is the APU command-stream module; its entry points live in the
    // AcpHal shim next to the Apu* allocation surface.
    RegisterRedirect("xapu.dll", "AcpHal.dll", true);
}

void DllRedirectionTable::RegisterRedirect(const std::string& guest_dll,
                                           const std::string& host_dll,
                                           bool is_hle) {
    entries_[ToLowerAscii(guest_dll)] = Entry{host_dll, is_hle};
}

bool DllRedirectionTable::Resolve(const std::string& guest_dll,
                                  std::string& out_host_dll,
                                  bool& out_is_hle) const {
    const auto it = entries_.find(ToLowerAscii(guest_dll));
    if (it == entries_.end()) return false;
    out_host_dll = it->second.host_dll;
    out_is_hle = it->second.is_hle;
    return true;
}

}  // namespace scarlettx
