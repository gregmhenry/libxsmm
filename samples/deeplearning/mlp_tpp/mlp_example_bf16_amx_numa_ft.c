/******************************************************************************
* Copyright (c) Intel Corporation - All rights reserved.                      *
* This file is part of the LIBXSMM library.                                   *
*                                                                             *
* For information on the license, see the LICENSE file.                       *
* Further information: https://github.com/hfp/libxsmm/                        *
* SPDX-License-Identifier: BSD-3-Clause                                       *
******************************************************************************/
/* Evangelos Georganas, Alexander Heinecke (Intel Corp.)
******************************************************************************/
#include <libxsmm.h>
#include <libxsmm_sync.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#if defined(_OPENMP)
# include <omp.h>
#endif

/* include c-based dnn library */
#include "../common/dnn_common.h"

#define CHECK_L1
#define OVERWRITE_DOUTPUT_BWDUPD

#define _mm512_load_fil(A)   _mm512_castsi512_ps(_mm512_slli_epi32(_mm512_cvtepi16_epi32(_mm256_loadu_si256((__m256i*)(A))),16))
#define _mm512_store_fil(A,B)  _mm256_storeu_si256((__m256i*)(A), _mm512_cvtneps_pbh((B)))

#define BYPASS_SGD

LIBXSMM_INLINE void my_init_buf(float* buf, size_t size, int initPos, int initOne)
{
  int i;
  zero_buf(buf, size);
  for (i = 0; i < (int)size; ++i) {
    buf[i] = (float)((initOne != 0) ? 1.0 : ((initPos != 0) ? libxsmm_rng_f64() : (0.05 - libxsmm_rng_f64()/10.0)));
  }
}

LIBXSMM_INLINE void my_init_buf_bf16(libxsmm_bfloat16* buf, size_t size, int initPos, int initOne)
{
  int i;
  zero_buf_bf16(buf, size);
  for (i = 0; i < (int)size; ++i) {
    libxsmm_bfloat16_hp tmp;
    tmp.f = (float)((initOne != 0) ? 1.0 : ((initPos != 0) ? libxsmm_rng_f64() : (0.05 - libxsmm_rng_f64()/10.0)));
    buf[i] = tmp.i[1];
  }
}

#if 0
LIBXSMM_INLINE void my_matrix_copy_KCCK_to_KCCK_vnni(float *src, float *dst, int C, int K, int bc, int bk)
{
  int k1, k2, c1, c2;
  int kBlocks = K/bk;
  int cBlocks = C/bc;
  LIBXSMM_VLA_DECL(4, float, real_src, src, cBlocks, bc, bk);
  LIBXSMM_VLA_DECL(5, float, real_dst, dst, cBlocks, bc/2, bk, 2);

  for (k1 = 0; k1 < kBlocks; k1++) {
    for (c1 = 0; c1 < cBlocks; c1++) {
      for (c2 = 0; c2 < bc; c2++) {
        for (k2 = 0; k2 < bk; k2++) {
            LIBXSMM_VLA_ACCESS(5, real_dst, k1, c1, c2/2, k2, c2%2, cBlocks, bc/2, bk, 2) = LIBXSMM_VLA_ACCESS(4, real_src, k1, c1, c2, k2, cBlocks, bc, bk);
        }
      }
    }
  }
}
#endif

typedef enum my_eltwise_fuse {
  MY_ELTWISE_FUSE_NONE = 0,
  MY_ELTWISE_FUSE_BIAS = 1,
  MY_ELTWISE_FUSE_RELU = 2,
  MY_ELTWISE_FUSE_BIAS_RELU = MY_ELTWISE_FUSE_BIAS | MY_ELTWISE_FUSE_RELU
} my_eltwise_fuse;

typedef enum my_pass {
  MY_PASS_FWD   = 1,
  MY_PASS_BWD_D = 2,
  MY_PASS_BWD_W = 4,
  MY_PASS_BWD   = 6
} my_pass;

typedef struct my_opt_config {
  libxsmm_blasint C;
  libxsmm_blasint K;
  libxsmm_blasint bc;
  libxsmm_blasint bk;
  libxsmm_blasint threads;
  float           lr;
  size_t          scratch_size;
  libxsmm_barrier* barrier;
} my_opt_config;

typedef struct my_smax_fwd_config {
  libxsmm_blasint N;
  libxsmm_blasint C;
  libxsmm_blasint bn;
  libxsmm_blasint bc;
  libxsmm_blasint threads;
  size_t          scratch_size;
  libxsmm_barrier* barrier;
} my_smax_fwd_config;

typedef struct my_smax_bwd_config {
  libxsmm_blasint N;
  libxsmm_blasint C;
  libxsmm_blasint bn;
  libxsmm_blasint bc;
  libxsmm_blasint threads;
  size_t          scratch_size;
  float           loss_weight;
  libxsmm_barrier* barrier;
} my_smax_bwd_config;


typedef struct my_fc_fwd_config {
  libxsmm_blasint N;
  libxsmm_blasint C;
  libxsmm_blasint K;
  libxsmm_blasint bn;
  libxsmm_blasint bc;
  libxsmm_blasint bk;
  libxsmm_blasint threads;
  my_eltwise_fuse fuse_type;
  libxsmm_blasint fwd_bf;
  libxsmm_blasint fwd_2d_blocking;
  libxsmm_blasint fwd_row_teams;
  libxsmm_blasint fwd_column_teams;
  libxsmm_blasint fwd_model_hyperpartitions;
  size_t          scratch_size;
  libxsmm_barrier* barrier;
  libxsmm_bsmmfunction fwd_config_kernel;
  libxsmm_bsmmfunction tilerelease_kernel;
  libxsmm_bsmmfunction_reducebatch_strd gemm_fwd;
  libxsmm_bsmmfunction_reducebatch_strd gemm_fwd2;
  libxsmm_bmmfunction_reducebatch_strd gemm_fwd3;
  libxsmm_bmmfunction_reducebatch_strd_meltwfused gemm_fwd4;
  libxsmm_bmmfunction_reducebatch_strd_meltwfused gemm_fwd5;
  libxsmm_bmmfunction_reducebatch_strd_meltwfused gemm_fwd6;
  libxsmm_bmmfunction_reducebatch_strd_meltwfused gemm_fwd7;
  libxsmm_bmmfunction_reducebatch_strd_meltwfused gemm_fwd8;
  libxsmm_meltwfunction_cvtfp32bf16     fwd_cvtfp32bf16_kernel;
  libxsmm_meltwfunction_cvtfp32bf16_act fwd_cvtfp32bf16_relu_kernel;
  libxsmm_meltwfunction_act_cvtfp32bf16 fwd_sigmoid_cvtfp32bf16_kernel;
  libxsmm_meltwfunction_copy            fwd_zero_kernel;
  libxsmm_meltwfunction_copy            fwd_copy_bf16fp32_kernel;
  libxsmm_meltwfunction_copy            fwd_colbcast_bf16fp32_copy_kernel;
} my_fc_fwd_config;

typedef struct my_fc_bwd_config {
  libxsmm_blasint N;
  libxsmm_blasint C;
  libxsmm_blasint K;
  libxsmm_blasint bn;
  libxsmm_blasint bc;
  libxsmm_blasint bk;
  libxsmm_blasint threads;
  my_eltwise_fuse fuse_type;
  libxsmm_blasint bwd_bf;
  libxsmm_blasint bwd_2d_blocking;
  libxsmm_blasint bwd_row_teams;
  libxsmm_blasint bwd_column_teams;
  libxsmm_blasint bwd_model_hyperpartitions;
  libxsmm_blasint upd_bf;
  libxsmm_blasint upd_2d_blocking;
  libxsmm_blasint upd_row_teams;
  libxsmm_blasint upd_column_teams;
  libxsmm_blasint upd_model_hyperpartitions;
  libxsmm_blasint ifm_subtasks;
  libxsmm_blasint ofm_subtasks;
  size_t          scratch_size;
  size_t  doutput_scratch_mark;
  libxsmm_barrier* barrier;
  libxsmm_bsmmfunction bwd_config_kernel;
  libxsmm_bsmmfunction upd_config_kernel;
  libxsmm_bsmmfunction tilerelease_kernel;
  libxsmm_bsmmfunction_reducebatch_strd gemm_bwd;
  libxsmm_bsmmfunction_reducebatch_strd gemm_bwd2;
  libxsmm_bmmfunction_reducebatch_strd gemm_bwd3;
  libxsmm_bsmmfunction_reducebatch_strd gemm_upd;
  libxsmm_bsmmfunction_reducebatch_strd gemm_upd2;
  libxsmm_bmmfunction_reducebatch_strd gemm_upd3;
  libxsmm_meltwfunction_cvtfp32bf16     bwd_cvtfp32bf16_kernel;
  libxsmm_meltwfunction_cvtfp32bf16     upd_cvtfp32bf16_kernel;
  libxsmm_meltwfunction_relu            bwd_relu_kernel;
  libxsmm_meltwfunction_copy            bwd_zero_kernel;
  libxsmm_meltwfunction_copy            upd_zero_kernel;
  libxsmm_meltwfunction_reduce          delbias_reduce_kernel;
  libxsmm_meltwfunction_transform       vnni_to_vnniT_kernel;
  libxsmm_meltwfunction_transform       norm_to_normT_kernel;
  libxsmm_meltwfunction_transform       norm_to_vnni_kernel;
} my_fc_bwd_config;

my_fc_fwd_config setup_my_fc_fwd(libxsmm_blasint N, libxsmm_blasint C, libxsmm_blasint K, libxsmm_blasint bn,
                                 libxsmm_blasint bc, libxsmm_blasint bk, libxsmm_blasint threads, my_eltwise_fuse fuse_type) {
  my_fc_fwd_config res;
  libxsmm_blasint lda = bk;
  libxsmm_blasint ldb = bc;
  libxsmm_blasint ldc = bk;
  libxsmm_blasint ld_zero = bk*bn;
  libxsmm_blasint ld_upconvert = K;
  float alpha = 1.0f;
  float beta = 1.0f;
  float zerobeta = 0.0f;
  libxsmm_meltw_flags fusion_flags;
  int l_flags, l_tc_flags;
  int l_tr_flags = LIBXSMM_GEMM_FLAG_NO_SETUP_TILECONFIG | ( LIBXSMM_GEMM_VNNI_FLAGS('N', 'N', 'V', 'N') );
  libxsmm_blasint unroll_hint;

  /* setting up some handle values */
  res.N = N;
  res.C = C;
  res.K = K;
  res.bn = bn;
  res.bc = bc;
  res.bk = bk;
  res.threads = threads;
  res.fuse_type = fuse_type;

  /* setup parallelization strategy */
  res.fwd_model_hyperpartitions = 1;
  if (threads == 16) {
    res.fwd_bf = 1;
    res.fwd_2d_blocking = 1;
    res.fwd_row_teams = 2;
    res.fwd_column_teams = 8;
  } else if (threads == 14) {
    res.fwd_bf = 1;
    res.fwd_2d_blocking = 1;
    res.fwd_row_teams = 2;
    res.fwd_column_teams = 7;
  } else if (threads == 56) {
    res.fwd_bf = 1;
    res.fwd_2d_blocking = 1;
    res.fwd_row_teams = 4;
    res.fwd_column_teams = 14;
    res.fwd_model_hyperpartitions = 1;
  } else {
    res.fwd_bf = 1;
    res.fwd_2d_blocking = 0;
    res.fwd_row_teams = 1;
    res.fwd_column_teams = 1;
  }

#if 0
  res.fwd_bf = atoi(getenv("FWD_BF"));
  res.fwd_2d_blocking = atoi(getenv("FWD_2D_BLOCKING"));
  res.fwd_row_teams = atoi(getenv("FWD_ROW_TEAMS"));
  res.fwd_column_teams = atoi(getenv("FWD_COLUMN_TEAMS"));
#endif

  /* setting up the barrier */
  res.barrier = libxsmm_barrier_create(threads, 1);

  /* TPP creation */
  l_flags = ( LIBXSMM_GEMM_VNNI_FLAGS('N', 'N', 'V', 'N') ) | LIBXSMM_GEMM_FLAG_NO_RESET_TILECONFIG | LIBXSMM_GEMM_FLAG_NO_SETUP_TILECONFIG;
  l_tc_flags = LIBXSMM_GEMM_FLAG_NO_RESET_TILECONFIG | ( LIBXSMM_GEMM_VNNI_FLAGS('N', 'N', 'V', 'N') );
  unroll_hint = (res.C/res.bc)/res.fwd_bf;

  res.fwd_config_kernel = libxsmm_bsmmdispatch(res.bk, res.bn, res.bc, &lda, &ldb, &ldc, NULL, &beta, &l_tc_flags, NULL);
  if ( res.fwd_config_kernel == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP fwd_config_kernel failed. Bailing...!\n");
    exit(-1);
  }
  res.gemm_fwd = libxsmm_bsmmdispatch_reducebatch_strd_unroll(res.bk, res.bn, res.bc, res.bk*res.bc*sizeof(libxsmm_bfloat16), res.bc*res.bn*sizeof(libxsmm_bfloat16), unroll_hint, &lda, &ldb, &ldc, &alpha, &beta, &l_flags, NULL);
  if ( res.gemm_fwd == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_fwd failed. Bailing...!\n");
    exit(-1);
  }
  res.gemm_fwd2 = libxsmm_bsmmdispatch_reducebatch_strd_unroll(res.bk, res.bn, res.bc, res.bk*res.bc*sizeof(libxsmm_bfloat16), res.bc*res.bn*sizeof(libxsmm_bfloat16), unroll_hint, &lda, &ldb, &ldc, &alpha, &zerobeta, &l_flags, NULL);
  if ( res.gemm_fwd2 == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_fwd2 failed. Bailing...!\n");
    exit(-1);
  }
  res.gemm_fwd3 = libxsmm_bmmdispatch_reducebatch_strd_unroll(res.bk, res.bn, res.bc, res.bk*res.bc*sizeof(libxsmm_bfloat16), res.bc*res.bn*sizeof(libxsmm_bfloat16), unroll_hint, &lda, &ldb, &ldc, &alpha, &zerobeta, &l_flags, NULL);
  if ( res.gemm_fwd3 == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_fwd3 failed. Bailing...!\n");
    exit(-1);
  }
  fusion_flags = LIBXSMM_MELTW_FLAG_COLBIAS_OVERWRITE_C;
  res.gemm_fwd4 = libxsmm_bmmdispatch_reducebatch_strd_meltwfused_unroll(res.bk, res.bn, res.bc, res.bk*res.bc*sizeof(libxsmm_bfloat16), res.bc*res.bn*sizeof(libxsmm_bfloat16), unroll_hint, &lda, &ldb, &ldc, &alpha, &zerobeta, &l_flags, NULL, LIBXSMM_MELTW_OPERATION_COLBIAS_ACT, LIBXSMM_DATATYPE_F32, fusion_flags, 0, 0, 0, 0);
  if ( res.gemm_fwd4 == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_fwd4 failed. Bailing...!\n");
    exit(-1);
  }
  fusion_flags = LIBXSMM_MELTW_FLAG_ACT_RELU_OVERWRITE_C;
  res.gemm_fwd5 = libxsmm_bmmdispatch_reducebatch_strd_meltwfused_unroll(res.bk, res.bn, res.bc, res.bk*res.bc*sizeof(libxsmm_bfloat16), res.bc*res.bn*sizeof(libxsmm_bfloat16), unroll_hint, &lda, &ldb, &ldc, &alpha, &zerobeta, &l_flags, NULL, LIBXSMM_MELTW_OPERATION_COLBIAS_ACT, LIBXSMM_DATATYPE_F32, fusion_flags, 0, 0, 0, 0);
  if ( res.gemm_fwd5 == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_fwd5 failed. Bailing...!\n");
    exit(-1);
  }
  fusion_flags = LIBXSMM_MELTW_FLAG_ACT_SIGM_OVERWRITE_C;
  res.gemm_fwd6 = libxsmm_bmmdispatch_reducebatch_strd_meltwfused_unroll(res.bk, res.bn, res.bc, res.bk*res.bc*sizeof(libxsmm_bfloat16), res.bc*res.bn*sizeof(libxsmm_bfloat16), unroll_hint, &lda, &ldb, &ldc, &alpha, &zerobeta, &l_flags, NULL, LIBXSMM_MELTW_OPERATION_COLBIAS_ACT, LIBXSMM_DATATYPE_F32, fusion_flags, 0, 0, 0, 0);
  if ( res.gemm_fwd6 == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_fwd6 failed. Bailing...!\n");
    exit(-1);
  }
  fusion_flags = LIBXSMM_MELTW_FLAG_COLBIAS_ACT_RELU_OVERWRITE_C;
  res.gemm_fwd7 = libxsmm_bmmdispatch_reducebatch_strd_meltwfused_unroll(res.bk, res.bn, res.bc, res.bk*res.bc*sizeof(libxsmm_bfloat16), res.bc*res.bn*sizeof(libxsmm_bfloat16), unroll_hint, &lda, &ldb, &ldc, &alpha, &zerobeta, &l_flags, NULL, LIBXSMM_MELTW_OPERATION_COLBIAS_ACT, LIBXSMM_DATATYPE_F32, fusion_flags, 0, 0, 0, 0);
  if ( res.gemm_fwd7 == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_fwd7 failed. Bailing...!\n");
    exit(-1);
  }
  fusion_flags = LIBXSMM_MELTW_FLAG_COLBIAS_ACT_SIGM_OVERWRITE_C;
  res.gemm_fwd8 = libxsmm_bmmdispatch_reducebatch_strd_meltwfused_unroll(res.bk, res.bn, res.bc, res.bk*res.bc*sizeof(libxsmm_bfloat16), res.bc*res.bn*sizeof(libxsmm_bfloat16), unroll_hint, &lda, &ldb, &ldc, &alpha, &zerobeta, &l_flags, NULL, LIBXSMM_MELTW_OPERATION_COLBIAS_ACT, LIBXSMM_DATATYPE_F32, fusion_flags, 0, 0, 0, 0);
  if ( res.gemm_fwd8 == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_fwd8 failed. Bailing...!\n");
    exit(-1);
  }

  /* Also JIT eltwise TPPs... */
  res.fwd_cvtfp32bf16_kernel = libxsmm_dispatch_meltw_cvtfp32bf16(res.bk, res.bn, &ldc, &ldc, LIBXSMM_DATATYPE_F32, LIBXSMM_DATATYPE_BF16, LIBXSMM_MELTW_FLAG_CVT_NONE);
  if ( res.fwd_cvtfp32bf16_kernel == NULL ) {
    fprintf( stderr, "JIT for TPP fwd_cvtfp32bf16_kernel failed. Bailing...!\n");
    exit(-1);
  }
  res.fwd_cvtfp32bf16_relu_kernel = libxsmm_dispatch_meltw_cvtfp32bf16_act(res.bk, res.bn, &ldc, &ldc, LIBXSMM_DATATYPE_F32, LIBXSMM_DATATYPE_BF16, LIBXSMM_MELTW_FLAG_CVTA_FUSE_RELU, 0);
  if ( res.fwd_cvtfp32bf16_relu_kernel == NULL ) {
    fprintf( stderr, "JIT for TPP fwd_cvtfp32bf16_relu_kernel failed. Bailing...!\n");
    exit(-1);
  }
  res.fwd_sigmoid_cvtfp32bf16_kernel = libxsmm_dispatch_meltw_act_cvtfp32bf16(res.bk, res.bn, &ldc, &ldc, LIBXSMM_DATATYPE_F32, LIBXSMM_DATATYPE_BF16, LIBXSMM_MELTW_FLAG_ACVT_FUSE_SIGM, 0);
  if ( res.fwd_sigmoid_cvtfp32bf16_kernel == NULL ) {
    fprintf( stderr, "JIT for TPP fwd_sigmoid_cvtfp32bf16_kernel failed. Bailing...!\n");
    exit(-1);
  }
  res.tilerelease_kernel = libxsmm_bsmmdispatch(res.bk, res.bk, res.bk, NULL, NULL, NULL, NULL, NULL, &l_tr_flags, NULL);
  if ( res.tilerelease_kernel == NULL ) {
    fprintf( stderr, "JIT for TPP tilerelease_kernel failed. Bailing...!\n");
    exit(-1);
  }


  res.fwd_zero_kernel = libxsmm_dispatch_meltw_copy(bn*bk, 1, &ld_zero, &ld_zero, LIBXSMM_DATATYPE_F32, LIBXSMM_DATATYPE_F32, LIBXSMM_MELTW_FLAG_COPY_ZERO);
  if ( res.fwd_zero_kernel == NULL ) {
    fprintf( stderr, "JIT for TPP fwd_zero_kernel failed. Bailing...!\n");
    exit(-1);
  }

  res.fwd_colbcast_bf16fp32_copy_kernel = libxsmm_dispatch_meltw_copy(bk, bn, &ldc, &ldc, LIBXSMM_DATATYPE_BF16, LIBXSMM_DATATYPE_F32, LIBXSMM_MELTW_FLAG_COPY_COLBCAST);
  if ( res.fwd_colbcast_bf16fp32_copy_kernel == NULL ) {
    fprintf( stderr, "JIT for TPP fwd_colbcast_bf16fp32_copy_kernel failed. Bailing...!\n");
    exit(-1);
  }

  res.fwd_copy_bf16fp32_kernel = libxsmm_dispatch_meltw_copy(K, 1, &ld_upconvert, &ld_upconvert, LIBXSMM_DATATYPE_BF16, LIBXSMM_DATATYPE_F32, LIBXSMM_MELTW_FLAG_COPY_NONE);
  if ( res.fwd_copy_bf16fp32_kernel == NULL ) {
    fprintf( stderr, "JIT for TPP fwd_copy_bf16fp32_kernel failed. Bailing...!\n");
    exit(-1);
  }

  /* init scratch */
  res.scratch_size = sizeof(float) *  LIBXSMM_MAX(res.K * res.N, res.threads * LIBXSMM_MAX(res.bk * res.bn, res.K));

  return res;
}

my_fc_bwd_config setup_my_fc_bwd(libxsmm_blasint N, libxsmm_blasint C, libxsmm_blasint K, libxsmm_blasint bn,
                                 libxsmm_blasint bc, libxsmm_blasint bk, libxsmm_blasint threads, my_eltwise_fuse fuse_type) {
  my_fc_bwd_config res;
  const libxsmm_trans_descriptor* tr_desc = 0;
  libxsmm_descriptor_blob blob;
  libxsmm_blasint lda = bk;
  libxsmm_blasint ldb = bc;
  libxsmm_blasint ldc = bk;
  libxsmm_blasint ld_zero_bwd = bc*bn;
  libxsmm_blasint ld_zero_upd = bk;
  libxsmm_blasint delbias_K = K;
  libxsmm_blasint delbias_N = N;
  float alpha = 1.0f;
  float beta = 1.0f;
  float zerobeta = 0.0f;
  libxsmm_blasint updM;
  libxsmm_blasint updN;
  libxsmm_meltw_flags fusion_flags;
  int l_flags, l_tc_flags;
  int l_tr_flags = LIBXSMM_GEMM_FLAG_NO_SETUP_TILECONFIG | ( LIBXSMM_GEMM_VNNI_FLAGS('N', 'N', 'V', 'N') );
  libxsmm_blasint unroll_hint;
  size_t size_bwd_scratch;
  size_t size_upd_scratch;
  libxsmm_blasint bbk;
  libxsmm_blasint bbc;
  libxsmm_meltw_transform_flags trans_flags;
  libxsmm_blasint ldaT = bc;
  libxsmm_blasint ldb_orig= bc;

  /* setting up some handle values */
  res.N = N;
  res.C = C;
  res.K = K;
  res.bn = bn;
  res.bc = bc;
  res.bk = bk;
  res.threads = threads;
  res.fuse_type = fuse_type;

  /* setup parallelization strategy */
  res.bwd_model_hyperpartitions = 1;
  res.upd_model_hyperpartitions = 1;
  if (threads == 16) {
    res.bwd_bf = 1;
    res.bwd_2d_blocking = 1;
    res.bwd_row_teams = 2;
    res.bwd_column_teams = 8;
    res.upd_bf = 1;
    res.upd_2d_blocking = 1;
    res.upd_row_teams = 2;
    res.upd_column_teams = 8;
    res.ifm_subtasks = 1;
    res.ofm_subtasks = 1;
  } else if (threads == 14) {
    res.bwd_bf = 1;
    res.bwd_2d_blocking = 1;
    res.bwd_row_teams = 2;
    res.bwd_column_teams = 7;
    res.upd_bf = 1;
    res.upd_2d_blocking = 1;
    res.upd_row_teams = 2;
    res.upd_column_teams = 7;
    res.ifm_subtasks = 1;
    res.ofm_subtasks = 1;
  } else if (threads == 56) {
    res.bwd_bf = 1;
    res.bwd_2d_blocking = 1;
    res.bwd_row_teams = 4;
    res.bwd_column_teams = 14;
    res.bwd_model_hyperpartitions = 1;
    res.upd_bf = 1;
    res.upd_2d_blocking = 1;
    res.upd_row_teams = 4;
    res.upd_column_teams = 14;
    res.upd_model_hyperpartitions = 1;
    res.ifm_subtasks = 1;
    res.ofm_subtasks = 1;
  } else {
    res.bwd_bf = 1;
    res.bwd_2d_blocking = 0;
    res.bwd_row_teams = 1;
    res.bwd_column_teams = 1;
    res.upd_bf = 1;
    res.upd_2d_blocking = 0;
    res.upd_row_teams = 1;
    res.upd_column_teams = 1;
    res.ifm_subtasks = 1;
    res.ofm_subtasks = 1;
  }

  bbk = (res.upd_2d_blocking == 1) ? bk : bk/res.ofm_subtasks;
  bbc = (res.upd_2d_blocking == 1) ? bc : bc/res.ifm_subtasks;

#if 0
  res.bwd_bf = atoi(getenv("BWD_BF"));
  res.bwd_2d_blocking = atoi(getenv("BWD_2D_BLOCKING"));
  res.bwd_row_teams = atoi(getenv("BWD_ROW_TEAMS"));
  res.bwd_column_teams = atoi(getenv("BWD_COLUMN_TEAMS"));
  res.upd_bf = atoi(getenv("UPD_BF"));
  res.upd_2d_blocking = atoi(getenv("UPD_2D_BLOCKING"));
  res.upd_row_teams = atoi(getenv("UPD_ROW_TEAMS"));
  res.upd_column_teams = atoi(getenv("UPD_COLUMN_TEAMS"));
  res.ifm_subtasks = atoi(getenv("IFM_SUBTASKS"));
  res.ofm_subtasks = atoi(getenv("OFM_SUBTASKS"));
#endif

  /* setting up the barrier */
  res.barrier = libxsmm_barrier_create(threads, 1);

  /* TPP creation */
  /* BWD GEMM */
  l_flags = ( LIBXSMM_GEMM_VNNI_FLAGS('N', 'N', 'V', 'N') ) | LIBXSMM_GEMM_FLAG_NO_RESET_TILECONFIG | LIBXSMM_GEMM_FLAG_NO_SETUP_TILECONFIG;
  l_tc_flags = LIBXSMM_GEMM_FLAG_NO_RESET_TILECONFIG | ( LIBXSMM_GEMM_VNNI_FLAGS('N', 'N', 'V', 'N') );
  unroll_hint = (res.K/res.bk)/res.bwd_bf;

  res.gemm_bwd = libxsmm_bsmmdispatch_reducebatch_strd_unroll(res.bc, res.bn, res.bk, res.bk*res.bc*sizeof(libxsmm_bfloat16), res.bk*res.bn*sizeof(libxsmm_bfloat16), unroll_hint, &ldb, &lda, &ldb, &alpha, &beta, &l_flags, NULL);
  if ( res.gemm_bwd == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_bwd failed. Bailing...!\n");
    exit(-1);
  }
  res.gemm_bwd2 = libxsmm_bsmmdispatch_reducebatch_strd_unroll(res.bc, res.bn, res.bk, res.bk*res.bc*sizeof(libxsmm_bfloat16), res.bk*res.bn*sizeof(libxsmm_bfloat16), unroll_hint, &ldb, &lda, &ldb, &alpha, &zerobeta, &l_flags, NULL);
  if ( res.gemm_bwd2 == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_bwd2 failed. Bailing...!\n");
    exit(-1);
  }
  res.gemm_bwd3 = libxsmm_bmmdispatch_reducebatch_strd_unroll(res.bc, res.bn, res.bk, res.bk*res.bc*sizeof(libxsmm_bfloat16), res.bk*res.bn*sizeof(libxsmm_bfloat16), unroll_hint, &ldb, &lda, &ldb, &alpha, &zerobeta, &l_flags, NULL);
  if ( res.gemm_bwd3 == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_bwd3 failed. Bailing...!\n");
    exit(-1);
  }
  res.bwd_config_kernel = libxsmm_bsmmdispatch(res.bc, res.bn, res.bk, &ldb, &lda, &ldb, NULL, &beta, &l_tc_flags, NULL);
  if ( res.bwd_config_kernel == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP bwd_config_kernel failed. Bailing...!\n");
    exit(-1);
  }

  /* Also JIT eltwise TPPs... */
  res.bwd_cvtfp32bf16_kernel  = libxsmm_dispatch_meltw_cvtfp32bf16(res.bc, res.bn, &ldb, &ldb, LIBXSMM_DATATYPE_F32, LIBXSMM_DATATYPE_BF16, LIBXSMM_MELTW_FLAG_CVT_NONE);
  if ( res.bwd_cvtfp32bf16_kernel == NULL ) {
    fprintf( stderr, "JIT for TPP bwd_cvtfp32bf16_kernel failed. Bailing...!\n");
    exit(-1);
  }

  res.bwd_relu_kernel  = libxsmm_dispatch_meltw_relu(res.bc, res.bn, &ldb, &ldb, LIBXSMM_DATATYPE_BF16, LIBXSMM_DATATYPE_BF16, LIBXSMM_MELTW_FLAG_RELU_BWD, 0);
  if ( res.bwd_relu_kernel == NULL ) {
    fprintf( stderr, "JIT for TPP bwd_relu_kernel failed. Bailing...!\n");
    exit(-1);
  }

  res.bwd_zero_kernel = libxsmm_dispatch_meltw_copy(bn*bc, 1, &ld_zero_bwd, &ld_zero_bwd, LIBXSMM_DATATYPE_F32, LIBXSMM_DATATYPE_F32, LIBXSMM_MELTW_FLAG_COPY_ZERO);
  if ( res.bwd_zero_kernel == NULL ) {
    fprintf( stderr, "JIT for TPP bwd_zero_kernel failed. Bailing...!\n");
    exit(-1);
  }

  /* JITing the tranpose kernel */
  trans_flags = LIBXSMM_MELTW_FLAG_TRANSFORM_VNNI_TO_VNNIT;
  res.vnni_to_vnniT_kernel = libxsmm_dispatch_meltw_transform(bk, bc, &lda, &ldaT, LIBXSMM_DATATYPE_BF16, LIBXSMM_DATATYPE_BF16, trans_flags);
  if ( res.vnni_to_vnniT_kernel == NULL ) {
    fprintf( stderr, "JIT for TPP vnni_to_vnniT_kernel failed. Bailing...!\n");
    exit(-1);
  }

  /* UPD GEMM */
  lda = res.bk;
  ldb = res.bn;
  ldc = res.bk;
  updM = res.bk/res.ofm_subtasks;
  updN = res.bc/res.ifm_subtasks;

  l_flags = ( LIBXSMM_GEMM_VNNI_FLAGS('N', 'N', 'V', 'N') ) | LIBXSMM_GEMM_FLAG_NO_RESET_TILECONFIG | LIBXSMM_GEMM_FLAG_NO_SETUP_TILECONFIG;
  l_tc_flags = LIBXSMM_GEMM_FLAG_NO_RESET_TILECONFIG | ( LIBXSMM_GEMM_VNNI_FLAGS('N', 'N', 'V', 'N') );
  unroll_hint = (res.N/res.bn)/res.upd_bf;
  res.gemm_upd = libxsmm_bsmmdispatch_reducebatch_strd_unroll(updM, updN, res.bn, res.bk*res.bn*sizeof(libxsmm_bfloat16), res.bc*res.bn*sizeof(libxsmm_bfloat16), unroll_hint, &lda, &ldb, &ldc, &alpha, &beta, &l_flags, NULL);
  if ( res.gemm_upd == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_upd failed. Bailing...!\n");
    exit(-1);
  }
  res.gemm_upd2 = libxsmm_bsmmdispatch_reducebatch_strd_unroll(updM, updN, res.bn, res.bk*res.bn*sizeof(libxsmm_bfloat16), res.bc*res.bn*sizeof(libxsmm_bfloat16), unroll_hint, &lda, &ldb, &ldc, &alpha, &zerobeta, &l_flags, NULL);
  if ( res.gemm_upd2 == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_upd2 failed. Bailing...!\n");
    exit(-1);
  }
  l_flags = l_flags | LIBXSMM_GEMM_FLAG_VNNI_C;
  res.gemm_upd3 = libxsmm_bmmdispatch_reducebatch_strd_unroll(updM, updN, res.bn, res.bk*res.bn*sizeof(libxsmm_bfloat16), res.bc*res.bn*sizeof(libxsmm_bfloat16), unroll_hint, &lda, &ldb, &ldc, &alpha, &zerobeta, &l_flags, NULL);
  if ( res.gemm_upd3 == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_upd3 failed. Bailing...!\n");
    exit(-1);
  }
  res.upd_config_kernel = libxsmm_bsmmdispatch(updM, updN, res.bn, &lda, &ldb, &ldc, NULL, &beta, &l_tc_flags, NULL);
  if ( res.upd_config_kernel == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP upd_config_kernel failed. Bailing...!\n");
    exit(-1);
  }

  res.tilerelease_kernel = libxsmm_bsmmdispatch(res.bk, res.bk, res.bk, NULL, NULL, NULL, NULL, NULL, &l_tr_flags, NULL);
  if ( res.tilerelease_kernel == NULL ) {
    fprintf( stderr, "JIT for TPP tilerelease_kernel failed. Bailing...!\n");
    exit(-1);
  }

  /* Also JIT eltwise TPPs... */
  res.upd_cvtfp32bf16_kernel  = libxsmm_dispatch_meltw_cvtfp32bf16(bbk, bbc, &ldc, &ldc, LIBXSMM_DATATYPE_F32, LIBXSMM_DATATYPE_BF16, LIBXSMM_MELTW_FLAG_CVT_VNNI_FORMAT);
  if ( res.upd_cvtfp32bf16_kernel == NULL ) {
    fprintf( stderr, "JIT for TPP upd_cvtfp32bf16_kernel failed. Bailing...!\n");
    exit(-1);
  }

  res.upd_zero_kernel = libxsmm_dispatch_meltw_copy(bbk, bbc, &ld_zero_upd, &ld_zero_upd, LIBXSMM_DATATYPE_F32, LIBXSMM_DATATYPE_F32, LIBXSMM_MELTW_FLAG_COPY_ZERO);
  if ( res.upd_zero_kernel == NULL ) {
    fprintf( stderr, "JIT for TPP upd_zero_kernel failed. Bailing...!\n");
    exit(-1);
  }

  res.delbias_reduce_kernel = libxsmm_dispatch_meltw_reduce(bk, bn, &delbias_K, &delbias_N, LIBXSMM_DATATYPE_BF16, LIBXSMM_DATATYPE_BF16, LIBXSMM_MELTW_FLAG_REDUCE_OP_ADD | LIBXSMM_MELTW_FLAG_REDUCE_COLS | LIBXSMM_MELTW_FLAG_REDUCE_ELTS | LIBXSMM_MELTW_FLAG_REDUCE_NCNC_FORMAT, 0);
  if ( res.delbias_reduce_kernel == NULL ) {
    fprintf( stderr, "JIT for TPP delbias_reduce_kernel failed. Bailing...!\n");
    exit(-1);
  }

  /* JITing the tranpose kernels */
  trans_flags = LIBXSMM_MELTW_FLAG_TRANSFORM_NORM_TO_VNNI;
  res.norm_to_vnni_kernel = libxsmm_dispatch_meltw_transform(bk, bn, &lda, &lda, LIBXSMM_DATATYPE_BF16, LIBXSMM_DATATYPE_BF16, trans_flags);
  if ( res.norm_to_vnni_kernel == NULL ) {
    fprintf( stderr, "JIT for TPP norm_to_vnni_kernel failed. Bailing...!\n");
    exit(-1);
  }

  trans_flags = LIBXSMM_MELTW_FLAG_TRANSFORM_NORM_TO_NORMT;
  res.norm_to_normT_kernel = libxsmm_dispatch_meltw_transform(bc, bn, &ldb, &ldb_orig, LIBXSMM_DATATYPE_BF16, LIBXSMM_DATATYPE_BF16, trans_flags);
  if ( res.norm_to_normT_kernel == NULL ) {
    fprintf( stderr, "JIT for TPP norm_to_normT_kernel failed. Bailing...!\n");
    exit(-1);
  }

  /* init scratch */
  size_bwd_scratch = sizeof(float) * LIBXSMM_MAX(res.C * res.N, res.threads * res.bc * res.bn) + sizeof(libxsmm_bfloat16) * res.C * res.K;
  size_upd_scratch = sizeof(float) * LIBXSMM_MAX(res.C * res.K, res.threads * res.bc * res.bk) + sizeof(libxsmm_bfloat16) * res.threads * res.bk * res.bc + sizeof(libxsmm_bfloat16) * (res.N * (res.C + res.K));
#ifdef OVERWRITE_DOUTPUT_BWDUPD
  res.scratch_size = LIBXSMM_MAX(size_bwd_scratch, size_upd_scratch) + sizeof(libxsmm_bfloat16) * res.N * res.K;
#else
  res.scratch_size = LIBXSMM_MAX(size_bwd_scratch, size_upd_scratch) + 2 * sizeof(libxsmm_bfloat16) * res.N * res.K;
#endif
  res.doutput_scratch_mark = LIBXSMM_MAX(size_bwd_scratch, size_upd_scratch) ;

  return res;
}

my_opt_config setup_my_opt(libxsmm_blasint C, libxsmm_blasint K, libxsmm_blasint bc, libxsmm_blasint bk,
                           libxsmm_blasint threads, float lr) {
  my_opt_config res;

  /* setting up some handle values */
  res.C = C;
  res.K = K;
  res.bc = bc;
  res.bk = bk;
  res.threads = threads;
  res.lr = lr;

  /* setting up the barrier */
  res.barrier = libxsmm_barrier_create(threads, 1);

  /* init scratch */
  res.scratch_size = 0;

  return res;
}

my_smax_fwd_config setup_my_smax_fwd(libxsmm_blasint N, libxsmm_blasint C, libxsmm_blasint bn, libxsmm_blasint bc,
                                     libxsmm_blasint threads) {
  my_smax_fwd_config res;

  /* setting up some handle values */
  res.C = C;
  res.N = N;
  res.bc = bc;
  res.bn = bn;
  res.threads = threads;

  /* setting up the barrier */
  res.barrier = libxsmm_barrier_create(threads, 1);

  /* init scratch */
  res.scratch_size = (sizeof(float)*res.C*res.N*2);;

  return res;
}

my_smax_bwd_config setup_my_smax_bwd(libxsmm_blasint N, libxsmm_blasint C, libxsmm_blasint bn, libxsmm_blasint bc,
                                     libxsmm_blasint threads, float loss_weight) {
  my_smax_bwd_config res;

  /* setting up some handle values */
  res.C = C;
  res.N = N;
  res.bc = bc;
  res.bn = bn;
  res.threads = threads;
  res.loss_weight = loss_weight;

  /* setting up the barrier */
  res.barrier = libxsmm_barrier_create(threads, 1);

  /* init scratch */
  res.scratch_size = (sizeof(float)*res.C*res.N*2);;

  return res;
}

void my_fc_fwd_exec( my_fc_fwd_config cfg, const libxsmm_bfloat16* wt_ptr, const libxsmm_bfloat16* in_act_ptr, libxsmm_bfloat16* out_act_ptr,
                     const libxsmm_bfloat16* bias_ptr, unsigned char* relu_ptr, int start_tid, int my_tid, void* scratch ) {
  const libxsmm_blasint nBlocksIFm = cfg.C / cfg.bc;
  const libxsmm_blasint nBlocksOFm = cfg.K / cfg.bk;
  const libxsmm_blasint nBlocksMB  = cfg.N / cfg.bn;
  const libxsmm_blasint bn = cfg.bn;
  const libxsmm_blasint bk = cfg.bk;
  const libxsmm_blasint lpb = 2;
  const libxsmm_blasint bc_lp = cfg.bc/lpb;
  /* const libxsmm_blasint bc = cfg.bc;*/
  libxsmm_blasint use_2d_blocking = cfg.fwd_2d_blocking;

  /* computing first logical thread */
  const libxsmm_blasint ltid = my_tid - start_tid;
  /* number of tasks that could be run in parallel */
  const libxsmm_blasint work = nBlocksOFm * nBlocksMB;
  /* compute chunk size */
  const libxsmm_blasint chunksize = (work % cfg.threads == 0) ? (work / cfg.threads) : ((work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint thr_begin = (ltid * chunksize < work) ? (ltid * chunksize) : work;
  const libxsmm_blasint thr_end = ((ltid + 1) * chunksize < work) ? ((ltid + 1) * chunksize) : work;

  /* loop variables */
  libxsmm_blasint mb1ofm1 = 0, mb1 = 0, ofm1 = 0, ifm1 = 0;
  libxsmm_blasint im_tasks_per_thread = 0, in_tasks_per_thread = 0, my_in_start = 0, my_in_end = 0, my_im_start = 0, my_im_end = 0, my_row_id = 0, my_col_id = 0, row_teams = 0, column_teams = 0;
  LIBXSMM_VLA_DECL(4, libxsmm_bfloat16,       output,  out_act_ptr, nBlocksOFm, cfg.bn, cfg.bk);
  LIBXSMM_VLA_DECL(4, const libxsmm_bfloat16,  input,   in_act_ptr,  nBlocksIFm, cfg.bn, cfg.bc);
  LIBXSMM_VLA_DECL(5, const libxsmm_bfloat16, filter,       wt_ptr, nBlocksIFm, bc_lp, cfg.bk, lpb);
  LIBXSMM_VLA_DECL(4, float, output_f32, (float*)scratch, nBlocksOFm, bn, bk);
  libxsmm_meltw_gemm_param gemm_eltwise_params;
  libxsmm_blasint mb2 = 0;
  float* fp32_bias_scratch =  ((cfg.fuse_type & MY_ELTWISE_FUSE_BIAS) == MY_ELTWISE_FUSE_BIAS) ? (float*)scratch + ltid * cfg.K : NULL;
  LIBXSMM_VLA_DECL(2, const libxsmm_bfloat16, bias, ((cfg.fuse_type & MY_ELTWISE_FUSE_BIAS) == MY_ELTWISE_FUSE_BIAS) ? (libxsmm_bfloat16*) bias_ptr : NULL, cfg.bk);
  LIBXSMM_VLA_DECL(4, __mmask32,  relubitmask, ((cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU) ? (__mmask32*)relu_ptr : NULL, nBlocksOFm, cfg.bn, cfg.bk/32);
  libxsmm_meltwfunction_cvtfp32bf16_act eltwise_kernel_act = cfg.fwd_cvtfp32bf16_relu_kernel;
  libxsmm_meltw_cvtfp32bf16_act_param   eltwise_params_act;
  libxsmm_meltwfunction_cvtfp32bf16     eltwise_kernel = cfg.fwd_cvtfp32bf16_kernel;
  libxsmm_meltw_cvtfp32bf16_param       eltwise_params;
  libxsmm_bmmfunction_reducebatch_strd_meltwfused bf16_batchreduce_kernel_zerobeta_fused_eltwise;
  libxsmm_meltw_copy_param              copy_params;

  unsigned long long  blocks = nBlocksIFm;
  libxsmm_blasint CB_BLOCKS = nBlocksIFm, BF = 1;

  if (((cfg.fuse_type & MY_ELTWISE_FUSE_BIAS) == MY_ELTWISE_FUSE_BIAS) && ((cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU )) {
    bf16_batchreduce_kernel_zerobeta_fused_eltwise = cfg.gemm_fwd7;
  } else if ((cfg.fuse_type & MY_ELTWISE_FUSE_BIAS) == MY_ELTWISE_FUSE_BIAS) {
    bf16_batchreduce_kernel_zerobeta_fused_eltwise = cfg.gemm_fwd4;
  } else if ((cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU) {
    bf16_batchreduce_kernel_zerobeta_fused_eltwise = cfg.gemm_fwd5;
  }

  BF = cfg.fwd_bf;
  CB_BLOCKS = nBlocksIFm/BF;
  blocks = CB_BLOCKS;

  if (use_2d_blocking == 1) {
    int _ltid, hyperpartition_id, _nBlocksOFm;
    row_teams = cfg.fwd_row_teams;
    column_teams = cfg.fwd_column_teams;
    _nBlocksOFm = nBlocksOFm/cfg.fwd_model_hyperpartitions;
    _ltid = ltid % (row_teams * column_teams);
    hyperpartition_id = ltid / (row_teams * column_teams);
    my_col_id = _ltid % column_teams;
    my_row_id = _ltid / column_teams;
    im_tasks_per_thread = (nBlocksMB + row_teams-1)/row_teams;
    in_tasks_per_thread = (_nBlocksOFm + column_teams-1)/column_teams;
    my_im_start = LIBXSMM_MIN( my_row_id * im_tasks_per_thread, nBlocksMB);
    my_im_end = LIBXSMM_MIN( (my_row_id+1) * im_tasks_per_thread, nBlocksMB);
    my_in_start = hyperpartition_id * _nBlocksOFm + LIBXSMM_MIN( my_col_id * in_tasks_per_thread, _nBlocksOFm);
    my_in_end = hyperpartition_id * _nBlocksOFm + LIBXSMM_MIN( (my_col_id+1) * in_tasks_per_thread, _nBlocksOFm);
  }

  /* lazy barrier init */
  libxsmm_barrier_init(cfg.barrier, ltid);

  cfg.fwd_config_kernel(NULL, NULL, NULL);

  if (use_2d_blocking == 1) {
    if (BF > 1) {
      for ( ifm1 = 0; ifm1 < BF; ++ifm1 ) {
        for (ofm1 = my_in_start; ofm1 < my_in_end; ++ofm1) {
          for (mb1 = my_im_start; mb1 < my_im_end; ++mb1) {
            if ( ifm1 == 0 ) {
              if ( (cfg.fuse_type & MY_ELTWISE_FUSE_BIAS) == MY_ELTWISE_FUSE_BIAS ) {
                copy_params.in_ptr  = &LIBXSMM_VLA_ACCESS(2, bias, ofm1, 0,cfg.bk);
                copy_params.out_ptr = &LIBXSMM_VLA_ACCESS(4, output_f32, mb1, ofm1, 0, 0, nBlocksOFm,cfg.bn,cfg.bk);
                cfg.fwd_colbcast_bf16fp32_copy_kernel(&copy_params);
              } else {
                copy_params.out_ptr = &LIBXSMM_VLA_ACCESS(4, output_f32, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk);
                cfg.fwd_zero_kernel(&copy_params);
              }
            }

            cfg.gemm_fwd( &LIBXSMM_VLA_ACCESS(5, filter, ofm1, ifm1*CB_BLOCKS, 0, 0, 0, nBlocksIFm, bc_lp, cfg.bk, lpb),
                &LIBXSMM_VLA_ACCESS(4, input,  mb1, ifm1*CB_BLOCKS, 0, 0, nBlocksIFm, cfg.bn, cfg.bc),
                &LIBXSMM_VLA_ACCESS(4, output_f32, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk), &blocks);

            if ( ifm1 == BF-1  ) {
              if ( (cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU ) {
                eltwise_params_act.in_ptr = &LIBXSMM_VLA_ACCESS(4, output_f32, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk);
                eltwise_params_act.out_ptr = &LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk);
                eltwise_params_act.actstore_ptr = &LIBXSMM_VLA_ACCESS(4, relubitmask, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk/32);
                eltwise_kernel_act(&eltwise_params_act);
              } else {
                eltwise_params.in_ptr = &LIBXSMM_VLA_ACCESS(4, output_f32, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk);
                eltwise_params.out_ptr = &LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk);
                eltwise_kernel(&eltwise_params);
              }
            }
          }
        }
      }
    } else {
      if ( (cfg.fuse_type & MY_ELTWISE_FUSE_BIAS) == MY_ELTWISE_FUSE_BIAS ) {
        copy_params.in_ptr  = &LIBXSMM_VLA_ACCESS(2, bias, 0, 0,cfg.bk);
        copy_params.out_ptr = fp32_bias_scratch;
        cfg.fwd_copy_bf16fp32_kernel(&copy_params);
      }
      for (ofm1 = my_in_start; ofm1 < my_in_end; ++ofm1) {
        for (mb1 = my_im_start; mb1 < my_im_end; ++mb1) {
          if ( ((cfg.fuse_type & MY_ELTWISE_FUSE_BIAS) == MY_ELTWISE_FUSE_BIAS) || ((cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU )) {
            if ((cfg.fuse_type & MY_ELTWISE_FUSE_BIAS) == MY_ELTWISE_FUSE_BIAS) {
              gemm_eltwise_params.bias_ptr  = (float*) fp32_bias_scratch + ofm1 * cfg.bk;
            }
            if ((cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU) {
              gemm_eltwise_params.out_ptr   = &LIBXSMM_VLA_ACCESS(4, relubitmask, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk/32);
            }
            bf16_batchreduce_kernel_zerobeta_fused_eltwise( &LIBXSMM_VLA_ACCESS(5, filter, ofm1, 0, 0, 0, 0, nBlocksIFm, bc_lp, cfg.bk, lpb),
              &LIBXSMM_VLA_ACCESS(4, input,  mb1, 0,  0, 0, nBlocksIFm, cfg.bn, cfg.bc),
              &LIBXSMM_VLA_ACCESS(4, output, mb1,  ofm1, 0, 0, nBlocksOFm, bn, bk), &blocks, &gemm_eltwise_params);
          } else {
            cfg.gemm_fwd3( &LIBXSMM_VLA_ACCESS(5, filter, ofm1, 0, 0, 0, 0, nBlocksIFm, bc_lp, cfg.bk, lpb),
              &LIBXSMM_VLA_ACCESS(4, input,  mb1, 0,  0, 0, nBlocksIFm, cfg.bn, cfg.bc),
              &LIBXSMM_VLA_ACCESS(4, output, mb1,  ofm1, 0, 0, nBlocksOFm, bn, bk), &blocks);
          }
        }
      }
    }
  } else {
    if (BF > 1) {
      for ( ifm1 = 0; ifm1 < BF; ++ifm1 ) {
        for ( mb1ofm1 = thr_begin; mb1ofm1 < thr_end; ++mb1ofm1 ) {
          mb1  = mb1ofm1%nBlocksMB;
          ofm1 = mb1ofm1/nBlocksMB;
          /* Initialize libxsmm_blasintermediate f32 tensor */
          if ( ifm1 == 0 ) {
            if ( (cfg.fuse_type & MY_ELTWISE_FUSE_BIAS) == MY_ELTWISE_FUSE_BIAS ) {
              copy_params.in_ptr  = &LIBXSMM_VLA_ACCESS(2, bias, ofm1, 0,cfg.bk);
              copy_params.out_ptr = &LIBXSMM_VLA_ACCESS(4, output_f32, mb1, ofm1, 0, 0, nBlocksOFm,cfg.bn,cfg.bk);
              cfg.fwd_colbcast_bf16fp32_copy_kernel(&copy_params);
            } else {
              copy_params.out_ptr = &LIBXSMM_VLA_ACCESS(4, output_f32, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk);
              cfg.fwd_zero_kernel(&copy_params);
            }
          }
          cfg.gemm_fwd( &LIBXSMM_VLA_ACCESS(5, filter, ofm1, ifm1*CB_BLOCKS, 0, 0, 0, nBlocksIFm, bc_lp, cfg.bk, lpb),
              &LIBXSMM_VLA_ACCESS(4, input,  mb1, ifm1*CB_BLOCKS, 0, 0, nBlocksIFm, cfg.bn, cfg.bc),
              &LIBXSMM_VLA_ACCESS(4, output_f32, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk), &blocks);

          if ( ifm1 == BF-1  ) {
            if ( (cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU ) {
              eltwise_params_act.in_ptr = &LIBXSMM_VLA_ACCESS(4, output_f32, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk);
              eltwise_params_act.out_ptr = &LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk);
              eltwise_params_act.actstore_ptr = &LIBXSMM_VLA_ACCESS(4, relubitmask, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk/32);
              eltwise_kernel_act(&eltwise_params_act);
            } else {
              eltwise_params.in_ptr = &LIBXSMM_VLA_ACCESS(4, output_f32, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk);
              eltwise_params.out_ptr = &LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk);
              eltwise_kernel(&eltwise_params);
            }
          }
        }
      }
    } else {
      if ( (cfg.fuse_type & MY_ELTWISE_FUSE_BIAS) == MY_ELTWISE_FUSE_BIAS ) {
        copy_params.in_ptr  = &LIBXSMM_VLA_ACCESS(2, bias, 0, 0,cfg.bk);
        copy_params.out_ptr = fp32_bias_scratch;
        cfg.fwd_copy_bf16fp32_kernel(&copy_params);
      }
      for ( mb1ofm1 = thr_begin; mb1ofm1 < thr_end; ++mb1ofm1 ) {
        mb1  = mb1ofm1%nBlocksMB;
        ofm1 = mb1ofm1/nBlocksMB;
        if ( ((cfg.fuse_type & MY_ELTWISE_FUSE_BIAS) == MY_ELTWISE_FUSE_BIAS) || ((cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU )) {
          if ((cfg.fuse_type & MY_ELTWISE_FUSE_BIAS) == MY_ELTWISE_FUSE_BIAS) {
            gemm_eltwise_params.bias_ptr  = (float*) fp32_bias_scratch + ofm1 * cfg.bk;
          }
          if ((cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU) {
            gemm_eltwise_params.out_ptr   = &LIBXSMM_VLA_ACCESS(4, relubitmask, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk/32);
          }
          bf16_batchreduce_kernel_zerobeta_fused_eltwise( &LIBXSMM_VLA_ACCESS(5, filter, ofm1, 0, 0, 0, 0, nBlocksIFm, bc_lp, cfg.bk, lpb),
            &LIBXSMM_VLA_ACCESS(4, input,  mb1, 0,  0, 0, nBlocksIFm, cfg.bn, cfg.bc),
            &LIBXSMM_VLA_ACCESS(4, output, mb1,  ofm1, 0, 0, nBlocksOFm, bn, bk), &blocks, &gemm_eltwise_params);
        } else {
          cfg.gemm_fwd3( &LIBXSMM_VLA_ACCESS(5, filter, ofm1, 0, 0, 0, 0, nBlocksIFm, bc_lp, cfg.bk, lpb),
            &LIBXSMM_VLA_ACCESS(4, input,  mb1, 0,  0, 0, nBlocksIFm, cfg.bn, cfg.bc),
            &LIBXSMM_VLA_ACCESS(4, output, mb1,  ofm1, 0, 0, nBlocksOFm, bn, bk), &blocks);
        }
      }
    }
  }

  cfg.tilerelease_kernel(NULL, NULL, NULL);
  libxsmm_barrier_wait(cfg.barrier, ltid);
}

void my_fc_bwd_exec( my_fc_bwd_config cfg, const libxsmm_bfloat16* wt_ptr, libxsmm_bfloat16* din_act_ptr,
                     const libxsmm_bfloat16* dout_act_ptr, libxsmm_bfloat16* dwt_ptr, const libxsmm_bfloat16* in_act_ptr,
                     libxsmm_bfloat16* dbias_ptr, const unsigned char* relu_ptr, my_pass pass, int start_tid, int my_tid, void* scratch ) {
  /* size variables, all const */
  /* here we assume that input and output blocking is similar */
  const libxsmm_blasint bn = cfg.bn;
  const libxsmm_blasint bk = cfg.bk;
  const libxsmm_blasint bc = cfg.bc;
  libxsmm_blasint lpb = 2;
  const libxsmm_blasint bc_lp = bc/lpb;
  const libxsmm_blasint bk_lp = bk/lpb;
  const libxsmm_blasint bn_lp = bn/lpb;
  const libxsmm_blasint nBlocksIFm = cfg.C / cfg.bc;
  const libxsmm_blasint nBlocksOFm = cfg.K / cfg.bk;
  const libxsmm_blasint nBlocksMB  = cfg.N / cfg.bn;
  libxsmm_blasint mb1ofm1 = 0, mb1 = 0, ofm1 = 0, mb2 = 0, ofm2 = 0;
  libxsmm_blasint iteri = 0, iterj = 0;
  libxsmm_blasint performed_doutput_transpose = 0;
  libxsmm_meltw_transform_param trans_param;

  /* computing first logical thread */
  const libxsmm_blasint ltid = my_tid - start_tid;

  /* number of tasks for transpose that could be run in parallel */
  const libxsmm_blasint eltwise_work = nBlocksOFm * nBlocksMB;
  /* compute chunk size */
  const libxsmm_blasint eltwise_chunksize = (eltwise_work % cfg.threads == 0) ? (eltwise_work / cfg.threads) : ((eltwise_work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint eltwise_thr_begin = (ltid * eltwise_chunksize < eltwise_work) ? (ltid * eltwise_chunksize) : eltwise_work;
  const libxsmm_blasint eltwise_thr_end = ((ltid + 1) * eltwise_chunksize < eltwise_work) ? ((ltid + 1) * eltwise_chunksize) : eltwise_work;

  /* number of tasks for transpose that could be run in parallel */
  const libxsmm_blasint dbias_work = nBlocksOFm;
  /* compute chunk size */
  const libxsmm_blasint dbias_chunksize = (dbias_work % cfg.threads == 0) ? (dbias_work / cfg.threads) : ((dbias_work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint dbias_thr_begin = (ltid * dbias_chunksize < dbias_work) ? (ltid * dbias_chunksize) : dbias_work;
  const libxsmm_blasint dbias_thr_end = ((ltid + 1) * dbias_chunksize < dbias_work) ? ((ltid + 1) * dbias_chunksize) : dbias_work;

  LIBXSMM_VLA_DECL(2, libxsmm_bfloat16, dbias, ((cfg.fuse_type & MY_ELTWISE_FUSE_BIAS) == MY_ELTWISE_FUSE_BIAS) ? (libxsmm_bfloat16*) dbias_ptr : NULL, cfg.bk);
  LIBXSMM_VLA_DECL(4,     __mmask32, relubitmask, ((cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU) ? (__mmask32*)relu_ptr : NULL, nBlocksOFm, cfg.bn, cfg.bk/32);

#ifdef OVERWRITE_DOUTPUT_BWDUPD
  libxsmm_bfloat16 *grad_output_ptr = (libxsmm_bfloat16*)dout_act_ptr;
  libxsmm_bfloat16 *tr_doutput_ptr = (((cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU)) ? (libxsmm_bfloat16*)((char*)scratch + cfg.doutput_scratch_mark) : (libxsmm_bfloat16*)scratch;
#else
  libxsmm_bfloat16 *grad_output_ptr = (((cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU)) ? (libxsmm_bfloat16*)((char*)scratch + cfg.doutput_scratch_mark) : (libxsmm_bfloat16*)dout_act_ptr;
  libxsmm_bfloat16 *tr_doutput_ptr = (((cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU)) ? (libxsmm_bfloat16*)grad_output_ptr + cfg.N * cfg.K : (libxsmm_bfloat16*)scratch;
#endif
  LIBXSMM_VLA_DECL(4, const libxsmm_bfloat16,   doutput_orig, (libxsmm_bfloat16*)dout_act_ptr, nBlocksOFm, bn, bk);
  libxsmm_meltw_relu_param   relu_params;
  libxsmm_meltwfunction_relu relu_kernel = cfg.bwd_relu_kernel;
  LIBXSMM_VLA_DECL(4, libxsmm_bfloat16,   doutput, grad_output_ptr, nBlocksOFm, bn, bk);
  LIBXSMM_VLA_DECL(5, libxsmm_bfloat16, doutput_tr, tr_doutput_ptr, nBlocksMB, bn_lp, bk, lpb);

  libxsmm_meltwfunction_cvtfp32bf16 eltwise_kernel  = cfg.bwd_cvtfp32bf16_kernel;
  libxsmm_meltwfunction_cvtfp32bf16 eltwise_kernel2 = cfg.upd_cvtfp32bf16_kernel;
  libxsmm_meltw_cvtfp32bf16_param   eltwise_params;
  libxsmm_meltw_copy_param          copy_params;
  libxsmm_meltw_reduce_param        delbias_params;

  /* lazy barrier init */
  libxsmm_barrier_init(cfg.barrier, ltid);
  cfg.bwd_config_kernel(NULL, NULL, NULL);

  /* Apply to doutput potential fusions */
  if (((cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU)) {
    for ( mb1ofm1 = eltwise_thr_begin; mb1ofm1 < eltwise_thr_end; ++mb1ofm1 ) {
      mb1  = mb1ofm1/nBlocksOFm;
      ofm1 = mb1ofm1%nBlocksOFm;

      relu_params.in_ptr   = &LIBXSMM_VLA_ACCESS(4, doutput_orig, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk);
      relu_params.out_ptr  = &LIBXSMM_VLA_ACCESS(4, doutput, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk);
      relu_params.mask_ptr = &LIBXSMM_VLA_ACCESS(4, relubitmask, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk/32);
      relu_kernel(&relu_params);

      /* If in UPD pass, also perform transpose of doutput  */
      if ( (pass & MY_PASS_BWD_W) == MY_PASS_BWD_W ) {
        trans_param.in_ptr  = &LIBXSMM_VLA_ACCESS(4, doutput,  mb1, ofm1, 0, 0, nBlocksOFm, bn, bk);
        trans_param.out_ptr = &LIBXSMM_VLA_ACCESS(5, doutput_tr, ofm1, mb1, 0, 0, 0, nBlocksMB, bn_lp, bk, lpb);
        cfg.norm_to_vnni_kernel(&trans_param);
      }
    }

    if ( (pass & MY_PASS_BWD_W) == MY_PASS_BWD_W ) {
      performed_doutput_transpose = 1;
    }
    libxsmm_barrier_wait(cfg.barrier, ltid);
  }

  /* Accumulation of bias happens in f32 */
  if (((cfg.fuse_type & MY_ELTWISE_FUSE_BIAS) == MY_ELTWISE_FUSE_BIAS)) {
    for ( ofm1 = dbias_thr_begin; ofm1 < dbias_thr_end; ++ofm1 ) {
      delbias_params.in_ptr     = &LIBXSMM_VLA_ACCESS(4,  doutput, 0, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk);
      delbias_params.out_ptr_0  = &LIBXSMM_VLA_ACCESS(2,  dbias, ofm1, 0, cfg.bk);
      cfg.delbias_reduce_kernel(&delbias_params);
    }
    /* wait for eltwise to finish */
    libxsmm_barrier_wait(cfg.barrier, ltid);
  }

  if ( (pass & MY_PASS_BWD_D) == MY_PASS_BWD_D ){
    libxsmm_blasint use_2d_blocking = cfg.bwd_2d_blocking;

    /* number of tasks that could be run in parallel */
    const libxsmm_blasint work = nBlocksIFm * nBlocksMB;
    /* compute chunk size */
    const libxsmm_blasint chunksize = (work % cfg.threads == 0) ? (work / cfg.threads) : ((work / cfg.threads) + 1);
    /* compute thr_begin and thr_end */
    const libxsmm_blasint thr_begin = (ltid * chunksize < work) ? (ltid * chunksize) : work;
    const libxsmm_blasint thr_end = ((ltid + 1) * chunksize < work) ? ((ltid + 1) * chunksize) : work;

    /* number of tasks for transpose that could be run in parallel */
    const libxsmm_blasint transpose_work = nBlocksIFm * nBlocksOFm;
    /* compute chunk size */
    const libxsmm_blasint transpose_chunksize = (transpose_work % cfg.threads == 0) ? (transpose_work / cfg.threads) : ((transpose_work / cfg.threads) + 1);
    /* compute thr_begin and thr_end */
    const libxsmm_blasint transpose_thr_begin = (ltid * transpose_chunksize < transpose_work) ? (ltid * transpose_chunksize) : transpose_work;
    const libxsmm_blasint transpose_thr_end = ((ltid + 1) * transpose_chunksize < transpose_work) ? ((ltid + 1) * transpose_chunksize) : transpose_work;

    /* loop variables */
    libxsmm_blasint ifm1 = 0, ifm2 = 0, ifm1ofm1 = 0, mb1ifm1 = 0;
    libxsmm_blasint im_tasks_per_thread = 0, in_tasks_per_thread = 0, my_in_start = 0, my_in_end = 0, my_im_start = 0, my_im_end = 0, my_row_id = 0, my_col_id = 0, row_teams = 0, column_teams = 0;

    LIBXSMM_VLA_DECL(5, const libxsmm_bfloat16, filter, (libxsmm_bfloat16*)wt_ptr, nBlocksIFm, bc_lp, bk, lpb);
    LIBXSMM_VLA_DECL(4,        libxsmm_bfloat16,    dinput, (libxsmm_bfloat16* )din_act_ptr, nBlocksIFm, bn, bc);
    LIBXSMM_VLA_DECL(5,       libxsmm_bfloat16, filter_tr, (libxsmm_bfloat16*)scratch, nBlocksOFm, bk_lp, bc, lpb);
    float* temp_output = (float*)scratch + (cfg.C * cfg.K)/2;
    LIBXSMM_VLA_DECL(4,        float,    dinput_f32, (float*) temp_output, nBlocksIFm, bn, bc);

    unsigned long long  blocks = nBlocksOFm;
    libxsmm_blasint KB_BLOCKS = nBlocksOFm, BF = 1;
    BF = cfg.bwd_bf;
    KB_BLOCKS = nBlocksOFm/BF;
    blocks = KB_BLOCKS;

    if (use_2d_blocking == 1) {
      int _ltid, hyperpartition_id, _nBlocksIFm;
      row_teams = cfg.bwd_row_teams;
      column_teams = cfg.bwd_column_teams;
      _nBlocksIFm = nBlocksIFm/cfg.bwd_model_hyperpartitions;
      _ltid = ltid % (row_teams * column_teams);
      hyperpartition_id = ltid / (row_teams * column_teams);
      my_col_id = _ltid % column_teams;
      my_row_id = _ltid / column_teams;
      im_tasks_per_thread = (nBlocksMB + row_teams-1)/row_teams;
      in_tasks_per_thread = (_nBlocksIFm + column_teams-1)/column_teams;
      my_im_start = LIBXSMM_MIN( my_row_id * im_tasks_per_thread, nBlocksMB);
      my_im_end = LIBXSMM_MIN( (my_row_id+1) * im_tasks_per_thread, nBlocksMB);
      my_in_start = hyperpartition_id * _nBlocksIFm + LIBXSMM_MIN( my_col_id * in_tasks_per_thread, _nBlocksIFm);
      my_in_end = hyperpartition_id * _nBlocksIFm + LIBXSMM_MIN( (my_col_id+1) * in_tasks_per_thread, _nBlocksIFm);
    }

    /* transpose weight */
    for (ifm1ofm1 = transpose_thr_begin; ifm1ofm1 < transpose_thr_end; ++ifm1ofm1) {
      ofm1 = ifm1ofm1 / nBlocksIFm;
      ifm1 = ifm1ofm1 % nBlocksIFm;
      trans_param.in_ptr  = &LIBXSMM_VLA_ACCESS(5, filter,  ofm1, ifm1, 0, 0, 0, nBlocksIFm, bc_lp, bk, lpb);
      trans_param.out_ptr = &LIBXSMM_VLA_ACCESS(5, filter_tr, ifm1, ofm1, 0, 0, 0, nBlocksOFm, bk_lp, bc, lpb);
      cfg.vnni_to_vnniT_kernel(&trans_param);
    }

    /* wait for transpose to finish */
    libxsmm_barrier_wait(cfg.barrier, ltid);

    if (use_2d_blocking == 1) {
      if (BF > 1) {
        for ( ofm1 = 0; ofm1 < BF; ++ofm1 ) {
          for (ifm1 = my_in_start; ifm1 < my_in_end; ++ifm1) {
            for (mb1 = my_im_start; mb1 < my_im_end; ++mb1) {
              /* Initialize libxsmm_blasintermediate f32 tensor */
              if ( ofm1 == 0 ) {
                copy_params.out_ptr = &LIBXSMM_VLA_ACCESS(4, dinput_f32, mb1, ifm1, 0, 0, nBlocksIFm, bn, bc);
                cfg.bwd_zero_kernel(&copy_params);
              }
              cfg.gemm_bwd( &LIBXSMM_VLA_ACCESS(5, filter_tr, ifm1, ofm1*KB_BLOCKS, 0, 0, 0, nBlocksOFm, bk_lp, bc, lpb),
                  &LIBXSMM_VLA_ACCESS(4, doutput,   mb1,  ofm1*KB_BLOCKS, 0, 0, nBlocksOFm, bn, bk),
                  &LIBXSMM_VLA_ACCESS(4, dinput_f32,    mb1,  ifm1, 0, 0, nBlocksIFm, bn, bc), &blocks);
              /* downconvert libxsmm_blasintermediate f32 tensor to bf 16 and store to final C */
              if ( ofm1 == BF-1  ) {
                eltwise_params.in_ptr = &LIBXSMM_VLA_ACCESS(4, dinput_f32,    mb1,  ifm1, 0, 0, nBlocksIFm, bn, bc);
                eltwise_params.out_ptr = &LIBXSMM_VLA_ACCESS(4, dinput,    mb1,  ifm1, 0, 0, nBlocksIFm, bn, bc);
                eltwise_kernel(&eltwise_params);
              }
            }
          }
        }
      } else {
        for (ifm1 = my_in_start; ifm1 < my_in_end; ++ifm1) {
          for (mb1 = my_im_start; mb1 < my_im_end; ++mb1) {
            cfg.gemm_bwd3( &LIBXSMM_VLA_ACCESS(5, filter_tr, ifm1, 0, 0, 0, 0, nBlocksOFm, bk_lp, bc, lpb),
                &LIBXSMM_VLA_ACCESS(4, doutput,   mb1,  0, 0, 0, nBlocksOFm, bn, bk),
                &LIBXSMM_VLA_ACCESS(4, dinput,    mb1,  ifm1, 0, 0, nBlocksIFm, bn, bc), &blocks);
          }
        }
      }
    } else {
      if (BF > 1) {
        for ( ofm1 = 0; ofm1 < BF; ++ofm1 ) {
          for ( mb1ifm1 = thr_begin; mb1ifm1 < thr_end; ++mb1ifm1 ) {
            mb1  = mb1ifm1%nBlocksMB;
            ifm1 = mb1ifm1/nBlocksMB;
            /* Initialize libxsmm_blasintermediate f32 tensor */
            if ( ofm1 == 0 ) {
              copy_params.out_ptr = &LIBXSMM_VLA_ACCESS(4, dinput_f32, mb1, ifm1, 0, 0, nBlocksIFm, bn, bc);
              cfg.bwd_zero_kernel(&copy_params);
            }
            cfg.gemm_bwd( &LIBXSMM_VLA_ACCESS(5, filter_tr, ifm1, ofm1*KB_BLOCKS, 0, 0, 0, nBlocksOFm, bk_lp, bc, lpb),
                &LIBXSMM_VLA_ACCESS(4, doutput,   mb1,  ofm1*KB_BLOCKS, 0, 0, nBlocksOFm, bn, bk),
                &LIBXSMM_VLA_ACCESS(4, dinput_f32,    mb1,  ifm1, 0, 0, nBlocksIFm, bn, bc), &blocks);
            /* downconvert libxsmm_blasintermediate f32 tensor to bf 16 and store to final C */
            if ( ofm1 == BF-1  ) {
                eltwise_params.in_ptr = &LIBXSMM_VLA_ACCESS(4, dinput_f32,    mb1,  ifm1, 0, 0, nBlocksIFm, bn, bc);
                eltwise_params.out_ptr = &LIBXSMM_VLA_ACCESS(4, dinput,    mb1,  ifm1, 0, 0, nBlocksIFm, bn, bc);
                eltwise_kernel(&eltwise_params);
            }
          }
        }
      } else {
        for ( mb1ifm1 = thr_begin; mb1ifm1 < thr_end; ++mb1ifm1 ) {
          mb1  = mb1ifm1%nBlocksMB;
          ifm1 = mb1ifm1/nBlocksMB;
          cfg.gemm_bwd3( &LIBXSMM_VLA_ACCESS(5, filter_tr, ifm1, 0, 0, 0, 0, nBlocksOFm, bk_lp, bc, lpb),
              &LIBXSMM_VLA_ACCESS(4, doutput,   mb1,  0, 0, 0, nBlocksOFm, bn, bk),
              &LIBXSMM_VLA_ACCESS(4, dinput,    mb1,  ifm1, 0, 0, nBlocksIFm, bn, bc), &blocks);
        }
      }
    }

    libxsmm_barrier_wait(cfg.barrier, ltid);
  }

  if ( (pass & MY_PASS_BWD_W) == MY_PASS_BWD_W ) {
    /* number of tasks that could be run in parallel */
    const libxsmm_blasint ofm_subtasks = (cfg.upd_2d_blocking == 1) ? 1 : cfg.ofm_subtasks;
    const libxsmm_blasint ifm_subtasks = (cfg.upd_2d_blocking == 1) ? 1 : cfg.ifm_subtasks;
    const libxsmm_blasint bbk = (cfg.upd_2d_blocking == 1) ? bk : bk/ofm_subtasks;
    const libxsmm_blasint bbc = (cfg.upd_2d_blocking == 1) ? bc : bc/ifm_subtasks;
    const libxsmm_blasint work = nBlocksIFm * ifm_subtasks * nBlocksOFm * ofm_subtasks;
    const libxsmm_blasint Cck_work = nBlocksIFm * ifm_subtasks * ofm_subtasks;
    const libxsmm_blasint Cc_work = nBlocksIFm * ifm_subtasks;

    /* 2D blocking parameters  */
    libxsmm_blasint use_2d_blocking = cfg.upd_2d_blocking;
    libxsmm_blasint im_tasks_per_thread = 0, in_tasks_per_thread = 0, my_in_start = 0, my_in_end = 0, my_im_start = 0, my_im_end = 0, my_row_id = 0, my_col_id = 0, row_teams = 0, column_teams = 0;

    /* compute chunk size */
    const libxsmm_blasint chunksize = (work % cfg.threads == 0) ? (work / cfg.threads) : ((work / cfg.threads) + 1);
    /* compute thr_begin and thr_end */
    const libxsmm_blasint thr_begin = (ltid * chunksize < work) ? (ltid * chunksize) : work;
    const libxsmm_blasint thr_end = ((ltid + 1) * chunksize < work) ? ((ltid + 1) * chunksize) : work;
    libxsmm_blasint BF = cfg.upd_bf;

    /* loop variables */
    libxsmm_blasint ifm1ofm1 = 0, ifm1 = 0, ifm2 = 0, bfn = 0, ii = 0, jj = 0, mb1ifm1 = 0, jc = 0, jk = 0;

    /* Batch reduce related variables */
    unsigned long long  blocks = nBlocksMB/BF;

    LIBXSMM_VLA_DECL(4, const libxsmm_bfloat16,  input,    (libxsmm_bfloat16* )in_act_ptr, nBlocksIFm, bn, bc);
    LIBXSMM_VLA_DECL(5,       libxsmm_bfloat16, dfilter,  (libxsmm_bfloat16*)dwt_ptr, nBlocksIFm, bc_lp, bk, lpb);

    /* Set up tensors for transposing/scratch before vnni reformatting dfilter */
    libxsmm_bfloat16  *tr_inp_ptr = (libxsmm_bfloat16*) ((libxsmm_bfloat16*)scratch + cfg.N * cfg.K);
    float               *dfilter_f32_ptr = (float*) ((libxsmm_bfloat16*)tr_inp_ptr + cfg.N * cfg.C);
    libxsmm_bfloat16 *dfilter_scratch = (libxsmm_bfloat16*) ((float*)dfilter_f32_ptr + cfg.C * cfg.K) + ltid * bc * bk;

    LIBXSMM_VLA_DECL(4, libxsmm_bfloat16,  input_tr,    (libxsmm_bfloat16*)tr_inp_ptr, nBlocksMB, bc, bn);
    LIBXSMM_VLA_DECL(4,       float, dfilter_f32,  (float*)dfilter_f32_ptr, nBlocksIFm, bc, bk);
    LIBXSMM_VLA_DECL(2, libxsmm_bfloat16, dfilter_block,  (libxsmm_bfloat16*)dfilter_scratch, bk);

    const libxsmm_blasint tr_out_work = nBlocksMB * nBlocksOFm;
    const libxsmm_blasint tr_out_chunksize = (tr_out_work % cfg.threads == 0) ? (tr_out_work / cfg.threads) : ((tr_out_work / cfg.threads) + 1);
    const libxsmm_blasint tr_out_thr_begin = (ltid * tr_out_chunksize < tr_out_work) ? (ltid * tr_out_chunksize) : tr_out_work;
    const libxsmm_blasint tr_out_thr_end = ((ltid + 1) * tr_out_chunksize < tr_out_work) ? ((ltid + 1) * tr_out_chunksize) : tr_out_work;

    const libxsmm_blasint tr_inp_work = nBlocksMB * nBlocksIFm;
    const libxsmm_blasint tr_inp_chunksize = (tr_inp_work % cfg.threads == 0) ? (tr_inp_work / cfg.threads) : ((tr_inp_work / cfg.threads) + 1);
    const libxsmm_blasint tr_inp_thr_begin = (ltid * tr_inp_chunksize < tr_inp_work) ? (ltid * tr_inp_chunksize) : tr_inp_work;
    const libxsmm_blasint tr_inp_thr_end = ((ltid + 1) * tr_inp_chunksize < tr_inp_work) ? ((ltid + 1) * tr_inp_chunksize) : tr_inp_work;

    /* These are used for the vnni reformatting of the f32 output  */
    __m512 a01, b01;
    __m512i c01 = LIBXSMM_INTRINSICS_MM512_UNDEFINED_EPI32();
    const __m512i perm_index = LIBXSMM_INTRINSICS_MM512_SET_EPI16(31, 15, 30, 14, 29, 13, 28, 12, 27, 11, 26, 10, 25, 9, 24, 8, 23, 7, 22, 6, 21, 5, 20, 4, 19, 3, 18, 2, 17, 1, 16, 0);

    if (use_2d_blocking == 1) {
      int _ltid, hyperpartition_id, _nBlocksOFm;
      row_teams = cfg.upd_row_teams;
      column_teams = cfg.upd_column_teams;
      _nBlocksOFm = nBlocksOFm/cfg.upd_model_hyperpartitions;
      _ltid = ltid % (row_teams * column_teams);
      hyperpartition_id = ltid / (row_teams * column_teams);
      my_col_id = _ltid % column_teams;
      my_row_id = _ltid / column_teams;
      im_tasks_per_thread = (nBlocksIFm + row_teams-1)/row_teams;
      in_tasks_per_thread = (_nBlocksOFm + column_teams-1)/column_teams;
      my_im_start = LIBXSMM_MIN( my_row_id * im_tasks_per_thread, nBlocksIFm);
      my_im_end = LIBXSMM_MIN( (my_row_id+1) * im_tasks_per_thread, nBlocksIFm);
      my_in_start = hyperpartition_id * _nBlocksOFm + LIBXSMM_MIN( my_col_id * in_tasks_per_thread, _nBlocksOFm);
      my_in_end = hyperpartition_id * _nBlocksOFm + LIBXSMM_MIN( (my_col_id+1) * in_tasks_per_thread, _nBlocksOFm);
    }

    /* Required upfront tranposes */
    for (mb1ifm1 = tr_inp_thr_begin; mb1ifm1 < tr_inp_thr_end; mb1ifm1++) {
      mb1 = mb1ifm1%nBlocksMB;
      ifm1 = mb1ifm1/nBlocksMB;
      trans_param.in_ptr  = &LIBXSMM_VLA_ACCESS(4, input, mb1, ifm1, 0, 0, nBlocksIFm, bn, bc);
      trans_param.out_ptr = &LIBXSMM_VLA_ACCESS(4, input_tr, ifm1, mb1, 0, 0, nBlocksMB, bc, bn);
      cfg.norm_to_normT_kernel(&trans_param);
    }

    if (performed_doutput_transpose == 0) {
      for (mb1ofm1 = tr_out_thr_begin; mb1ofm1 < tr_out_thr_end; mb1ofm1++) {
        mb1 = mb1ofm1%nBlocksMB;
        ofm1 = mb1ofm1/nBlocksMB;
        trans_param.in_ptr  = &LIBXSMM_VLA_ACCESS(4, doutput,  mb1, ofm1, 0, 0, nBlocksOFm, bn, bk);
        trans_param.out_ptr = &LIBXSMM_VLA_ACCESS(5, doutput_tr, ofm1, mb1, 0, 0, 0, nBlocksMB, bn_lp, bk, lpb);
        cfg.norm_to_vnni_kernel(&trans_param);
      }
    }

    libxsmm_barrier_wait(cfg.barrier, ltid);

    if (use_2d_blocking == 1) {
      ifm2 = 0;
      ofm2 = 0;
      if (BF == 1) {
        for (ofm1 = my_in_start; ofm1 < my_in_end; ++ofm1) {
          for (ifm1 = my_im_start; ifm1 < my_im_end; ++ifm1) {
            cfg.gemm_upd3(&LIBXSMM_VLA_ACCESS(5, doutput_tr, ofm1, 0, 0, ofm2*bbk, 0, nBlocksMB, bn_lp, bk, lpb), &LIBXSMM_VLA_ACCESS(4, input_tr, ifm1, 0, ifm2*bbc, 0, nBlocksMB, bc, bn), &LIBXSMM_VLA_ACCESS(5, dfilter, ofm1, ifm1, 0, 0, 0, nBlocksIFm, bc_lp, bk, lpb), &blocks);
          }
        }
      } else {
        for (bfn = 0; bfn < BF; bfn++) {
          for (ofm1 = my_in_start; ofm1 < my_in_end; ++ofm1) {
            for (ifm1 = my_im_start; ifm1 < my_im_end; ++ifm1) {
              /* initialize current work task to zero */
              if (bfn == 0) {
                copy_params.out_ptr = &LIBXSMM_VLA_ACCESS(4, dfilter_f32, ofm1, ifm1, ifm2*bbc, ofm2*bbk, nBlocksIFm, bc, bk);
                cfg.upd_zero_kernel(&copy_params);
              }
              cfg.gemm_upd(&LIBXSMM_VLA_ACCESS(5, doutput_tr, ofm1, bfn*blocks, 0, ofm2*bbk, 0, nBlocksMB, bn_lp, bk, lpb), &LIBXSMM_VLA_ACCESS(4, input_tr, ifm1, bfn*blocks, ifm2*bbc, 0, nBlocksMB, bc, bn), &LIBXSMM_VLA_ACCESS(4, dfilter_f32, ofm1, ifm1, ifm2*bbc, ofm2*bbk, nBlocksIFm, bc, bk), &blocks);
              /* Downconvert result to BF16 and vnni format */
              if (bfn == BF-1) {
                eltwise_params.in_ptr = &LIBXSMM_VLA_ACCESS(4, dfilter_f32, ofm1, ifm1, 0, 0, nBlocksIFm, bc, bk);
                eltwise_params.out_ptr = &LIBXSMM_VLA_ACCESS(5, dfilter, ofm1, ifm1, 0, 0, 0, nBlocksIFm, bc_lp, bk, lpb);
                eltwise_kernel2(&eltwise_params);
              }
            }
          }
        }
      }
    } else {
      if (BF == 1) {
        for ( ifm1ofm1 = thr_begin; ifm1ofm1 < thr_end; ++ifm1ofm1 ) {
          ofm1 = ifm1ofm1 / Cck_work;
          ofm2 = (ifm1ofm1 % Cck_work) / Cc_work;
          ifm1 = ((ifm1ofm1 % Cck_work) % Cc_work) / ifm_subtasks;
          ifm2 = ((ifm1ofm1 % Cck_work) % Cc_work) % ifm_subtasks;
          cfg.gemm_upd3(&LIBXSMM_VLA_ACCESS(5, doutput_tr, ofm1, 0, 0, ofm2*bbk, 0, nBlocksMB, bn_lp, bk, lpb), &LIBXSMM_VLA_ACCESS(4, input_tr, ifm1, 0, ifm2*bbc, 0, nBlocksMB, bc, bn), &LIBXSMM_VLA_ACCESS(5, dfilter, ofm1, ifm1, (ifm2*bbc)/lpb, ofm2*bbk, 0, nBlocksIFm, bc_lp, bk, lpb), &blocks);
        }
      } else {
        for (bfn = 0; bfn < BF; bfn++) {
          for ( ifm1ofm1 = thr_begin; ifm1ofm1 < thr_end; ++ifm1ofm1 ) {
            ofm1 = ifm1ofm1 / Cck_work;
            ofm2 = (ifm1ofm1 % Cck_work) / Cc_work;
            ifm1 = ((ifm1ofm1 % Cck_work) % Cc_work) / ifm_subtasks;
            ifm2 = ((ifm1ofm1 % Cck_work) % Cc_work) % ifm_subtasks;
            /* initialize current work task to zero */
            if (bfn == 0) {
              copy_params.out_ptr = &LIBXSMM_VLA_ACCESS(4, dfilter_f32, ofm1, ifm1, ifm2*bbc, ofm2*bbk, nBlocksIFm, bc, bk);
              cfg.upd_zero_kernel(&copy_params);
            }
            cfg.gemm_upd(&LIBXSMM_VLA_ACCESS(5, doutput_tr, ofm1, bfn*blocks, 0, ofm2*bbk, 0, nBlocksMB, bn_lp, bk, lpb), &LIBXSMM_VLA_ACCESS(4, input_tr, ifm1, bfn*blocks, ifm2*bbc, 0, nBlocksMB, bc, bn), &LIBXSMM_VLA_ACCESS(4, dfilter_f32, ofm1, ifm1, ifm2*bbc, ofm2*bbk, nBlocksIFm, bc, bk), &blocks);
            /* Downconvert result to BF16 and vnni format */
            if (bfn == BF-1) {
              eltwise_params.in_ptr = &LIBXSMM_VLA_ACCESS(4, dfilter_f32, ofm1, ifm1, ifm2*bbc, ofm2*bbk, nBlocksIFm, bc, bk);
              eltwise_params.out_ptr = &LIBXSMM_VLA_ACCESS(5, dfilter, ofm1, ifm1, (ifm2*bbc)/lpb, ofm2*bbk, 0, nBlocksIFm, bc_lp, bk, lpb);
              eltwise_kernel2(&eltwise_params);
            }
          }
        }
      }
    }
    libxsmm_barrier_wait(cfg.barrier, ltid);
  }
  cfg.tilerelease_kernel(NULL, NULL, NULL);
}

void my_opt_exec( my_opt_config cfg, libxsmm_bfloat16* wt_ptr, float* master_wt_ptr, const libxsmm_bfloat16* delwt_ptr, int start_tid, int my_tid, void* scratch ) {
  /* loop counters */
  libxsmm_blasint i;

  /* computing first logical thread */
  const libxsmm_blasint ltid = my_tid - start_tid;

  /* number of tasks that could run in parallel for the filters */
  const libxsmm_blasint work = cfg.C * cfg.K;
  /* compute chunk size */
  const libxsmm_blasint chunksize = (work % cfg.threads == 0) ? (work / cfg.threads) : ((work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint thr_begin = (ltid * chunksize < work) ? (ltid * chunksize) : work;
  const libxsmm_blasint thr_end = ((ltid + 1) * chunksize < work) ? ((ltid + 1) * chunksize) : work;

  /* lazy barrier init */
  libxsmm_barrier_init( cfg.barrier, ltid );

#if defined(__AVX512BW__)
  libxsmm_blasint iv = ( (thr_end-thr_begin)/16 ) * 16; /* compute iterations which are vectorizable */
  __m512 vlr = _mm512_set1_ps( cfg.lr );
  for ( i = thr_begin; i < thr_begin+iv; i+=16 ) {
    __m512 newfilter = _mm512_sub_ps( _mm512_loadu_ps( master_wt_ptr+i ), _mm512_mul_ps( vlr, _mm512_load_fil( delwt_ptr + i ) ) );
    _mm512_store_fil( wt_ptr+i, newfilter );
    _mm512_storeu_ps( master_wt_ptr+i, newfilter );
  }
  for ( i = thr_begin+iv; i < thr_end; ++i ) {
    libxsmm_bfloat16_hp t1, t2;
    t1.i[0] =0;
    t1.i[1] = delwt_ptr[i];
    master_wt_ptr[i] = master_wt_ptr[i] - (cfg.lr*t1.f);
    t2.f = master_wt_ptr[i];
    wt_ptr[i] = t2.i[1];
  }
#else
  for ( i = thr_begin; i < thr_end; ++i ) {
    libxsmm_bfloat16_hp t1, t2;
    t1.i[0] =0;
    t1.i[1] = delwt_ptr[i];
    master_wt_ptr[i] = master_wt_ptr[i] - (cfg.lr*t1.f);
    t2.f = master_wt_ptr[i];
    wt_ptr[i] = t2.i[1];
  }
#endif

  libxsmm_barrier_wait( cfg.barrier, ltid );
}

void my_smax_fwd_exec( my_smax_fwd_config cfg, const libxsmm_bfloat16* in_act_ptr, libxsmm_bfloat16* out_act_ptr, const int* label_ptr, float* loss, int start_tid, int my_tid, void* scratch ) {
  libxsmm_blasint bn = cfg.bn;
  libxsmm_blasint Bn = cfg.N/cfg.bn;
  libxsmm_blasint bc = cfg.bc;
  libxsmm_blasint Bc = cfg.C/cfg.bc;

  /* loop counters */
  libxsmm_blasint i = 0;
  libxsmm_blasint img1, img2, ifm1, ifm2;

  /* computing first logical thread */
  const libxsmm_blasint ltid = my_tid - start_tid;

  /* number of tasks that could run in parallel for the batch */
  const libxsmm_blasint n_work = Bn * bn;
  /* compute chunk size */
  const libxsmm_blasint n_chunksize = (n_work % cfg.threads == 0) ? (n_work / cfg.threads) : ((n_work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint n_thr_begin = (ltid * n_chunksize < n_work) ? (ltid * n_chunksize) : n_work;
  const libxsmm_blasint n_thr_end = ((ltid + 1) * n_chunksize < n_work) ? ((ltid + 1) * n_chunksize) : n_work;

  /* number of tasks that could run in parallel for the batch */
  const libxsmm_blasint nc_work = Bn * bn;
  /* compute chunk size */
  const libxsmm_blasint nc_chunksize = (nc_work % cfg.threads == 0) ? (nc_work / cfg.threads) : ((nc_work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint nc_thr_begin = (ltid * nc_chunksize < nc_work) ? (ltid * nc_chunksize) : nc_work;
  const libxsmm_blasint nc_thr_end = ((ltid + 1) * nc_chunksize < nc_work) ? ((ltid + 1) * nc_chunksize) : nc_work;

  libxsmm_bfloat16* poutput_bf16 = out_act_ptr;
  const libxsmm_bfloat16* pinput_bf16  = in_act_ptr;
  float*            poutput_fp32 = (float*)scratch;
  float*            pinput_fp32  = ((float*)scratch)+(cfg.N*cfg.C);
  LIBXSMM_VLA_DECL(4,       float, output, poutput_fp32, Bc, bn, bc);
  LIBXSMM_VLA_DECL(4, const float,  input, pinput_fp32,  Bc, bn, bc);
  LIBXSMM_VLA_DECL(2,   const int,  label,   label_ptr,         bn);

  /* lazy barrier init */
  libxsmm_barrier_init( cfg.barrier, ltid );

#if defined(__AVX512BW__)
  LIBXSMM_DNN_CONVERT_BUFFER_BF16_F32(pinput_bf16+nc_thr_begin, pinput_fp32+nc_thr_begin, nc_thr_end-nc_thr_begin);
#else
  for ( i = nc_thr_begin; i < nc_thr_end; ++i ) {
    libxsmm_bfloat16_hp in;
    in.i[0] = 0;
    in.i[1] = pinput_bf16[i];
    pinput_fp32[i] = in.f;
  }
#endif

  libxsmm_barrier_wait( cfg.barrier, ltid );

  for ( i = n_thr_begin; i < n_thr_end; ++i ) {
    float max =        FLT_MIN;
    float sum_of_exp = 0.0f;

    img1 = i/bn;
    img2 = i%bn;

    /* set output to input and set compute max per image */
    for ( ifm1 = 0; ifm1 < Bc; ++ifm1 ) {
      for ( ifm2 = 0; ifm2 < bc; ++ifm2 ) {
        LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1, img2, ifm2, Bc, bn, bc ) = LIBXSMM_VLA_ACCESS( 4, input, img1, ifm1, img2, ifm2, Bc, bn, bc );
        if ( LIBXSMM_VLA_ACCESS( 4, input, img1, ifm1, img2, ifm2, Bc, bn, bc ) > max ) {
          max = LIBXSMM_VLA_ACCESS( 4, input, img1, ifm1, img2, ifm2, Bc, bn, bc );
        }
      }
    }

    /* sum exp over outputs */
    for ( ifm1 = 0; ifm1 < Bc; ++ifm1 ) {
      for ( ifm2 = 0; ifm2 < bc; ++ifm2 ) {
        LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1, img2, ifm2, Bc, bn, bc ) = (float)exp( (double)(LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1, img2, ifm2, Bc, bn, bc ) - max) );
        sum_of_exp += LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1, img2, ifm2, Bc, bn, bc );
      }
    }

    /* scale output */
    sum_of_exp = 1.0f/sum_of_exp;
    for ( ifm1 = 0; ifm1 < Bc; ++ifm1 ) {
      for ( ifm2 = 0; ifm2 < bc; ++ifm2 ) {
        LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1, img2, ifm2, Bc, bn, bc ) = LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1, img2, ifm2, Bc, bn, bc ) * sum_of_exp;
      }
    }
  }

  libxsmm_barrier_wait( cfg.barrier, ltid );

  /* calculate loss single threaded */
  if ( ltid == 0 ) {
    (*loss) = 0.0f;
    for ( img1 = 0; img1 < Bn; ++img1 ) {
      for ( img2 = 0; img2 <bn; ++img2 ) {
        libxsmm_blasint ifm = (libxsmm_blasint)LIBXSMM_VLA_ACCESS( 2, label, img1, img2, bn );
        libxsmm_blasint ifm1b = ifm/bc;
        libxsmm_blasint ifm2b = ifm%bc;
        float val = ( LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1b, img2, ifm2b, Bc, bn, bc ) > FLT_MIN ) ? LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1b, img2, ifm2b, Bc, bn, bc ) : FLT_MIN;
        *loss = LIBXSMM_LOGF( val );
      }
    }
    *loss = ((-1.0f)*(*loss))/cfg.N;
  }

  libxsmm_barrier_wait( cfg.barrier, ltid );

#if defined(__AVX512BW__)
  LIBXSMM_DNN_CONVERT_BUFFER_F32_BF16(poutput_fp32+nc_thr_begin, poutput_bf16+nc_thr_begin, nc_thr_end-nc_thr_begin);
#else
  for ( i = nc_thr_begin; i < nc_thr_end; ++i ) {
    libxsmm_bfloat16_hp in;
    in.f = poutput_fp32[i];
    poutput_bf16[i] = in.i[1];
  }
#endif
  libxsmm_barrier_wait( cfg.barrier, ltid );
}

void my_smax_bwd_exec( my_smax_bwd_config cfg, libxsmm_bfloat16* delin_act_ptr, const libxsmm_bfloat16* out_act_ptr, const int* label_ptr, int start_tid, int my_tid, void* scratch ) {
  libxsmm_blasint bn = cfg.bn;
  libxsmm_blasint Bn = cfg.N/cfg.bn;
  libxsmm_blasint bc = cfg.bc;
  libxsmm_blasint Bc = cfg.C/cfg.bc;

  /* loop counters */
  libxsmm_blasint i = 0;
  libxsmm_blasint img1, img2, ifm1, ifm2;

  float rcp_N = 1.0f/cfg.N;

  /* computing first logical thread */
  const libxsmm_blasint ltid = my_tid - start_tid;
  /* number of tasks that could run in parallel for the batch */
  const libxsmm_blasint n_work = Bn * bn;
  /* compute chunk size */
  const libxsmm_blasint n_chunksize = (n_work % cfg.threads == 0) ? (n_work / cfg.threads) : ((n_work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint n_thr_begin = (ltid * n_chunksize < n_work) ? (ltid * n_chunksize) : n_work;
  const libxsmm_blasint n_thr_end = ((ltid + 1) * n_chunksize < n_work) ? ((ltid + 1) * n_chunksize) : n_work;

  /* number of tasks that could run in parallel for the batch */
  const int nc_work = Bn * bn;
  /* compute chunk size */
  const int nc_chunksize = (nc_work % cfg.threads == 0) ? (nc_work / cfg.threads) : ((nc_work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const int nc_thr_begin = (ltid * nc_chunksize < nc_work) ? (ltid * nc_chunksize) : nc_work;
  const int nc_thr_end = ((ltid + 1) * nc_chunksize < nc_work) ? ((ltid + 1) * nc_chunksize) : nc_work;

  const libxsmm_bfloat16* poutput_bf16 = out_act_ptr;
  libxsmm_bfloat16* pdinput_bf16 = delin_act_ptr;
  float*            poutput_fp32 = (float*)scratch;
  float*            pdinput_fp32 = ((float*)scratch)+(cfg.N*cfg.C);
  LIBXSMM_VLA_DECL(4, const float, output, poutput_fp32, Bc, bn, bc);
  LIBXSMM_VLA_DECL(4,       float, dinput, pdinput_fp32, Bc, bn, bc);
  LIBXSMM_VLA_DECL(2,   const int,  label,     label_ptr,          bn);

  /* lazy barrier init */
  libxsmm_barrier_init( cfg.barrier, ltid );

#if defined(__AVX512BW__)
  LIBXSMM_DNN_CONVERT_BUFFER_BF16_F32(poutput_bf16+nc_thr_begin, poutput_fp32+nc_thr_begin, nc_thr_end-nc_thr_begin);
#else
  for ( i = nc_thr_begin; i < nc_thr_end; ++i ) {
    libxsmm_bfloat16_hp out;
    out.i[0] = 0;
    out.i[1] = poutput_bf16[i];
    poutput_fp32[i] = out.f;
  }
#endif

  libxsmm_barrier_wait( cfg.barrier, ltid );

  for ( i = n_thr_begin; i < n_thr_end; ++i ) {
    img1 = i/bn;
    img2 = i%bn;

    /* set output to input and set compute max per image */
    for ( ifm1 = 0; ifm1 < Bc; ++ifm1 ) {
      for ( ifm2 = 0; ifm2 < bc; ++ifm2 ) {
        if ( (ifm1*Bc)+ifm2 == (libxsmm_blasint)LIBXSMM_VLA_ACCESS( 2, label, img1, img2, bn ) ) {
          LIBXSMM_VLA_ACCESS( 4, dinput, img1, ifm1, img2, ifm2, Bc, bn, bc ) =
            ( LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1, img2, ifm2, Bc, bn, bc ) - 1.0f ) * rcp_N * cfg.loss_weight;
        } else {
          LIBXSMM_VLA_ACCESS( 4, dinput, img1, ifm1, img2, ifm2, Bc, bn, bc ) =
            LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1, img2, ifm2, Bc, bn, bc ) * rcp_N * cfg.loss_weight;
        }
      }
    }
  }

  libxsmm_barrier_wait( cfg.barrier, ltid );

#if defined(__AVX512BW__)
  LIBXSMM_DNN_CONVERT_BUFFER_F32_BF16(pdinput_fp32+nc_thr_begin, pdinput_bf16+nc_thr_begin, nc_thr_end-nc_thr_begin);
#else
  for ( i = nc_thr_begin; i < nc_thr_end; ++i ) {
    libxsmm_bfloat16_hp in;
    in.f = pdinput_fp32[i];
    pdinput_bf16[i] = in.i[1];
  }
#endif
  libxsmm_barrier_wait( cfg.barrier, ltid );
}

void init_on_numa_node_master_weights( my_opt_config cfg, float* master_wt_ptr, int start_tid, int my_tid) {
  /* loop counters */
  libxsmm_blasint i;

  /* computing first logical thread */
  const libxsmm_blasint ltid = my_tid - start_tid;

  /* number of tasks that could run in parallel for the filters */
  const libxsmm_blasint work = cfg.C * cfg.K;
  /* compute chunk size */
  const libxsmm_blasint chunksize = (work % cfg.threads == 0) ? (work / cfg.threads) : ((work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint thr_begin = (ltid * chunksize < work) ? (ltid * chunksize) : work;
  const libxsmm_blasint thr_end = ((ltid + 1) * chunksize < work) ? ((ltid + 1) * chunksize) : work;

  /* lazy barrier init */
  libxsmm_barrier_init( cfg.barrier, ltid );

  my_init_buf(master_wt_ptr, thr_end - thr_begin, 0, 0);

  libxsmm_barrier_wait( cfg.barrier, ltid );
}

void init_on_numa_node_weights( my_fc_fwd_config cfg, const libxsmm_bfloat16* wt_ptr, const float *wt_master_ptr, int start_tid, int my_tid ) {
  const libxsmm_blasint nBlocksIFm = cfg.C / cfg.bc;
  const libxsmm_blasint nBlocksOFm = cfg.K / cfg.bk;
  const libxsmm_blasint nBlocksMB  = cfg.N / cfg.bn;
  const libxsmm_blasint bn = cfg.bn;
  const libxsmm_blasint bk = cfg.bk;
  const libxsmm_blasint lpb = 2;
  const libxsmm_blasint bc_lp = cfg.bc/lpb;
  /* const libxsmm_blasint bc = cfg.bc;*/
  libxsmm_blasint use_2d_blocking = cfg.fwd_2d_blocking;


  const libxsmm_blasint OFM_shift = nBlocksIFm * bc_lp * cfg.bk * lpb;
  const libxsmm_blasint IFM_shift = bc_lp * cfg.bk * lpb;

  /* computing first logical thread */
  const libxsmm_blasint ltid = my_tid - start_tid;
  /* number of tasks that could be run in parallel */
  const libxsmm_blasint work = nBlocksOFm * nBlocksMB;
  /* compute chunk size */
  const libxsmm_blasint chunksize = (work % cfg.threads == 0) ?
          (work / cfg.threads) : ((work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint thr_begin = (ltid * chunksize < work) ? (ltid * chunksize) : work;
  const libxsmm_blasint thr_end = ((ltid + 1) * chunksize < work) ? ((ltid + 1) * chunksize) : work;

  /* loop variables */
  libxsmm_blasint ofm1 = 0, ifm1 = 0;
  libxsmm_blasint im_tasks_per_thread = 0, in_tasks_per_thread = 0, my_in_start = 0, my_in_end = 0, my_im_start = 0, my_im_end = 0, my_row_id = 0, my_col_id = 0, row_teams = 0, column_teams = 0;

  libxsmm_blasint BF = cfg.fwd_bf;
  libxsmm_blasint CB_BLOCKS = nBlocksIFm/BF;
  unsigned long long blocks = CB_BLOCKS;

  if (use_2d_blocking == 1) {
    int _ltid, hyperpartition_id, _nBlocksOFm;
    row_teams = cfg.fwd_row_teams;
    column_teams = cfg.fwd_column_teams;
    _nBlocksOFm = nBlocksOFm/cfg.fwd_model_hyperpartitions;
    _ltid = ltid % (row_teams * column_teams);
    hyperpartition_id = ltid / (row_teams * column_teams);
    my_col_id = _ltid % column_teams;
    my_row_id = _ltid / column_teams;
    im_tasks_per_thread = (nBlocksMB + row_teams-1)/row_teams;
    in_tasks_per_thread = (_nBlocksOFm + column_teams-1)/column_teams;
    my_im_start = LIBXSMM_MIN( my_row_id * im_tasks_per_thread, nBlocksMB);
    my_im_end = LIBXSMM_MIN( (my_row_id+1) * im_tasks_per_thread, nBlocksMB);
    my_in_start = hyperpartition_id * _nBlocksOFm + LIBXSMM_MIN( my_col_id * in_tasks_per_thread, _nBlocksOFm);
    my_in_end = hyperpartition_id * _nBlocksOFm + LIBXSMM_MIN( (my_col_id+1) * in_tasks_per_thread, _nBlocksOFm);
  }

  /* lazy barrier init */
  libxsmm_barrier_init(cfg.barrier, ltid);

  if (use_2d_blocking == 1) {
    if (BF > 1) {
      for (ofm1 = my_in_start; ofm1 < my_in_end; ++ofm1) {
        int ofm_offset = ofm1 * OFM_shift;
        for ( ifm1 = 0; ifm1 < BF; ++ifm1 ) {
          /* -> &LIBXSMM_VLA_ACCESS(5, filter, ofm1, ifm1*CB_BLOCKS, 0, 0, 0, nBlocksIFm, bc_lp, cfg.bk, lpb), */
          int ifm_offset = ifm1 * CB_BLOCKS * IFM_shift;
          libxsmm_bfloat16 *l_buf = (libxsmm_bfloat16*) wt_ptr + ofm_offset + ifm_offset;
          float *l_buf_master = (float*) wt_master_ptr + ofm_offset + ifm_offset;
          libxsmm_rne_convert_fp32_bf16( l_buf_master, l_buf, CB_BLOCKS * IFM_shift );
        }
      }
    } else {
      for (ofm1 = my_in_start; ofm1 < my_in_end; ++ofm1) {
        /* -> &LIBXSMM_VLA_ACCESS(5, filter, ofm1, 0, 0, 0, 0, nBlocksIFm, bc_lp, cfg.bk, lpb), */
        libxsmm_bfloat16 *l_buf = (libxsmm_bfloat16*) wt_ptr + ofm1 * OFM_shift;
        float *l_buf_master = (float*) wt_master_ptr + ofm1 * OFM_shift;
        libxsmm_rne_convert_fp32_bf16( l_buf_master, l_buf, OFM_shift );
      }
    }
  } else {
    int ofm_s = thr_begin / nBlocksMB;
    int ofm_e = thr_end / nBlocksMB;
    if (BF > 1) {
      for ( ofm1 = ofm_s; ofm1 < ofm_e; ++ofm1 ) {
        int ofm_offset = ofm1 * OFM_shift;
        for ( ifm1 = 0; ifm1 < BF; ++ifm1 ) {
          /* -> &LIBXSMM_VLA_ACCESS(5, filter, ofm1, ifm1*CB_BLOCKS, 0, 0, 0, nBlocksIFm, bc_lp, cfg.bk, lpb), */
          int ifm_offset = ifm1 * CB_BLOCKS * IFM_shift;
          libxsmm_bfloat16 *l_buf = (libxsmm_bfloat16*) wt_ptr + ofm_offset + ifm_offset;
          float *l_buf_master = (float*) wt_master_ptr + ofm_offset + ifm_offset;
          libxsmm_rne_convert_fp32_bf16( l_buf_master, l_buf, CB_BLOCKS * IFM_shift );
        }
      }
    } else {
      for ( ofm1 = ofm_s; ofm1 < ofm_e; ++ofm1 ) {
        /* -> LIBXSMM_VLA_ACCESS(5, filter, ofm1, 0, 0, 0, 0, nBlocksIFm, bc_lp, cfg.bk, lpb), */
        libxsmm_bfloat16 *l_buf = (libxsmm_bfloat16*) wt_ptr + ofm1 * OFM_shift;
        float *l_buf_master = (float*) wt_master_ptr + ofm1 * OFM_shift;
        libxsmm_rne_convert_fp32_bf16( l_buf_master, l_buf, OFM_shift );
      }
    }
  }

  libxsmm_barrier_wait(cfg.barrier, ltid);
}

void init_on_numa_node_bwd_d ( my_fc_bwd_config cfg, libxsmm_bfloat16* filter_tr, int start_tid, int my_tid ) {
  /* here we assume that input and output blocking is similar */
  const libxsmm_blasint bn = cfg.bn;
  const libxsmm_blasint bk = cfg.bk;
  const libxsmm_blasint bc = cfg.bc;
  const libxsmm_blasint nBlocksIFm = cfg.C / bc;
  const libxsmm_blasint nBlocksOFm = cfg.K / bk;
  const libxsmm_blasint nBlocksMB  = cfg.N / bn;
  const libxsmm_blasint lpb = 2;
  const libxsmm_blasint bk_lp = bk/lpb;

  const libxsmm_blasint OFM_shift = bk_lp * bc * lpb;
  const libxsmm_blasint IFM_shift = nBlocksOFm * bk_lp * bc * lpb;

  /* computing first logical thread */
  const libxsmm_blasint ltid = my_tid - start_tid;
  /* number of tasks that could be run in parallel */
  const libxsmm_blasint work = nBlocksIFm * nBlocksMB;
  /* compute chunk size */
  const libxsmm_blasint chunksize = (work % cfg.threads == 0) ? (work / cfg.threads) : ((work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint thr_begin = (ltid * chunksize < work) ? (ltid * chunksize) : work;
  const libxsmm_blasint thr_end = ((ltid + 1) * chunksize < work) ? ((ltid + 1) * chunksize) : work;

  libxsmm_blasint BF = cfg.bwd_bf;
  libxsmm_blasint KB_BLOCKS = nBlocksOFm/BF;
  unsigned long long blocks = KB_BLOCKS;

  libxsmm_blasint column_teams = cfg.bwd_column_teams;
  libxsmm_blasint my_col_id = ltid % column_teams;
  libxsmm_blasint in_tasks_per_thread = LIBXSMM_UPDIV(nBlocksIFm, column_teams);

  libxsmm_blasint my_in_start = LIBXSMM_MIN(my_col_id * in_tasks_per_thread, nBlocksIFm);
  libxsmm_blasint my_in_end = LIBXSMM_MIN((my_col_id+1) * in_tasks_per_thread, nBlocksIFm);

  int ifm1, ofm1;
  /* lazy barrier init */
  libxsmm_barrier_init(cfg.barrier, ltid);

  if (cfg.bwd_2d_blocking == 1) {
    if (BF > 1) {
      for (ifm1 = my_in_start; ifm1 < my_in_end; ++ifm1) {
        int ifm_offset = ifm1 * IFM_shift;
        for ( ofm1 = 0; ofm1 < BF; ++ofm1 ) {
          int ofm_offset = ofm1 * KB_BLOCKS * OFM_shift;
          libxsmm_bfloat16 *l_buf = (libxsmm_bfloat16*) filter_tr + ifm_offset + ofm_offset;
          my_init_buf_bf16(l_buf, KB_BLOCKS * OFM_shift, 0, 0);
        }
      }
    } else {
      for (ifm1 = my_in_start; ifm1 < my_in_end; ++ifm1) {
        libxsmm_bfloat16 *l_buf = (libxsmm_bfloat16*) filter_tr + ifm1 * IFM_shift;
        my_init_buf_bf16(l_buf, IFM_shift, 0, 0);
      }
    }
  } else {
    int ifm_s = thr_begin / nBlocksMB;
    int ifm_e = thr_end / nBlocksMB;
    if (BF > 1) {
      for ( ifm1 = ifm_s; ifm1 < ifm_e; ++ifm1 ) {
        int ifm_offset = ifm1 * IFM_shift;
        for ( ofm1 = 0; ofm1 < BF; ++ofm1 ) {
          int ofm_offset = ofm1 * KB_BLOCKS * OFM_shift;
          libxsmm_bfloat16 *l_buf = (libxsmm_bfloat16*) filter_tr + ifm_offset + ofm_offset;
          my_init_buf_bf16(l_buf, KB_BLOCKS * OFM_shift, 0, 0);
        }
      }
    } else {
      for ( ifm1 = ifm_s; ifm1 < ifm_e; ++ifm1 ) {
        libxsmm_bfloat16 *l_buf = (libxsmm_bfloat16*) filter_tr + ifm1 * IFM_shift;
        my_init_buf_bf16(l_buf, IFM_shift, 0, 0);
      }
    }
  }

  libxsmm_barrier_wait(cfg.barrier, ltid);
}

void init_on_numa_node_bwd_weights( my_fc_bwd_config cfg, libxsmm_bfloat16* doutput, int start_tid, int my_tid ) {
  const libxsmm_blasint bn = cfg.bn;
  const libxsmm_blasint bk = cfg.bk;
  const libxsmm_blasint bc = cfg.bc;
  libxsmm_blasint lpb = 2;
  const libxsmm_blasint bc_lp = bc/lpb;
  const libxsmm_blasint bk_lp = bk/lpb;
  const libxsmm_blasint bn_lp = bn/lpb;
  const libxsmm_blasint nBlocksIFm = cfg.C / cfg.bc;
  const libxsmm_blasint nBlocksOFm = cfg.K / cfg.bk;
  const libxsmm_blasint nBlocksMB  = cfg.N / cfg.bn;

  const libxsmm_blasint OFM_shift = bn_lp * bk * lpb;
  const libxsmm_blasint N_shift = nBlocksOFm * bn_lp * bk * lpb;

  /* computing first logical thread */
  const libxsmm_blasint ltid = my_tid - start_tid;
  /* number of tasks that could be run in parallel */
  const libxsmm_blasint ofm_subtasks = (cfg.upd_2d_blocking == 1) ? 1 : cfg.ofm_subtasks;
  const libxsmm_blasint ifm_subtasks = (cfg.upd_2d_blocking == 1) ? 1 : cfg.ifm_subtasks;
  const libxsmm_blasint bbk = (cfg.upd_2d_blocking == 1) ? bk : bk/ofm_subtasks;
  const libxsmm_blasint work = nBlocksIFm * ifm_subtasks * nBlocksOFm * ofm_subtasks;
  const libxsmm_blasint Cck_work = nBlocksIFm * ifm_subtasks * ofm_subtasks;
  const libxsmm_blasint Cc_work = nBlocksIFm * ifm_subtasks;

  /* 2D blocking parameters  */
  libxsmm_blasint use_2d_blocking = cfg.upd_2d_blocking;
  libxsmm_blasint in_tasks_per_thread = 0, my_in_start = 0, my_in_end = 0, my_col_id = 0, column_teams = 0;

  /* compute chunk size */
  const libxsmm_blasint chunksize = (work % cfg.threads == 0) ? (work / cfg.threads) : ((work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint thr_begin = (ltid * chunksize < work) ? (ltid * chunksize) : work;
  const libxsmm_blasint thr_end = ((ltid + 1) * chunksize < work) ? ((ltid + 1) * chunksize) : work;
  libxsmm_blasint BF = cfg.upd_bf;

  /* loop variables */
  libxsmm_blasint ifm1ofm1 = 0, bfn = 0;

  /* Batch reduce related variables */
  unsigned long long  blocks = nBlocksMB/BF;

  if (use_2d_blocking == 1) {
    column_teams = cfg.upd_column_teams;
    my_col_id = ltid % column_teams;
    in_tasks_per_thread = (nBlocksOFm + column_teams-1)/column_teams;

    my_in_start = LIBXSMM_MIN( my_col_id * in_tasks_per_thread, nBlocksOFm);
    my_in_end = LIBXSMM_MIN( (my_col_id+1) * in_tasks_per_thread, nBlocksOFm);
  }

  /* lazy barrier init */
  libxsmm_barrier_init(cfg.barrier, ltid);

  libxsmm_blasint ofm2 = 0, ofm1 = 0;
  if (use_2d_blocking == 1) {
    if (BF == 1) {
      for (ofm1 = my_in_start; ofm1 < my_in_end; ++ofm1) {
        /* -> (&LIBXSMM_VLA_ACCESS(5, doutput_tr, ofm1, 0, 0, ofm2*bbk, 0, nBlocksMB, bn_lp, bk, lpb) */
        libxsmm_bfloat16 *l_buf = (libxsmm_bfloat16*) doutput + ofm1 * OFM_shift;
        my_init_buf_bf16(l_buf, OFM_shift, 0, 0);
      }
    } else {
      for (bfn = 0; bfn < BF; bfn++) {
        for (ofm1 = my_in_start; ofm1 < my_in_end; ++ofm1) {
          /* -> &LIBXSMM_VLA_ACCESS(5, doutput_tr, ofm1, bfn*blocks, 0, ofm2*bbk, 0, nBlocksMB, bn_lp, bk, lpb) */
          libxsmm_bfloat16 *l_buf = (libxsmm_bfloat16*) doutput + bfn * blocks * N_shift +  ofm1 * OFM_shift;
          my_init_buf_bf16(l_buf, OFM_shift, 0, 0);
        }
      }
    }
  } else {
    if (BF == 1) {
      for ( ifm1ofm1 = thr_begin; ifm1ofm1 < thr_end; ++ifm1ofm1 ) {
        ofm1 = ifm1ofm1 / Cck_work;
        ofm2 = (ifm1ofm1 % Cck_work) / Cc_work;
        /* -> &LIBXSMM_VLA_ACCESS(5, doutput_tr, ofm1, 0, 0, ofm2*bbk, 0, nBlocksMB, bn_lp, bk, lpb) */
        libxsmm_bfloat16 *l_buf = (libxsmm_bfloat16*) doutput + ofm1 * OFM_shift + ofm2*bbk;
        my_init_buf_bf16(l_buf, bbk * bk, 0, 0);
      }
    } else {
      for (bfn = 0; bfn < BF; bfn++) {
        for ( ifm1ofm1 = thr_begin; ifm1ofm1 < thr_end; ++ifm1ofm1 ) {
          ofm1 = ifm1ofm1 / Cck_work;
          ofm2 = (ifm1ofm1 % Cck_work) / Cc_work;
          /* -> &LIBXSMM_VLA_ACCESS(5, doutput_tr, ofm1, bfn*blocks, 0, ofm2*bbk, 0, nBlocksMB, bn_lp, bk, lpb) */
          libxsmm_bfloat16 *l_buf = (libxsmm_bfloat16*) doutput + bfn * blocks * N_shift +  ofm1 * OFM_shift + ofm2*bbk;
          my_init_buf_bf16(l_buf, bbk * bk, 0, 0);
        }
      }
    }
  }

  libxsmm_barrier_wait(cfg.barrier, ltid);

}

void init_on_numa_node_bwd_dweights ( my_fc_bwd_config cfg, libxsmm_bfloat16* dwt, int start_tid, int my_tid ) {
  const libxsmm_blasint bn = cfg.bn;
  const libxsmm_blasint bk = cfg.bk;
  const libxsmm_blasint bc = cfg.bc;
  libxsmm_blasint lpb = 2;
  const libxsmm_blasint bc_lp = bc/lpb;
  const libxsmm_blasint bk_lp = bk/lpb;
  const libxsmm_blasint bn_lp = bn/lpb;
  const libxsmm_blasint nBlocksIFm = cfg.C / cfg.bc;
  const libxsmm_blasint nBlocksOFm = cfg.K / cfg.bk;
  const libxsmm_blasint nBlocksMB  = cfg.N / cfg.bn;

  const libxsmm_blasint OFM_shift = bn_lp * bk * lpb;
  const libxsmm_blasint N_shift = nBlocksOFm * bn_lp * bk * lpb;

  /* computing first logical thread */
  const libxsmm_blasint ltid = my_tid - start_tid;
  /* number of tasks that could be run in parallel */
  const libxsmm_blasint ofm_subtasks = (cfg.upd_2d_blocking == 1) ? 1 : cfg.ofm_subtasks;
  const libxsmm_blasint ifm_subtasks = (cfg.upd_2d_blocking == 1) ? 1 : cfg.ifm_subtasks;
  const libxsmm_blasint bbk = (cfg.upd_2d_blocking == 1) ? bk : bk/ofm_subtasks;
  const libxsmm_blasint bbc = (cfg.upd_2d_blocking == 1) ? bc : bc/ifm_subtasks;
  const libxsmm_blasint work = nBlocksIFm * ifm_subtasks * nBlocksOFm * ofm_subtasks;
  const libxsmm_blasint Cck_work = nBlocksIFm * ifm_subtasks * ofm_subtasks;
  const libxsmm_blasint Cc_work = nBlocksIFm * ifm_subtasks;

  /* 2D blocking parameters  */
  libxsmm_blasint use_2d_blocking = cfg.upd_2d_blocking;
  libxsmm_blasint im_tasks_per_thread = 0, in_tasks_per_thread = 0,
                  my_in_start         = 0, my_in_end           = 0,
                  my_im_start         = 0, my_im_end           = 0,
                  my_row_id           = 0, my_col_id           = 0,
                  row_teams           = 0, column_teams        = 0;

  /* compute chunk size */
  const libxsmm_blasint chunksize = (work % cfg.threads == 0) ? (work / cfg.threads) : ((work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint thr_begin = (ltid * chunksize < work) ? (ltid * chunksize) : work;
  const libxsmm_blasint thr_end = ((ltid + 1) * chunksize < work) ? ((ltid + 1) * chunksize) : work;
  libxsmm_blasint BF = cfg.upd_bf;

  /* loop variables */
  libxsmm_blasint ifm1ofm1 = 0, bfn = 0;

  /* Batch reduce related variables */
  unsigned long long  blocks = nBlocksMB/BF;

    if (use_2d_blocking == 1) {
      int _ltid, hyperpartition_id, _nBlocksOFm;
      row_teams = cfg.upd_row_teams;
      column_teams = cfg.upd_column_teams;
      _nBlocksOFm = nBlocksOFm/cfg.upd_model_hyperpartitions;
      _ltid = ltid % (row_teams * column_teams);
      hyperpartition_id = ltid / (row_teams * column_teams);
      my_col_id = _ltid % column_teams;
      my_row_id = _ltid / column_teams;
      im_tasks_per_thread = (nBlocksIFm + row_teams-1)/row_teams;
      in_tasks_per_thread = (_nBlocksOFm + column_teams-1)/column_teams;
      my_im_start = LIBXSMM_MIN( my_row_id * im_tasks_per_thread, nBlocksIFm);
      my_im_end = LIBXSMM_MIN( (my_row_id+1) * im_tasks_per_thread, nBlocksIFm);
      my_in_start = hyperpartition_id * _nBlocksOFm + LIBXSMM_MIN( my_col_id * in_tasks_per_thread, _nBlocksOFm);
      my_in_end = hyperpartition_id * _nBlocksOFm + LIBXSMM_MIN( (my_col_id+1) * in_tasks_per_thread, _nBlocksOFm);
    }

  LIBXSMM_VLA_DECL(5, libxsmm_bfloat16, dfilter, dwt, nBlocksIFm, bc_lp, bk, lpb);

  /* lazy barrier init */
  libxsmm_barrier_init(cfg.barrier, ltid);

  libxsmm_blasint ofm2 = 0, ofm1 = 0;
  libxsmm_blasint ifm2 = 0, ifm1 = 0;
  if (use_2d_blocking == 1) {
    for (ofm1 = my_in_start; ofm1 < my_in_end; ++ofm1) {
      for (ifm1 = my_im_start; ifm1 < my_im_end; ++ifm1) {
        libxsmm_bfloat16 *df_buf = &LIBXSMM_VLA_ACCESS(5, dfilter, ofm1, ifm1, 0, 0, 0, nBlocksIFm, bc_lp, bk, lpb);
        my_init_buf_bf16(df_buf, bc_lp * bk * lpb, 0, 0);
      }
    }
  } else {
    for ( ifm1ofm1 = thr_begin; ifm1ofm1 < thr_end; ++ifm1ofm1 ) {
      ofm1 = ifm1ofm1 / Cck_work;
      ofm2 = (ifm1ofm1 % Cck_work) / Cc_work;
      ifm1 = ((ifm1ofm1 % Cck_work) % Cc_work) / ifm_subtasks;
      ifm2 = ((ifm1ofm1 % Cck_work) % Cc_work) % ifm_subtasks;

      libxsmm_bfloat16 *df_buf = &LIBXSMM_VLA_ACCESS(5, dfilter , ofm1, ifm1, (ifm2*bbc)/lpb, ofm2*bbk, 0, nBlocksIFm, bc_lp, bk, lpb);
      my_init_buf_bf16(df_buf, bbk * lpb, 0, 0);
    }
  }

  libxsmm_barrier_wait(cfg.barrier, ltid);

}

int main(int argc, char* argv[])
{
  libxsmm_bfloat16 **act_libxsmm, **fil_libxsmm, **delact_libxsmm, **delfil_libxsmm;
  libxsmm_bfloat16 **bias_libxsmm, **delbias_libxsmm;
  float **fil_master;
  unsigned char **relumask_libxsmm;
  int *label_libxsmm;
  my_eltwise_fuse my_fuse;
  my_fc_fwd_config* my_fc_fwd;
  my_fc_bwd_config* my_fc_bwd;
  my_opt_config* my_opt;
  my_smax_fwd_config my_smax_fwd;
  my_smax_bwd_config my_smax_bwd;
  void* scratch = NULL;
  size_t scratch_size = 0;
#ifdef CHECK_L1
  float *last_act_fwd_f32 = NULL;
  float *first_wt_bwdupd_f32 = NULL;
#endif

  /* some parameters we can overwrite via cli,
     default is some inner layer of overfeat */
  int iters = 10;       /* repetitions of benchmark */
  int MB = 32;          /* mini-batch size, "N" */
  int fuse_type = 0;    /* 0: nothing fused, 1: relu fused, 2: elementwise fused, 3: relu and elementwise fused */
  char type = 'A';      /* 'A': ALL, 'F': FP, 'B': BP */
  int bn = 64;
  int bk = 64;
  int bc = 64;
  int *C;               /* number of input feature maps, "C" */
  int num_layers = 0;

  const char *const env_check = getenv("CHECK");
  const double check = LIBXSMM_ABS(0 == env_check ? 1 : atof(env_check));

#if defined(_OPENMP)
  int nThreads = omp_get_max_threads(); /* number of threads */
#else
  int nThreads = 1; /* number of threads */
#endif

  unsigned long long l_start, l_end;
  double l_total = 0.0;
  double gflop = 0.0;
  int i, j;
  double act_size = 0.0;
  double fil_size = 0.0;
  float lr = 0.2f;
  float loss = 0;
  float loss_weight = 0.1f;

  libxsmm_matdiff_info norms_fwd, norms_bwd, norms_upd, diff;
  libxsmm_matdiff_clear(&norms_fwd);
  libxsmm_matdiff_clear(&norms_bwd);
  libxsmm_matdiff_clear(&norms_upd);
  libxsmm_matdiff_clear(&diff);

  if (argc > 1 && !strncmp(argv[1], "-h", 3)) {
    printf("Usage: %s iters MB fuse_type type bn bk bc C1 C2 ... CN\n", argv[0]);
    return 0;
  }
  libxsmm_rng_set_seed(1);

  /* reading new values from cli */
  i = 1;
  num_layers = argc - 9;
  if (argc > i) iters      = atoi(argv[i++]);
  if (argc > i) MB         = atoi(argv[i++]);
  if (argc > i) fuse_type  = atoi(argv[i++]);
  if (argc > i) type       = *(argv[i++]);
  if (argc > i) bn         = atoi(argv[i++]);
  if (argc > i) bk         = atoi(argv[i++]);
  if (argc > i) bc         = atoi(argv[i++]);
  /* allocate the number of channles buffer */
  if ( num_layers < 1 ) {
    printf("Usage: %s iters MB fuse_type type bn bk bc C1 C2 ... CN\n", argv[0]);
    return 0;
  }
  C = (int*)malloc((num_layers+2)*sizeof(int));
  for (j = 0 ; i < argc; ++i, ++j ) {
    C[j] = atoi(argv[i]);
  }
  /* handle softmax config */
  C[num_layers+1] = C[num_layers];

  if (type != 'A' && type != 'F' && type != 'B') {
    printf("type needs to be 'A' (All), 'F' (FP only), 'B' (BP only)\n");
    return -1;
  }
  if ( (fuse_type < 0) || (fuse_type > 5) ) {
    printf("fuse type needs to be 0 (None), 1 (Bias), 2 (ReLU), 3 (Sigmoid), 4 (Bias+ReLU), 5 (Bias+Sigmoid)\n");
    return -1;
  }

#if defined(__SSE3__)
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
  _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
  _MM_SET_ROUNDING_MODE(_MM_ROUND_NEAREST);
#endif

  /* print some summary */
  printf("##########################################\n");
  printf("#          Setting Up (Common)           #\n");
  printf("##########################################\n");
  printf("PARAMS: N:%d\n", MB);
  printf("PARAMS: Layers: %d\n", num_layers);
  printf("PARAMS: ITERS:%d", iters); if (LIBXSMM_FEQ(0, check)) printf("  Threads:%d\n", nThreads); else printf("\n");
  for (i = 0; i < num_layers; ++i ) {
    if (i == 0) {
      act_size += (double)(MB*C[i]*sizeof(libxsmm_bfloat16))/(1024.0*1024.0);
      printf("SIZE Activations  %i (%dx%d): %10.2f MiB\n", i, MB, C[i], (double)(MB*C[i]*sizeof(libxsmm_bfloat16))/(1024.0*1024.0) );
    }
    act_size += (double)(MB*C[i+1]*sizeof(libxsmm_bfloat16))/(1024.0*1024.0);
    fil_size += (double)(C[i]*C[i+1]*sizeof(libxsmm_bfloat16))/(1024.0*1024.0);
    printf("SIZE Filter       %i (%dx%d): %10.2f MiB\n", i, C[i], C[i+1], (double)(C[i]*C[i+1]*sizeof(libxsmm_bfloat16))/(1024.0*1024.0) );
    printf("SIZE Activations  %i (%dx%d): %10.2f MiB\n", i+1, MB, C[i+1], (double)(MB*C[i+1]*sizeof(libxsmm_bfloat16))/(1024.0*1024.0) );
  }
  act_size += (double)(MB*C[num_layers+1]*sizeof(float))/(1024.0*1024.0);
  printf("SIZE Activations softmax (%dx%d): %10.2f MiB\n", MB, C[num_layers+1], (double)(MB*C[num_layers+1]*sizeof(libxsmm_bfloat16))/(1024.0*1024.0) );
  printf("\nTOTAL SIZE Activations:            %10.2f MiB\n", act_size );
  printf("TOTAL SIZE Filter (incl. master):  %10.2f MiB\n", 3.0*fil_size );
  printf("TOTAL SIZE delActivations:         %10.2f MiB\n", act_size );
  printf("TOTAL SIZE delFilter:              %10.2f MiB\n", fil_size );
  printf("TOTAL SIZE MLP:                    %10.2f MiB\n", (4.0*fil_size) + (2.0*act_size) );

  /* allocate data */
  act_libxsmm    = (libxsmm_bfloat16**)malloc( (num_layers+2)*sizeof(libxsmm_bfloat16*) );
  delact_libxsmm = (libxsmm_bfloat16**)malloc( (num_layers+1)*sizeof(libxsmm_bfloat16*) );
  for ( i = 0 ; i < num_layers+2; ++i ) {
    act_libxsmm[i]                = (libxsmm_bfloat16*)libxsmm_aligned_malloc( MB*C[i]*sizeof(libxsmm_bfloat16), 2097152);
    /* softmax has no incoming gradients */
    if ( i < num_layers+1 ) {
      delact_libxsmm[i]             = (libxsmm_bfloat16*)libxsmm_aligned_malloc( MB*C[i]*sizeof(libxsmm_bfloat16), 2097152);
    }
  }
  fil_master     = (float**)           malloc( num_layers*sizeof(float*) );
  fil_libxsmm    = (libxsmm_bfloat16**)malloc( num_layers*sizeof(libxsmm_bfloat16*) );
  delfil_libxsmm = (libxsmm_bfloat16**)malloc( num_layers*sizeof(libxsmm_bfloat16*) );
  for ( i = 0 ; i < num_layers; ++i ) {
    fil_master[i]                 = (float*)           libxsmm_aligned_malloc( C[i]*C[i+1]*sizeof(float), 2097152);
    fil_libxsmm[i]                = (libxsmm_bfloat16*)libxsmm_aligned_malloc( C[i]*C[i+1]*sizeof(libxsmm_bfloat16), 2097152);
    delfil_libxsmm[i]             = (libxsmm_bfloat16*)libxsmm_aligned_malloc( C[i]*C[i+1]*sizeof(libxsmm_bfloat16), 2097152);
  }
  bias_libxsmm    = (libxsmm_bfloat16**)malloc( num_layers*sizeof(libxsmm_bfloat16*) );
  delbias_libxsmm = (libxsmm_bfloat16**)malloc( num_layers*sizeof(libxsmm_bfloat16*) );
  for ( i = 0 ; i < num_layers; ++i ) {
    bias_libxsmm[i]               = (libxsmm_bfloat16*)libxsmm_aligned_malloc( C[i+1]*sizeof(libxsmm_bfloat16), 2097152);
    delbias_libxsmm[i]            = (libxsmm_bfloat16*)libxsmm_aligned_malloc( C[i+1]*sizeof(libxsmm_bfloat16), 2097152);
  }
  relumask_libxsmm = (unsigned char**)malloc( num_layers*sizeof(unsigned char*) );
  for ( i = 0 ; i < num_layers; ++i ) {
    relumask_libxsmm[i]           = (unsigned char*)libxsmm_aligned_malloc( MB*C[i+1]*sizeof(unsigned char), 2097152);
  }
  label_libxsmm = (int*)libxsmm_aligned_malloc( MB*sizeof(int), 2097152);

  printf("\n");
  printf("##########################################\n");
  printf("#      Setting Up  (custom-Storage)      #\n");
  printf("##########################################\n");

  if ( fuse_type == 0 ) {
    my_fuse = MY_ELTWISE_FUSE_NONE;
  } else if ( fuse_type == 1 ) {
    my_fuse = MY_ELTWISE_FUSE_BIAS;
  } else if ( fuse_type == 2 ) {
    my_fuse = MY_ELTWISE_FUSE_RELU;
  } else if ( fuse_type == 4 ) {
    my_fuse = MY_ELTWISE_FUSE_BIAS_RELU;
  } else {
    /* cannot happen */
  }

  /* allocating handles */
  my_fc_fwd = (my_fc_fwd_config*) malloc( num_layers*sizeof(my_fc_fwd_config) );
  my_fc_bwd = (my_fc_bwd_config*) malloc( num_layers*sizeof(my_fc_bwd_config) );
  my_opt    = (my_opt_config*)    malloc( num_layers*sizeof(my_opt_config)    );

  /* setting up handles + scratch */
  int max_bwd_layer = 0, max_bwd_wu_layer = 0;
  size_t max_bwd_scratch_size = 0, max_doutput_scratch_mark = 0;
  scratch_size = 0;
  for ( i = 0; i < num_layers; ++i ) {
    my_fc_fwd[i] = setup_my_fc_fwd(MB, C[i], C[i+1], (MB % bn == 0) ? bn : MB,
                                             (C[i  ] % bc == 0) ? bc : C[i  ],
                                             (C[i+1] % bk == 0) ? bk : C[i+1],
                                             nThreads, my_fuse);

    my_fc_bwd[i] = setup_my_fc_bwd(MB, C[i], C[i+1], (MB % bn == 0) ? bn : MB,
                                             (C[i  ] % bc == 0) ? bc : C[i  ],
                                             (C[i+1] % bk == 0) ? bk : C[i+1],
                                              nThreads, my_fuse);

    my_opt[i] = setup_my_opt( C[i], C[i+1], (C[i  ] % bc == 0) ? bc : C[i  ],
                                            (C[i+1] % bk == 0) ? bk : C[i+1],
                                            nThreads, lr );
    if (my_fc_bwd[i].scratch_size > 0 && my_fc_bwd[i].scratch_size > max_bwd_scratch_size) {
        max_bwd_layer = i;
        max_bwd_scratch_size =  my_fc_bwd[i].scratch_size;
    }

    if (my_fc_bwd[i].doutput_scratch_mark > 0 && my_fc_bwd[i].doutput_scratch_mark > max_doutput_scratch_mark) {
        max_bwd_wu_layer = i;
        max_doutput_scratch_mark = my_fc_bwd[i].doutput_scratch_mark;
    }
    /* let's allocate and bind scratch */
    if ( my_fc_fwd[i].scratch_size > 0 || my_fc_bwd[i].scratch_size > 0 || my_opt[i].scratch_size > 0 ) {
      size_t alloc_size = LIBXSMM_MAX( LIBXSMM_MAX( my_fc_fwd[i].scratch_size, my_fc_bwd[i].scratch_size), my_opt[i].scratch_size );
      if ( alloc_size > scratch_size ) {
        scratch_size = alloc_size;
      }
    }
  }

  /* softmax+loss is treated as N+! layer */
  my_smax_fwd = setup_my_smax_fwd( MB, C[num_layers+1], (MB % bn == 0) ? bn : MB,
                                       (C[num_layers+1] % bk == 0) ? bk : C[num_layers+1],
                                       nThreads );

  my_smax_bwd = setup_my_smax_bwd( MB, C[num_layers+1], (MB % bn == 0) ? bn : MB,
                                       (C[num_layers+1] % bk == 0) ? bk : C[num_layers+1],
                                       nThreads, loss_weight );

  if ( my_smax_fwd.scratch_size > 0 || my_smax_bwd.scratch_size > 0 ) {
    size_t alloc_size = LIBXSMM_MAX( my_smax_fwd.scratch_size, my_smax_bwd.scratch_size );
    if ( alloc_size > scratch_size ) {
      scratch_size = alloc_size;
    }
  }
  scratch = libxsmm_aligned_scratch( scratch_size, 2097152 );

  /* init data */
  for ( i = 0 ; i < num_layers+2; ++i ) {
    my_init_buf_bf16( act_libxsmm[i], MB*C[i], 0, 0 );
  }
  for ( i = 0 ; i < num_layers+1; ++i ) {
    my_init_buf_bf16( delact_libxsmm[i], MB*C[i], 0, 0 );
  }
#if 0
  for ( i = 0 ; i < num_layers; ++i ) {
    float *cur_fil = (float*) malloc(C[i]*C[i+1]*sizeof(float));
    my_init_buf( cur_fil, C[i]*C[i+1], 0, 0 );
    my_matrix_copy_KCCK_to_KCCK_vnni(cur_fil, fil_master[i], C[i], C[i+1], bc, bk);
    libxsmm_rne_convert_fp32_bf16( fil_master[i], fil_libxsmm[i], C[i]*C[i+1] );
    free(cur_fil);
  }
#endif
#if 0
  for ( i = 0 ; i < num_layers; ++i ) {
    float *cur_fil = (float*) malloc(C[i]*C[i+1]*sizeof(float));
    float *cur_fil_vnni = (float*) malloc(C[i]*C[i+1]*sizeof(float));
    my_init_buf( cur_fil, C[i]*C[i+1], 0, 0 );
    my_matrix_copy_KCCK_to_KCCK_vnni(cur_fil, cur_fil_vnni, C[i], C[i+1], bc, bk);
    libxsmm_rne_convert_fp32_bf16( cur_fil_vnni, delfil_libxsmm[i], C[i]*C[i+1] );
    free(cur_fil);
    free(cur_fil_vnni);
  }
#endif
  for ( i = 0 ; i < num_layers; ++i ) {
    /*my_init_buf_bf16( delfil_libxsmm[i], C[i]*C[i+1], 0, 0 );*/
  }
  for ( i = 0 ; i < num_layers; ++i ) {
    my_init_buf_bf16( bias_libxsmm[i], C[i+1], 0, 0 );
  }
  for ( i = 0 ; i < num_layers; ++i ) {
    my_init_buf_bf16( delbias_libxsmm[i], C[i+1], 0, 0 );
  }
  for ( i = 0 ; i < num_layers; ++i ) {
    zero_buf_uint8( relumask_libxsmm[i], MB*C[i+1] );
  }
  zero_buf_int32( label_libxsmm, MB );

#if defined(_OPENMP)
# pragma omp parallel
#endif
  {
#if defined(_OPENMP)
    const int tid = omp_get_thread_num();
#else
    const int tid = 0;
#endif
    int l = 0;
    for (l = 0; l < num_layers; l++)
      init_on_numa_node_master_weights( my_opt[l], fil_master[l], 0, tid);
  }

#if defined(_OPENMP)
# pragma omp parallel
#endif
  {
#if defined(_OPENMP)
    const int tid = omp_get_thread_num();
#else
    const int tid = 0;
#endif
    int l = 0;
    for ( l = 0; l < num_layers; ++l )
      init_on_numa_node_weights( my_fc_fwd[l], fil_libxsmm[l], fil_master[l], 0, tid );

    init_on_numa_node_bwd_d( my_fc_bwd[max_bwd_layer], (libxsmm_bfloat16*)scratch, 0, tid );

    libxsmm_bfloat16 *bwd_wu = NULL;
    if((my_fc_bwd[max_bwd_wu_layer].fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU) {
#ifdef OVERWRITE_DOUTPUT_BWDUPD
      bwd_wu = (libxsmm_bfloat16*)((char*)scratch + max_doutput_scratch_mark);
#else
      bwd_wu = (libxsmm_bfloat16*)((char*)scratch + max_doutput_scratch_mark) + my_fc_bwd[max_bwd_wu_layer].N * my_fc_bwd[max_bwd_wu_layer].K;
#endif
      init_on_numa_node_bwd_weights( my_fc_bwd[max_bwd_wu_layer], bwd_wu, 0, tid );
    }
    for ( l = 0; l < num_layers; ++l )
      init_on_numa_node_bwd_dweights( my_fc_bwd[l], delfil_libxsmm[l], 0, tid );
  }

  if ( type == 'F') {
    printf("##########################################\n");
    printf("#   Performance - FWD (custom-Storage)   #\n");
    printf("##########################################\n");
    l_start = libxsmm_timer_tick();
#if defined(_OPENMP)
#   pragma omp parallel private(i,j)
#endif
    {
#if defined(_OPENMP)
      const int tid = omp_get_thread_num();
#else
      const int tid = 0;
#endif
      for (j = 0; j < iters; ++j) {
        for ( i = 0; i < num_layers; ++i) {
          my_fc_fwd_exec( my_fc_fwd[i], fil_libxsmm[i], act_libxsmm[i], act_libxsmm[i+1],
                          bias_libxsmm[i], relumask_libxsmm[i], 0, tid, scratch );
        }
#ifdef USE_SOFTMAX
        my_smax_fwd_exec( my_smax_fwd, act_libxsmm[num_layers], act_libxsmm[num_layers+1], label_libxsmm, &loss,
                          0, tid, scratch );
#endif
      }
    }
    l_end = libxsmm_timer_tick();
    l_total = libxsmm_timer_duration(l_start, l_end);

    gflop = 0.0;
    for ( i = 0; i < num_layers; ++i) {
      gflop += (2.0*(double)MB*(double)C[i]*(double)C[i+1]*(double)iters) / (1000.0*1000.0*1000.0);
    }
    printf("GFLOP  = %.5g\n", gflop/(double)iters);
    printf("fp time = %.5g\n", ((double)(l_total/iters)));
    printf("GFLOPS  = %.5g\n", gflop/l_total);
    printf("PERFDUMP,FP,%s,%i,%i,", LIBXSMM_VERSION, nThreads, MB );
    for ( i = 0; i < num_layers; ++i ) {
      printf("%i,", C[i] );
    }
    printf("%f,%f\n", ((double)(l_total/iters)), gflop/l_total);
  }

  if (type == 'B') {
    printf("##########################################\n");
    printf("#   Performance - BWD (custom-Storage)   #\n");
    printf("##########################################\n");
    l_start = libxsmm_timer_tick();
#if defined(_OPENMP)
#   pragma omp parallel private(i,j)
#endif
    {
#if defined(_OPENMP)
      const int tid = omp_get_thread_num();
#else
      const int tid = 0;
#endif
      for (j = 0; j < iters; ++j) {
#ifdef USE_SOFTMAX
        my_smax_bwd_exec( my_smax_bwd, delact_libxsmm[num_layers], act_libxsmm[num_layers+1], label_libxsmm,
                          0, tid, scratch );
#endif
        for ( i = num_layers-1; i > 0; --i) {
          my_fc_bwd_exec( my_fc_bwd[i], fil_libxsmm[i], delact_libxsmm[i], delact_libxsmm[i+1], delfil_libxsmm[i],
                          act_libxsmm[i], delbias_libxsmm[i], relumask_libxsmm[i], MY_PASS_BWD, 0, tid, scratch );
          my_opt_exec( my_opt[i], fil_libxsmm[i], fil_master[i], delfil_libxsmm[i], 0, tid, scratch );
        }
        my_fc_bwd_exec( my_fc_bwd[0], fil_libxsmm[0], delact_libxsmm[0], delact_libxsmm[0+1], delfil_libxsmm[0],
                        act_libxsmm[0], delbias_libxsmm[0], relumask_libxsmm[0], MY_PASS_BWD_W, 0, tid, scratch );
        my_opt_exec( my_opt[0], fil_libxsmm[0], fil_master[0], delfil_libxsmm[0], 0, tid, scratch );
      }
    }
    l_end = libxsmm_timer_tick();
    l_total = libxsmm_timer_duration(l_start, l_end);

    gflop = 0.0;
    for ( i = num_layers-1; i > 0; --i) {
      gflop += (4.0*(double)MB*(double)C[i]*(double)C[i+1]*(double)iters) / (1000.0*1000.0*1000.0);
    }
    gflop += (2.0*(double)MB*(double)C[0]*(double)C[1]*(double)iters) / (1000.0*1000.0*1000.0);
    printf("GFLOP  = %.5g\n", gflop/(double)iters);
    printf("fp time = %.5g\n", ((double)(l_total/iters)));
    printf("GFLOPS  = %.5g\n", gflop/l_total);
    printf("PERFDUMP,BP,%s,%i,%i,", LIBXSMM_VERSION, nThreads, MB );
    for ( i = 0; i < num_layers; ++i ) {
      printf("%i,", C[i] );
    }
    printf("%f,%f\n", ((double)(l_total/iters)), gflop/l_total);
  }

  if (type == 'A') {
    printf("##########################################\n");
    printf("# Performance - FWD-BWD (custom-Storage) #\n");
    printf("##########################################\n");
    l_start = libxsmm_timer_tick();
#if defined(_OPENMP)
#   pragma omp parallel private(i,j)
#endif
    {
#if defined(_OPENMP)
      const int tid = omp_get_thread_num();
#else
      const int tid = 0;
#endif
      for (j = 0; j < iters; ++j) {
        for ( i = 0; i < num_layers; ++i) {
          my_fc_fwd_exec( my_fc_fwd[i], fil_libxsmm[i], act_libxsmm[i], act_libxsmm[i+1],
                          bias_libxsmm[i], relumask_libxsmm[i], 0, tid, scratch );
        }
#ifdef USE_SOFTMAX
        my_smax_fwd_exec( my_smax_fwd, act_libxsmm[num_layers], act_libxsmm[num_layers+1], label_libxsmm, &loss,
                          0, tid, scratch );
        my_smax_bwd_exec( my_smax_bwd, delact_libxsmm[num_layers], act_libxsmm[num_layers+1], label_libxsmm,
                          0, tid, scratch );
#endif
        for ( i = num_layers-1; i > 0; --i) {
          my_fc_bwd_exec( my_fc_bwd[i], fil_libxsmm[i], delact_libxsmm[i], delact_libxsmm[i+1], delfil_libxsmm[i],
                          act_libxsmm[i], delbias_libxsmm[i], relumask_libxsmm[i], MY_PASS_BWD, 0, tid, scratch );
#ifndef BYPASS_SGD
          my_opt_exec( my_opt[i], fil_libxsmm[i], fil_master[i], delfil_libxsmm[i], 0, tid, scratch );
#endif
        }
        my_fc_bwd_exec( my_fc_bwd[0], fil_libxsmm[0], delact_libxsmm[0], delact_libxsmm[0+1], delfil_libxsmm[0],
                        act_libxsmm[0], delbias_libxsmm[0], relumask_libxsmm[0], MY_PASS_BWD_W, 0, tid, scratch );
#ifndef BYPASS_SGD
        my_opt_exec( my_opt[0], fil_libxsmm[0], fil_master[0], delfil_libxsmm[0], 0, tid, scratch );
#endif
      }
    }
    l_end = libxsmm_timer_tick();
    l_total = libxsmm_timer_duration(l_start, l_end);

#ifdef CHECK_L1
    /* Print some norms on last act for fwd and weights of first layer after all iterations */
    last_act_fwd_f32    = (float*) malloc(MB*C[num_layers]*sizeof(float));
    first_wt_bwdupd_f32 = (float*) malloc(C[0]*C[1]*sizeof(float));
    libxsmm_convert_bf16_f32( act_libxsmm[num_layers], last_act_fwd_f32, MB*C[num_layers]);
#if 1
    libxsmm_convert_bf16_f32( fil_libxsmm[0], first_wt_bwdupd_f32, C[0]*C[1]);
    libxsmm_matdiff(&norms_fwd, LIBXSMM_DATATYPE_F32, MB*C[num_layers], 1, last_act_fwd_f32, last_act_fwd_f32, 0, 0);
    printf("L1 of act[num_layers]  : %.25g\n", norms_fwd.l1_ref);
    libxsmm_matdiff_reduce(&diff, &norms_fwd);
    libxsmm_matdiff(&norms_bwd, LIBXSMM_DATATYPE_F32, C[0]*C[1], 1, first_wt_bwdupd_f32, first_wt_bwdupd_f32, 0, 0);
    printf("L1 of wt[0]  : %.25g\n", norms_bwd.l1_ref);
    libxsmm_matdiff_reduce(&diff, &norms_bwd);
#else
    {
      int e = 0;
      FILE *fileAct, *fileWt;
      float *ref_last_act_fwd_f32    = (float*) malloc(MB*C[num_layers]*sizeof(float));
      float *ref_first_wt_bwdupd_f32 = (float*) malloc(C[0]*C[1]*sizeof(float));
      float *ref_first_wt_bwdupd_f32_kc = (float*) malloc(C[0]*C[1]*sizeof(float));
      libxsmm_bfloat16 *first_wt_bwdupd_bf16 = (libxsmm_bfloat16*) malloc(C[0]*C[1]*sizeof(libxsmm_bfloat16));

      fileAct = fopen("acts.txt","r");
      if (fileAct != NULL) {
        int bufferLength = 255;
        char buffer[bufferLength];
        e = 0;
        while(fgets(buffer, bufferLength, fileAct)) {
          ref_last_act_fwd_f32[e] = atof(buffer);
          e++;
        }
        fclose(fileAct);
      }
      /* compare */
      libxsmm_matdiff(&norms_fwd, LIBXSMM_DATATYPE_F32, MB*C[num_layers], 1, ref_last_act_fwd_f32, last_act_fwd_f32, 0, 0);
      printf("##########################################\n");
      printf("#   Correctness - Last fwd act           #\n");
      printf("##########################################\n");
      printf("L1 reference  : %.25g\n", norms_fwd.l1_ref);
      printf("L1 test       : %.25g\n", norms_fwd.l1_tst);
      printf("L2 abs.error  : %.24f\n", norms_fwd.l2_abs);
      printf("L2 rel.error  : %.24f\n", norms_fwd.l2_rel);
      printf("Linf abs.error: %.24f\n", norms_fwd.linf_abs);
      printf("Linf rel.error: %.24f\n", norms_fwd.linf_rel);
      printf("Check-norm    : %.24f\n", norms_fwd.normf_rel);
      libxsmm_matdiff_reduce(&diff, &norms_fwd);


      fileWt = fopen("weights.txt","r");
      if (fileWt != NULL) {
        int bufferLength = 255;
        char buffer[bufferLength];
        e = 0;
        while(fgets(buffer, bufferLength, fileWt)) {
          ref_first_wt_bwdupd_f32[e] = atof(buffer);
          e++;
        }
        fclose(fileWt);
      }
      matrix_copy_KCCK_to_KC( ref_first_wt_bwdupd_f32, ref_first_wt_bwdupd_f32_kc, C[0], C[1], bc, bk );
      matrix_copy_KCCK_to_KC_bf16( fil_libxsmm[0], first_wt_bwdupd_bf16, C[0], C[1], bc, bk );
      libxsmm_convert_bf16_f32( first_wt_bwdupd_bf16, first_wt_bwdupd_f32, C[0]*C[1] );
      /* compare */
      libxsmm_matdiff(&norms_bwd, LIBXSMM_DATATYPE_F32, C[0]*C[1], 1, ref_first_wt_bwdupd_f32_kc, first_wt_bwdupd_f32, 0, 0);
      printf("##########################################\n");
      printf("#   Correctness - First bwdupd wt        #\n");
      printf("##########################################\n");
      printf("L1 reference  : %.25g\n", norms_bwd.l1_ref);
      printf("L1 test       : %.25g\n", norms_bwd.l1_tst);
      printf("L2 abs.error  : %.24f\n", norms_bwd.l2_abs);
      printf("L2 rel.error  : %.24f\n", norms_bwd.l2_rel);
      printf("Linf abs.error: %.24f\n", norms_bwd.linf_abs);
      printf("Linf rel.error: %.24f\n", norms_bwd.linf_rel);
      printf("Check-norm    : %.24f\n", norms_bwd.normf_rel);
      libxsmm_matdiff_reduce(&diff, &norms_bwd);

      free(ref_last_act_fwd_f32);
      free(ref_first_wt_bwdupd_f32);
      free(ref_first_wt_bwdupd_f32_kc);
      free(first_wt_bwdupd_bf16);
    }
#endif
    free(first_wt_bwdupd_f32);
    free(last_act_fwd_f32);
#endif

    gflop = 0.0;
    for ( i = num_layers-1; i > 0; --i) {
      gflop += (6.0*(double)MB*(double)C[i]*(double)C[i+1]*(double)iters) / (1000.0*1000.0*1000.0);
    }
    gflop += (4.0*(double)MB*(double)C[0]*(double)C[1]*(double)iters) / (1000.0*1000.0*1000.0);
    printf("GFLOP  = %.5g\n", gflop/(double)iters);
    printf("fp time = %.5g\n", ((double)(l_total/iters)));
    printf("GFLOPS  = %.5g\n", gflop/l_total);
    printf("PERFDUMP,BP,%s,%i,%i,", LIBXSMM_VERSION, nThreads, MB );
    for ( i = 0; i < num_layers; ++i ) {
      printf("%i,", C[i] );
    }
    printf("%f,%f\n", ((double)(l_total/iters)), gflop/l_total);
  }

  /* deallocate data */
  if ( scratch != NULL ) {
    libxsmm_free(scratch);
  }

  for ( i = 0; i < num_layers; ++i ) {
    if ( i == 0 ) {
      libxsmm_free(act_libxsmm[i]);
      libxsmm_free(delact_libxsmm[i]);
    }
    libxsmm_free(act_libxsmm[i+1]);
    libxsmm_free(delact_libxsmm[i+1]);

    libxsmm_free(fil_libxsmm[i]);
    libxsmm_free(delfil_libxsmm[i]);
    libxsmm_free(bias_libxsmm[i]);
    libxsmm_free(delbias_libxsmm[i]);
    libxsmm_free(relumask_libxsmm[i]);
    libxsmm_free(fil_master[i]);
  }
  libxsmm_free(act_libxsmm[num_layers+1]);
  libxsmm_free(label_libxsmm);

  free( my_opt );
  free( my_fc_fwd );
  free( my_fc_bwd );

  free( act_libxsmm );
  free( delact_libxsmm );
  free( fil_master );
  free( fil_libxsmm );
  free( delfil_libxsmm );
  free( bias_libxsmm );
  free( delbias_libxsmm );
  free( relumask_libxsmm );

  free( C );

  /* some empty lines at the end */
  printf("\n\n\n");

  return 0;
}

