/* Host stand-in for the board header. The firmware sources included here only need the build
 * switches, which the bench passes on the command line (-DUSE_RTTY_PROCESSOR, -DRF_BRD_MCHF, ...).
 * Deliberately no hardware registers, no CMSIS, no HAL. */
#ifndef DIGI_BENCH_UHSDR_BOARD_H
#define DIGI_BENCH_UHSDR_BOARD_H
#define HOST_BENCH 1
#endif
