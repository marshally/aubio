/*
  Copyright (C) 2003-2009 Paul Brossier <piem@aubio.org>

  This file is part of aubio.

  aubio is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  aubio is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with aubio.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "aubio_priv.h"
#include "fvec.h"
#include "cvec.h"
#include "mathutils.h"
#include "spectral/fft.h"
#include "pitch/pitchyinfft.h"

/** pitch yinfft structure */
struct _aubio_pitchyinfft_t
{
  fvec_t *win;        /**< temporal weighting window */
  fvec_t *winput;     /**< windowed spectrum */
  cvec_t *res;        /**< complex vector to compute square difference function */
  fvec_t *sqrmag;     /**< square difference function */
  fvec_t *weight;     /**< spectral weighting window (psychoacoustic model) */
  cvec_t *fftout;     /**< Fourier transform output */
  aubio_fft_t *fft;   /**< fft object to compute square difference function */
  fvec_t *yinfft;     /**< Yin function */
  smpl_t tol;         /**< Yin tolerance */
};

static const smpl_t freqs[] = { 0., 20., 25., 31.5, 40., 50., 63., 80., 100.,
  125., 160., 200., 250., 315., 400., 500., 630., 800., 1000., 1250.,
  1600., 2000., 2500., 3150., 4000., 5000., 6300., 8000., 9000., 10000.,
  12500., 15000., 20000., 25100
};

static const smpl_t weight[] = { -75.8, -70.1, -60.8, -52.1, -44.2, -37.5,
  -31.3, -25.6, -20.9, -16.5, -12.6, -9.6, -7.0, -4.7, -3.0, -1.8, -0.8,
  -0.2, -0.0, 0.5, 1.6, 3.2, 5.4, 7.8, 8.1, 5.3, -2.4, -11.1, -12.8,
  -12.2, -7.4, -17.8, -17.8, -17.8
};

aubio_pitchyinfft_t *
new_aubio_pitchyinfft (uint_t bufsize)
{
  aubio_pitchyinfft_t *p = AUBIO_NEW (aubio_pitchyinfft_t);
  p->winput = new_fvec (bufsize);
  p->fft = new_aubio_fft (bufsize);
  p->fftout = new_cvec (bufsize);
  p->sqrmag = new_fvec (bufsize);
  p->res = new_cvec (bufsize);
  p->yinfft = new_fvec (bufsize / 2 + 1);
  p->tol = 0.85;
  p->win = new_aubio_window ("hanningz", bufsize);
  p->weight = new_fvec (bufsize / 2 + 1);
  uint_t i = 0, j = 1;
  smpl_t freq = 0, a0 = 0, a1 = 0, f0 = 0, f1 = 0;
  for (i = 0; i < p->weight->length; i++) {
    freq = (smpl_t) i / (smpl_t) bufsize *(smpl_t) 44100.;
    while (freq > freqs[j]) {
      j += 1;
    }
    a0 = weight[j - 1];
    f0 = freqs[j - 1];
    a1 = weight[j];
    f1 = freqs[j];
    if (f0 == f1) {           // just in case
      p->weight->data[i] = a0;
    } else if (f0 == 0) {     // y = ax+b
      p->weight->data[i] = (a1 - a0) / f1 * freq + a0;
    } else {
      p->weight->data[i] = (a1 - a0) / (f1 - f0) * freq +
          (a0 - (a1 - a0) / (f1 / f0 - 1.));
    }
    while (freq > freqs[j]) {
      j += 1;
    }
    //AUBIO_DBG("%f\n",p->weight->data[i]);
    p->weight->data[i] = DB2LIN (p->weight->data[i]);
    //p->weight->data[i] = SQRT(DB2LIN(p->weight->data[i]));
  }
  return p;
}

void
aubio_pitchyinfft_do (aubio_pitchyinfft_t * p, fvec_t * input, fvec_t * output)
{
  uint_t tau, l;
  uint_t halfperiod;
  smpl_t tmp, sum;
  cvec_t *res = (cvec_t *) p->res;
  fvec_t *yin = (fvec_t *) p->yinfft;
  l = 0;
  tmp = 0.;
  sum = 0.;
  for (l = 0; l < input->length; l++) {
    p->winput->data[l] = p->win->data[l] * input->data[l];
  }
  aubio_fft_do (p->fft, p->winput, p->fftout);
  for (l = 0; l < p->fftout->length; l++) {
    p->sqrmag->data[l] = SQR (p->fftout->norm[l]);
    p->sqrmag->data[l] *= p->weight->data[l];
  }
  for (l = 1; l < p->fftout->length; l++) {
    p->sqrmag->data[(p->fftout->length - 1) * 2 - l] =
        SQR (p->fftout->norm[l]);
    p->sqrmag->data[(p->fftout->length - 1) * 2 - l] *=
        p->weight->data[l];
  }
  for (l = 0; l < p->sqrmag->length / 2 + 1; l++) {
    sum += p->sqrmag->data[l];
  }
  sum *= 2.;
  aubio_fft_do (p->fft, p->sqrmag, res);
  yin->data[0] = 1.;
  for (tau = 1; tau < yin->length; tau++) {
    yin->data[tau] = sum - res->norm[tau] * COS (res->phas[tau]);
    tmp += yin->data[tau];
    yin->data[tau] *= tau / tmp;
  }
  tau = fvec_min_elem (yin);
  if (yin->data[tau] < p->tol) {
    /* no interpolation */
    //return tau;
    /* 3 point quadratic interpolation */
    //return fvec_quadint_min(yin,tau,1);
    /* additional check for (unlikely) octave doubling in higher frequencies */
    if (tau > 35) {
      output->data[0] = fvec_quadint (yin, tau);
    } else {
      /* should compare the minimum value of each interpolated peaks */
      halfperiod = FLOOR (tau / 2 + .5);
      if (yin->data[halfperiod] < p->tol)
        output->data[0] = fvec_quadint (yin, halfperiod);
      else
        output->data[0] = fvec_quadint (yin, tau);
    }
  } else {
    output->data[0] = 0.;
  }
}

void
del_aubio_pitchyinfft (aubio_pitchyinfft_t * p)
{
  del_fvec (p->win);
  del_aubio_fft (p->fft);
  del_fvec (p->yinfft);
  del_fvec (p->sqrmag);
  del_cvec (p->res);
  del_cvec (p->fftout);
  del_fvec (p->winput);
  del_fvec (p->weight);
  AUBIO_FREE (p);
}

uint_t
aubio_pitchyinfft_set_tolerance (aubio_pitchyinfft_t * p, smpl_t tol)
{
  p->tol = tol;
  return 0;
}

smpl_t
aubio_pitchyinfft_get_tolerance (aubio_pitchyinfft_t * p)
{
  return p->tol;
}
