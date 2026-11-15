/*
 * Stamptronic RFM433-LC / LC+ radio modem (as used in the RF2000 radio box)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHAR_RFM433_H
#define HW_CHAR_RFM433_H

#include "chardev/char.h"

#define TYPE_CHARDEV_RFM433 "chardev-rfm433"

/*
 * Create a modem.  The returned character device goes to the host UART
 * (the modem's serial port).  @air, which may be NULL, is the radio side:
 * in transparent mode everything the host sends goes there and everything
 * read from it is "received over the air" and passed to the host.
 */
Chardev *rfm433_new(const char *id, Chardev *air, Error **errp);

#endif
