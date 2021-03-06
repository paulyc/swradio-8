#
/*
 *    Copyright (C) 2015 .. 2017
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *    Lazy Chair programming
 *
 *    This file is part of the SDR-J 
 *    SDR-J is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    SDR-J is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with SDR-J; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include	<stdio.h>
#include	<stdlib.h>
#include	<math.h>
#include	"word-collector.h"
#include	"reader.h"
#include	"drm-decoder.h"
#include	"basics.h"

//	The wordCollector will handle segments of a given size,
//	do all kinds of frequency correction (timecorrection
//	is done in the syncer) and map them onto ofdm words.
//	
//	The caller just calls upon "getWord" to get a new ofdm word
//
//	The frequency shifter is in steps of 0.01 Hz
	wordCollector::wordCollector (Reader	*b,
	                              int32_t	sampleRate,
	                              uint8_t	Mode,
	                              uint8_t	Spectrum,
	                              drmDecoder *mr):
	                                 theShifter (100 * sampleRate) {
	this	-> buffer	= b;
	this	-> sampleRate	= sampleRate;
	this	-> Mode		= Mode;
	this	-> Spectrum	= Spectrum;
	this	-> master	= mr;
	this	-> theAngle	= 0;
	this	-> Tu		= Tu_of (Mode);
	this	-> Ts		= Ts_of (Mode);
	this	-> Tg		= Tg_of (Mode);
	this	-> K_min	= Kmin	(Mode, Spectrum);
	this	-> K_max	= Kmax	(Mode, Spectrum);
	this	-> displayCount	= 0;
	this	-> timedelay	= 0;
	fft_vector		= (DSPCOMPLEX *)
	                               fftwf_malloc (Tu *
	                                            sizeof (fftwf_complex));
	hetPlan			= fftwf_plan_dft_1d (Tu,
	                    reinterpret_cast <fftwf_complex *>(fft_vector),
	                    reinterpret_cast <fftwf_complex *>(fft_vector),
	                    FFTW_FORWARD, FFTW_ESTIMATE);
	connect (this, SIGNAL (show_fineOffset (float)),
	         mr, SLOT (show_fineOffset (float)));
	connect (this, SIGNAL (show_coarseOffset (float)),
	         mr, SLOT (show_coarseOffset (float)));
	connect (this, SIGNAL (show_timeDelay	(float)),
	         mr, SLOT (show_timeDelay (float)));
	connect (this, SIGNAL (show_timeOffset	(float)),
	         mr, SLOT (show_timeOffset (float)));
	connect (this, SIGNAL (show_clockOffset (float)),
	         mr, SLOT (show_clockOffset (float)));
	connect (this, SIGNAL (show_angle (float)),
	         mr, SLOT (show_angle (float)));
}

		wordCollector::~wordCollector (void) {
	fftwf_free (fft_vector);
	fftwf_destroy_plan (hetPlan);
}

//	when starting up, we "borrow" the precomputed frequency offset
//	and start building up the spectrumbuffer.
//	
void	wordCollector::getWord (DSPCOMPLEX	*out,
	                        int32_t		initialFreq,
	                        float		offsetFractional) {
DSPCOMPLEX	temp [Ts];
int16_t		i;
DSPCOMPLEX	angle	= DSPCOMPLEX (0, 0);
float		offset	= 0;
int32_t		bufMask	= buffer -> bufSize - 1;
int16_t	d;
float	timeOffsetFractional;

	buffer		-> waitfor (Ts + Ts / 2);
	timedelay	= offsetFractional;	// was computed by getMode

	if (timedelay < 0)
	   timedelay = 0;
	d = floor (timedelay + 0.5);
	timeOffsetFractional	= timedelay - d;
	timedelay		-= d;
//	To take into account the fractional timing difference,
//	keep it simple, just linear interpolation
	int f = (int)(floor (buffer -> currentIndex)) & bufMask;
	if (timeOffsetFractional < 0) {
	   timeOffsetFractional = 1 + timeOffsetFractional;
	   f -= 1;
	}
	for (i = 0; i < Ts; i ++) {
	   DSPCOMPLEX one = buffer -> data [(f + i) & bufMask];
	   DSPCOMPLEX two = buffer -> data [(f + i + 1) & bufMask];
	   temp [i] = cmul (one, 1 - timeOffsetFractional) +
	                       cmul (two, timeOffsetFractional);
	}

//	And we shift the bufferpointer here
	buffer -> currentIndex = (buffer -> currentIndex + Ts) & bufMask;

//	Now: determine the fine grain offset.
	for (i = 0; i < Tg; i ++)
	   angle += conj (temp [Tu + i]) * temp [i];
//	simple averaging
	theAngle	= 0.9 * theAngle + 0.1 * arg (angle);
//
//	offset  (and shift) in Hz / 100
	offset		= theAngle / (2 * M_PI) * 100 * sampleRate / Tu;
	if (offset == offset)	// precaution to handle undefines
	   theShifter. do_shift (temp, Ts, 100 * initialFreq - offset);

	if (++displayCount > 20) {
	   displayCount = 0;
	   show_coarseOffset	(initialFreq);
	   show_fineOffset	(- offset / 100);
	   show_angle		(arg (angle));
	   show_timeDelay	(offsetFractional);
	}

//	and extract the Tu set of samples for fft processsing
	memcpy (fft_vector, &temp [Tg], Tu * sizeof (DSPCOMPLEX));

	fftwf_execute (hetPlan);
//	extract the "useful" data
	if (K_min < 0) {
	   memcpy (out,
	           &fft_vector [Tu + K_min], - K_min * sizeof (DSPCOMPLEX));
	   memcpy (&out [- K_min],
	           &fft_vector [0], (K_max + 1) * sizeof (DSPCOMPLEX));
	}
	else
	   memcpy (out,
	           &fft_vector [-K_min],
	           (K_max - K_min + 1) * sizeof (DSPCOMPLEX));
}

//
//	The getWord as below is used in the main loop, to obtain
//	a next ofdm word
//	With this "getWord" we  also correct the frequency and
//	timing offsets based on tracking values, computed
//	during equalization
void	wordCollector::getWord (DSPCOMPLEX	*out,
	                        int32_t		initialFreq,
	                        bool		firstTime,
	                        float		offsetFractional,
	                        float		angle,
	                        float		clockOffset) {
DSPCOMPLEX	temp [Ts];
int16_t		i;
float	offset	= 0;
int32_t		bufMask		= buffer -> bufSize - 1;

int16_t	timeOffsetInteger;
float	timeOffsetFractional;
//	If we start in the main cycle, we "compute" the
//	time offset, once in the main cycle, we "track"
//	based on the measured difference
	buffer		-> waitfor (Ts + Ts / 2);

	if (firstTime) {		// set the values
	   theAngle		= 0;
	   sampleclockOffset	= 0;
	}
	else { 			// apparently tracking mode
//	Note that the constants are chosen pretty arbitrarily, which works
//	as long as the tracking values are not too large
	   sampleclockOffset	= 0.9 * sampleclockOffset + 0.1 * clockOffset;
	   timedelay		-= 0.5 * offsetFractional;
	}
	
	timeOffsetInteger	= floor (timedelay + 0.5);
	timeOffsetFractional	= timedelay - timeOffsetInteger;
	timedelay		-= timeOffsetInteger;
//	To take into account the fractional timing difference,
//	we do some interpolation. We assume that -0.5 <= c && c <= 0.5
	int f	= (int)(floor (buffer -> currentIndex + 
	                              timeOffsetInteger)) & bufMask;
//	just linear interpolation
	for (i = 0; i < Ts; i ++) {
	   if (timeOffsetFractional < 0) {
	      timeOffsetFractional = 1 + timeOffsetFractional;
	      f -= 1;
	   }
	   else 
	   if (timeOffsetFractional > 1) {
	      timeOffsetFractional -= 1;
	      f += 1;
	   }
	   DSPCOMPLEX one = buffer -> data [(f + i) & bufMask];
	   DSPCOMPLEX two = buffer -> data [(f + i + 1) & bufMask];
	   temp [i] = cmul (one, 1 - timeOffsetFractional) +
	              cmul (two, timeOffsetFractional);
//	   timeOffsetFractional -= sampleclockOffset / 2;
	}

//	And we adjust the bufferpointer here
	buffer -> currentIndex = (buffer -> currentIndex + 
	                                timeOffsetInteger + Ts) & bufMask;
//	correct the phase
//
//	If we are here for the first time, we compute an initial offset.
	if (firstTime) {	// compute a vfo offset
	   DSPCOMPLEX c = DSPCOMPLEX (0, 0);
	   for (i = 0; i < Tg; i ++)
	      c += conj (temp [Tu + i]) * temp [i];
	   theAngle = arg (c);
	}
	else
	   theAngle	= theAngle - 0.20 * angle;
//	offset in 0.01 * Hz
	offset          = theAngle / (2 * M_PI) * 100 * sampleRate / Tu;
	if (offset == offset)	// precaution to handle undefines
	   theShifter. do_shift (temp, Ts,  100 * initialFreq - offset);
	
	if (++displayCount > 20) {
	   displayCount = 0;
	   show_coarseOffset	(initialFreq);
	   show_fineOffset	(- offset / 100);
	   show_angle		(angle);
	   show_timeOffset	(offsetFractional);
	   show_timeDelay	(timedelay);
	   show_clockOffset	(Ts * clockOffset);
	}

//	and extract the Tu set of samples for fft processsing
	memcpy (fft_vector, &temp [Tg], Tu * sizeof (DSPCOMPLEX));

	fftwf_execute (hetPlan);
//	extract the "useful" data
	if (Kmin (Mode, Spectrum) < 0) {
	   memcpy (out,
	           &fft_vector [Tu + K_min], - K_min * sizeof (DSPCOMPLEX));
	   memcpy (&out [- K_min],
	           &fft_vector [0], (K_max + 1) * sizeof (DSPCOMPLEX));
	}
	else
	   memcpy (out, &fft_vector [-K_min], (K_max - K_min + 1) * sizeof (DSPCOMPLEX));
}
