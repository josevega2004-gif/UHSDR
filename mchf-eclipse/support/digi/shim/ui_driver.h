/* Host stand-in for drivers/ui/ui_driver.h.
 * The RTTY modem asks for exactly two things from the UI layer: a character sink for decoded
 * text and ts.rtty_cwi. Both are captured/observable in modem_host.c. */
#ifndef DIGI_BENCH_UI_DRIVER_H
#define DIGI_BENCH_UI_DRIVER_H
#include <stdint.h>

typedef struct
{
    uint8_t rtty_cwi;   // 1 = reverse sideband (space and mark swapped)
} bench_ts_t;

extern bench_ts_t ts;

void UiDriver_TextMsgPutChar(char ch);
#endif
