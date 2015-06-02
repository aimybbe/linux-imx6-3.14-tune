/*
 * Copyright (C) 2011-2013 Freescale Semiconductor, Inc. All Rights Reserved.
 * Copyright (C) 2015 Simon Stürz <simon.stuerz@guh.guru>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/types.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/console.h>
#include <linux/io.h>
#include <linux/bitops.h>
#include <linux/spinlock.h>
#include <linux/mipi_dsi.h>
#include <linux/mxcfb.h>
#include <video/mipi_display.h>

#include "mipi_dsi.h"

/* Source: https://www.kernel.org/doc/Documentation/fb/framebuffer.txt
  +----------+---------------------------------------------+----------+-------+
  |          |                ↑                            |          |       |
  |          |                |upper_margin                |          |       |
  |          |                ↓                            |          |       |
  +----------###############################################----------+-------+
  |          #                ↑                            #          |       |
  |          #                |                            #          |       |
  |          #                |                            #          |       |
  |          #                |                            #          |       |
  |   left   #                |                            #  right   | hsync |
  |  margin  #                |       xres                 #  margin  |  len  |
  |<-------->#<---------------+--------------------------->#<-------->|<----->|
  |          #                |                            #          |       |
  |          #                |                            #          |       |
  |          #                |                            #          |       |
  |          #                |yres                        #          |       |
  |          #                |                            #          |       |
  |          #                |                            #          |       |
  |          #                |                            #          |       |
  |          #                |                            #          |       |
  |          #                |                            #          |       |
  |          #                |                            #          |       |
  |          #                |                            #          |       |
  |          #                |                            #          |       |
  |          #                ↓                            #          |       |
  +----------###############################################----------+-------+
  |          |                ↑                            |          |       |
  |          |                |lower_margin                |          |       |
  |          |                ↓                            |          |       |
  +----------+---------------------------------------------+----------+-------+
  |          |                ↑                            |          |       |
  |          |                |vsync_len                   |          |       |
  |          |                ↓                            |          |       |
  +----------+---------------------------------------------+----------+-------+

  Source: Datatsheet
  ________________________________________
 |    Linux          |   Orise   | value  |
 |    name           | Tech name |        |
 |-------------------+-----------+--------+
 | left margin       |   HBP     |   12   |
 | right margin      |   HFP     |   12   |
 | upper margin      |   VBP     |   2    |
 | lower margin      |   VFP     |   18   |
 | hsync length      |   HSW     |   10   |
 | vsync length      |   VSW     |   2    |
 |-------------------+-----------+--------|

  Pixel clock = 1*10^12 [ps] / ( (xRes + leftMargin + rightMargin + hSync) * (yRes + upperMargin + lowerMargin + vSync) * refresh) =
              = 1*10^12 [ps] / ( (320+12+12+10)*(320+2+18+2)*60 ) = 137663.6821 = 137663

  $ fbset --info
  mode "320x320-60"
      # D: 7.264 MHz, H: 20.520 kHz, V: 60.000 Hz
      geometry 320 320 320 320 24
      timings 137663 12 12 2 18 10 2
      sync 0x80000000
      rgba 8/16,8/8,8/0,0/0
  endmode

  Frame buffer device information:
      Name        : DISP3 BG
      Address     : 0x40080000
      Size        : 307200
      Type        : PACKED PIXELS
      Visual      : TRUECOLOR
      XPanStep    : 1
      YPanStep    : 1
      YWrapStep   : 1
      LineLength  : 960
      Accelerator : No
*/

#define REFRESH      60
#define XRES         320
#define YRES         320
#define LEFT_MARGIN  12
#define RIGHT_MARGIN 12
#define UPPER_MARGIN 2
#define LOWER_MARGIN 18
#define HSYNC_LEN    10
#define VSYNC_LEN    2
#define PIXCLOCK (1e12/((XRES+LEFT_MARGIN+RIGHT_MARGIN+HSYNC_LEN)*(YRES+UPPER_MARGIN+LOWER_MARGIN+VSYNC_LEN)*REFRESH))
#define CHECK_RETCODE(ret)                  \
do {                                        \
    if (ret < 0) {                          \
        dev_err(&mipi_dsi->pdev->dev,       \
            "%s ERR: ret:%d, line:%d.\n",   \
            __func__, ret, __LINE__);       \
        return ret;                         \
    }                                       \
} while (0)


static struct fb_videomode otm3201a_lcd_modedb[] = {
    {
        "OTM3201A",                 /* name */
        REFRESH,                    /* refresh /frame rate */
        XRES, YRES,                 /* resolution */
        PIXCLOCK,                   /* pixel clock*/
        LEFT_MARGIN, RIGHT_MARGIN,  /* l/r margin */
        UPPER_MARGIN, LOWER_MARGIN, /* u/l margin */
        HSYNC_LEN, VSYNC_LEN,       /* hsync/vsync length */
        FB_SYNC_OE_LOW_ACT,         /* sync */
        FB_VMODE_NONINTERLACED,     /* vmode */
        0,                          /* flag */
    },
};

static struct mipi_lcd_config lcd_config = {
    .virtual_ch     = 0,
    .data_lane_num  = 1,
    .max_phy_clk    = 800,
    .dpi_fmt        = MIPI_RGB565_PACKED,
};

void mipid_otm3201a_get_lcd_videomode(struct fb_videomode **mode, int *size,
                                    struct mipi_lcd_config **data)
{
    *mode = &otm3201a_lcd_modedb[0];
    *size = ARRAY_SIZE(otm3201a_lcd_modedb);
    *data = &lcd_config;
}

int mipid_otm3201a_lcd_setup(struct mipi_dsi_info *mipi_dsi)
{
    u32 buf[DSI_CMD_BUF_MAXSIZE];
    int err;

    /* printk("MIPI DSI pixelclock = %i",PIXCLOCK); */
    dev_info(&mipi_dsi->pdev->dev, "MIPI DSI LCD setup.\n");

    /* Orise Engineering Mode Enable (RF0h) */
    buf[0] = 0xF05447;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 3);
    CHECK_RETCODE(err);

    /* Register Read Mode Enable (RA0h) */
    buf[0] = 0xA000;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 2);
    CHECK_RETCODE(err);

    /* Display Inversion Control (RB1h) */
    buf[0] = 0xB122;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 2);
    CHECK_RETCODE(err);

    /* Memory Data Access Control (R36h)*/
    buf[0] = 0x3600;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 2);
    CHECK_RETCODE(err);

    /* Interface Pixel Format (3AH) (24 bits/pixel) */
    buf[0] = 0x3A77;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 2);
    CHECK_RETCODE(err);

    /* RGB Interface Blanking Porch setting (RB3h)
     *
     ________________________________________
    |    Linux          |   Orise   | value  |
    |    name           | Tech name |        |
    |-------------------+-----------+--------+
    | left margin       |   HBP     |   12   |
    | right margin      |   HFP     |   12   |
    | upper margin      |   VBP     |   2    |
    | lower margin      |   VFP     |   18   |
    | hsync length      |   HSW     |   10   |
    | vsync length      |   VSW     |   2    |
    |-------------------+-----------+--------|

     * VFP... Set the delay period from last valid line to falling edge of Vsync signal
     * VBP... Set the delay period from falling edge of Vsync signal to first valid line.
     * VSW... Set the VSync low pulse width
     * HBP... Set the delay period from falling edge of Hsync signal to first valid line
     * HFP... Set the delay period from last valid line to falling edge of Hsync signal.
     * HSW... Set the HSync low pulse width
     *
     * (0xB3,VFP,VBP,HFP,HBP,(VSW*16+HSW))
     */
     /*buf[0] = 0xB30a0a0a;
     buf[1] = 0x0a2a; */
     buf[0] = 0xB312020c;
     buf[1] = 0x0c2a;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 6);
    CHECK_RETCODE(err);

    /* Mux1 to 9 CKH timing structure register (RBDH) */
    buf[0] = 0xBD000131;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 4);
    CHECK_RETCODE(err);

    /* Display Waveform Cycle setting (RBAh)  */
    buf[0] = 0xBA051520;
    buf[1] = 0x01;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 5);
    CHECK_RETCODE(err);

    /* Landscape MIPI Video Mode One Line Clock Number (RE9h) */
    buf[0] = 0xE916;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 2);
    CHECK_RETCODE(err);

    /* MIPI RX Delay Setting (RE2h) */
    buf[0] = 0xE2F5;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 2);
    CHECK_RETCODE(err);

    /* Gamma Voltage adjust Control (RB5h) */
    buf[0] = 0xB55C5C7A;
    buf[1] = 0xFA;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 5);
    CHECK_RETCODE(err);

    /* Display Inversion Off (R20h) */
    buf[0] = 0x20;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 1);
    CHECK_RETCODE(err);

    /* Gamma (+ polarity) Correction Characteristics Setting R gamma (RC0h) */
    buf[0] = 0xC0000617;
    buf[1] = 0x1116250e;
    buf[2] = 0x0c0c0e0c;
    buf[3] = 0x0f070a3f;
    buf[4] = 0x3f3f;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 18);
    CHECK_RETCODE(err);

    /* Gamma ( - polarity) Correction Characteristics Setting R gamma (RC1h) */
    buf[0] = 0xC1000617;
    buf[1] = 0x1116250e;
    buf[2] = 0x0c0c0e0c;
    buf[3] = 0x0f070a3f;
    buf[4] = 0x3f3f;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 18);
    CHECK_RETCODE(err);

    /* Gamma ( + polarity) Correction Characteristics Setting G gamma (RC2h) */
    buf[0] = 0xC2000617;
    buf[1] = 0x1116250e;
    buf[2] = 0x0c0c0e0c;
    buf[3] = 0x0f070a3f;
    buf[4] = 0x3f3f;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 18);
    CHECK_RETCODE(err);

    /* Gamma ( - polarity) Correction Characteristics Setting G gamma (RC3h) */
    buf[0] = 0xC3000617;
    buf[1] = 0x1116250e;
    buf[2] = 0x0c0c0e0c;
    buf[3] = 0x0f070a3f;
    buf[4] = 0x3f3f;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 18);
    CHECK_RETCODE(err);

    /* Gamma ( + polarity) Correction Characteristics Setting B gamma (RC4h) */
    buf[0] = 0xC4000617;
    buf[1] = 0x1116250e;
    buf[2] = 0x0c0c0e0c;
    buf[3] = 0x0f070a3f;
    buf[4] = 0x3f3f;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 18);
    CHECK_RETCODE(err);

    /* Gamma ( - polarity) Correction Characteristics Setting B gamma (RC5h) */
    buf[0] = 0xC5000617;
    buf[1] = 0x1116250e;
    buf[2] = 0x0c0c0e0c;
    buf[3] = 0x0f070a3f;
    buf[4] = 0x3f3f;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 18);
    CHECK_RETCODE(err);

    /* Idle Mode Off (R38h) */
    buf[0] = 0x38;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 1);
    CHECK_RETCODE(err);


    /* Sleep out (R11h) */
    buf[0] = 0x11;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 1);
    CHECK_RETCODE(err);

    /* wait until sleepout is finished */
    msleep(10);

    /* Display on (R29h) */
    buf[0] = 0x29;
    err = mipi_dsi_pkt_write(mipi_dsi, MIPI_DSI_GENERIC_LONG_WRITE, buf, 1);
    CHECK_RETCODE(err);
    dev_info(&mipi_dsi->pdev->dev, "OTM3201A MIPI DSI setup done.\n");

    /* wait until display is ready */
    msleep(10);

    return err;
}

