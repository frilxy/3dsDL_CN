#include "platform.hpp"

#include "timezone.hpp"

#include <3ds.h>

#include <cstdio>
#include <ctime>
#include <vector>

namespace platform {

Wifi wifiStatus()
{
    u32 status = 0;
    if (R_FAILED(ACU_GetWifiStatus(&status))) {
        return Wifi::Unknown;
    }
    return status == 0 ? Wifi::Disconnected : Wifi::Connected;
}

int64_t sdFreeBytes()
{
    FS_ArchiveResource resource {};
    if (R_FAILED(FSUSER_GetArchiveResource(&resource, SYSTEM_MEDIATYPE_SD))) {
        return -1;
    }
    return static_cast<int64_t>(resource.clusterSize) *
           static_cast<int64_t>(resource.freeClusters);
}

uint64_t sdClusterSize()
{
    FS_ArchiveResource resource {};
    if (R_FAILED(FSUSER_GetArchiveResource(&resource, SYSTEM_MEDIATYPE_SD))) {
        return 0;
    }
    return resource.clusterSize;
}

int64_t sdTotalBytes()
{
    FS_ArchiveResource resource {};
    if (R_FAILED(FSUSER_GetArchiveResource(&resource, SYSTEM_MEDIATYPE_SD))) {
        return -1;
    }
    return static_cast<int64_t>(resource.clusterSize) *
           static_cast<int64_t>(resource.totalClusters);
}

uint64_t sdCardId()
{
    uint64_t id = 14695981039346656037ULL; // FNV-1a
    const auto add = [&](const u8 *bytes, size_t size) {
        for (size_t index = 0; index < size; ++index) {
            id = (id ^ bytes[index]) * 1099511628211ULL;
        }
    };
    u8 cid[16] = {};
    const bool hasCid = R_SUCCEEDED(FSUSER_GetSdmcCid(cid, sizeof cid));
    if (hasCid) {
        add(cid, sizeof cid);
    }
    const int64_t total = sdTotalBytes();
    if (total > 0) {
        add(reinterpret_cast<const u8 *>(&total), sizeof total);
    }
    return hasCid || total > 0 ? id : 0;
}

int64_t now()
{
    return static_cast<int64_t>(std::time(nullptr));
}

int systemModel()
{
    if (R_FAILED(cfguInit())) {
        return -1;
    }
    u8 model = 0;
    const Result result = CFGU_GetSystemModel(&model);
    cfguExit();
    return R_SUCCEEDED(result) ? model : -1;
}

std::string systemVersion()
{
    // Read from the NVer and CVer system titles, as System Settings does.
    OS_VersionBin nver {};
    OS_VersionBin cver {};
    char text[32] = {};
    if (R_FAILED(osGetSystemVersionDataString(&nver, &cver, text,
                                              sizeof(text)))) {
        return "";
    }
    return text;
}

bool setConsoleClock(int64_t localUnixMilliseconds, std::string &error)
{
    // The console shows the hardware clock plus a user time offset that PTM
    // keeps in memory and in config block 0x30001. SetUserTime moves that
    // offset, so the change shows at once; SetSystemTime then records the
    // time as set, as System Settings does. Writing the block directly would
    // only take effect after a restart.
    const auto explain = [&](const char *step, Result result) {
        char code[16];
        std::snprintf(code, sizeof(code), "0x%08lX",
                      static_cast<unsigned long>(result));
        error = std::string(step) + " was refused (" + code +
                "). Try the CIA build of 3dsDL, or set the clock from "
                "Rosalina: press L + Down + Select, open Miscellaneous "
                "options and choose \"Update time and date via NTP\".";
    };

    Result result = ptmSysmInit();
    if (R_FAILED(result)) {
        explain("Access to the clock service", result);
        return false;
    }
    result = ptmSetsInit();
    if (R_FAILED(result)) {
        ptmSysmExit();
        explain("Access to the clock service", result);
        return false;
    }
    result = cfguInit();
    if (R_FAILED(result)) {
        ptmSetsExit();
        ptmSysmExit();
        explain("Access to the system settings", result);
        return false;
    }

    const s64 msY2k = localUnixMilliseconds - tz::UnixToY2kMs;
    result = PTMSYSM_SetUserTime(msY2k);
    if (R_FAILED(result)) {
        explain("Setting the time", result);
    } else {
        result = PTMSETS_SetSystemTime(msY2k);
        if (R_FAILED(result)) {
            explain("Setting the time", result);
        } else {
            result = CFG_UpdateConfigSavegame();
            if (R_FAILED(result)) {
                explain("Saving the system settings", result);
            }
        }
    }

    cfguExit();
    ptmSetsExit();
    ptmSysmExit();
    return R_SUCCEEDED(result);
}

bool keyboard(KeyboardKind kind, const std::string &hint,
              const std::string &initial, std::string &output)
{
    constexpr int MaxCharacters = 2000;

    SwkbdState state;
    swkbdInit(&state, SWKBD_TYPE_NORMAL, 2,
              kind == KeyboardKind::Name ? 120 : MaxCharacters);
    swkbdSetHintText(&state, hint.c_str());
    swkbdSetButton(&state, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&state, SWKBD_BUTTON_RIGHT,
                   kind == KeyboardKind::Url ? "Download" : "OK", true);
    if (kind == KeyboardKind::Text) {
        swkbdSetValidation(&state, SWKBD_ANYTHING, 0, 0);
        swkbdSetFeatures(&state, SWKBD_DARKEN_TOP_SCREEN | SWKBD_MULTILINE);
    } else {
        swkbdSetValidation(&state, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
        swkbdSetFeatures(&state, SWKBD_DARKEN_TOP_SCREEN);
    }
    if (!initial.empty()) {
        swkbdSetInitialText(&state, initial.c_str());
    }

    // Each UTF-16 unit becomes at most three bytes of UTF-8.
    std::vector<char> buffer(MaxCharacters * 3 + 1, '\0');
    const SwkbdButton button =
        swkbdInputText(&state, buffer.data(), buffer.size());
    if (button != SWKBD_BUTTON_RIGHT) {
        return false;
    }
    output = buffer.data();
    return true;
}

} // namespace platform
