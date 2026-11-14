/*  -*-  mode: c; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4; coding: utf-8  -*-  */
/************************************************************************************
**                                                                                 **
**                               mcHF QRP Transceiver                              **
**                             K Atanassov - M0NKA 2014                            **
**                                                                                 **
**---------------------------------------------------------------------------------**
**                                                                                 **
**  File name:                                                                     **
**  Description:                                                                   **
**  Last Modified:                                                                 **
**  Licence:        GNU GPLv3                                                      **
************************************************************************************/
#ifndef __CONFIG_STORAGE_H
#define __CONFIG_STORAGE_H

/*
 * Number of configuration values the storage can address, the highest usable value id is therefore
 * NB_OF_VAR - 1. Every value occupies one 32 bit record in the flash page used for the EEPROM
 * emulation and two bytes in a serial EEPROM. The flash page has to hold several complete saves
 * before it is swapped, which is what limits this number, see uhsdr_flash.c.
 */
#define NB_OF_VAR               (0x3ff)


void ConfigStorage_Init();

uint16_t ConfigStorage_ReadVariable(uint16_t addr, uint16_t *value);
uint16_t ConfigStorage_WriteVariable(uint16_t addr, uint16_t value);

void ConfigStorage_CopyFlash2Serial(void);
void ConfigStorage_CopySerial2Flash(void);

void ConfigStorage_CopySerial2RAMCache();
uint16_t ConfigStorage_CopyRAMCache2Serial();

uint16_t ConfigStorage_CopyArray2Serial(uint32_t Addr, const uint8_t *buffer, uint16_t length);
void ConfigStorage_CopySerial2Array(uint32_t Addr, uint8_t *buffer, uint16_t length);

#endif
