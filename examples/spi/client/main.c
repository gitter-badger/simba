/**
 * @file main.c
 * @version 1.0
 *
 * @section License
 * Copyright (C) 2014-2015, Erik Moqvist
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * This file is part of the Simba project.
 */

#include "simba.h"

#define DS18B20_ID { 0x28, 0x09, 0x1e, 0xa3, 0x05, 0x00, 0x00, 0x42 }

FS_COMMAND_DEFINE("/temp/set_min_max", set_min_max);

static volatile long temp_min = 230000;
static volatile long temp_max = 290000;

int set_min_max(int argc,
                const char *argv[],
                void *out_p,
                void *in_p)
{
    long min, max;

    UNUSED(in_p);

    if (argc != 3) {
        std_fprintf(out_p, FSTR("2 argument required.\r\n"));
        return (1);
    }

    if ((std_strtol(argv[1], &min) != 0) ||
        (std_strtol(argv[2], &max) != 0)) {
        std_fprintf(out_p,
                    FSTR("bad min or max value '%s' '%s'\r\n"),
                    argv[1],
                    argv[2]);
        return (1);
    }

    temp_min = 10000 * min;
    temp_max = 10000 * max;
    std_fprintf(out_p,
                FSTR("min set to %ld and max set to %ld\r\n"),
                temp_min / 10000,
                temp_max / 10000);

    return (0);
}

static char qinbuf[32];
static struct uart_driver_t uart;
static struct shell_args_t shell_args;
static THRD_STACK(shell_stack, 456);

int main()
{
    struct owi_driver_t owi;
    struct ds18b20_driver_t ds;
    struct owi_device_t devices[4];
    struct spi_driver_t spi;
    int read_temp;
    long temp, resolution;
    uint8_t state;
    uint8_t id[8] = DS18B20_ID;

    sys_start();
    uart_module_init();

    uart_init(&uart, &uart_device[0], 38400, qinbuf, sizeof(qinbuf));
    uart_start(&uart);

    spi_init(&spi,
             &spi_device[0],
             &pin_d10_dev,
             SPI_MODE_MASTER,
             SPI_SPEED_250KBPS,
             1,
             1);

    /* Initialize temperature sensor. */
    owi_init(&owi, &pin_d6_dev, devices, membersof(devices));
    ds18b20_init(&ds, &owi);

    shell_args.chin_p = &uart.chin;
    shell_args.chout_p = &uart.chout;
    thrd_spawn(shell_entry,
               &shell_args,
               0,
               shell_stack,
               sizeof(shell_stack));

    /* Read temperature periodically. */
    while (1) {
        /* Read temperature. */
        ds18b20_convert(&ds);
        ds18b20_get_temperature(&ds, id, &read_temp);

        temp = read_temp;
        temp = (10000 * (temp >> 4) + 625 * (temp & 0xf));

        /* Update led. */
        if (temp <= temp_min) {
            state = 0x1;
        } else if (temp >= temp_max) {
            state = 0x7;
        } else {
            temp -= temp_min;
            resolution = ((temp_max - temp_min) / 8);
            temp /= resolution;
            state = temp;

            if (state == 0x0) {
                state = 0x1;
            }
        }

        /* Send state to server. */
        spi_write(&spi, &state, sizeof(state));
    }

    return (0);
}
