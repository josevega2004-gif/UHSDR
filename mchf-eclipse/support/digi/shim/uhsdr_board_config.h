/* UHSDR digital mode host bench - stand-in for hardware/uhsdr_board_config.h
 *
 * The real header selects a UI and an RF board and then includes that board's CMSIS headers
 * (stm32f4xx.h, core_cm4.h) - it is about registers and interrupt frequencies, none of which exist
 * on a host. drivers/audio/audio_agc.c includes it for two numbers: the rate the AGC's ring buffer
 * is sized by and the block size its interface takes. Those are mirrored here, at the values the
 * mcHF build uses, and nothing else: a firmware source that reaches for a board setting the bench
 * has not mirrored fails to compile instead of silently getting a wrong constant.
 *
 * audio_agc.c finds this header ahead of hardware/uhsdr_board_config.h because quoted includes
 * search the compiler's -I list in order and the shim directory comes before hardware/. (That is
 * also why a shim cannot stand in for audio_driver.h: that one sits next to the source that
 * includes it, and the including file's own directory is searched first.)
 */
#ifndef DIGI_BENCH_UHSDR_BOARD_CONFIG_H
#define DIGI_BENCH_UHSDR_BOARD_CONFIG_H

#define AUDIO_SAMPLE_RATE   (48000)                             // hardware/uhsdr_board_config.h
#define IQ_SAMPLE_RATE      (48000)
#define IQ_INTERRUPT_FREQ   (1500)
#define AUDIO_BLOCK_SIZE    (AUDIO_SAMPLE_RATE / IQ_INTERRUPT_FREQ)   // 32 samples
#define IQ_BLOCK_SIZE       (IQ_SAMPLE_RATE / IQ_INTERRUPT_FREQ)      // 32 samples

#endif
