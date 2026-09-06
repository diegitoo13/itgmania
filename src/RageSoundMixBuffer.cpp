#include "RageSoundMixBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

RageSoundMixBuffer::RageSoundMixBuffer() : m_iOffset(0) {}
RageSoundMixBuffer::~RageSoundMixBuffer() {}

/* write() will start mixing iOffset samples into the buffer.  Be careful; this
 * is measured in samples, not frames, so if the data is stereo, multiply by
 * two. */
void RageSoundMixBuffer::SetWriteOffset(int iOffset) {
  m_iOffset = static_cast<int>(iOffset);
}

void RageSoundMixBuffer::Extend(unsigned iSamples) {
  const unsigned realsize = iSamples + m_iOffset;
  if (m_pMixbuf.size() < realsize) {
    m_pMixbuf.resize(realsize, 0.0f);
  }
}

void RageSoundMixBuffer::write(
    const float* pBuf, unsigned iSize, int iSourceStride, int iDestStride) {
  if (iSize == 0) {
    return;
  }

  // iSize = 3, iDestStride = 2 uses 4 frames.  Don't allocate the stride of the
  // last sample.
  Extend(iSize * iDestStride - (iDestStride - 1));

  // Load the audio at m_iOffset into the buffer.
  float* pDestBuf = &m_pMixbuf[m_iOffset];

  while (iSize) {
    *pDestBuf += *pBuf;
    pBuf += iSourceStride;
    pDestBuf += iDestStride;
    --iSize;
  }
}

/* Soft limiter for the final mix. On dense/loud sections the summed mix can
 * exceed full scale; a hard clamp at +-1 crackles, so saturate smoothly
 * instead: linear below |0.8|, tanh knee bending toward +-1 above it.
 * Applied here, at the single post-mix boundary, so every backend (and the
 * stream-audio tap) receives identical data. */
static float SoftClipMix(float s) {
  if (s > 0.8f) {
    return 0.8f + 0.2f * tanhf((s - 0.8f) * 5.0f);
  }
  if (s < -0.8f) {
    return -0.8f + 0.2f * tanhf((s + 0.8f) * 5.0f);
  }
  return s;
}

void RageSoundMixBuffer::read(int16_t* pBuf) {
  for (unsigned iPos = 0; iPos < m_pMixbuf.size(); ++iPos) {
    // do the read
    float iOut = m_pMixbuf[iPos];
    // keep the mix within bounds; saturate smoothly to avoid clamp crackle
    iOut = SoftClipMix(iOut);
    // round rather than truncate to minimize distortion
    pBuf[iPos] = static_cast<int16_t>(std::round(iOut * INT16_MAX));
  }
  m_pMixbuf.clear();
}

void RageSoundMixBuffer::read(float* pBuf) {
  // keep the mix within bounds; saturate smoothly to avoid clamp crackle
  std::transform(m_pMixbuf.begin(), m_pMixbuf.end(), pBuf, SoftClipMix);
  m_pMixbuf.clear();
}

void RageSoundMixBuffer::read_deinterlace(float** pBufs, int channels) {
  for (unsigned i = 0; i < m_pMixbuf.size() / channels; ++i) {
    for (int ch = 0; ch < channels; ++ch) {
      pBufs[ch][i] = m_pMixbuf[channels * i + ch];
    }
  }
  m_pMixbuf.clear();
}

/*
 * Copyright (c) 2002-2004 Glenn Maynard
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
