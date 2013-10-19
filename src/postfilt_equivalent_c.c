/**
 *  g729a codec for iOS,...
 *  Copyright (C) 2009-2012 Samuel <samuelv0304@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
/****************************************************************************************
Portions of this file are derived from the following ITU standard:
   ITU-T G.729A Speech Coder    ANSI-C Source Code
   Version 1.1    Last modified: September 1996

   Copyright (c) 1996,
   AT&T, France Telecom, NTT, Universite de Sherbrooke
****************************************************************************************/

/*------------------------------------------------------------------------*
 *  Procedure     Post_Filter:                                            *
 *                ~~~~~~~~~~~                                             *
 *------------------------------------------------------------------------*
 *  The postfiltering process is described as follows:                    *
 *                                                                        *
 *  - inverse filtering of syn[] through A(z/GAMMA2_PST) to get res2[]    *
 *  - use res2[] to compute pitch parameters                              *
 *  - perform pitch postfiltering                                         *
 *  - tilt compensation filtering; 1 - MU*k*z^-1                          *
 *  - synthesis filtering through 1/A(z/GAMMA1_PST)                       *
 *  - adaptive gain control                                               *
 *------------------------------------------------------------------------*/
void g729a_Post_Filter(
    g729a_post_filter_state *state,
    Word16 *syn,       /* in/out: synthesis speech (postfiltered is output)    */
    Word16 *Az_4,       /* input : interpolated LPC parameters in all subframes */
    Word16 *T            /* input : decoded pitch lags in all subframes          */
  )
 {
 /*-------------------------------------------------------------------*
  *           Declaration of parameters                               *
  *-------------------------------------------------------------------*/

 Word16 res2_pst[L_SUBFR];  /* res2[] after pitch postfiltering */
 Word16 syn_pst[L_FRAME];   /* post filtered synthesis speech   */

 Word16 Ap3[MP1], Ap4[MP1];  /* bandwidth expanded LP parameters */

 Word16 *Az;                 /* pointer to Az_4:                 */
                             /*  LPC parameters in each subframe */
 Word16   t0_max, t0_min;    /* closed-loop pitch search range   */
 Word16   i_subfr;           /* index for beginning of subframe  */

 Word16 h[L_H];

 Word16  i, j;
 Word16  temp1, temp2;
 Word32  L_tmp;

   /*-----------------------------------------------------*
    * Post filtering                                      *
    *-----------------------------------------------------*/

    Az = Az_4;

    for (i_subfr = 0; i_subfr < L_FRAME; i_subfr += L_SUBFR)
    {
      /* Find pitch range t0_min - t0_max */

      t0_min = sub(*T++, 3);
      t0_max = add(t0_min, 6);
      if (sub(t0_max, PIT_MAX) > 0) {
        t0_max = PIT_MAX;
        t0_min = sub(t0_max, 6);
      }

      /* Find weighted filter coefficients Ap3[] and ap[4] */

      Weight_Az(Az, GAMMA2_PST, M, Ap3);
      Weight_Az(Az, GAMMA1_PST, M, Ap4);

      /* filtering of synthesis speech by A(z/GAMMA2_PST) to find res2[] */

      Residu(Ap3, &syn[i_subfr], state->res2, L_SUBFR);

      /* scaling of "res2[]" to avoid energy overflow */

      for (j=0; j<L_SUBFR; j++)
      {
        state->scal_res2[j] = shr(state->res2[j], 2);
      }

      /* pitch postfiltering */

      pit_pst_filt(state->res2, state->scal_res2, t0_min, t0_max, L_SUBFR, res2_pst);

      /* tilt compensation filter */

      /* impulse response of A(z/GAMMA2_PST)/A(z/GAMMA1_PST) */

      Copy(Ap3, h, M+1);
      Set_zero(&h[M+1], L_H-M-1);
      Syn_filt(Ap4, h, h, L_H, &h[M+1], 0);

      /* 1st correlation of h[] */

      L_tmp = L_mult(h[0], h[0]);
      for (i=1; i<L_H; i++) L_tmp = L_mac(L_tmp, h[i], h[i]);
      temp1 = extract_h(L_tmp);

      L_tmp = L_mult(h[0], h[1]);
      for (i=1; i<L_H-1; i++) L_tmp = L_mac(L_tmp, h[i], h[i+1]);
      temp2 = extract_h(L_tmp);

      if(temp2 <= 0) {
        temp2 = 0;
      }
      else {
        temp2 = mult(temp2, MU);
        temp2 = div_s(temp2, temp1);
      }

      preemphasis(res2_pst, temp2, L_SUBFR);

      /* filtering through  1/A(z/GAMMA1_PST) */

      Syn_filt(Ap4, res2_pst, &syn_pst[i_subfr], L_SUBFR, state->mem_syn_pst, 1);

      /* scale output to input */

      agc(&syn[i_subfr], &syn_pst[i_subfr], L_SUBFR);

      /* update res2[] buffer;  shift by L_SUBFR */

      Copy(&state->res2[L_SUBFR-PIT_MAX], &state->res2[-PIT_MAX], PIT_MAX);
      Copy(&state->scal_res2[L_SUBFR-PIT_MAX], &state->scal_res2[-PIT_MAX], PIT_MAX);

      Az += MP1;
    }

    /* update syn[] buffer */

    Copy(&syn[L_FRAME-M], &syn[-M], M);

    /* overwrite synthesis speech by postfiltered synthesis speech */

    Copy(syn_pst, syn, L_FRAME);
}

/*---------------------------------------------------------------------------*
 * procedure pitch_pst_filt                                                  *
 * ~~~~~~~~~~~~~~~~~~~~~~~~                                                  *
 * Find the pitch period  around the transmitted pitch and perform           *
 * harmonic postfiltering.                                                   *
 * Filtering through   (1 + g z^-T) / (1+g) ;   g = min(pit_gain*gammap, 1)  *
 *--------------------------------------------------------------------------*/
static void pit_pst_filt(
  Word16 *signal,      /* (i)     : input signal                        */
  Word16 *scal_sig,    /* (i)     : input signal (scaled, divided by 4) */
  Word16 t0_min,       /* (i)     : minimum value in the searched range */
  Word16 t0_max,       /* (i)     : maximum value in the searched range */
  Word16 L_subfr,      /* (i)     : size of filtering                   */
  Word16 *signal_pst   /* (o)     : harmonically postfiltered signal    */
)
{
  Word16 i, j, t0;
  Word16 g0, gain, cmax, en, en0;
  Word16 *p, *p1, *deb_sig;
  Word32 corr, cor_max, ener, ener0, temp;
  Word32 L_temp;

/*---------------------------------------------------------------------------*
 * Compute the correlations for all delays                                   *
 * and select the delay which maximizes the correlation                      *
 *---------------------------------------------------------------------------*/

  deb_sig = &scal_sig[-t0_min];
  cor_max = MIN_32;
  t0 = t0_min;             /* Only to remove warning from some compilers */
  for (i=t0_min; i<=t0_max; i++)
  {
    corr = 0;
    p    = scal_sig;
    p1   = deb_sig;
    for (j=0; j<L_subfr; j++)
       corr = L_mac(corr, *p++, *p1++);

    L_temp = L_sub(corr, cor_max);
    if (L_temp > (Word32)0)
    {
      cor_max = corr;
      t0 = i;
    }
    deb_sig--;
  }

  /* Compute the energy of the signal delayed by t0 */

  ener = 1;
  p = scal_sig - t0;
  for ( i=0; i<L_subfr ;i++, p++)
    ener = L_mac(ener, *p, *p);

  /* Compute the signal energy in the present subframe */

  ener0 = 1;
  p = scal_sig;
  for ( i=0; i<L_subfr; i++, p++)
    ener0 = L_mac(ener0, *p, *p);

  if (cor_max < 0)
  {
    cor_max = 0;
  }

  /* scale "cor_max", "ener" and "ener0" on 16 bits */

  temp = cor_max;
  if (ener > temp)
  {
    temp = ener;
  }
  if (ener0 > temp)
  {
    temp = ener0;
  }
  j = norm_l(temp);
  cmax = round(L_shl(cor_max, j));
  en = round(L_shl(ener, j));
  en0 = round(L_shl(ener0, j));

  /* prediction gain (dB)= -10 log(1-cor_max*cor_max/(ener*ener0)) */

  /* temp = (cor_max * cor_max) - (0.5 * ener * ener0)  */
  temp = L_mult(cmax, cmax);
  temp = L_sub(temp, L_shr(L_mult(en, en0), 1));

  if (temp < (Word32)0)           /* if prediction gain < 3 dB   */
  {                               /* switch off pitch postfilter */
    for (i = 0; i < L_subfr; i++)
      signal_pst[i] = signal[i];
    return;
  }

  if (sub(cmax, en) > 0)      /* if pitch gain > 1 */
  {
    g0 = INV_GAMMAP;
    gain = GAMMAP_2;
  }
  else {
    cmax = shr(mult(cmax, GAMMAP), 1);  /* cmax(Q14) = cmax(Q15) * GAMMAP */
    en = shr(en, 1);          /* Q14 */
    i = add(cmax, en);
    if(i > 0)
    {
      gain = div_s(cmax, i);    /* gain(Q15) = cor_max/(cor_max+ener)  */
      g0 = sub(32767, gain);    /* g0(Q15) = 1 - gain */
    }
    else
    {
      g0 =  32767;
      gain = 0;
    }
  }


  for (i = 0; i < L_subfr; i++)
  {
    /* signal_pst[i] = g0*signal[i] + gain*signal[i-t0]; */

    signal_pst[i] = add(mult(g0, signal[i]), mult(gain, signal[i-t0]));

  }
}

/*---------------------------------------------------------------------*
 * routine preemphasis()                                               *
 * ~~~~~~~~~~~~~~~~~~~~~                                               *
 * Preemphasis: filtering through 1 - g z^-1                           *
 *---------------------------------------------------------------------*/
static void preemphasis(
  Word16 *signal,  /* (i/o)   : input signal overwritten by the output */
  Word16 g,        /* (i) Q15 : preemphasis coefficient                */
  Word16 L         /* (i)     : size of filtering                      */
)
{
  static Word16 mem_pre = 0; // FIXME state??
  Word16 *p1, *p2, temp, i;

  p1 = signal + L - 1;
  p2 = p1 - 1;
  temp = *p1;

  for (i = 0; i <= L-2; i++)
  {
    *p1-- = sub(*p1, mult(g, *p2--));
  }

  *p1 = sub(*p1, mult(g, mem_pre));

  mem_pre = temp;
}

/*----------------------------------------------------------------------*
 *   routine agc()                                                      *
 *   ~~~~~~~~~~~~~                                                      *
 * Scale the postfilter output on a subframe basis by automatic control *
 * of the subframe gain.                                                *
 *  gain[n] = AGC_FAC * gain[n-1] + (1 - AGC_FAC) g_in/g_out            *
 *----------------------------------------------------------------------*/
static void agc(
  Word16 *sig_in,   /* (i)     : postfilter input signal  */
  Word16 *sig_out,  /* (i/o)   : postfilter output signal */
  Word16 l_trm      /* (i)     : subframe size            */
)
{
  static Word16 past_gain=4096;         /* past_gain = 1.0 (Q12) */ // FIXME state??
  Word16 i, exp;
  Word16 gain_in, gain_out, g0, gain;                     /* Q12 */
  Word32 s;

  Word16 signal[L_SUBFR];

  /* calculate gain_out with exponent */

  for(i=0; i<l_trm; i++)
    signal[i] = shr(sig_out[i], 2);

  s = 0;
  for(i=0; i<l_trm; i++)
    s = L_mac(s, signal[i], signal[i]);

  if (s == 0) {
    past_gain = 0;
    return;
  }
  exp = sub(norm_l(s), 1);
  gain_out = round(L_shl(s, exp));

  /* calculate gain_in with exponent */

  for(i=0; i<l_trm; i++)
    signal[i] = shr(sig_in[i], 2);

  s = 0;
  for(i=0; i<l_trm; i++)
    s = L_mac(s, signal[i], signal[i]);

  if (s == 0) {
    g0 = 0;
  }
  else {
    i = norm_l(s);
    gain_in = round(L_shl(s, i));
    exp = sub(exp, i);

   /*---------------------------------------------------*
    *  g0(Q12) = (1-AGC_FAC) * sqrt(gain_in/gain_out);  *
    *---------------------------------------------------*/

    s = L_deposit_l(div_s(gain_out,gain_in));   /* Q15 */
    s = L_shl(s, 7);           /* s(Q22) = gain_out / gain_in */
    s = L_shr(s, exp);         /* Q22, add exponent */

    /* i(Q12) = s(Q19) = 1 / sqrt(s(Q22)) */
    s = Inv_sqrt(s);           /* Q19 */
    i = round(L_shl(s,9));     /* Q12 */

    /* g0(Q12) = i(Q12) * (1-AGC_FAC)(Q15) */
    g0 = mult(i, AGC_FAC1);       /* Q12 */
  }

  /* compute gain(n) = AGC_FAC gain(n-1) + (1-AGC_FAC)gain_in/gain_out */
  /* sig_out(n) = gain(n) sig_out(n)                                   */

  gain = past_gain;
  for(i=0; i<l_trm; i++) {
    gain = mult(gain, AGC_FAC);
    gain = add(gain, g0);
    sig_out[i] = extract_h(L_shl(L_mult(sig_out[i], gain), 3));
  }
  past_gain = gain;
}
