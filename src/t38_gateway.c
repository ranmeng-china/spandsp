//#define LOG_FAX_AUDIO
/*
 * SpanDSP - a series of DSP components for telephony
 *
 * t38_gateway.c - A T.38 gateway, less the packet exchange part
 *
 * Written by Steve Underwood <steveu@coppice.org>
 *
 * Copyright (C) 2005, 2006, 2007, 2008 Steve Underwood
 *
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * $Id: t38_gateway.c,v 1.139 2008/08/17 16:25:52 steveu Exp $
 */

/*! \file */

#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include "floating_fudge.h"
#if defined(HAVE_TGMATH_H)
#include <tgmath.h>
#endif
#if defined(HAVE_MATH_H)
#include <math.h>
#endif
#include <assert.h>
#if defined(LOG_FAX_AUDIO)
#include <unistd.h>
#endif
#include <tiffio.h>

#include "spandsp/telephony.h"
#include "spandsp/logging.h"
#include "spandsp/queue.h"
#include "spandsp/dc_restore.h"
#include "spandsp/bit_operations.h"
#include "spandsp/power_meter.h"
#include "spandsp/complex.h"
#include "spandsp/tone_detect.h"
#include "spandsp/tone_generate.h"
#include "spandsp/async.h"
#include "spandsp/crc.h"
#include "spandsp/hdlc.h"
#include "spandsp/silence_gen.h"
#include "spandsp/fsk.h"
#include "spandsp/v29rx.h"
#include "spandsp/v29tx.h"
#include "spandsp/v27ter_rx.h"
#include "spandsp/v27ter_tx.h"
#include "spandsp/v17rx.h"
#include "spandsp/v17tx.h"
#include "spandsp/super_tone_rx.h"
#include "spandsp/modem_connect_tones.h"
#include "spandsp/t4.h"
#include "spandsp/t30_fcf.h"
#include "spandsp/t35.h"
#include "spandsp/t30.h"
#include "spandsp/t30_logging.h"

#include "spandsp/fax_modems.h"
#include "spandsp/t38_core.h"
#include "spandsp/t38_non_ecm_buffer.h"
#include "spandsp/t38_gateway.h"

/* This is the target time per transmission chunk. The actual
   packet timing will sync to the data octets. */
#define MS_PER_TX_CHUNK                 30
#define HDLC_START_BUFFER_LEVEL         8

#define INDICATOR_TX_COUNT              3
#define DATA_TX_COUNT                   1
#define DATA_END_TX_COUNT               3

enum
{
    DISBIT1 = 0x01,
    DISBIT2 = 0x02,
    DISBIT3 = 0x04,
    DISBIT4 = 0x08,
    DISBIT5 = 0x10,
    DISBIT6 = 0x20,
    DISBIT7 = 0x40,
    DISBIT8 = 0x80
};

enum
{
    T38_NONE,
    T38_V27TER_RX,
    T38_V29_RX,
    T38_V17_RX
};

enum
{
    HDLC_FLAG_FINISHED = 0x01,
    HDLC_FLAG_CORRUPT_CRC = 0x02,
    HDLC_FLAG_PROCEED_WITH_OUTPUT = 0x04,
    HDLC_FLAG_MISSING_DATA = 0x08
};

enum
{
    FLAG_INDICATOR = 0x100,
    FLAG_DATA = 0x200
};

#define MAX_NSX_SUPPRESSION             10

#define HDLC_FRAMING_OK_THRESHOLD       5

static uint8_t nsx_overwrite[2][MAX_NSX_SUPPRESSION] =
{
    {0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};

static int restart_rx_modem(t38_gateway_state_t *s);
static int process_rx_indicator(t38_core_state_t *t, void *user_data, int indicator);
static void hdlc_underflow_handler(void *user_data);
static void to_t38_buffer_init(t38_gateway_to_t38_state_t *s);
static void t38_hdlc_rx_put_bit(hdlc_rx_state_t *t, int new_bit);
static void non_ecm_put_bit(void *user_data, int bit);
static void non_ecm_remove_fill_and_put_bit(void *user_data, int bit);
static void non_ecm_push_residue(t38_gateway_state_t *s);
static void tone_detected(void *user_data, int on, int level, int delay);

static void set_rx_handler(t38_gateway_state_t *s, span_rx_handler_t *handler, void *user_data)
{
    if (s->audio.modems.rx_handler != span_dummy_rx)
        s->audio.modems.rx_handler = handler;
    s->audio.base_rx_handler = handler;
    s->audio.modems.rx_user_data = user_data;
}
/*- End of function --------------------------------------------------------*/

static void set_rx_active(t38_gateway_state_t *s, int active)
{
    s->audio.modems.rx_handler = (active)  ?  s->audio.base_rx_handler  :  span_dummy_rx;
}
/*- End of function --------------------------------------------------------*/

static int v17_v21_rx(void *user_data, const int16_t amp[], int len)
{
    t38_gateway_state_t *t;
    fax_modems_state_t *s;

    t = (t38_gateway_state_t *) user_data;
    s = &t->audio.modems;
    v17_rx(&s->v17_rx, amp, len);
    fsk_rx(&s->v21_rx, amp, len);
    if (s->rx_signal_present)
    {
        if (s->rx_trained)
        {
            /* The fast modem has trained, so we no longer need to run the slow
               one in parallel. */
            span_log(&t->logging, SPAN_LOG_FLOW, "Switching from V.17 + V.21 to V.17 (%.2fdBm0)\n", v17_rx_signal_power(&s->v17_rx));
            set_rx_handler(t, (span_rx_handler_t *) &v17_rx, &s->v17_rx);
        }
        else
        {
            span_log(&t->logging, SPAN_LOG_FLOW, "Switching from V.17 + V.21 to V.21 (%.2fdBm0)\n", fsk_rx_signal_power(&s->v21_rx));
            set_rx_handler(t, (span_rx_handler_t *) &fsk_rx, &s->v21_rx);
        }
        /*endif*/
    }
    /*endif*/
    return 0;
}
/*- End of function --------------------------------------------------------*/

static int v27ter_v21_rx(void *user_data, const int16_t amp[], int len)
{
    t38_gateway_state_t *t;
    fax_modems_state_t *s;

    t = (t38_gateway_state_t *) user_data;
    s = &t->audio.modems;
    v27ter_rx(&s->v27ter_rx, amp, len);
    fsk_rx(&s->v21_rx, amp, len);
    if (s->rx_signal_present)
    {
        if (s->rx_trained)
        {
            /* The fast modem has trained, so we no longer need to run the slow
               one in parallel. */
            span_log(&t->logging, SPAN_LOG_FLOW, "Switching from V.27ter + V.21 to V.27ter (%.2fdBm0)\n", v27ter_rx_signal_power(&s->v27ter_rx));
            set_rx_handler(t, (span_rx_handler_t *) &v27ter_rx, &s->v27ter_rx);
        }
        else
        {
            span_log(&t->logging, SPAN_LOG_FLOW, "Switching from V.27ter + V.21 to V.21 (%.2fdBm0)\n", fsk_rx_signal_power(&s->v21_rx));
            set_rx_handler(t, (span_rx_handler_t *) &fsk_rx, &s->v21_rx);
        }
        /*endif*/
    }
    /*endif*/
    return 0;
}
/*- End of function --------------------------------------------------------*/

static int v29_v21_rx(void *user_data, const int16_t amp[], int len)
{
    t38_gateway_state_t *t;
    fax_modems_state_t *s;

    t = (t38_gateway_state_t *) user_data;
    s = &t->audio.modems;
    v29_rx(&s->v29_rx, amp, len);
    fsk_rx(&s->v21_rx, amp, len);
    if (s->rx_signal_present)
    {
        if (s->rx_trained)
        {
            /* The fast modem has trained, so we no longer need to run the slow
               one in parallel. */
            span_log(&t->logging, SPAN_LOG_FLOW, "Switching from V.29 + V.21 to V.29 (%.2fdBm0)\n", v29_rx_signal_power(&s->v29_rx));
            set_rx_handler(t, (span_rx_handler_t *) &v29_rx, &s->v29_rx);
        }
        else
        {
            span_log(&t->logging, SPAN_LOG_FLOW, "Switching from V.29 + V.21 to V.21 (%.2fdBm0)\n", fsk_rx_signal_power(&s->v21_rx));
            set_rx_handler(t, (span_rx_handler_t *) &fsk_rx, &s->v21_rx);
        }
        /*endif*/
    }
    /*endif*/
    return 0;
}
/*- End of function --------------------------------------------------------*/

static void t38_fax_modems_init(fax_modems_state_t *s, int use_tep, void *user_data)
{
    s->use_tep = use_tep;

    hdlc_rx_init(&s->hdlc_rx, FALSE, TRUE, HDLC_FRAMING_OK_THRESHOLD, NULL, user_data);
    hdlc_tx_init(&s->hdlc_tx, FALSE, 2, TRUE, hdlc_underflow_handler, user_data);
    fsk_rx_init(&s->v21_rx, &preset_fsk_specs[FSK_V21CH2], TRUE, (put_bit_func_t) t38_hdlc_rx_put_bit, &s->hdlc_rx);
#if 0
    fsk_rx_signal_cutoff(&s->v21_rx, -45.5);
#endif
    fsk_tx_init(&s->v21_tx, &preset_fsk_specs[FSK_V21CH2], (get_bit_func_t) hdlc_tx_get_bit, &s->hdlc_tx);
    v17_rx_init(&s->v17_rx, 14400, non_ecm_put_bit, user_data);
    v17_tx_init(&s->v17_tx, 14400, s->use_tep, t38_non_ecm_buffer_get_bit, user_data);
    v29_rx_init(&s->v29_rx, 9600, non_ecm_put_bit, user_data);
#if 0
    v29_rx_signal_cutoff(&s->v29_rx, -45.5);
#endif
    v29_tx_init(&s->v29_tx, 9600, s->use_tep, t38_non_ecm_buffer_get_bit, user_data);
    v27ter_rx_init(&s->v27ter_rx, 4800, non_ecm_put_bit, user_data);
    v27ter_tx_init(&s->v27ter_tx, 4800, s->use_tep, t38_non_ecm_buffer_get_bit, user_data);
    silence_gen_init(&s->silence_gen, 0);
    modem_connect_tones_tx_init(&s->connect_tx, MODEM_CONNECT_TONES_FAX_CNG);
    modem_connect_tones_rx_init(&s->connect_rx,
                                MODEM_CONNECT_TONES_FAX_CNG,
                                tone_detected,
                                user_data);
    dc_restore_init(&s->dc_restore);

    s->rx_signal_present = FALSE;
    s->rx_handler = (span_rx_handler_t *) &span_dummy_rx;
    s->rx_user_data = NULL;
    s->tx_handler = (span_tx_handler_t *) &silence_gen;
    s->tx_user_data = &s->silence_gen;
}
/*- End of function --------------------------------------------------------*/

static void tone_detected(void *user_data, int on, int level, int delay)
{
    t38_gateway_state_t *s;

    s = (t38_gateway_state_t *) user_data;
    span_log(&s->logging, SPAN_LOG_FLOW, "FAX tone declared %s (%ddBm0)\n", (on)  ?  "on"  :  "off", level);
}
/*- End of function --------------------------------------------------------*/

static void hdlc_underflow_handler(void *user_data)
{
    t38_gateway_state_t *s;
    t38_gateway_hdlc_state_t *t;
    int old_data_type;

    s = (t38_gateway_state_t *) user_data;
    t = &s->core.hdlc_to_modem;
    span_log(&s->logging, SPAN_LOG_FLOW, "HDLC underflow at %d\n", t->out);
    /* If the current HDLC buffer is not at the HDLC_FLAG_PROCEED_WITH_OUTPUT stage, this
       underflow must be an end of preamble condition. */
    if ((t->flags[t->out] & HDLC_FLAG_PROCEED_WITH_OUTPUT))
    {
        old_data_type = t->contents[t->out];
        t->len[t->out] = 0;
        t->flags[t->out] = 0;
        t->contents[t->out] = 0;
        if (++t->out >= T38_TX_HDLC_BUFS)
            t->out = 0;
        span_log(&s->logging, SPAN_LOG_FLOW, "HDLC next is 0x%X\n", t->contents[t->out]);
        if ((t->contents[t->out] & FLAG_INDICATOR))
        {
            /* The next thing in the queue is an indicator, so we need to stop this modem. */
            span_log(&s->logging, SPAN_LOG_FLOW, "HDLC shutdown\n");
            hdlc_tx_frame(&s->audio.modems.hdlc_tx, NULL, 0);
        }
        else if ((t->contents[t->out] & FLAG_DATA))
        {
            /* Check if we should start sending the next frame */
            if ((t->flags[t->out] & HDLC_FLAG_PROCEED_WITH_OUTPUT))
            {
                /* This frame is ready to go, and uses the same modem we are running now. So, send
                   whatever we have. This might or might not be an entire frame. */
                span_log(&s->logging, SPAN_LOG_FLOW, "HDLC start next frame\n");
                hdlc_tx_frame(&s->audio.modems.hdlc_tx, t->buf[t->out], t->len[t->out]);
                if ((t->flags[t->out] & HDLC_FLAG_CORRUPT_CRC))
                    hdlc_tx_corrupt_frame(&s->audio.modems.hdlc_tx);
                /*endif*/
            }
            /*endif*/
        }
        /*endif*/
    }
    /*endif*/
}
/*- End of function --------------------------------------------------------*/

static int set_next_tx_type(t38_gateway_state_t *s)
{
    get_bit_func_t get_bit_func;
    void *get_bit_user_data;
    int indicator;
    int short_train;
    fax_modems_state_t *t;
    t38_gateway_hdlc_state_t *u;

    t = &s->audio.modems;
    u = &s->core.hdlc_to_modem;
    if (t->next_tx_handler)
    {
        /* There is a handler queued, so that is the next one */
        t->tx_handler = t->next_tx_handler;
        t->tx_user_data = t->next_tx_user_data;
        t->next_tx_handler = NULL;
        if (t->tx_handler == (span_tx_handler_t *) &(silence_gen)
            ||
            t->tx_handler == (span_tx_handler_t *) &(tone_gen))
        {
            set_rx_active(s, TRUE);
        }
        else
        {
            set_rx_active(s, FALSE);
        }
        /*endif*/
        return TRUE;
    }
    /*endif*/
    if (u->in == u->out)
        return FALSE;
    /*endif*/
    if ((u->contents[u->out] & FLAG_INDICATOR) == 0)
        return FALSE;
    /*endif*/
    indicator = (u->contents[u->out] & 0xFF);
    u->len[u->out] = 0;
    u->flags[u->out] = 0;
    u->contents[u->out] = 0;
    if (++u->out >= T38_TX_HDLC_BUFS)
        u->out = 0;
    /*endif*/
    span_log(&s->logging, SPAN_LOG_FLOW, "Changing to %s\n", t38_indicator_to_str(indicator));
    if (s->core.image_data_mode  &&  s->core.ecm_mode)
    {
        span_log(&s->logging, SPAN_LOG_FLOW, "HDLC mode\n");
        hdlc_tx_init(&t->hdlc_tx, FALSE, 2, TRUE, hdlc_underflow_handler, s);
        get_bit_func = (get_bit_func_t) hdlc_tx_get_bit;
        get_bit_user_data = (void *) &t->hdlc_tx;
    }
    else
    {
        span_log(&s->logging, SPAN_LOG_FLOW, "non-ECM mode\n");
        get_bit_func = t38_non_ecm_buffer_get_bit;
        get_bit_user_data = (void *) &s->core.non_ecm_to_modem;
    }
    /*endif*/
    switch (indicator)
    {
    case T38_IND_NO_SIGNAL:
        t->tx_bit_rate = 0;
        /* Impose 75ms minimum on transmitted silence */
        //silence_gen_set(&t->silence_gen, ms_to_samples(75));
        t->tx_handler = (span_tx_handler_t *) &(silence_gen);
        t->tx_user_data = &t->silence_gen;
        t->next_tx_handler = NULL;
        set_rx_active(s, TRUE);
        break;
    case T38_IND_CNG:
        t->tx_bit_rate = 0;
        modem_connect_tones_tx_init(&t->connect_tx, MODEM_CONNECT_TONES_FAX_CNG);
        t->tx_handler = (span_tx_handler_t *) &modem_connect_tones_tx;
        t->tx_user_data = &t->connect_tx;
        silence_gen_set(&t->silence_gen, 0);
        t->next_tx_handler = (span_tx_handler_t *) &(silence_gen);
        t->next_tx_user_data = &t->silence_gen;
        set_rx_active(s, TRUE);
        break;
    case T38_IND_CED:
        t->tx_bit_rate = 0;
        modem_connect_tones_tx_init(&t->connect_tx, MODEM_CONNECT_TONES_FAX_CED);
        t->tx_handler = (span_tx_handler_t *) &modem_connect_tones_tx;
        t->tx_user_data = &t->connect_tx;
        t->next_tx_handler = NULL;
        set_rx_active(s, TRUE);
        break;
    case T38_IND_V21_PREAMBLE:
        t->tx_bit_rate = 300;
        hdlc_tx_init(&t->hdlc_tx, FALSE, 2, TRUE, hdlc_underflow_handler, s);
        hdlc_tx_flags(&t->hdlc_tx, 32);
        silence_gen_alter(&t->silence_gen, ms_to_samples(75));
        u->len[u->in] = 0;
        fsk_tx_init(&t->v21_tx, &preset_fsk_specs[FSK_V21CH2], (get_bit_func_t) hdlc_tx_get_bit, &t->hdlc_tx);
        t->tx_handler = (span_tx_handler_t *) &(silence_gen);
        t->tx_user_data = &t->silence_gen;
        t->next_tx_handler = (span_tx_handler_t *) &(fsk_tx);
        t->next_tx_user_data = &t->v21_tx;
        set_rx_active(s, TRUE);
        break;
    case T38_IND_V27TER_2400_TRAINING:
    case T38_IND_V27TER_4800_TRAINING:
        switch (indicator)
        {
        case T38_IND_V27TER_2400_TRAINING:
            t->tx_bit_rate = 2400;
            break;
        case T38_IND_V27TER_4800_TRAINING:
            t->tx_bit_rate = 2400;
            break;
        }
        silence_gen_alter(&t->silence_gen, ms_to_samples(75));
        v27ter_tx_restart(&t->v27ter_tx, t->tx_bit_rate, t->use_tep);
        v27ter_tx_set_get_bit(&t->v27ter_tx, get_bit_func, get_bit_user_data);
        t->tx_handler = (span_tx_handler_t *) &(silence_gen);
        t->tx_user_data = &t->silence_gen;
        t->next_tx_handler = (span_tx_handler_t *) &(v27ter_tx);
        t->next_tx_user_data = &t->v27ter_tx;
        set_rx_active(s, TRUE);
        break;
    case T38_IND_V29_7200_TRAINING:
    case T38_IND_V29_9600_TRAINING:
        switch (indicator)
        {
        case T38_IND_V29_7200_TRAINING:
            t->tx_bit_rate = 7200;
            break;
        case T38_IND_V29_9600_TRAINING:
            t->tx_bit_rate = 9600;
            break;
        }
        silence_gen_alter(&t->silence_gen, ms_to_samples(75));
        v29_tx_restart(&t->v29_tx, t->tx_bit_rate, t->use_tep);
        v29_tx_set_get_bit(&t->v29_tx, get_bit_func, get_bit_user_data);
        t->tx_handler = (span_tx_handler_t *) &(silence_gen);
        t->tx_user_data = &t->silence_gen;
        t->next_tx_handler = (span_tx_handler_t *) &(v29_tx);
        t->next_tx_user_data = &t->v29_tx;
        set_rx_active(s, TRUE);
        break;
    case T38_IND_V17_7200_SHORT_TRAINING:
    case T38_IND_V17_7200_LONG_TRAINING:
    case T38_IND_V17_9600_SHORT_TRAINING:
    case T38_IND_V17_9600_LONG_TRAINING:
    case T38_IND_V17_12000_SHORT_TRAINING:
    case T38_IND_V17_12000_LONG_TRAINING:
    case T38_IND_V17_14400_SHORT_TRAINING:
    case T38_IND_V17_14400_LONG_TRAINING:
        short_train = FALSE;
        switch (indicator)
        {
        case T38_IND_V17_7200_SHORT_TRAINING:
            short_train = TRUE;
            t->tx_bit_rate = 7200;
            break;
        case T38_IND_V17_7200_LONG_TRAINING:
            t->tx_bit_rate = 7200;
            break;
        case T38_IND_V17_9600_SHORT_TRAINING:
            short_train = TRUE;
            t->tx_bit_rate = 9600;
            break;
        case T38_IND_V17_9600_LONG_TRAINING:
            t->tx_bit_rate = 9600;
            break;
        case T38_IND_V17_12000_SHORT_TRAINING:
            short_train = TRUE;
            t->tx_bit_rate = 12000;
            break;
        case T38_IND_V17_12000_LONG_TRAINING:
            t->tx_bit_rate = 12000;
            break;
        case T38_IND_V17_14400_SHORT_TRAINING:
            short_train = TRUE;
            t->tx_bit_rate = 14400;
            break;
        case T38_IND_V17_14400_LONG_TRAINING:
            t->tx_bit_rate = 14400;
            break;
        }
        silence_gen_alter(&t->silence_gen, ms_to_samples(75));
        v17_tx_restart(&t->v17_tx, t->tx_bit_rate, t->use_tep, short_train);
        v17_tx_set_get_bit(&t->v17_tx, get_bit_func, get_bit_user_data);
        t->tx_handler = (span_tx_handler_t *) &(silence_gen);
        t->tx_user_data = &t->silence_gen;
        t->next_tx_handler = (span_tx_handler_t *) &(v17_tx);
        t->next_tx_user_data = &t->v17_tx;
        set_rx_active(s, TRUE);
        break;
    case T38_IND_V8_ANSAM:
        t->tx_bit_rate = 0;
        break;
    case T38_IND_V8_SIGNAL:
        t->tx_bit_rate = 0;
        break;
    case T38_IND_V34_CNTL_CHANNEL_1200:
        t->tx_bit_rate = 0;
        break;
    case T38_IND_V34_PRI_CHANNEL:
        t->tx_bit_rate = 0;
        break;
    case T38_IND_V34_CC_RETRAIN:
        t->tx_bit_rate = 0;
        break;
    case T38_IND_V33_12000_TRAINING:
        t->tx_bit_rate = 12000;
        break;
    case T38_IND_V33_14400_TRAINING:
        t->tx_bit_rate = 14400;
        break;
    default:
        break;
    }
    /*endswitch*/
    /* For any fast modem, set 200ms of preamble flags */
    if (t->tx_bit_rate > 300)
        hdlc_tx_flags(&t->hdlc_tx, t->tx_bit_rate/(8*5));
    /*endif*/
    t38_non_ecm_buffer_report_status(&s->core.non_ecm_to_modem, &s->logging);
    t38_non_ecm_buffer_init(&s->core.non_ecm_to_modem, s->core.image_data_mode, s->core.min_row_bits);
    s->t38x.in_progress_rx_indicator = indicator;
    return TRUE;
}
/*- End of function --------------------------------------------------------*/

static void pump_out_final_hdlc(t38_gateway_state_t *s, int good_fcs)
{
    if (!good_fcs)
        s->core.hdlc_to_modem.flags[s->core.hdlc_to_modem.in] |= HDLC_FLAG_CORRUPT_CRC;
    /*endif*/
    if (s->core.hdlc_to_modem.in == s->core.hdlc_to_modem.out)
    {
        /* This is the frame in progress at the output. */
        if ((s->core.hdlc_to_modem.flags[s->core.hdlc_to_modem.out] & HDLC_FLAG_PROCEED_WITH_OUTPUT) == 0)
        {
            /* Output of this frame has not yet begun. Throw it all out now. */
            hdlc_tx_frame(&s->audio.modems.hdlc_tx, s->core.hdlc_to_modem.buf[s->core.hdlc_to_modem.out], s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.out]);
        }
        /*endif*/
        if ((s->core.hdlc_to_modem.flags[s->core.hdlc_to_modem.out] & HDLC_FLAG_CORRUPT_CRC))
            hdlc_tx_corrupt_frame(&s->audio.modems.hdlc_tx);
        /*endif*/
    }
    /*endif*/
    s->core.hdlc_to_modem.flags[s->core.hdlc_to_modem.in] |= (HDLC_FLAG_PROCEED_WITH_OUTPUT | HDLC_FLAG_FINISHED);
    if (++s->core.hdlc_to_modem.in >= T38_TX_HDLC_BUFS)
        s->core.hdlc_to_modem.in = 0;
    /*endif*/
}
/*- End of function --------------------------------------------------------*/

static void edit_control_messages(t38_gateway_state_t *s, int from_modem, uint8_t *buf, int len)
{
    /* Frames need to be fed to this routine byte by byte as they arrive. It basically just
       edits the last byte received, based on the frame up to that point. */
    if (s->t38x.corrupt_current_frame[from_modem])
    {
        /* We simply need to overwrite a section of the message, so it is not recognisable at
           the receiver. This is used for the NSF, NSC, and NSS messages. Several strategies are
           possible for the replacement data. If you have a manufacturer code of your own, the
           sane thing is to overwrite the original data with that. */
        if (len <= s->t38x.suppress_nsx_len[from_modem])
            buf[len - 1] = nsx_overwrite[from_modem][len - 4];
        /*endif*/
        return;
    }
    /*endif*/
    /* Edit the message, if we need to control the communication between the end points. */
    switch (len)
    {
    case 3:
        switch (buf[2])
        {
        case T30_NSF:
        case T30_NSC:
        case T30_NSS:
            if (s->t38x.suppress_nsx_len[from_modem])
            {
                /* Corrupt the message, so it will be ignored by the far end. If it were
                   processed, 2 machines which recognise each other might do special things
                   we cannot handle as a middle man. */
                span_log(&s->logging, SPAN_LOG_FLOW, "Corrupting %s message to prevent recognition\n", t30_frametype(buf[2]));
                s->t38x.corrupt_current_frame[from_modem] = TRUE;
            }
            /*endif*/
            break;
        }
        /*endswitch*/
        break;
    case 5:
        switch (buf[2])
        {
        case T30_DIS:
            /* We may need to adjust the capabilities, so they do not exceed our own */
            span_log(&s->logging, SPAN_LOG_FLOW, "Applying fast modem type constraints.\n");
            switch (buf[4] & (DISBIT6 | DISBIT5 | DISBIT4 | DISBIT3))
            {
            case 0:
            case DISBIT4:
                /* V.27ter only */
                break;
            case DISBIT3:
            case (DISBIT4 | DISBIT3):
                /* V.27ter and V.29 */
                if (!(s->core.supported_modems & T30_SUPPORT_V29))
                    buf[4] &= ~DISBIT3;
                /*endif*/
                break;
            case (DISBIT6 | DISBIT4 | DISBIT3):
                /* V.27ter, V.29 and V.17 */
                if (!(s->core.supported_modems & T30_SUPPORT_V17))
                    buf[4] &= ~DISBIT6;
                /*endif*/
                if (!(s->core.supported_modems & T30_SUPPORT_V29))
                    buf[4] &= ~DISBIT3;
                /*endif*/
                break;
            case (DISBIT5 | DISBIT4):
            case (DISBIT6 | DISBIT4):
            case (DISBIT6 | DISBIT5 | DISBIT4):
            case (DISBIT6 | DISBIT5 | DISBIT4 | DISBIT3):
                /* Reserved */
                buf[4] &= ~(DISBIT6 | DISBIT5);
                buf[4] |= (DISBIT4 | DISBIT3);
                break;
            default:
                /* Not used */
                buf[4] &= ~(DISBIT6 | DISBIT5);
                buf[4] |= (DISBIT4 | DISBIT3);
                break;
            }
            /*endswitch*/
            break;
        }
        /*endswitch*/
        break;
    case 7:
        switch (buf[2])
        {
        case T30_DIS:
            if (!s->core.ecm_allowed)
            {
                /* Do not allow ECM or T.6 coding */
                span_log(&s->logging, SPAN_LOG_FLOW, "Inhibiting ECM\n");
                buf[6] &= ~(DISBIT3 | DISBIT7);
            }
            /*endif*/
            break;
        }
        /*endswitch*/
        break;
    }
    /*endswitch*/
}
/*- End of function --------------------------------------------------------*/

static void monitor_control_messages(t38_gateway_state_t *s, int from_modem, uint8_t *buf, int len)
{
    static const struct
    {
        int bit_rate;
        int modem_type;
        uint8_t dcs_code;
    } modem_codes[] =
    {
        {14400, T38_V17_RX,      DISBIT6},
        {12000, T38_V17_RX,      (DISBIT6 | DISBIT4)},
        { 9600, T38_V17_RX,      (DISBIT6 | DISBIT3)},
        { 9600, T38_V29_RX,      DISBIT3},
        { 7200, T38_V17_RX,      (DISBIT6 | DISBIT4 | DISBIT3)},
        { 7200, T38_V29_RX,      (DISBIT4 | DISBIT3)},
        { 4800, T38_V27TER_RX,   DISBIT4},
        { 2400, T38_V27TER_RX,   0},
        {    0, T38_NONE,        0}
    };
    static const int minimum_scan_line_times[8] =
    {
        20,
        5,
        10,
        0,
        40,
        0,
        0,
        0
    };
    int dcs_code;
    int i;
    int j;

    /* Monitor the control messages, at the point where we have the whole message, so we can
       see what is happening to things like training success/failure. */
    span_log(&s->logging, SPAN_LOG_FLOW, "Monitoring %s\n", t30_frametype(buf[2]));
    if (len < 3)
        return;
    /*endif*/
    s->core.tcf_mode_predictable_modem_start = 0;
    switch (buf[2])
    {
    case T30_CFR:
        /* We are changing from TCF exchange to image exchange */
        /* Successful training means we should change to short training */
        s->core.image_data_mode = TRUE;
        s->core.short_train = TRUE;
        span_log(&s->logging, SPAN_LOG_FLOW, "CFR - short train = %d, ECM = %d\n", s->core.short_train, s->core.ecm_mode);
        if (!from_modem)
            restart_rx_modem(s);
        /*endif*/
        break;
    case T30_RTN:
    case T30_RTP:
        /* We are going back to the exchange of fresh TCF */
        s->core.image_data_mode = FALSE;
        s->core.short_train = FALSE;
        break;
    case T30_CTR:
        /* T.30 says the first image data after this does full training, yet does not
           return to TCF. This seems to be the sole case of long training for image
           data. */
        s->core.short_train = FALSE;
        break;
    case T30_DTC:
    case T30_DCS:
    case T30_DCS | 1:
        /* We need to check which modem type is about to be used, so we can start the
           correct modem. */
        if (len >= 5)
        {
            /* The table is short, and not searched often, so a brain-dead linear scan seems OK */
            dcs_code = buf[4] & (DISBIT6 | DISBIT5 | DISBIT4 | DISBIT3);
            for (i = 0;  modem_codes[i].bit_rate;  i++)
            {
                if (modem_codes[i].dcs_code == dcs_code)
                    break;
                /*endif*/
            }
            /*endfor*/
            s->core.fast_bit_rate = modem_codes[i].bit_rate;
            s->core.fast_modem = modem_codes[i].modem_type;
        }
        /*endif*/
        if (len >= 6)
        {
            j = (buf[5] & (DISBIT7 | DISBIT6 | DISBIT5)) >> 4;
            span_log(&s->logging, SPAN_LOG_FLOW, "Min bits test = 0x%X\n", buf[5]);
            s->core.min_row_bits = (s->core.fast_bit_rate*minimum_scan_line_times[j])/1000;
            span_log(&s->logging, SPAN_LOG_FLOW, "Min bits per row = %d\n", j);
        }
        /*endif*/
        s->core.ecm_mode = (len >= 7)  &&  (buf[6] & DISBIT3);
        s->core.image_data_mode = FALSE;
        s->core.short_train = FALSE;
        if (from_modem)
            s->core.tcf_mode_predictable_modem_start = 2;
        /*endif*/
        break;
    case T30_PPS:
    case T30_PPS | 1:
        switch (buf[3] & 0xFE)
        {
        case T30_EOP:
        case T30_EOM:
        case T30_EOS:
        case T30_MPS:
        case T30_PRI_EOP:
        case T30_PRI_EOM:
        case T30_PRI_MPS:
            s->core.count_page_on_mcf = TRUE;
            break;
        }
        /*endswitch*/
        break;
    case T30_EOP:
    case T30_EOM:
    case T30_EOS:
    case T30_MPS:
    case T30_PRI_EOP:
    case T30_PRI_EOM:
    case T30_PRI_MPS:
    case T30_EOP | 1:
    case T30_EOM | 1:
    case T30_EOS | 1:
    case T30_MPS | 1:
    case T30_PRI_EOP | 1:
    case T30_PRI_EOM | 1:
    case T30_PRI_MPS | 1:
        s->core.count_page_on_mcf = TRUE;
        break;
    case T30_MCF:
    case T30_MCF | 1:
        if (s->core.count_page_on_mcf)
        {
            s->core.pages_confirmed++;
            span_log(&s->logging, SPAN_LOG_FLOW, "Pages confirmed = %d\n", s->core.pages_confirmed);
            s->core.count_page_on_mcf = FALSE;
        }
        /*endif*/
        break;
    default:
        break;
    }
    /*endswitch*/
}
/*- End of function --------------------------------------------------------*/

static void queue_missing_indicator(t38_gateway_state_t *s, int data_type)
{
    t38_core_state_t *t;
    
    t = &s->t38x.t38;
    /* Missing packets might have lost us the indicator that should have put us in
       the required mode of operation. It might be a bit late to fill in such a gap
       now, but we should try. We may also want to force indicators into the queue,
       such as when the data says 'end of signal'. */
    switch (data_type)
    {
    case T38_DATA_NONE:
        if (t->current_rx_indicator != T38_IND_NO_SIGNAL)
            process_rx_indicator(t, (void *) s, T38_IND_NO_SIGNAL);
        break;
    case T38_DATA_V21:
        if (t->current_rx_indicator != T38_IND_V21_PREAMBLE)
            process_rx_indicator(t, (void *) s, T38_IND_V21_PREAMBLE);
        break;
    case T38_DATA_V27TER_2400:
        if (t->current_rx_indicator != T38_IND_V27TER_2400_TRAINING)
            process_rx_indicator(t, (void *) s, T38_IND_V27TER_2400_TRAINING);
        break;
    case T38_DATA_V27TER_4800:
        if (t->current_rx_indicator != T38_IND_V27TER_4800_TRAINING)
            process_rx_indicator(t, (void *) s, T38_IND_V27TER_4800_TRAINING);
        break;
    case T38_DATA_V29_7200:
        if (t->current_rx_indicator != T38_IND_V29_7200_TRAINING)
            process_rx_indicator(t, (void *) s, T38_IND_V29_7200_TRAINING);
        break;
    case T38_DATA_V29_9600:
        if (t->current_rx_indicator != T38_IND_V29_9600_TRAINING)
            process_rx_indicator(t, (void *) s, T38_IND_V29_9600_TRAINING);
        break;
    case T38_DATA_V17_7200:
        if (t->current_rx_indicator != T38_IND_V17_7200_SHORT_TRAINING  &&  t->current_rx_indicator != T38_IND_V17_7200_LONG_TRAINING)
            process_rx_indicator(t, (void *) s, T38_IND_V17_7200_LONG_TRAINING);
        break;
    case T38_DATA_V17_9600:
        if (t->current_rx_indicator != T38_IND_V17_9600_SHORT_TRAINING  &&  t->current_rx_indicator != T38_IND_V17_9600_LONG_TRAINING)
            process_rx_indicator(t, (void *) s, T38_IND_V17_9600_LONG_TRAINING);
        break;
    case T38_DATA_V17_12000:
        if (t->current_rx_indicator != T38_IND_V17_12000_SHORT_TRAINING  &&  t->current_rx_indicator != T38_IND_V17_12000_LONG_TRAINING)
            process_rx_indicator(t, (void *) s, T38_IND_V17_12000_LONG_TRAINING);
        break;
    case T38_DATA_V17_14400:
        if (t->current_rx_indicator != T38_IND_V17_14400_SHORT_TRAINING  &&  t->current_rx_indicator != T38_IND_V17_14400_LONG_TRAINING)
            process_rx_indicator(t, (void *) s, T38_IND_V17_14400_LONG_TRAINING);
        break;
    case T38_DATA_V8:
        break;
    case T38_DATA_V34_PRI_RATE:
        break;
    case T38_DATA_V34_CC_1200:
        break;
    case T38_DATA_V34_PRI_CH:
        break;
    case T38_DATA_V33_12000:
        break;
    case T38_DATA_V33_14400:
        break;
    }
}
/*- End of function --------------------------------------------------------*/

static int process_rx_missing(t38_core_state_t *t, void *user_data, int rx_seq_no, int expected_seq_no)
{
    t38_gateway_state_t *s;
    
    s = (t38_gateway_state_t *) user_data;
    s->core.hdlc_to_modem.flags[s->core.hdlc_to_modem.in] |= HDLC_FLAG_MISSING_DATA;
    return 0;
}
/*- End of function --------------------------------------------------------*/

static int process_rx_indicator(t38_core_state_t *t, void *user_data, int indicator)
{
    t38_gateway_state_t *s;
    
    s = (t38_gateway_state_t *) user_data;

    if (t->current_rx_indicator == indicator)
    {
        /* This is probably due to the far end repeating itself. Ignore it. Its harmless */
        return 0;
    }
    /*endif*/
    if (s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in])
    {
        if (++s->core.hdlc_to_modem.in >= T38_TX_HDLC_BUFS)
            s->core.hdlc_to_modem.in = 0;
        /*endif*/
    }
    /*endif*/
    s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] = (indicator | FLAG_INDICATOR);
    if (++s->core.hdlc_to_modem.in >= T38_TX_HDLC_BUFS)
        s->core.hdlc_to_modem.in = 0;
    /*endif*/
    span_log(&s->logging,
             SPAN_LOG_FLOW,
             "Queued change - (%d) %s -> %s\n",
             silence_gen_remainder(&(s->audio.modems.silence_gen)),
             t38_indicator_to_str(t->current_rx_indicator),
             t38_indicator_to_str(indicator));
    s->t38x.current_rx_field_class = T38_FIELD_CLASS_NONE;
    /* We need to set this here, since we might have been called as a fake
       indication when the real one was missing */
    t->current_rx_indicator = indicator;
    return 0;
}
/*- End of function --------------------------------------------------------*/

static int process_rx_data(t38_core_state_t *t, void *user_data, int data_type, int field_type, const uint8_t *buf, int len)
{
    int i;
    int previous;
    t38_gateway_state_t *s;
    t38_gateway_t38_state_t *xx;

    s = (t38_gateway_state_t *) user_data;
    xx = &s->t38x;
    switch (field_type)
    {
    case T38_FIELD_HDLC_DATA:
        xx->current_rx_field_class = T38_FIELD_CLASS_HDLC;
        if (s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] != (data_type | FLAG_DATA))
            queue_missing_indicator(s, data_type);
        /*endif*/
        previous = s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.in];
        /* Check if this data would overflow the buffer. */
        if (s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.in] + len > T38_MAX_HDLC_LEN)
            break;
        /*endif*/
        s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] = (data_type | FLAG_DATA);
        bit_reverse(&s->core.hdlc_to_modem.buf[s->core.hdlc_to_modem.in][s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.in]], buf, len);
        /* We need to send out the control messages as they are arriving. They are
           too slow to capture a whole frame, and then pass it on.
           For the faster frames, take in the whole frame before sending it out. Also, there
           is no need to monitor, or modify, the contents of the faster frames. */
        if (data_type == T38_DATA_V21)
        {
            for (i = 1;  i <= len;  i++)
                edit_control_messages(s, 0, s->core.hdlc_to_modem.buf[s->core.hdlc_to_modem.in], s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.in] + i);
            /*endfor*/
            /* Don't start pumping data into the actual output stream until there is
               enough backlog to create some elasticity for jitter tolerance. */
            if (s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.in] + len >= HDLC_START_BUFFER_LEVEL)
            {
                if (s->core.hdlc_to_modem.in == s->core.hdlc_to_modem.out)
                {
                    if ((s->core.hdlc_to_modem.flags[s->core.hdlc_to_modem.in] & HDLC_FLAG_PROCEED_WITH_OUTPUT) == 0)
                        previous = 0;
                    /*endif*/
                    hdlc_tx_frame(&s->audio.modems.hdlc_tx, s->core.hdlc_to_modem.buf[s->core.hdlc_to_modem.out] + previous, s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.out] - previous + len);
                }
                /*endif*/
                s->core.hdlc_to_modem.flags[s->core.hdlc_to_modem.in] |= HDLC_FLAG_PROCEED_WITH_OUTPUT;
            }
            /*endif*/
        }
        /*endif*/
        s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.in] += len;
        break;
    case T38_FIELD_HDLC_FCS_OK:
        xx->current_rx_field_class = T38_FIELD_CLASS_HDLC;
        if (len > 0)
        {
            span_log(&s->logging, SPAN_LOG_WARNING, "There is data in a T38_FIELD_HDLC_FCS_OK!\n");
            /* The sender has incorrectly included data in this message. It is unclear what we should do
               with it, to maximise tolerance of buggy implementations. */
        }
        /*endif*/
        /* Some T.38 implementations send multiple T38_FIELD_HDLC_FCS_OK messages, in IFP packets with
           incrementing sequence numbers, which are actually repeats. They get through to this point because
           of the incrementing sequence numbers. We need to filter them here in a context sensitive manner. */
        if (t->current_rx_data_type != data_type
            ||
            t->current_rx_field_type != field_type)
        {
            span_log(&s->logging, SPAN_LOG_FLOW, "HDLC frame type %s - CRC good\n", t30_frametype(s->core.hdlc_to_modem.buf[s->core.hdlc_to_modem.in][2]));
            if (s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] != (data_type | FLAG_DATA))
                queue_missing_indicator(s, data_type);
            /*endif*/
            s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] = (data_type | FLAG_DATA);
            if (data_type == T38_DATA_V21)
            {
                if ((s->core.hdlc_to_modem.flags[s->core.hdlc_to_modem.in] & HDLC_FLAG_MISSING_DATA) == 0)
                {
                    monitor_control_messages(s, FALSE, s->core.hdlc_to_modem.buf[s->core.hdlc_to_modem.in], s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.in]);
                    if (s->core.real_time_frame_handler)
                        s->core.real_time_frame_handler(s, s->core.real_time_frame_user_data, FALSE, s->core.hdlc_to_modem.buf[s->core.hdlc_to_modem.in], s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.in]);
                    /*endif*/
                }
                /*endif*/
            }
            else
            {
                /* Make sure we go back to short training if CTC/CTR has kicked us into
                   long training. Theer has to be more than one value HDLC frame in a
                   chunk of image data, so just setting short training mode heer should
                   be enough. */
                s->core.short_train = TRUE;
            }
            /*endif*/
            pump_out_final_hdlc(s, (s->core.hdlc_to_modem.flags[s->core.hdlc_to_modem.in] & HDLC_FLAG_MISSING_DATA) == 0);
        }
        /*endif*/
        s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.in] = 0;
        s->core.hdlc_to_modem.flags[s->core.hdlc_to_modem.in] = 0;
        xx->corrupt_current_frame[0] = FALSE;
        break;
    case T38_FIELD_HDLC_FCS_BAD:
        xx->current_rx_field_class = T38_FIELD_CLASS_HDLC;
        if (len > 0)
        {
            span_log(&s->logging, SPAN_LOG_WARNING, "There is data in a T38_FIELD_HDLC_FCS_BAD!\n");
            /* The sender has incorrectly included data in this message. We can safely ignore it, as the
               bad FCS means we will throw away the whole message, anyway. */
        }
        /*endif*/
        /* Some T.38 implementations send multiple T38_FIELD_HDLC_FCS_BAD messages, in IFP packets with
           incrementing sequence numbers, which are actually repeats. They get through to this point because
           of the incrementing sequence numbers. We need to filter them here in a context sensitive manner. */
        if (t->current_rx_data_type != data_type  ||  t->current_rx_field_type != field_type)
        {
            span_log(&s->logging, SPAN_LOG_FLOW, "HDLC frame type %s - CRC bad\n", t30_frametype(s->core.hdlc_to_modem.buf[s->core.hdlc_to_modem.in][2]));
            if (s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] != (data_type | FLAG_DATA))
                queue_missing_indicator(s, data_type);
            /*endif*/
            if (s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.in] > 0)
            {
                s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] = (data_type | FLAG_DATA);
                pump_out_final_hdlc(s, FALSE);
            }
            /*endif*/
        }
        /*endif*/
        s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.in] = 0;
        s->core.hdlc_to_modem.flags[s->core.hdlc_to_modem.in] = 0;
        xx->corrupt_current_frame[0] = FALSE;
        break;
    case T38_FIELD_HDLC_FCS_OK_SIG_END:
        xx->current_rx_field_class = T38_FIELD_CLASS_HDLC;
        if (len > 0)
        {
            span_log(&s->logging, SPAN_LOG_WARNING, "There is data in a T38_FIELD_HDLC_FCS_OK_SIG_END!\n");
            /* The sender has incorrectly included data in this message. It is unclear what we should do
               with it, to maximise tolerance of buggy implementations. */
        }
        /*endif*/
        /* Some T.38 implementations send multiple T38_FIELD_HDLC_FCS_OK_SIG_END messages, in IFP packets with
           incrementing sequence numbers, which are actually repeats. They get through to this point because
           of the incrementing sequence numbers. We need to filter them here in a context sensitive manner. */
        if (t->current_rx_data_type != data_type  ||  t->current_rx_field_type != field_type)
        {
            span_log(&s->logging, SPAN_LOG_FLOW, "HDLC frame type %s - CRC OK, sig end\n", t30_frametype(s->core.hdlc_to_modem.buf[s->core.hdlc_to_modem.in][2]));
            if (s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] != (data_type | FLAG_DATA))
                queue_missing_indicator(s, data_type);
            /*endif*/
            s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] = (data_type | FLAG_DATA);
            if (data_type == T38_DATA_V21  &&  (s->core.hdlc_to_modem.flags[s->core.hdlc_to_modem.in] & HDLC_FLAG_MISSING_DATA) == 0)
            {
                monitor_control_messages(s, FALSE, s->core.hdlc_to_modem.buf[s->core.hdlc_to_modem.in], s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.in]);
                if (s->core.real_time_frame_handler)
                    s->core.real_time_frame_handler(s, s->core.real_time_frame_user_data, FALSE, s->core.hdlc_to_modem.buf[s->core.hdlc_to_modem.in], s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.in]);
                /*endif*/
            }
            /*endif*/
            pump_out_final_hdlc(s, (s->core.hdlc_to_modem.flags[s->core.hdlc_to_modem.in] & HDLC_FLAG_MISSING_DATA) == 0);
            s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.in] = 0;
            s->core.hdlc_to_modem.flags[s->core.hdlc_to_modem.in] = 0;
            s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] = 0;
            queue_missing_indicator(s, T38_DATA_NONE);
            xx->current_rx_field_class = T38_FIELD_CLASS_NONE;
        }
        /*endif*/
        xx->corrupt_current_frame[0] = FALSE;
        break;
    case T38_FIELD_HDLC_FCS_BAD_SIG_END:
        xx->current_rx_field_class = T38_FIELD_CLASS_HDLC;
        if (len > 0)
        {
            span_log(&s->logging, SPAN_LOG_WARNING, "There is data in a T38_FIELD_HDLC_FCS_BAD_SIG_END!\n");
            /* The sender has incorrectly included data in this message. We can safely ignore it, as the
               bad FCS means we will throw away the whole message, anyway. */
        }
        /*endif*/
        /* Some T.38 implementations send multiple T38_FIELD_HDLC_FCS_BAD_SIG_END messages, in IFP packets with
           incrementing sequence numbers, which are actually repeats. They get through to this point because
           of the incrementing sequence numbers. We need to filter them here in a context sensitive manner. */
        if (t->current_rx_data_type != data_type  ||  t->current_rx_field_type != field_type)
        {
            span_log(&s->logging, SPAN_LOG_FLOW, "HDLC frame type %s - CRC bad, sig end\n", t30_frametype(s->core.hdlc_to_modem.buf[s->core.hdlc_to_modem.in][2]));
            if (s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] != (data_type | FLAG_DATA))
                queue_missing_indicator(s, data_type);
            /*endif*/
            if (s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.in] > 0)
            {
                s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] = (data_type | FLAG_DATA);
                pump_out_final_hdlc(s, FALSE);
            }
            /*endif*/
            s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.in] = 0;
            s->core.hdlc_to_modem.flags[s->core.hdlc_to_modem.in] = 0;
            s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] = 0;
            queue_missing_indicator(s, T38_DATA_NONE);
            xx->current_rx_field_class = T38_FIELD_CLASS_NONE;
        }
        /*endif*/
        xx->corrupt_current_frame[0] = FALSE;
        break;
    case T38_FIELD_HDLC_SIG_END:
        if (len > 0)
        {
            span_log(&s->logging, SPAN_LOG_WARNING, "There is data in a T38_FIELD_HDLC_SIG_END!\n");
            /* The sender has incorrectly included data in this message, but there seems nothing meaningful
               it could be. There could not be an FCS good/bad report beyond this. */
        }
        /*endif*/
        /* Some T.38 implementations send multiple T38_FIELD_HDLC_SIG_END messages, in IFP packets with
           incrementing sequence numbers, which are actually repeats. They get through to this point because
           of the incrementing sequence numbers. We need to filter them here in a context sensitive manner. */
        if (t->current_rx_data_type != data_type  ||  t->current_rx_field_type != field_type)
        {
            if (s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] != (data_type | FLAG_DATA))
                queue_missing_indicator(s, data_type);
            /* WORKAROUND: At least some Mediatrix boxes have a bug, where they can send this message at the
                           end of non-ECM data. We need to tolerate this. */
            if (xx->current_rx_field_class == T38_FIELD_CLASS_NON_ECM)
            {
                span_log(&s->logging, SPAN_LOG_WARNING, "T38_FIELD_HDLC_SIG_END received at the end of non-ECM data!\n");
                /* Don't flow control the data any more. Just pump out the remainder as fast as we can. */
                t38_non_ecm_buffer_push(&s->core.non_ecm_to_modem);
            }
            else
            {
                /* This message is expected under 2 circumstances. One is as an alternative to T38_FIELD_HDLC_FCS_OK_SIG_END - 
                   i.e. they send T38_FIELD_HDLC_FCS_OK, and then T38_FIELD_HDLC_SIG_END when the carrier actually drops.
                   The other is because the HDLC signal drops unexpectedly - i.e. not just after a final frame. */
                s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.in] = 0;
                s->core.hdlc_to_modem.flags[s->core.hdlc_to_modem.in] = 0;
                s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] = 0;
            }
            /*endif*/
            queue_missing_indicator(s, T38_DATA_NONE);
            xx->current_rx_field_class = T38_FIELD_CLASS_NONE;
        }
        /*endif*/
        xx->corrupt_current_frame[0] = FALSE;
        break;
    case T38_FIELD_T4_NON_ECM_DATA:
        xx->current_rx_field_class = T38_FIELD_CLASS_NON_ECM;
        if (s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] != (data_type | FLAG_DATA))
            queue_missing_indicator(s, data_type);
        t38_non_ecm_buffer_inject(&s->core.non_ecm_to_modem, buf, len);
        xx->corrupt_current_frame[0] = FALSE;
        break;
    case T38_FIELD_T4_NON_ECM_SIG_END:
        /* Some T.38 implementations send multiple T38_FIELD_T4_NON_ECM_SIG_END messages, in IFP packets with
           incrementing sequence numbers, which are actually repeats. They get through to this point because
           of the incrementing sequence numbers. We need to filter them here in a context sensitive manner. */
        if (t->current_rx_data_type != data_type  ||  t->current_rx_field_type != field_type)
        {
            /* WORKAROUND: At least some Mediatrix boxes have a bug, where they can send HDLC signal end where
                           they should send non-ECM signal end. It is possible they also do the opposite.
                           We need to tolerate this. */
            if (xx->current_rx_field_class == T38_FIELD_CLASS_NON_ECM)
            {
                if (len > 0)
                {
                    if (s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] != (data_type | FLAG_DATA))
                        queue_missing_indicator(s, data_type);
                    /*endif*/
                    t38_non_ecm_buffer_inject(&s->core.non_ecm_to_modem, buf, len);
                }
                /*endif*/
                if (s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] != (data_type | FLAG_DATA))
                    queue_missing_indicator(s, data_type);
                /*endif*/
                /* Don't flow control the data any more. Just pump out the remainder as fast as we can. */
                t38_non_ecm_buffer_push(&s->core.non_ecm_to_modem);
            }
            else
            {
                span_log(&s->logging, SPAN_LOG_WARNING, "T38_FIELD_NON_ECM_SIG_END received at the end of HDLC data!\n");
                if (s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] != (data_type | FLAG_DATA))
                    queue_missing_indicator(s, data_type);
                /*endif*/
                s->core.hdlc_to_modem.len[s->core.hdlc_to_modem.in] = 0;
                s->core.hdlc_to_modem.flags[s->core.hdlc_to_modem.in] = 0;
                s->core.hdlc_to_modem.contents[s->core.hdlc_to_modem.in] = 0;
            }
            /*endif*/
            queue_missing_indicator(s, T38_DATA_NONE);
            xx->current_rx_field_class = T38_FIELD_CLASS_NONE;
        }
        /*endif*/
        xx->corrupt_current_frame[0] = FALSE;
        break;
    case T38_FIELD_CM_MESSAGE:
        if (len >= 1)
            span_log(&s->logging, SPAN_LOG_FLOW, "CM profile %d - %s\n", buf[0] - '0', t38_cm_profile_to_str(buf[0]));
        else
            span_log(&s->logging, SPAN_LOG_FLOW, "Bad length for CM message - %d\n", len);
        /*endif*/
        break;
    case T38_FIELD_JM_MESSAGE:
        if (len >= 2)
            span_log(&s->logging, SPAN_LOG_FLOW, "JM - %s\n", t38_jm_to_str(buf, len));
        else
            span_log(&s->logging, SPAN_LOG_FLOW, "Bad length for JM message - %d\n", len);
        /*endif*/
        break;
    case T38_FIELD_CI_MESSAGE:
        if (len >= 1)
            span_log(&s->logging, SPAN_LOG_FLOW, "CI 0x%X\n", buf[0]);
        else
            span_log(&s->logging, SPAN_LOG_FLOW, "Bad length for CI message - %d\n", len);
        /*endif*/
        break;
    case T38_FIELD_V34RATE:
        if (len >= 3)
        {
            xx->t38.v34_rate = t38_v34rate_to_bps(buf, len);
            span_log(&s->logging, SPAN_LOG_FLOW, "V.34 rate %d bps\n", xx->t38.v34_rate);
        }
        else
        {
            span_log(&s->logging, SPAN_LOG_FLOW, "Bad length for V34rate message - %d\n", len);
        }
        /*endif*/
        break;
    default:
        break;
    }
    /*endswitch*/

#if 0
    if (span_log_test(&s->logging, SPAN_LOG_FLOW))
    {
        int i;

        if (len > 0)
        {
            span_log(&s->logging, SPAN_LOG_FLOW, "Data: ");
            for (i = 0;  i < len;  i++)
                span_log(&s->logging, SPAN_LOG_FLOW | SPAN_LOG_SUPPRESS_LABELLING, " %02X", buf[i]);
            /*endfor*/
        }
        /*endif*/
    }
    /*endif*/
    span_log(&s->logging, SPAN_LOG_FLOW | SPAN_LOG_SUPPRESS_LABELLING, "\n");
#endif
    return 0;
}
/*- End of function --------------------------------------------------------*/

static void set_octets_per_data_packet(t38_gateway_state_t *s, int bit_rate)
{
    int octets;
    
    octets = MS_PER_TX_CHUNK*bit_rate/(8*1000);
    if (octets < 1)
        octets = 1;
    /*endif*/
    s->core.to_t38.octets_per_data_packet = octets;
}
/*- End of function --------------------------------------------------------*/

static int set_slow_packetisation(t38_gateway_state_t *s)
{
    set_octets_per_data_packet(s, 300);
    s->t38x.current_tx_data_type = T38_DATA_V21;
    return T38_IND_V21_PREAMBLE;
}
/*- End of function --------------------------------------------------------*/

static int set_fast_packetisation(t38_gateway_state_t *s)
{
    int ind;

    ind = T38_IND_NO_SIGNAL;
    switch (s->core.fast_rx_active)
    {
    case T38_V17_RX:
        set_octets_per_data_packet(s, s->core.fast_bit_rate);
        switch (s->core.fast_bit_rate)
        {
        case 7200:
            ind = (s->core.short_train)  ?  T38_IND_V17_7200_SHORT_TRAINING  :  T38_IND_V17_7200_LONG_TRAINING;
            s->t38x.current_tx_data_type = T38_DATA_V17_7200;
            break;
        case 9600:
            ind = (s->core.short_train)  ?  T38_IND_V17_9600_SHORT_TRAINING  :  T38_IND_V17_9600_LONG_TRAINING;
            s->t38x.current_tx_data_type = T38_DATA_V17_9600;
            break;
        case 12000:
            ind = (s->core.short_train)  ?  T38_IND_V17_12000_SHORT_TRAINING  :  T38_IND_V17_12000_LONG_TRAINING;
            s->t38x.current_tx_data_type = T38_DATA_V17_12000;
            break;
        default:
        case 14400:
            ind = (s->core.short_train)  ?  T38_IND_V17_14400_SHORT_TRAINING  :  T38_IND_V17_14400_LONG_TRAINING;
            s->t38x.current_tx_data_type = T38_DATA_V17_14400;
            break;
        }
        break;
    case T38_V27TER_RX:
        set_octets_per_data_packet(s, s->core.fast_bit_rate);
        switch (s->core.fast_bit_rate)
        {
        case 2400:
            ind = T38_IND_V27TER_2400_TRAINING;
            s->t38x.current_tx_data_type = T38_DATA_V27TER_2400;
            break;
        default:
        case 4800:
            ind = T38_IND_V27TER_4800_TRAINING;
            s->t38x.current_tx_data_type = T38_DATA_V27TER_4800;
            break;
        }
        break;
    case T38_V29_RX:
        set_octets_per_data_packet(s, s->core.fast_bit_rate);
        switch (s->core.fast_bit_rate)
        {
        case 7200:
            ind = T38_IND_V29_7200_TRAINING;
            s->t38x.current_tx_data_type = T38_DATA_V29_7200;
            break;
        default:
        case 9600:
            ind = T38_IND_V29_9600_TRAINING;
            s->t38x.current_tx_data_type = T38_DATA_V29_9600;
            break;
        }
        break;
    }
    return ind;
}
/*- End of function --------------------------------------------------------*/

static void announce_training(t38_gateway_state_t *s)
{
    t38_core_send_indicator(&s->t38x.t38, set_fast_packetisation(s), s->t38x.t38.indicator_tx_count);
}
/*- End of function --------------------------------------------------------*/

static void non_ecm_rx_status(void *user_data, int status)
{
    t38_gateway_state_t *s;

    s = (t38_gateway_state_t *) user_data;
    switch (status)
    {
    case PUTBIT_TRAINING_IN_PROGRESS:
        span_log(&s->logging, SPAN_LOG_FLOW, "Non-ECM carrier training in progress\n");
        if (s->core.tcf_mode_predictable_modem_start)
            s->core.tcf_mode_predictable_modem_start = 0;
        else
            announce_training(s);
        break;
    case PUTBIT_TRAINING_FAILED:
        span_log(&s->logging, SPAN_LOG_FLOW, "Non-ECM carrier training failed\n");
        break;
    case PUTBIT_TRAINING_SUCCEEDED:
        /* The modem is now trained */
        span_log(&s->logging, SPAN_LOG_FLOW, "Non-ECM carrier trained\n");
        s->audio.modems.rx_signal_present = TRUE;
        s->audio.modems.rx_trained = TRUE;
        to_t38_buffer_init(&s->core.to_t38);
        break;
    case PUTBIT_CARRIER_UP:
        span_log(&s->logging, SPAN_LOG_FLOW, "Non-ECM carrier up\n");
        break;
    case PUTBIT_CARRIER_DOWN:
        span_log(&s->logging, SPAN_LOG_FLOW, "Non-ECM carrier down\n");
        s->core.tcf_mode_predictable_modem_start = 0;
        switch (s->t38x.current_tx_data_type)
        {
        case T38_DATA_V17_7200:
        case T38_DATA_V17_9600:
        case T38_DATA_V17_12000:
        case T38_DATA_V17_14400:
        case T38_DATA_V27TER_2400:
        case T38_DATA_V27TER_4800:
        case T38_DATA_V29_7200:
        case T38_DATA_V29_9600:
            non_ecm_push_residue(s);
            t38_core_send_indicator(&s->t38x.t38, T38_IND_NO_SIGNAL, s->t38x.t38.indicator_tx_count);
            restart_rx_modem(s);
            break;
        }
        break;
    default:
        span_log(&s->logging, SPAN_LOG_WARNING, "Unexpected non-ECM special bit - %d!\n", status);
        break;
    }
}
/*- End of function --------------------------------------------------------*/

static void to_t38_buffer_init(t38_gateway_to_t38_state_t *s)
{
    s->data_ptr = 0;
    s->bit_stream = 0xFFFF;
    s->bit_no = 0;

    s->in_bits = 0;
    s->out_octets = 0;
}
/*- End of function --------------------------------------------------------*/

static void non_ecm_push_residue(t38_gateway_state_t *t)
{
    t38_gateway_to_t38_state_t *s;

    s = &t->core.to_t38;
    if (s->bit_no)
    {
        /* There is a fractional octet in progress. We might as well send every last bit we can. */
        s->data[s->data_ptr++] = s->bit_stream << (8 - s->bit_no);
    }
    t38_core_send_data(&t->t38x.t38, t->t38x.current_tx_data_type, T38_FIELD_T4_NON_ECM_SIG_END, s->data, s->data_ptr, t->t38x.t38.data_end_tx_count);
    s->out_octets += s->data_ptr;
    s->in_bits += s->bits_absorbed;
    s->data_ptr = 0;
}
/*- End of function --------------------------------------------------------*/

static void non_ecm_push(t38_gateway_state_t *t)
{
    t38_gateway_to_t38_state_t *s;

    s = &t->core.to_t38;
    if (s->data_ptr)
    {
        t38_core_send_data(&t->t38x.t38, t->t38x.current_tx_data_type, T38_FIELD_T4_NON_ECM_DATA, s->data, s->data_ptr, t->t38x.t38.data_tx_count);
        s->out_octets += s->data_ptr;
        s->in_bits += s->bits_absorbed;
        s->bits_absorbed = 0;
        s->data_ptr = 0;
    }
    /*endif*/
}
/*- End of function --------------------------------------------------------*/

static void non_ecm_put_bit(void *user_data, int bit)
{
    t38_gateway_state_t *t;
    t38_gateway_to_t38_state_t *s;
    
    if (bit < 0)
    {
        non_ecm_rx_status(user_data, bit);
        return;
    }
    t = (t38_gateway_state_t *) user_data;
    s = &t->core.to_t38;

    s->in_bits++;
    bit &= 1;
    s->bit_stream = (s->bit_stream << 1) | bit;
    if (++s->bit_no >= 8)
    {
        s->data[s->data_ptr++] = (uint8_t) s->bit_stream & 0xFF;
        if (s->data_ptr >= s->octets_per_data_packet)
            non_ecm_push(t);
        s->bit_no = 0;
    }
}
/*- End of function --------------------------------------------------------*/

static void non_ecm_remove_fill_and_put_bit(void *user_data, int bit)
{
    t38_gateway_state_t *t;
    t38_gateway_to_t38_state_t *s;
    
    if (bit < 0)
    {
        non_ecm_rx_status(user_data, bit);
        return;
    }
    t = (t38_gateway_state_t *) user_data;
    s = &t->core.to_t38;

    s->bits_absorbed++;
    bit &= 1;
    /* Drop any extra zero bits when we already have enough for an EOL symbol. */
    /* The snag here is that if we just look for 11 bits, a line ending with
       a code that has trailing zero bits will cause problems. The longest run of
       trailing zeros for any code is 3, so we need to look for at least 14 zeros
       if we don't want to actually analyse the compressed data in depth. This means
       we do not strip every fill bit, but we strip most of them. */
    if ((s->bit_stream & 0x3FFF) == 0  &&  bit == 0)
    {
        if (s->bits_absorbed > 2*8*s->octets_per_data_packet)
        {
            /* We need to pump out what we have, even though we have not accumulated a full
               buffer of data. If we don't, we stand to delay rows excessively, so the far
               end gateway (assuming the far end is a gateway) cannot play them out. */
            non_ecm_push(t);
        }
        return;
    }
    s->bit_stream = (s->bit_stream << 1) | bit;
    if (++s->bit_no >= 8)
    {
        s->data[s->data_ptr++] = (uint8_t) s->bit_stream & 0xFF;
        if (s->data_ptr >= s->octets_per_data_packet)
            non_ecm_push(t);
        s->bit_no = 0;
    }
}
/*- End of function --------------------------------------------------------*/

static void hdlc_rx_status(hdlc_rx_state_t *t, int status)
{
    t38_gateway_state_t *s;

    s = (t38_gateway_state_t *) t->user_data;
    switch (status)
    {
    case PUTBIT_TRAINING_IN_PROGRESS:
        span_log(&s->logging, SPAN_LOG_FLOW, "HDLC carrier training in progress\n");
        announce_training(s);
        break;
    case PUTBIT_TRAINING_FAILED:
        span_log(&s->logging, SPAN_LOG_FLOW, "HDLC carrier training failed\n");
        break;
    case PUTBIT_TRAINING_SUCCEEDED:
        /* The modem is now trained. */
        span_log(&s->logging, SPAN_LOG_FLOW, "HDLC carrier trained\n");
        s->audio.modems.rx_signal_present = TRUE;
        s->audio.modems.rx_trained = TRUE;
        /* Behave like HDLC preamble has been announced. */
        t->framing_ok_announced = TRUE;
        to_t38_buffer_init(&s->core.to_t38);
        break;
    case PUTBIT_CARRIER_UP:
        span_log(&s->logging, SPAN_LOG_FLOW, "HDLC carrier up\n");
        /* Reset the HDLC receiver. */
        t->raw_bit_stream = 0;
        t->len = 0;
        t->num_bits = 0;
        t->flags_seen = 0;
        t->framing_ok_announced = FALSE;
        to_t38_buffer_init(&s->core.to_t38);
        break;
    case PUTBIT_CARRIER_DOWN:
        span_log(&s->logging, SPAN_LOG_FLOW, "HDLC carrier down\n");
        if (t->framing_ok_announced)
        {
            t38_core_send_data(&s->t38x.t38, s->t38x.current_tx_data_type, T38_FIELD_HDLC_SIG_END, NULL, 0, s->t38x.t38.data_end_tx_count);
            t38_core_send_indicator(&s->t38x.t38, T38_IND_NO_SIGNAL, s->t38x.t38.indicator_tx_count);
            t->framing_ok_announced = FALSE;
        }
        restart_rx_modem(s);
        if (s->core.tcf_mode_predictable_modem_start == 2)
        {
            /* If we are doing TCF, we need to announce the fast carrier training very
               quickly, to ensure it starts 75+-20ms after the HDLC carrier ends. Waiting until
               it trains will be too late. We need to announce the fast modem a fixed time after
               the end of the V.21 carrier, in anticipation of its arrival. */
            s->core.samples_to_timeout = ms_to_samples(75);
            s->core.tcf_mode_predictable_modem_start = 1;
        }
        break;
    default:
        span_log(&s->logging, SPAN_LOG_WARNING, "Unexpected HDLC special bit - %d!\n", status);
        break;
    }
}
/*- End of function --------------------------------------------------------*/

static void rx_flag_or_abort(hdlc_rx_state_t *t)
{
    t38_gateway_state_t *s;
    t38_gateway_to_t38_state_t *u;
    
    s = (t38_gateway_state_t *) t->user_data;
    u = &s->core.to_t38;
    if ((t->raw_bit_stream & 0x80))
    {
        /* Hit HDLC abort */
        t->rx_aborts++;
        if (t->flags_seen < t->framing_ok_threshold)
            t->flags_seen = 0;
        else
            t->flags_seen = t->framing_ok_threshold - 1;
        /*endif*/
    }
    else
    {
        /* Hit HDLC flag */
        if (t->flags_seen >= t->framing_ok_threshold)
        {
            if (t->len)
            {
                /* This is not back-to-back flags */
                if (t->len >= 2)
                {
                    if (u->data_ptr)
                    {
                        bit_reverse(u->data, t->buffer + t->len - 2 - u->data_ptr, u->data_ptr);
                        t38_core_send_data(&s->t38x.t38, s->t38x.current_tx_data_type, T38_FIELD_HDLC_DATA, u->data, u->data_ptr, s->t38x.t38.data_tx_count);
                    }
                    /*endif*/
                    if (t->num_bits != 7)
                    {
                        t->rx_crc_errors++;
                        span_log(&s->logging, SPAN_LOG_FLOW, "HDLC frame type %s, misaligned terminating flag at %d\n", t30_frametype(t->buffer[2]), t->len);
                        /* It seems some boxes may not like us sending a _SIG_END here, and then another
                           when the carrier actually drops. Lets just send T38_FIELD_HDLC_FCS_OK here. */
                        if (t->len > 2)
                            t38_core_send_data(&s->t38x.t38, s->t38x.current_tx_data_type, T38_FIELD_HDLC_FCS_BAD, NULL, 0, s->t38x.t38.data_tx_count);
                        /*endif*/
                    }
                    else if ((u->crc & 0xFFFF) != 0xF0B8)
                    {
                        t->rx_crc_errors++;
                        span_log(&s->logging, SPAN_LOG_FLOW, "HDLC frame type %s, bad CRC at %d\n", t30_frametype(t->buffer[2]), t->len);
                        /* It seems some boxes may not like us sending a _SIG_END here, and then another
                           when the carrier actually drops. Lets just send T38_FIELD_HDLC_FCS_OK here. */
                        if (t->len > 2)
                            t38_core_send_data(&s->t38x.t38, s->t38x.current_tx_data_type, T38_FIELD_HDLC_FCS_BAD, NULL, 0, s->t38x.t38.data_tx_count);
                        /*endif*/
                    }
                    else
                    {
                        t->rx_frames++;
                        t->rx_bytes += t->len - 2;
                        span_log(&s->logging, SPAN_LOG_FLOW, "HDLC frame type %s, CRC OK\n", t30_frametype(t->buffer[2]));
                        if (s->t38x.current_tx_data_type == T38_DATA_V21)
                        {
                            monitor_control_messages(s, TRUE, t->buffer, t->len - 2);
                            if (s->core.real_time_frame_handler)
                                s->core.real_time_frame_handler(s, s->core.real_time_frame_user_data, TRUE, t->buffer, t->len - 2);
                            /*endif*/
                        }
                        else
                        {
                            /* Make sure we go back to short training if CTC/CTR has kicked us into
                               long training. Any successful HDLC frame received at a rate other than
                               V.21 is an adequate indication we should change. */
                            s->core.short_train = TRUE;
                        }
                        /*endif*/
                        /* It seems some boxes may not like us sending a _SIG_END here, and then another
                           when the carrier actually drops. Lets just send T38_FIELD_HDLC_FCS_OK here. */
                        t38_core_send_data(&s->t38x.t38, s->t38x.current_tx_data_type, T38_FIELD_HDLC_FCS_OK, NULL, 0, s->t38x.t38.data_tx_count);
                    }
                    /*endif*/
                }
                else
                {
                    /* Frame too short */
                    t->rx_length_errors++;
                }
                /*endif*/
            }
            /*endif*/
        }
        else
        {
            /* Check the flags are back-to-back when testing for valid preamble. This
               greatly reduces the chances of false preamble detection, and anything
               which doesn't send them back-to-back is badly broken. */
            if (t->num_bits != 7)
                t->flags_seen = 0;
            /*endif*/
            if (++t->flags_seen >= t->framing_ok_threshold  &&  !t->framing_ok_announced)
            {
                if (s->t38x.current_tx_data_type == T38_DATA_V21)
                {
                    t38_core_send_indicator(&s->t38x.t38, set_slow_packetisation(s), s->t38x.t38.indicator_tx_count);
                    s->audio.modems.rx_signal_present = TRUE;
                }
                /*endif*/
                if (s->t38x.in_progress_rx_indicator == T38_IND_CNG)
                    set_next_tx_type(s);
                /*endif*/
                t->framing_ok_announced = TRUE;
            }
            /*endif*/
        }
        /*endif*/
    }
    /*endif*/
    t->len = 0;
    t->num_bits = 0;
    u->crc = 0xFFFF;
    u->data_ptr = 0;
    s->t38x.corrupt_current_frame[1] = FALSE;
}
/*- End of function --------------------------------------------------------*/

static void t38_hdlc_rx_put_bit(hdlc_rx_state_t *t, int new_bit)
{
    t38_gateway_state_t *s;
    t38_gateway_to_t38_state_t *u;

    if (new_bit < 0)
    {
        hdlc_rx_status(t, new_bit);
        return;
    }
    /*endif*/
    t->raw_bit_stream = (t->raw_bit_stream << 1) | (new_bit & 1);
    if ((t->raw_bit_stream & 0x3F) == 0x3E)
    {
        /* Its time to either skip a bit, for stuffing, or process a flag or abort */
        if ((t->raw_bit_stream & 0x40))
            rx_flag_or_abort(t);
        return;
    }
    /*endif*/
    t->num_bits++;
    if (!t->framing_ok_announced)
        return;
    /*endif*/
    t->byte_in_progress = (t->byte_in_progress >> 1) | ((t->raw_bit_stream & 0x01) << 7);
    if (t->num_bits != 8)
        return;
    /*endif*/
    t->num_bits = 0;
    if (t->len >= (int) sizeof(t->buffer))
    {
        /* This is too long. Abandon the frame, and wait for the next flag octet. */
        t->rx_length_errors++;
        t->flags_seen = t->framing_ok_threshold - 1;
        t->len = 0;
        return;
    }
    /*endif*/
    s = (t38_gateway_state_t *) t->user_data;
    u = &s->core.to_t38;
    t->buffer[t->len] = (uint8_t) t->byte_in_progress;
    /* Calculate the CRC progressively, before we start altering the frame */
    u->crc = crc_itu16_calc(&t->buffer[t->len], 1, u->crc);
    /* Make the transmission lag by two octets, so we do not send the CRC, and
       do not report the CRC result too late. */
    if (++t->len <= 2)
        return;
    /*endif*/
    if (s->t38x.current_tx_data_type == T38_DATA_V21)
    {
        /* The V.21 control messages need to be monitored, and possibly corrupted, to manage the
           man-in-the-middle role of T.38 */
        edit_control_messages(s, 1, t->buffer, t->len);
    }
    if (++u->data_ptr >= u->octets_per_data_packet)
    {
        bit_reverse(u->data, t->buffer + t->len - 2 - u->data_ptr, u->data_ptr);
        t38_core_send_data(&s->t38x.t38, s->t38x.current_tx_data_type, T38_FIELD_HDLC_DATA, u->data, u->data_ptr, s->t38x.t38.data_tx_count);
        /* Since we delay transmission by 2 octets, we should now have sent the last of the data octets when
           we have just received the last of the CRC octets. */
        u->data_ptr = 0;
    }
    /*endif*/
}
/*- End of function --------------------------------------------------------*/

static int restart_rx_modem(t38_gateway_state_t *s)
{
    put_bit_func_t put_bit_func;
    void *put_bit_user_data;

    if (s->core.to_t38.in_bits  ||  s->core.to_t38.out_octets)
    {
        span_log(&s->logging,
                 SPAN_LOG_FLOW,
                 "%d incoming audio bits.  %d outgoing T.38 octets\n",
                 s->core.to_t38.in_bits,
                 s->core.to_t38.out_octets);
        s->core.to_t38.in_bits = 0;
        s->core.to_t38.out_octets = 0;
    }
    span_log(&s->logging, SPAN_LOG_FLOW, "Restart rx modem - modem = %d, short train = %d, ECM = %d\n", s->core.fast_modem, s->core.short_train, s->core.ecm_mode);

    hdlc_rx_init(&(s->audio.modems.hdlc_rx), FALSE, TRUE, HDLC_FRAMING_OK_THRESHOLD, NULL, s);
    s->audio.modems.rx_signal_present = FALSE;
    s->audio.modems.rx_trained = FALSE;
    /* Default to the transmit data being V.21, unless a faster modem pops up trained. */
    s->t38x.current_tx_data_type = T38_DATA_V21;
    fsk_rx_init(&(s->audio.modems.v21_rx), &preset_fsk_specs[FSK_V21CH2], TRUE, (put_bit_func_t) t38_hdlc_rx_put_bit, &(s->audio.modems.hdlc_rx));
#if 0
    fsk_rx_signal_cutoff(&(s->audio.modems.v21_rx), -45.5);
#endif
    if (s->core.image_data_mode  &&  s->core.ecm_mode)
    {
        put_bit_func = (put_bit_func_t) t38_hdlc_rx_put_bit;
        put_bit_user_data = (void *) &(s->audio.modems.hdlc_rx);
    }
    else
    {
        if (s->core.image_data_mode  &&  s->core.to_t38.fill_bit_removal)
            put_bit_func = non_ecm_remove_fill_and_put_bit;
        else
            put_bit_func = non_ecm_put_bit;
        put_bit_user_data = (void *) s;
    }
    /*endif*/
    to_t38_buffer_init(&s->core.to_t38);
    s->core.to_t38.octets_per_data_packet = 1;
    switch (s->core.fast_modem)
    {
    case T38_V17_RX:
        v17_rx_restart(&s->audio.modems.v17_rx, s->core.fast_bit_rate, s->core.short_train);
        v17_rx_set_put_bit(&s->audio.modems.v17_rx, put_bit_func, put_bit_user_data);
        set_rx_handler(s, (span_rx_handler_t *) &v17_v21_rx, s);
        s->core.fast_rx_active = T38_V17_RX;
        break;
    case T38_V27TER_RX:
        v27ter_rx_restart(&s->audio.modems.v27ter_rx, s->core.fast_bit_rate, FALSE);
        v27ter_rx_set_put_bit(&s->audio.modems.v27ter_rx, put_bit_func, put_bit_user_data);
        set_rx_handler(s, (span_rx_handler_t *) &v27ter_v21_rx, s);
        s->core.fast_rx_active = T38_V27TER_RX;
        break;
    case T38_V29_RX:
        v29_rx_restart(&s->audio.modems.v29_rx, s->core.fast_bit_rate, FALSE);
        v29_rx_set_put_bit(&s->audio.modems.v29_rx, put_bit_func, put_bit_user_data);
        set_rx_handler(s, (span_rx_handler_t *) &v29_v21_rx, s);
        s->core.fast_rx_active = T38_V29_RX;
        break;
    default:
        set_rx_handler(s, (span_rx_handler_t *) &fsk_rx, &(s->audio.modems.v21_rx));
        s->core.fast_rx_active = T38_NONE;
        break;
    }
    /*endswitch*/
    return 0;
}
/*- End of function --------------------------------------------------------*/

int t38_gateway_rx(t38_gateway_state_t *s, int16_t amp[], int len)
{
    int i;

#if defined(LOG_FAX_AUDIO)
    if (s->audio.modems.audio_rx_log >= 0)
        write(s->audio.modems.audio_rx_log, amp, len*sizeof(int16_t));
#endif
    if (s->core.samples_to_timeout > 0)
    {
        if ((s->core.samples_to_timeout -= len) <= 0)
        {
            if (s->core.tcf_mode_predictable_modem_start == 1)
                announce_training(s);
            /*endif*/
        }
        /*endif*/
    }
    /*endif*/
    for (i = 0;  i < len;  i++)
        amp[i] = dc_restore(&(s->audio.modems.dc_restore), amp[i]);
    /*endfor*/
    s->audio.modems.rx_handler(s->audio.modems.rx_user_data, amp, len);
    return  0;
}
/*- End of function --------------------------------------------------------*/

int t38_gateway_tx(t38_gateway_state_t *s, int16_t amp[], int max_len)
{
    int len;
#if defined(LOG_FAX_AUDIO)
    int required_len;
    
    required_len = max_len;
#endif
    if ((len = s->audio.modems.tx_handler(s->audio.modems.tx_user_data, amp, max_len)) < max_len)
    {
        if (set_next_tx_type(s))
        {
            /* Give the new handler a chance to file the remaining buffer space */
            len += s->audio.modems.tx_handler(s->audio.modems.tx_user_data, amp + len, max_len - len);
            if (len < max_len)
            {
                silence_gen_set(&(s->audio.modems.silence_gen), 0);
                set_next_tx_type(s);
            }
            /*endif*/
        }
        /*endif*/
    }
    /*endif*/
    if (s->audio.modems.transmit_on_idle)
    {
        /* Pad to the requested length with silence */
        memset(amp + len, 0, (max_len - len)*sizeof(int16_t));
        len = max_len;        
    }
    /*endif*/
#if defined(LOG_FAX_AUDIO)
    if (s->audio.modems.audio_tx_log >= 0)
    {
        if (len < required_len)
            memset(amp + len, 0, (required_len - len)*sizeof(int16_t));
        /*endif*/
        write(s->audio.modems.audio_tx_log, amp, required_len*sizeof(int16_t));
    }
    /*endif*/
#endif
    return len;
}
/*- End of function --------------------------------------------------------*/

void t38_gateway_get_transfer_statistics(t38_gateway_state_t *s, t38_stats_t *t)
{
    memset(t, 0, sizeof(*t));
    t->bit_rate = s->core.fast_bit_rate;
    t->error_correcting_mode = s->core.ecm_mode;
    t->pages_transferred = s->core.pages_confirmed;
}
/*- End of function --------------------------------------------------------*/

void t38_gateway_set_ecm_capability(t38_gateway_state_t *s, int ecm_allowed)
{
    s->core.ecm_allowed = ecm_allowed;
}
/*- End of function --------------------------------------------------------*/

void t38_gateway_set_transmit_on_idle(t38_gateway_state_t *s, int transmit_on_idle)
{
    s->audio.modems.transmit_on_idle = transmit_on_idle;
}
/*- End of function --------------------------------------------------------*/

void t38_gateway_set_supported_modems(t38_gateway_state_t *s, int supported_modems)
{
    s->core.supported_modems = supported_modems;
    if ((s->core.supported_modems & T30_SUPPORT_V17))
        s->t38x.t38.fastest_image_data_rate = 14400;
    else if ((s->core.supported_modems & T30_SUPPORT_V29))
        s->t38x.t38.fastest_image_data_rate = 9600;
    else
        s->t38x.t38.fastest_image_data_rate = 4800;
    /*endif*/
}
/*- End of function --------------------------------------------------------*/

void t38_gateway_set_nsx_suppression(t38_gateway_state_t *s,
                                     const uint8_t *from_t38,
                                     int from_t38_len,
                                     const uint8_t *from_modem,
                                     int from_modem_len)
{
    s->t38x.suppress_nsx_len[0] = (from_t38_len < 0  ||  from_t38_len < MAX_NSX_SUPPRESSION)  ?  (from_t38_len + 3)  :  0;
    s->t38x.suppress_nsx_len[1] = (from_modem_len < 0  ||  from_modem_len < MAX_NSX_SUPPRESSION)  ?  (from_modem_len + 3)  :  0;
}
/*- End of function --------------------------------------------------------*/

void t38_gateway_set_tep_mode(t38_gateway_state_t *s, int use_tep)
{
    s->audio.modems.use_tep = use_tep;
}
/*- End of function --------------------------------------------------------*/

void t38_gateway_set_fill_bit_removal(t38_gateway_state_t *s, int remove)
{
    s->core.to_t38.fill_bit_removal = remove;
}
/*- End of function --------------------------------------------------------*/

void t38_gateway_set_real_time_frame_handler(t38_gateway_state_t *s,
                                             t38_gateway_real_time_frame_handler_t *handler,
                                             void *user_data)
{
    s->core.real_time_frame_handler = handler;
    s->core.real_time_frame_user_data = user_data;
}
/*- End of function --------------------------------------------------------*/

static int t38_gateway_audio_init(t38_gateway_state_t *s)
{
    t38_fax_modems_init(&s->audio.modems, FALSE, s);
    return 0;
}
/*- End of function --------------------------------------------------------*/

static int t38_gateway_t38_init(t38_gateway_state_t *t,
                                t38_tx_packet_handler_t *tx_packet_handler,
                                void *tx_packet_user_data)
{
    t38_gateway_t38_state_t *s;

    s = &t->t38x;
    t38_core_init(&s->t38,
                  process_rx_indicator,
                  process_rx_data,
                  process_rx_missing,
                  (void *) t,
                  tx_packet_handler,
                  tx_packet_user_data);
    s->t38.indicator_tx_count = INDICATOR_TX_COUNT;
    s->t38.data_tx_count = DATA_TX_COUNT;
    s->t38.data_end_tx_count = DATA_END_TX_COUNT;
    return 0;
}
/*- End of function --------------------------------------------------------*/

t38_gateway_state_t *t38_gateway_init(t38_gateway_state_t *s,
                                      t38_tx_packet_handler_t *tx_packet_handler,
                                      void *tx_packet_user_data)
{
    if (tx_packet_handler == NULL)
        return NULL;
    /*endif*/
    if (s == NULL)
    {
        if ((s = (t38_gateway_state_t *) malloc(sizeof(*s))) == NULL)
            return NULL;
        /*endif*/
    }
    /*endif*/
    memset(s, 0, sizeof(*s));
    span_log_init(&s->logging, SPAN_LOG_NONE, NULL);
    span_log_set_protocol(&s->logging, "T.38G");

    t38_gateway_audio_init(s);
    t38_gateway_t38_init(s, tx_packet_handler, tx_packet_user_data);
    
    set_rx_active(s, TRUE);
    t38_gateway_set_supported_modems(s, T30_SUPPORT_V27TER | T30_SUPPORT_V29);
    t38_gateway_set_nsx_suppression(s, (const uint8_t *) "\x00\x00\x00", 3, (const uint8_t *) "\x00\x00\x00", 3);

    s->core.to_t38.octets_per_data_packet = 1;
    s->core.ecm_allowed = FALSE;
    t38_non_ecm_buffer_init(&s->core.non_ecm_to_modem, FALSE, 0);
    restart_rx_modem(s);
#if defined(LOG_FAX_AUDIO)
    {
        char buf[100 + 1];
        struct tm *tm;
        time_t now;

        time(&now);
        tm = localtime(&now);
        sprintf(buf,
                "/tmp/t38-rx-audio-%p-%02d%02d%02d%02d%02d%02d",
                s,
                tm->tm_year%100,
                tm->tm_mon + 1,
                tm->tm_mday,
                tm->tm_hour,
                tm->tm_min,
                tm->tm_sec);
        s->audio.modems.audio_rx_log = open(buf, O_CREAT | O_TRUNC | O_WRONLY, 0666);
        sprintf(buf,
                "/tmp/t38-tx-audio-%p-%02d%02d%02d%02d%02d%02d",
                s,
                tm->tm_year%100,
                tm->tm_mon + 1,
                tm->tm_mday,
                tm->tm_hour,
                tm->tm_min,
                tm->tm_sec);
        s->audio.modems.audio_tx_log = open(buf, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    }
#endif
    return s;
}
/*- End of function --------------------------------------------------------*/

int t38_gateway_free(t38_gateway_state_t *s)
{
    free(s);
    return 0;
}
/*- End of function --------------------------------------------------------*/
/*- End of file ------------------------------------------------------------*/