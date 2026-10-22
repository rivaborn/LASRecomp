// nfp:user (amiibo): a reader that is never present. nn::nfp::Initialize needs a real IUser object (the logging stub
// returns none, and the SDK then dereferences null); the game gets an initialized interface with no devices, so
// amiibo features just find nothing to scan. Command ids from switchbrew (NFC services, IUser).
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

typedef struct { uint32_t state; uint32_t availability_event; } NfpUser;

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

static void nfp_user(Service *s, IpcReq *req, IpcResp *resp) {
    NfpUser *u = s->state;
    switch (req->cmd) {
    case 0: u->state = 1; break;                                  // Initialize
    case 1: u->state = 0; break;                                  // Finalize
    case 2: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;      // ListDevices: none
    case 17: case 18:                                             // AttachActivateEvent / AttachDeactivateEvent
        resp->copy_handles[resp->ncopy++] = new_event_handle();
        break;
    case 19: OUT(uint32_t, 0) = u->state; resp_size(resp, 4); break;  // GetState
    case 20: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;     // GetDeviceState: unavailable
    case 23:                                                      // AttachAvailabilityChangeEvent
        if (!u->availability_event) u->availability_event = new_event_handle();
        resp->copy_handles[resp->ncopy++] = u->availability_event;
        break;
    case 24: break;                                               // IsNfcEnabled (newer firmware): false
    default: LOG("[nfp] IUser cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

SERVICE("nfp:user", nfp_user_manager) {
    (void)s;
    if (req->cmd == 0) {  // CreateUserInterface
        NfpUser *u = hle_alloc(sizeof *u);
        ipc_return_object(resp, service_new("nfp:IUser", nfp_user, u));
    }
}
