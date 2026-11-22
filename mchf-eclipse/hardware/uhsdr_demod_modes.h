#ifndef UHSDR_DEMOD_MODES_H
#define UHSDR_DEMOD_MODES_H

/* The receive demodulation modes, on their own because the host bench has to be able to name them:
 * audio_driver.h decides which of them the partitioned convolution filter takes over, and the check of
 * that decision (support/digi/convolution_probe.c) runs on a host, where hardware/uhsdr_board.h cannot
 * be included because it brings in the board registers, the codec and CMSIS.  Copying the enumeration
 * into the bench instead would have made the check agree with the bench's copy rather than with the
 * radio, which is the one thing a check like that must not do. */

typedef enum {
    DEMOD_USB       =   0,
    DEMOD_LSB       =   1,
    DEMOD_CW        =   2,
    DEMOD_AM        =   3,
    DEMOD_SAM       =   4,
    DEMOD_FM        =   5,
    DEMOD_DIGI		=   6,
#ifdef USE_TWO_CHANNEL_AUDIO
    DEMOD_SSBSTEREO =   7,
    DEMOD_IQ        =   8,
#endif
    DEMOD_NUM_MODE
} DemodModes_t;

#define DEMOD_MAX_MODE (DEMOD_NUM_MODE-1)

#endif
