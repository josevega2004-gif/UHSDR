/*  -*-  mode: c; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4; coding: utf-8  -*- */
/**
 * @file si5351_port.h
 * @brief the only two functions a platform must provide for the vendored si5351 driver
 *        (host bench: logging stubs; FK743: HAL_I2C, see port_stm32f743.c)
 */
#ifndef SI5351_PORT_H
#define SI5351_PORT_H

#include <stdint.h>

// module boards use 0x60 or 0x62 depending on the XO/MSI vendor; check with an I2C scan
#ifndef SI5351_PORT_I2C_ADDRESS
#define SI5351_PORT_I2C_ADDRESS 0x60
#endif

// write one register, blocking, retry on NACK until the chip answers
void si5351_port_i2c_write(uint8_t address, uint8_t reg, uint8_t value);

#endif
