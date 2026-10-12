#include <jeios/status.h>
const char *je_status_name(je_status s)
{
    switch (s) {
    case JE_OK: return "ok";
    case JE_ERR_INVALID: return "invalid";
    case JE_ERR_UNSUPPORTED: return "unsupported";
    case JE_ERR_TIMEOUT: return "timeout";
    case JE_ERR_NOMEM: return "out-of-memory";
    case JE_ERR_BUSY: return "busy";
    case JE_ERR_IO: return "io-error";
    case JE_ERR_DENIED: return "denied";
    default: return "unknown-status";
    }
}
