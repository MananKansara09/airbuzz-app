#ifndef AB_SESSION_H
#define AB_SESSION_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct AB_Session AB_Session;
typedef enum { AB_OP_READ, AB_OP_TAP, AB_OP_DOUBLE } AB_Operation;
enum { AB_OK = 0, AB_ERR_ARGUMENT = -1, AB_ERR_BUSY = -2, AB_ERR_MEMORY = -3,
       AB_ERR_STACK = -4, AB_ERR_TLS = -5, AB_ERR_TIMEOUT = -6,
       AB_ERR_COAP = -7, AB_ERR_SETTING = -8, AB_ERR_LINK = -9 };
/* Callbacks run on the loop thread. Copy borrowed data before returning.
 * Destroy must run on another thread. Stop external calls before Destroy. */
typedef void (*AB_SendFn)(void*, const uint8_t*, size_t);
typedef void (*AB_DoneFn)(void*, int, int, const char*);
typedef void (*AB_LogFn)(void*, const char*);
AB_Session* AB_Session_Create(AB_SendFn, AB_DoneFn, AB_LogFn, void*, size_t);
void AB_Session_Receive(AB_Session*, const uint8_t*, size_t);
void AB_Session_LinkLost(AB_Session*);
int AB_Session_Start(AB_Session*, AB_Operation);
/* Abort preserves a short restore/handoff window; done fires within 3 seconds.
 * IsMuted is thread-safe and also true after done. */
void AB_Session_Abort(AB_Session*);
int AB_Session_IsMuted(AB_Session*);
void AB_Session_Destroy(AB_Session*);
#ifdef AB_TESTING
/* Whole deadline includes the reserved handoff interval. Set before Start. */
void AB_Session_SetTimeouts(AB_Session*, unsigned, unsigned, unsigned);
#endif
#ifdef __cplusplus
}
#endif
#endif
