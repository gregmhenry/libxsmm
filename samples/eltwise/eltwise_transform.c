/******************************************************************************
* Copyright (c) Intel Corporation - All rights reserved.                      *
* This file is part of the LIBXSMM library.                                   *
*                                                                             *
* For information on the license, see the LICENSE file.                       *
* Further information: https://github.com/hfp/libxsmm/                        *
* SPDX-License-Identifier: BSD-3-Clause                                       *
******************************************************************************/
/* Alexander Heinecke (Intel Corp.)
******************************************************************************/
#include <libxsmm.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

void test_normal_to_normalT_64bit( libxsmm_blasint M, libxsmm_blasint N, libxsmm_blasint ldi, libxsmm_blasint ldo ) {
  double *in;
  double *out, *out_gold;
  unsigned int i, j;
  unsigned int s;
  libxsmm_meltw_transform_param trans_param;
  libxsmm_meltw_transform_flags trans_flags;

  if ( M > ldi ) {
    fprintf( stderr, "test_normal_to_normalT_64bit: ldi needs to be equal to or bigger than M\n");
    exit(-1);
  }
  if ( N > ldo ) {
    fprintf( stderr, "test_normal_to_normalT_64bit: ldo needs to be equal to or bigger than N\n");
    exit(-1);
  }

  in       = (double*) libxsmm_aligned_malloc( sizeof(double)*N*ldi, 64);
  out      = (double*) libxsmm_aligned_malloc( sizeof(double)*M*ldo, 64);
  out_gold = (double*) libxsmm_aligned_malloc( sizeof(double)*M*ldo, 64);

  /* init in */
  for ( i = 0; i < N; ++i ) {
    for ( j = 0; j < M; ++j ) {
      in[(i*ldi)+j] = (double)(((i*ldi)+j)%4096);
    }
  }

  /* init out */
  for ( i = 0; i < M*ldo; ++i ) {
    out[i] = 0;
  }
  for ( i = 0; i < M*ldo; ++i ) {
    out_gold[i] = 0;
  }

  /* compute out_gold */
  for ( i = 0; i < N; ++i ) {
    for ( j = 0; j < M; ++j ) {
      out_gold[(j*ldo)+i] = in[(i*ldi)+j];
    }
  }

  /* use jited tranpose */
  trans_param.in_ptr  = (void*)in;
  trans_param.out_ptr = (void*)out;
  trans_flags = LIBXSMM_MELTW_FLAG_TRANSFORM_NORM_TO_NORMT;
  libxsmm_meltwfunction_transform trans_kernel = libxsmm_dispatch_meltw_transform(M, N, &ldi, &ldo, LIBXSMM_DATATYPE_F64, LIBXSMM_DATATYPE_F64, trans_flags);
  if ( trans_kernel == NULL ) {
    fprintf( stderr, "JIT for NORM_TO_NORMT TPP. Bailing...!\n");
    exit(-1);
  }
  trans_kernel( &trans_param );

  /* compare result */
  s = 0;
  for ( i = 0; i < M; ++i ) {
    for ( j = 0; j < N; ++j ) {
      if ( out_gold[(i*ldo)+j] != out[(i*ldo)+j] ) {
        printf("error at possition i=%i, j=%i, %f, %f\n", i, j, out[(i*ldo)+j], out_gold[(i*ldo)+j]);
        s = 1;
      }
    }
  }
  if ( s == 0 ) {
    printf("SUCCESS 64bit\n");
  } else {
    printf("FAILURE 64bit\n");
  }

  libxsmm_free( out_gold );
  libxsmm_free( out );
  libxsmm_free( in );
}

void test_normal_to_normalT_32bit( libxsmm_blasint M, libxsmm_blasint N, libxsmm_blasint ldi, libxsmm_blasint ldo ) {
  float *in;
  float *out, *out_gold;
  unsigned int i, j;
  unsigned int s;
  libxsmm_meltw_transform_param trans_param;
  libxsmm_meltw_transform_flags trans_flags;

  if ( M > ldi ) {
    fprintf( stderr, "test_normal_to_normalT_32bit: ldi needs to be equal to or bigger than M\n");
    exit(-1);
  }
  if ( N > ldo ) {
    fprintf( stderr, "test_normal_to_normalT_32bit: ldo needs to be equal to or bigger than N\n");
    exit(-1);
  }

  in       = (float*) libxsmm_aligned_malloc( sizeof(float)*N*ldi, 64);
  out      = (float*) libxsmm_aligned_malloc( sizeof(float)*M*ldo, 64);
  out_gold = (float*) libxsmm_aligned_malloc( sizeof(float)*M*ldo, 64);

  /* init in */
  for ( i = 0; i < N; ++i ) {
    for ( j = 0; j < M; ++j ) {
      in[(i*ldi)+j] = (float)(((i*ldi)+j)%4096);
    }
  }

  /* init out */
  for ( i = 0; i < M*ldo; ++i ) {
    out[i] = 0;
  }
  for ( i = 0; i < M*ldo; ++i ) {
    out_gold[i] = 0;
  }

  /* compute out_gold */
  for ( i = 0; i < N; ++i ) {
    for ( j = 0; j < M; ++j ) {
      out_gold[(j*ldo)+i] = in[(i*ldi)+j];
    }
  }

  /* use jited tranpose */
  trans_param.in_ptr  = (void*)in;
  trans_param.out_ptr = (void*)out;
  trans_flags = LIBXSMM_MELTW_FLAG_TRANSFORM_NORM_TO_NORMT;
  libxsmm_meltwfunction_transform trans_kernel = libxsmm_dispatch_meltw_transform(M, N, &ldi, &ldo, LIBXSMM_DATATYPE_F32, LIBXSMM_DATATYPE_F32, trans_flags);
  if ( trans_kernel == NULL ) {
    fprintf( stderr, "JIT for NORM_TO_NORMT TPP. Bailing...!\n");
    exit(-1);
  }
  trans_kernel( &trans_param );

  /* compare result */
  s = 0;
  for ( i = 0; i < M; ++i ) {
    for ( j = 0; j < ldo; ++j ) {
      if ( out_gold[(i*ldo)+j] != out[(i*ldo)+j] ) {
        printf("error at possition i=%i, j=%i, %f, %f\n", i, j, out[(i*ldo)+j], out_gold[(i*ldo)+j]);
        s = 1;
      }
    }
  }
  if ( s == 0 ) {
    printf("SUCCESS 32bit\n");
  } else {
    printf("FAILURE 32bit\n");
  }

  libxsmm_free( out_gold );
  libxsmm_free( out );
  libxsmm_free( in );
}

void test_normal_to_normalT_16bit( libxsmm_blasint M, libxsmm_blasint N, libxsmm_blasint ldi, libxsmm_blasint ldo ) {
  unsigned short *in;
  unsigned short *out, *out_gold;
  unsigned int i, j;
  unsigned int s;
  libxsmm_meltw_transform_param trans_param;
  libxsmm_meltw_transform_flags trans_flags;

  if ( M > ldi ) {
    fprintf( stderr, "test_normal_to_normalT_16bit: ldi needs to be equal to or bigger than M\n");
    exit(-1);
  }
  if ( N > ldo ) {
    fprintf( stderr, "test_normal_to_normalT_16bit: ldo needs to be equal to or bigger than N\n");
    exit(-1);
  }

  in       = (unsigned short*) libxsmm_aligned_malloc( sizeof(unsigned short)*N*ldi, 64);
  out      = (unsigned short*) libxsmm_aligned_malloc( sizeof(unsigned short)*M*ldo, 64);
  out_gold = (unsigned short*) libxsmm_aligned_malloc( sizeof(unsigned short)*M*ldo, 64);

  /* init in */
  for ( i = 0; i < N; ++i ) {
    for ( j = 0; j < M; ++j ) {
      in[(i*ldi)+j] = (unsigned short)(((i*ldi)+j)%4096);
    }
  }

  /* init out */
  for ( i = 0; i < M*ldo; ++i ) {
    out[i] = 0;
  }
  for ( i = 0; i < M*ldo; ++i ) {
    out_gold[i] = 0;
  }

  /* compute out_gold */
  for ( i = 0; i < N; ++i ) {
    for ( j = 0; j < M; ++j ) {
      out_gold[(j*ldo)+i] = in[(i*ldi)+j];
    }
  }

  /* use jited tranpose */
  trans_param.in_ptr  = (void*)in;
  trans_param.out_ptr = (void*)out;
  trans_flags = LIBXSMM_MELTW_FLAG_TRANSFORM_NORM_TO_NORMT;
  libxsmm_meltwfunction_transform trans_kernel = libxsmm_dispatch_meltw_transform(M, N, &ldi, &ldo, LIBXSMM_DATATYPE_BF16, LIBXSMM_DATATYPE_BF16, trans_flags);
  if ( trans_kernel == NULL ) {
    fprintf( stderr, "JIT for NORM_TO_NORMT TPP. Bailing...!\n");
    exit(-1);
  }
  trans_kernel( &trans_param );

  /* compare result */
  s = 0;
  for ( i = 0; i < M; ++i ) {
    for ( j = 0; j < N; ++j ) {
      if ( out_gold[(i*ldo)+j] != out[(i*ldo)+j] ) {
        printf("error at possition i=%i, j=%i, %u, %u\n", i, j, out[(i*ldo)+j], out_gold[(i*ldo)+j]);
        s = 1;
      }
    }
  }
  if ( s == 0 ) {
    printf("SUCCESS 16bit\n");
  } else {
    printf("FAILURE 16bit\n");
  }

  libxsmm_free( out_gold );
  libxsmm_free( out );
  libxsmm_free( in );
}

void test_normal_to_normalT_08bit( libxsmm_blasint M, libxsmm_blasint N, libxsmm_blasint ldi, libxsmm_blasint ldo ) {
  unsigned char *in;
  unsigned char *out, *out_gold;
  unsigned int i, j;
  unsigned int s;
  libxsmm_meltw_transform_param trans_param;
  libxsmm_meltw_transform_flags trans_flags;

  if ( M > ldi ) {
    fprintf( stderr, "test_normal_to_normalT_08bit: ldi needs to be equal to or bigger than M\n");
    exit(-1);
  }
  if ( N > ldo ) {
    fprintf( stderr, "test_normal_to_normalT_08bit: ldo needs to be equal to or bigger than N\n");
    exit(-1);
  }

  in       = (unsigned char*) libxsmm_aligned_malloc( sizeof(unsigned char)*N*ldi, 64);
  out      = (unsigned char*) libxsmm_aligned_malloc( sizeof(unsigned char)*M*ldo, 64);
  out_gold = (unsigned char*) libxsmm_aligned_malloc( sizeof(unsigned char)*M*ldo, 64);

  /* init in */
  for ( i = 0; i < N; ++i ) {
    for ( j = 0; j < M; ++j ) {
      in[(i*ldi)+j] = (unsigned char)(((i*ldi)+j)%4096);
    }
  }

  /* init out */
  for ( i = 0; i < M*ldo; ++i ) {
    out[i] = 0;
  }
  for ( i = 0; i < M*ldo; ++i ) {
    out_gold[i] = 0;
  }

  /* compute out_gold */
  for ( i = 0; i < N; ++i ) {
    for ( j = 0; j < M; ++j ) {
      out_gold[(j*ldo)+i] = in[(i*ldi)+j];
    }
  }

  /* use jited tranpose */
  trans_param.in_ptr  = (void*)in;
  trans_param.out_ptr = (void*)out;
  trans_flags = LIBXSMM_MELTW_FLAG_TRANSFORM_NORM_TO_NORMT;
  libxsmm_meltwfunction_transform trans_kernel = libxsmm_dispatch_meltw_transform(M, N, &ldi, &ldo, LIBXSMM_DATATYPE_I8, LIBXSMM_DATATYPE_I8, trans_flags);
  if ( trans_kernel == NULL ) {
    fprintf( stderr, "JIT for NORM_TO_NORMT TPP. Bailing...!\n");
    exit(-1);
  }
  trans_kernel( &trans_param );

  /* compare result */
  s = 0;
  for ( i = 0; i < M; ++i ) {
    for ( j = 0; j < N; ++j ) {
      if ( out_gold[(i*ldo)+j] != out[(i*ldo)+j] ) {
        printf("error at possition i=%i, j=%i, %u, %u\n", i, j, out[(i*ldo)+j], out_gold[(i*ldo)+j]);
        s = 1;
      }
    }
  }
  if ( s == 0 ) {
    printf("SUCCESS 08bit\n");
  } else {
    printf("FAILURE 08bit\n");
  }

  libxsmm_free( out_gold );
  libxsmm_free( out );
  libxsmm_free( in );
}

void test_vnni_to_vnniT_16bit( libxsmm_blasint M, libxsmm_blasint N, libxsmm_blasint ldi, libxsmm_blasint ldo ) {
  unsigned short *in, *in_vnni;
  unsigned short *out, *out_gold, *out_vnni;
  unsigned int i, j, j2;
  unsigned int s;

  libxsmm_meltw_transform_param trans_param;
  libxsmm_meltw_transform_flags trans_flags;

  if ( M > ldi ) {
    fprintf( stderr, "test_vnni_to_vnniT_16bit: ldi needs to be equal to or bigger than M\n");
    exit(-1);
  }
  if ( N > ldo ) {
    fprintf( stderr, "test_vnni_to_vnniT_16bit: ldo needs to be equal to or bigger than N\n");
    exit(-1);
  }

  in       = (unsigned short*)libxsmm_aligned_malloc( sizeof(unsigned short)*ldi*N, 64);
  in_vnni  = (unsigned short*)libxsmm_aligned_malloc( sizeof(unsigned short)*ldi*N, 64);
  out      = (unsigned short*)libxsmm_aligned_malloc( sizeof(unsigned short)*M*ldo, 64);
  out_gold = (unsigned short*)libxsmm_aligned_malloc( sizeof(unsigned short)*M*ldo, 64);
  out_vnni = (unsigned short*)libxsmm_aligned_malloc( sizeof(unsigned short)*M*ldo, 64);

  /* init in */
  for ( i = 0; i < N; ++i ) {
    for ( j = 0; j < M; ++j ) {
      in[(i*ldi)+j] = (unsigned short)(((i*ldi)+j)%112);
    }
  }
  /* to vnni */
  for ( j = 0; j < N/2; ++j ) {
    for ( i = 0; i < M ; ++i ) {
      for( j2 = 0; j2 < 2; ++j2 ) {
        in_vnni[(j*ldi*2)+(i*2)+j2] = in[(((j*2)+j2)*ldi)+i];
      }
    }
  }

  /* init out */
  for ( i = 0; i < M*ldo; ++i ) {
    out[i] = 0;
  }
  for ( i = 0; i < M*ldo; ++i ) {
    out_gold[i] = 0;
  }

  /* compute out_gold */
  for ( i = 0; i < N; ++i ) {
    for ( j = 0; j < M; ++j ) {
      out_gold[(j*ldo)+i] = in[(i*ldi)+j];
    }
  }

  /* to vnni */
  for ( j = 0; j < M/2; ++j ) {
    for ( i = 0; i < N ; ++i ) {
      for( j2 = 0; j2 < 2; ++j2 ) {
        out_vnni[(j*ldo*2)+(i*2)+j2] = out_gold[(((j*2)+j2)*ldo)+i];
      }
    }
  }

  /* use jited tranpose */
  trans_param.in_ptr  = (void*)in_vnni;
  trans_param.out_ptr = (void*)out;
  trans_flags = LIBXSMM_MELTW_FLAG_TRANSFORM_VNNI_TO_VNNIT;
  libxsmm_meltwfunction_transform trans_kernel = libxsmm_dispatch_meltw_transform(M, N, &ldi, &ldo, LIBXSMM_DATATYPE_BF16, LIBXSMM_DATATYPE_BF16, trans_flags);
  if ( trans_kernel == NULL ) {
    fprintf( stderr, "JIT for VNNI_TO_VNNIT TPP. Bailing...!\n");
    exit(-1);
  }
  trans_kernel( &trans_param );

  /* compare result */
  s = 0;
  for ( i = 0; i < M; ++i ) {
    for ( j = 0; j < N; ++j ) {
      if ( out_vnni[(i*ldo)+j] != out[(i*ldo)+j] ) {
        printf("error at possition i=%i, j=%i\n", i, j);
        s = 1;
      }
    }
  }
  if ( s == 0 ) {
    printf("SUCCESS 16bit\n");
  } else {
    printf("FAILURE 16bit\n");
  }

  libxsmm_free( out_gold );
  libxsmm_free( out );
  libxsmm_free( out_vnni );
  libxsmm_free( in );
  libxsmm_free( in_vnni );
}

void test_norm_to_vnni_16bit( libxsmm_blasint M, libxsmm_blasint N, libxsmm_blasint ldi, libxsmm_blasint ldo ) {
  unsigned short *in;
  unsigned short *out, *out_gold;
  unsigned int i, j, j2;
  unsigned int s;
  libxsmm_blasint Nn = N + (N%2);

  libxsmm_meltw_transform_param trans_param;
  libxsmm_meltw_transform_flags trans_flags;

  if ( M > ldi ) {
    fprintf( stderr, "test_norm_to_vnni_16bit: ldi needs to be equal to or bigger than M\n");
    exit(-1);
  }
  if ( M > ldo ) {
    fprintf( stderr, "test_norm_to_vnni_16bit: ldo needs to be equal to or bigger than M\n");
    exit(-1);
  }

  in       = (unsigned short*)libxsmm_aligned_malloc( sizeof(unsigned short)*ldi*Nn, 64);
  out      = (unsigned short*)libxsmm_aligned_malloc( sizeof(unsigned short)*ldo*Nn, 64);
  out_gold = (unsigned short*)libxsmm_aligned_malloc( sizeof(unsigned short)*ldo*Nn, 64);

  /* init in */
  for ( i = 0; i < N; ++i ) {
    for ( j = 0; j < M; ++j ) {
      in[(i*ldi)+j] = (unsigned short)(((i*ldi)+j)%112);
    }
  }
  for ( i = N; i < Nn; ++i ) {
    for ( j = 0; j < M; ++j ) {
      in[(i*ldi)+j] = 0;
    }
  }

  /* init out */
  for ( i = 0; i < ldo*Nn; ++i ) {
    out[i] = 0;
  }
  for ( i = 0; i < ldo*Nn; ++i ) {
    out_gold[i] = 0;
  }

  /* to vnni */
  for ( j = 0; j < Nn/2; ++j ) {
    for ( i = 0; i < M ; ++i ) {
      for( j2 = 0; j2 < 2; ++j2 ) {
        out_gold[(j*ldo*2)+(i*2)+j2] = in[(((j*2)+j2)*ldi)+i];
      }
    }
  }

  /* use jited tranpose */
  trans_param.in_ptr  = (void*)in;
  trans_param.out_ptr = (void*)out;
  if ( N % 2 == 1 ) {
    trans_flags = LIBXSMM_MELTW_FLAG_TRANSFORM_NORM_TO_VNNI_PAD;
  } else {
    trans_flags = LIBXSMM_MELTW_FLAG_TRANSFORM_NORM_TO_VNNI;
  }
  libxsmm_meltwfunction_transform trans_kernel = libxsmm_dispatch_meltw_transform(M, N, &ldi, &ldo, LIBXSMM_DATATYPE_BF16, LIBXSMM_DATATYPE_BF16, trans_flags);
  if ( trans_kernel == NULL ) {
    fprintf( stderr, "JIT for NORM_TO_VNNI TPP. Bailing...!\n");
    exit(-1);
  }
  trans_kernel( &trans_param );

  /* compare result */
  s = 0;
  for ( i = 0; i < Nn; ++i ) {
    for ( j = 0; j < M; ++j ) {
      if ( out_gold[(i*ldo)+j] != out[(i*ldo)+j] ) {
        printf("error at possition i=%i, j=%i, %i %i\n", i, j, out_gold[(i*ldo)+j], out[(i*ldo)+j]);
        s = 1;
      }
    }
  }
  if ( s == 0 ) {
    printf("SUCCESS 16bit\n");
  } else {
    printf("FAILURE 16bit\n");
  }

  libxsmm_free( out_gold );
  libxsmm_free( out );
  libxsmm_free( in );
}

#if 0
void test_vnni_to_vnniT_08bit() {
  unsigned char *in, *in_vnni;
  unsigned char *out, *out_gold, *out_vnni;
  unsigned int i, j, j2;
  unsigned int s;

  in       = (unsigned char*)_mm_malloc( sizeof(unsigned char)*64*64, 64);
  in_vnni  = (unsigned char*)_mm_malloc( sizeof(unsigned char)*64*64, 64);
  out      = (unsigned char*)_mm_malloc( sizeof(unsigned char)*64*64, 64);
  out_gold = (unsigned char*)_mm_malloc( sizeof(unsigned char)*64*64, 64);
  out_vnni = (unsigned char*)_mm_malloc( sizeof(unsigned char)*64*64, 64);

  /* init in */
  for ( i = 0; i < 64; ++i ) {
    for ( j = 0; j < 64; ++j ) {
      in[(i*64)+j] = (unsigned char)(((i*64)+j)%112);
    }
  }
  /* to vnni */
  for ( j = 0; j < 64/4; ++j ) {
    for ( i = 0; i < 64 ; ++i ) {
      for( j2 = 0; j2 < 4; ++j2 ) {
        in_vnni[(j*64*4)+(i*4)+j2] = in[(((j*4)+j2)*64)+i];
      }
    }
  }

  /* init out */
  for ( i = 0; i < 64*64; ++i ) {
    out[i] = 0;
  }
  for ( i = 0; i < 64*64; ++i ) {
    out_gold[i] = 0;
  }

  /* compute out_gold */
  for ( i = 0; i < 64; ++i ) {
    for ( j = 0; j < 64; ++j ) {
      out_gold[(j*64)+i] = in[(i*64)+j];
    }
  }

  /* to vnni */
  for ( j = 0; j < 64/4; ++j ) {
    for ( i = 0; i < 64 ; ++i ) {
      for( j2 = 0; j2 < 4; ++j2 ) {
        out_vnni[(j*64*4)+(i*4)+j2] = out_gold[(((j*4)+j2)*64)+i];
      }
    }
  }

  /* use our tranpose */
  vnni_transpose_08bit_64by64( in_vnni, out );

  /* compare result */
  s = 0;
  for ( i = 0; i < 64; ++i ) {
    for ( j = 0; j < 64; ++j ) {
      if ( out_vnni[(i*64)+j] != out[(i*64)+j] ) {
        printf("error at possition i=%i, j=%i\n", i, j);
        s = 1;
      }
    }
  }
  if ( s == 0 ) {
    printf("SUCCESS  8bit\n");
  } else {
    printf("FAILURE  8bit\n");
  }

  _mm_free( out_gold );
  _mm_free( out );
  _mm_free( out_vnni );
  _mm_free( in );
  _mm_free( in_vnni );
}
#endif

int main( int argc, char* argv[] ) {
  libxsmm_blasint dtype;
  char op;
  libxsmm_blasint M;
  libxsmm_blasint N;
  libxsmm_blasint ldi;
  libxsmm_blasint ldo;

  if ( argc != 7 ) {
    printf(" Error! Usage: %s [T/V/R] [8/4/2/1] [M] [N] [ldi] [ldo]\n", argv[0] );
    exit(-1);
  }

  op  = *(argv[1]);
  dtype = atoi(argv[2]);
  M     = atoi(argv[3]);
  N     = atoi(argv[4]);
  ldi   = atoi(argv[5]);
  ldo   = atoi(argv[6]);

  if ( op == 'T' && dtype == 8 ) {
    printf("Testing 64bit Norm to Norm Transpose\n");
    test_normal_to_normalT_64bit( M, N, ldi, ldo );
  } else if ( op == 'T' && dtype == 4 ) {
    printf("Testing 32bit Norm to Norm Transpose\n");
    test_normal_to_normalT_32bit( M, N, ldi, ldo );
  } else if ( op == 'T' && dtype == 2 ) {
    printf("Testing 16bit Norm to Norm Transpose\n");
    test_normal_to_normalT_16bit( M, N, ldi, ldo );
  } else if ( op == 'T' && dtype == 1 ) {
    printf("Testing 08bit Norm to Norm Transpose\n");
    test_normal_to_normalT_08bit( M, N, ldi, ldo );
  } else if ( op == 'R' && dtype == 2 ) {
    printf("Testing 16bit VNNI to VNNI Transpose\n");
    test_vnni_to_vnniT_16bit( M, N, ldi, ldo );
  } else if ( op == 'V' && dtype == 2 ) {
    printf("Testing 16bit NORM to VNNI Reformat\n");
    test_norm_to_vnni_16bit( M, N, ldi, ldo );
  } else {
    printf(" Not implemented case! Usage: %s [T/V/R] [8/4/2/1] [M] [N] [ldi] [ldo]\n", argv[0] );
    exit(-1);
  }
}
