/*___________________________________________________
 |  _____                       _____ _ _       _    |
 | |  __ \                     |  __ (_) |     | |   |
 | | |__) |__ _ __   __ _ _   _| |__) || | ___ | |_  |
 | |  ___/ _ \ '_ \ / _` | | | |  ___/ | |/ _ \| __| |
 | | |  |  __/ | | | (_| | |_| | |   | | | (_) | |_  |
 | |_|   \___|_| |_|\__, |\__,_|_|   |_|_|\___/ \__| |
 |                   __/ |                           |
 |  GNU/Linux based |___/  Multi-Rotor UAV Autopilot |
 |___________________________________________________|
  
 Stabilizing PIID Controller Implementation

 Copyright (C) 2014 Tobias Simon, Ilmenau University of Technology
 Copyright (C) 2013 Alexander Barth, Ilmenau University of Technology
 Copyright (C) 2013 Benjamin Jahn, Ilmenau University of Technology

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details. */


#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <util.h>
#include <threadsafe_types.h>
#include <opcd_interface.h>

#include "piid.h"
#include "../util/pid.h"
#include "../../filters/filter.h"
#include "../../util/logger/logger.h"
#include "../../util/math/adams5.h"


/* configuration parameters: */
static tsfloat_t att_kp;
static tsfloat_t att_ki;
static tsfloat_t att_kii;
static tsfloat_t att_kd;
static tsfloat_t yaw_kp;
static tsfloat_t yaw_ki;
static tsfloat_t filt_fg;
static tsfloat_t filt_d;
static tsfloat_t jxx_jyy;
static tsfloat_t jzz;
static tsfloat_t tmc;


/* ff [x, y, z] 2nd order filters: */
Filter2 filters[3];

/* integrators: */
static adams5_t int_err1;
static adams5_t int_err2;

/* filters: */
static Filter1 filter_lp_err;
static Filter1 filter_hp_err;
static Filter2 filter_ref;

/* working memory: */
static float *xi_err = NULL;
static float *xii_err = NULL;
#define CTRL_NUM_TSTEP 7
static float ringbuf[3 * CTRL_NUM_TSTEP];
static int ringbuf_idx = 0;
pid_controller_t yaw_ctrl;


/* integrator enable flag: */
static int int_enable = 0;


void piid_init(float Ts)
{
   ASSERT_ONCE();

   opcd_param_t params[] =
   {
      {"att_kp", &att_kp},
      {"att_ki", &att_ki},
      {"att_kii", &att_kii},
      {"att_kd", &att_kd},
      {"yaw_kp", &yaw_kp},
      {"yaw_ki", &yaw_ki},
      {"filt_fg", &filt_fg},
      {"filt_d", &filt_d},
      {"jxx_jyy", &jxx_jyy},
      {"jzz", &jzz},
      {"tmc", &tmc},
      OPCD_PARAMS_END
   };
   opcd_params_apply("controllers.stabilizing.", params);
   pid_init(&yaw_ctrl, &yaw_kp, &yaw_ki, NULL, NULL);

   LOG(LL_INFO, "ctrl dt = %fs", Ts);
   LOG(LL_INFO, "filter: fg = %f Hz, d = %f",
                tsfloat_get(&filt_fg), tsfloat_get(&filt_d));
   LOG(LL_INFO, "att-ctrl: P = %f, I = %f, II = %f, D = %f",
                tsfloat_get(&att_kp), tsfloat_get(&att_ki), tsfloat_get(&att_kii), tsfloat_get(&att_kd));
   LOG(LL_INFO, "yaw-ctrl: P = %f, I = %f", tsfloat_get(&yaw_kp), tsfloat_get(&yaw_ki));
   LOG(LL_INFO, "feed-forward: jxx_jyy = %f, jzz = %f, tmc = %f",
                tsfloat_get(&jxx_jyy), tsfloat_get(&jzz), tsfloat_get(&tmc));
   
   /* initialize feed-forward system: */
   float T = 1.0f / (2.0f * M_PI * tsfloat_get(&filt_fg));
   float a0 = (4.0f * T * T + 4.0f * tsfloat_get(&filt_d) * T * Ts + Ts * Ts);

   float a[2];
   a[0] = (2.0f * Ts * Ts - 8.0f * T * T) / a0;
   a[1] = (4.0f * T * T   - 4.0f * tsfloat_get(&filt_d) * T * Ts + Ts * Ts) / a0;

   float b[3];
   #define __FF_B_SETUP(j) \
      b[0] =  (2.0f * j * (2.0f * tsfloat_get(&tmc) + Ts)) / a0; \
      b[1] = -(8.0f * j * tsfloat_get(&tmc)) / a0; \
      b[2] =  (2.0f * j * (2.0f * tsfloat_get(&tmc) - Ts)) / a0;

   /* x-axis: */
   __FF_B_SETUP(tsfloat_get(&jxx_jyy));
   filter2_init(&filters[0], a, b, Ts, 1);

   /* y-axis: */
   __FF_B_SETUP(tsfloat_get(&jxx_jyy));
   filter2_init(&filters[1], a, b, Ts, 1);

   /* z-axis: */
   __FF_B_SETUP(tsfloat_get(&jzz));
   filter2_init(&filters[2], a, b, Ts, 1);

   /* initialize multistep integrator: */
   adams5_init(&int_err1, 3);
   adams5_init(&int_err2, 3);

   /* initialize error and reference signal filters: */
   filter1_lp_init(&filter_lp_err, tsfloat_get(&filt_fg), Ts, 3);
   filter1_hp_init(&filter_hp_err, tsfloat_get(&filt_fg), Ts, 3);
   filter2_lp_init(&filter_ref, tsfloat_get(&filt_fg), tsfloat_get(&filt_d), Ts, 3);

   /* allocate some working memory: */
   xi_err = calloc(3, sizeof(float));
   ASSERT_NOT_NULL(xi_err);
   xii_err = calloc(3, sizeof(float));
   ASSERT_NOT_NULL(xii_err);

   /* init ring buffer: */
   memset(ringbuf, 0, sizeof(ringbuf));
   ringbuf_idx = 0;
}


void piid_int_enable(int val)
{
   int_enable = val;
}


void piid_reset(void)
{
   int_enable = 0;
   adams5_reset(&int_err1);
   adams5_reset(&int_err2);
   pid_reset(&yaw_ctrl);
}


void piid_run(float u_ctrl[4], float gyro[3], float rc[3], float dt)
{   
   float T = 1.0f / (2.0f * M_PI * tsfloat_get(&filt_fg));
   float a0 = (4.0f * T * T + 4.0f * tsfloat_get(&filt_d) * T * dt + dt * dt);

   float a[2];
   a[0] = (2.0f * dt * dt - 8.0f * T * T) / a0;
   a[1] = (4.0f * T * T   - 4.0f * tsfloat_get(&filt_d) * T * dt + dt * dt) / a0;

   float b[3];
   #define __FF_B_SETUP(j) \
      b[0] =  (2.0f * j * (2.0f * tsfloat_get(&tmc) + dt)) / a0; \
      b[1] = -(8.0f * j * tsfloat_get(&tmc)) / a0; \
      b[2] =  (2.0f * j * (2.0f * tsfloat_get(&tmc) - dt)) / a0;

   /* x-axis: */
   __FF_B_SETUP(tsfloat_get(&jxx_jyy));
   filter2_update_coeff(&filters[0], a, b);

   /* y-axis: */
   __FF_B_SETUP(tsfloat_get(&jxx_jyy));
   filter2_update_coeff(&filters[1], a, b);

   /* z-axis: */
   __FF_B_SETUP(tsfloat_get(&jzz));
   filter2_update_coeff(&filters[2], a, b);

   /* run feed-forward: */
   FOR_N(i, 3)
   {
      filter2_run(&filters[i], &rc[i], &u_ctrl[i]);
   }

   /* run stabilizing controller: */
   float error[3];
   float derror[3];
   float rc_filt[3];

   /* filter reference signals */
   filter2_lp_update_coeff(&filter_ref, tsfloat_get(&filt_fg), tsfloat_get(&filt_d), dt);
   filter2_run(&filter_ref, rc, rc_filt);

   FOR_N(i, 3)
   {
      error[i] = ringbuf[ringbuf_idx + i] - gyro[i];
      ringbuf[ringbuf_idx + i] = rc[i];
   }

   ringbuf_idx += 3;
   if (ringbuf_idx >= 3 * CTRL_NUM_TSTEP)
   {
      ringbuf_idx = 0;
   }

   /* error high/lowpass filter: */
   filter1_hp_update_coeff(&filter_hp_err, tsfloat_get(&filt_fg), dt);
   filter1_run(&filter_hp_err, error, derror);
   
   filter1_lp_update_coeff(&filter_lp_err, tsfloat_get(&filt_fg), dt);
   filter1_run(&filter_lp_err, error, error);

   /* 1st error integration: */
   adams5_run(&int_err1, xi_err, error, dt, int_enable);

   /* 2nd error integration: */
   adams5_run(&int_err2, xii_err, xi_err, dt, int_enable);

   /* attitude feedback: */
   FOR_N(i, 2)
   {
      u_ctrl[i] +=   tsfloat_get(&att_kp)  * error[i]
                   + tsfloat_get(&att_ki)  * xi_err[i]
                   + tsfloat_get(&att_kii) * xii_err[i] 
                   + tsfloat_get(&att_kd)  * derror[i];
   }

   /* yaw feedback: */
   u_ctrl[PIID_YAW] = pid_control(&yaw_ctrl, rc[PIID_YAW] - gyro[PIID_YAW], 0.0f, dt);
}

