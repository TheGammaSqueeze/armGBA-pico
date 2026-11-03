/* armGBA-pico: LCD Ghosting and Interlaced Rows (see pico_lcd.c).
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
 * the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_PICO_LCD_H
#define ARMGBA_PICO_LCD_H
#include <stdbool.h>
#include <stdint.h>
void picoLcdReset(void);
const uint16_t* picoLcdProcess(const uint16_t* picture, int strength, bool interlaced);
#endif
