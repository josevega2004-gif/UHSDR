/* Host stand-in for the board header. The firmware sources included here only need the build
 * switches, which the bench passes on the command line (-DUSE_RTTY_PROCESSOR, -DRF_BRD_MCHF, ...).
 * Deliberately no hardware registers, no CMSIS, no HAL. */
#ifndef DIGI_BENCH_UHSDR_BOARD_H
#define DIGI_BENCH_UHSDR_BOARD_H
#define HOST_BENCH 1

// The demodulation modes are not a hardware detail and the bench has to check code that switches on
// them, so it takes the real list from hardware/ rather than keeping a copy that could disagree.
#include "../../hardware/uhsdr_demod_modes.h"

#endif
