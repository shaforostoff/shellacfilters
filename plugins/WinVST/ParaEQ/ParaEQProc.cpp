/* ========================================
 *  ParaEQ - ParaEQProc.cpp
 *  MIT license, as the rest of the tree
 * ======================================== */

#ifndef __ParaEQ_H
#include "ParaEQ.h"
#endif

/*  There is no per-sample DSP written out here, and that is deliberate: the
 *  algorithm is a cascade of six cookbook biquads with per-sample coefficient
 *  gliding, and it lives in paraeq_core.cpp so that the foobar2000 DSP and this
 *  plug-in are running the identical code. What belongs in this file is only
 *  what a VST has to do that the foobar2000 wrapper does not:
 *
 *    - notice sample rate and slider changes, since a VST is told about
 *      neither (updateConfig(), in ParaEQ.cpp),
 *    - dither on the way down to 32 bit float.
 *
 *  Handing the host n samples out for every n in is free here: this core has no
 *  latency and works in place, so there is no lookahead to pre-roll, no FIFO,
 *  and nothing to flush at the end of a bounce. That is the same deal Dehum
 *  gets and the opposite of Declick's.
 *
 *  The blocks below exist only because the DSP is done in double while the
 *  float path has to dither on the way out: the scratch buffers are where the
 *  doubles live in between. The core does have a float instantiation of
 *  process(), and using it would save the copy - and would also round every
 *  sample to float on the way out of the cascade, so that the 32 and 64 bit
 *  paths were no longer the same DSP plus a dither but two different ones.
 *
 *  Splitting a long buffer across several calls is something the core cannot
 *  detect: process() carries its glide in member state and picks up exactly
 *  where it left off, which is what paraeq_vst_verify's block size invariance
 *  check establishes - including across a glide, which is the only time this
 *  core's output depends on anything but the current sample.
 */

void ParaEQ::processReplacing(float **inputs, float **outputs, VstInt32 sampleFrames)
{
    float* in1  =  inputs[0];
    float* in2  =  inputs[1];
    float* out1 = outputs[0];
    float* out2 = outputs[1];

	//FTZ for the duration of the buffer. A biquad's state is a recursion towards
	//zero, so a long fade-out reaches denormal range and pays for it on hardware
	//that traps. This is not just a speed guard either: FTZ changes results in
	//the last bits, so it is part of the numerical contract, and the ports only
	//match each other while they all agree about it. foo_dsp_paraeq holds the
	//same guard.
	paraeq::scoped_flush_denormals ftz;
	updateConfig();

	while (sampleFrames > 0) {
		const VstInt32 n = (sampleFrames > (VstInt32)kScratch)
		                 ? (VstInt32)kScratch : sampleFrames;

		for (VstInt32 i = 0; i < n; ++i) {
			scratchL[i] = in1[i];
			scratchR[i] = in2[i];
		}

		chanL.process(scratchL, (size_t)n, 1);
		chanR.process(scratchR, (size_t)n, 1);
		//non-finite input is silenced inside the core, so nothing can lodge
		//itself in a biquad's state and stay there for the rest of the stream

		for (VstInt32 i = 0; i < n; ++i) {
			double inputSampleL = scratchL[i];
			double inputSampleR = scratchR[i];

			//begin 32 bit stereo floating point dither
			int expon; frexpf((float)inputSampleL, &expon);
			fpdL ^= fpdL << 13; fpdL ^= fpdL >> 17; fpdL ^= fpdL << 5;
			inputSampleL += ((double(fpdL)-uint32_t(0x7fffffff)) * 5.5e-36l * pow(2,expon+62));
			frexpf((float)inputSampleR, &expon);
			fpdR ^= fpdR << 13; fpdR ^= fpdR >> 17; fpdR ^= fpdR << 5;
			inputSampleR += ((double(fpdR)-uint32_t(0x7fffffff)) * 5.5e-36l * pow(2,expon+62));
			//end 32 bit stereo floating point dither

			out1[i] = inputSampleL;
			out2[i] = inputSampleR;
		}

		in1 += n; in2 += n; out1 += n; out2 += n;
		sampleFrames -= n;
	}
}

void ParaEQ::processDoubleReplacing(double **inputs, double **outputs, VstInt32 sampleFrames)
{
    double* in1  =  inputs[0];
    double* in2  =  inputs[1];
    double* out1 = outputs[0];
    double* out2 = outputs[1];

	paraeq::scoped_flush_denormals ftz;
	updateConfig();

	while (sampleFrames > 0) {
		const VstInt32 n = (sampleFrames > (VstInt32)kScratch)
		                 ? (VstInt32)kScratch : sampleFrames;

		//in place, so the copy is the only reason a scratch buffer is involved:
		//a host may hand us the same pointer for input and output, or not
		for (VstInt32 i = 0; i < n; ++i) {
			scratchL[i] = in1[i];
			scratchR[i] = in2[i];
		}

		chanL.process(scratchL, (size_t)n, 1);
		chanR.process(scratchR, (size_t)n, 1);

		for (VstInt32 i = 0; i < n; ++i) {
			//begin 64 bit stereo floating point dither
			//int expon; frexp((double)inputSampleL, &expon);
			fpdL ^= fpdL << 13; fpdL ^= fpdL >> 17; fpdL ^= fpdL << 5;
			//inputSampleL += ((double(fpdL)-uint32_t(0x7fffffff)) * 1.1e-44l * pow(2,expon+62));
			//frexp((double)inputSampleR, &expon);
			fpdR ^= fpdR << 13; fpdR ^= fpdR >> 17; fpdR ^= fpdR << 5;
			//inputSampleR += ((double(fpdR)-uint32_t(0x7fffffff)) * 1.1e-44l * pow(2,expon+62));
			//end 64 bit stereo floating point dither
			//this path is left undithered, which also makes it the one to compare
			//against foo_dsp_paraeq sample for sample

			out1[i] = scratchL[i];
			out2[i] = scratchR[i];
		}

		in1 += n; in2 += n; out1 += n; out2 += n;
		sampleFrames -= n;
	}
}
