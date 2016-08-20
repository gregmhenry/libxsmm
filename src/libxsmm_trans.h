/******************************************************************************
** Copyright (c) 2016, Intel Corporation                                     **
** All rights reserved.                                                      **
**                                                                           **
** Redistribution and use in source and binary forms, with or without        **
** modification, are permitted provided that the following conditions        **
** are met:                                                                  **
** 1. Redistributions of source code must retain the above copyright         **
**    notice, this list of conditions and the following disclaimer.          **
** 2. Redistributions in binary form must reproduce the above copyright      **
**    notice, this list of conditions and the following disclaimer in the    **
**    documentation and/or other materials provided with the distribution.   **
** 3. Neither the name of the copyright holder nor the names of its          **
**    contributors may be used to endorse or promote products derived        **
**    from this software without specific prior written permission.          **
**                                                                           **
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS       **
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT         **
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR     **
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT      **
** HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,    **
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED  **
** TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR    **
** PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF    **
** LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING      **
** NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS        **
** SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.              **
******************************************************************************/
/* Hans Pabst (Intel Corp.)
******************************************************************************/
#ifndef LIBXSMM_TRANS_H
#define LIBXSMM_TRANS_H

#include <libxsmm.h>

/* external implementation, if a supported library is enabled at build-time */
#if !defined(LIBXSMM_TRANS_EXTERNAL) && \
   ((defined(__MKL) || defined(MKL_DIRECT_CALL_SEQ) || defined(MKL_DIRECT_CALL)) \
  || defined(__OPENBLAS))
/*# define LIBXSMM_TRANS_EXTERNAL*/
#endif

#define LIBXSMM_TRANS_OOP(TYPE, CHUNKSIZE, OUT, IN, M0, M1, N0, N1, N, LD, LDO) { \
  const TYPE *const a = (const TYPE*)(IN); \
  TYPE *const b = (TYPE*)(OUT); \
  libxsmm_blasint i, j; \
  if (CHUNKSIZE == (N) && 0 == LIBXSMM_MOD2((uintptr_t)b, LIBXSMM_ALIGNMENT)) { \
    for (i = M0; i < (M1); ++i) { \
      LIBXSMM_PRAGMA_VALIGNED_VARS(b) \
      LIBXSMM_PRAGMA_NONTEMPORAL \
      for (j = N0; j < (N0) + (CHUNKSIZE); ++j) { \
        /* use consecutive stores and strided loads */ \
        b[i*(LDO)+j] = a[j*(LD)+i]; \
      } \
    } \
  } \
  else { \
    for (i = M0; i < (M1); ++i) { \
      LIBXSMM_PRAGMA_NONTEMPORAL \
      for (j = N0; j < (N1); ++j) { \
        b[i*(LDO)+j] = a[j*(LD)+i]; \
      } \
    } \
  } \
}

#endif /*LIBXSMM_TRANS_H*/

