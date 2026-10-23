// New in BotW Recomp (based on LAS Recomp by Peiikai, MIT).
// BOTW_RECOMP_RDOC_CAPTURE=<frame>[,<frame>...]: RenderDoc captures those presented frames. Run the game under RenderDoc
// (renderdoccmd capture -c <file template> botw_recomp.exe ...): the in-app API is then loaded with the process, and
// TriggerCapture, called the frame before, captures the next one. Without RenderDoc this does nothing.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "../../../third_party/renderdoc/renderdoc_app.h"
#include "../../rt.h"
#include "vk.h"

// Names a Vulkan object for the capture (with BOTW_RECOMP_RDOC_CAPTURE: device.c enables VK_EXT_debug_utils), so
// RenderDoc shows e.g. the guest program address on each shader module.
void rdoc_name(VkObjectType type, uint64_t handle, const char *name) {
    static int init;
    static PFN_vkSetDebugUtilsObjectNameEXT set;
    if (!init) {
        init = 1;
        if (getenv("BOTW_RECOMP_RDOC_CAPTURE"))
            set = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(g_vk.inst, "vkSetDebugUtilsObjectNameEXT");
    }
    if (!set || !handle) return;
    VkDebugUtilsObjectNameInfoEXT ni = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                                        .objectType = type, .objectHandle = handle, .pObjectName = name};
    set(g_vk.dev, &ni);
}

void rdoc_frame(uint64_t frame) {
#ifdef _WIN32
    static int init;
    static RENDERDOC_API_1_1_2 *api;
    static char frames[256];
    if (!init) {
        init = 1;
        const char *e = getenv("BOTW_RECOMP_RDOC_CAPTURE");
        if (!e) return;
        snprintf(frames, sizeof frames, "%s", e);
        HMODULE m = GetModuleHandleA("renderdoc.dll");
        pRENDERDOC_GetAPI get = m ? (pRENDERDOC_GetAPI)(void *)GetProcAddress(m, "RENDERDOC_GetAPI") : NULL;
        if (!get || !get(eRENDERDOC_API_Version_1_1_2, (void **)&api)) {
            LOG("[rdoc] RenderDoc isn't loaded: run under renderdoccmd capture\n");
            api = NULL;
            return;
        }
        LOG("[rdoc] RenderDoc API ready: capturing frame(s) %s\n", frames);
    }
    if (!api) return;
    for (char *p = frames; *p;) {
        char *end;
        uint64_t f = strtoull(p, &end, 0);
        if (f && frame + 1 == f) {  // (the capture covers the next frame)
            api->TriggerCapture();
            LOG("[rdoc] capturing frame %llu\n", (unsigned long long)f);
        }
        p = *end ? end + 1 : end;
    }
#else
    (void)frame;
#endif
}
