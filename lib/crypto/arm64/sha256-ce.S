/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sha2-ce-core.S - core SHA-224/SHA-256 transform using v8 Crypto Extensions
 *
 * Copyright (C) 2014 Linaro Ltd <ard.biesheuvel@linaro.org>
 */

#include <linux/linkage.h>
#include <asm/assembler.h>

	.text
	.arch		armv8-a+crypto

	dga		.req	q20
	dgav		.req	v20
	dgb		.req	q21
	dgbv		.req	v21

	t0		.req	v22
	t1		.req	v23

	dg0q		.req	q24
	dg0v		.req	v24
	dg1q		.req	q25
	dg1v		.req	v25
	dg2q		.req	q26
	dg2v		.req	v26

	.macro		add_only, ev, rc, s0
	mov		dg2v.16b, dg0v.16b
	.ifeq		\ev
	add		t1.4s, v\s0\().4s, \rc\().4s
	sha256h		dg0q, dg1q, t0.4s
	sha256h2	dg1q, dg2q, t0.4s
	.else
	.ifnb		\s0
	add		t0.4s, v\s0\().4s, \rc\().4s
	.endif
	sha256h		dg0q, dg1q, t1.4s
	sha256h2	dg1q, dg2q, t1.4s
	.endif
	.endm

	.macro		add_update, ev, rc, s0, s1, s2, s3
	sha256su0	v\s0\().4s, v\s1\().4s
	add_only	\ev, \rc, \s1
	sha256su1	v\s0\().4s, v\s2\().4s, v\s3\().4s
	.endm

	/*
	 * The SHA-256 round constants
	 */
	.section	".rodata", "a"
	.align		4
.Lsha2_rcon:
	.word		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5
	.word		0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5
	.word		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3
	.word		0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174
	.word		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc
	.word		0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da
	.word		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7
	.word		0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967
	.word		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13
	.word		0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85
	.word		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3
	.word		0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070
	.word		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5
	.word		0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3
	.word		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208
	.word		0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2

	/*
	 * size_t __sha256_ce_transform(struct sha256_block_state *state,
	 *				const u8 *data, size_t nblocks);
	 */
	.text
SYM_FUNC_START(__sha256_ce_transform)
	/* load round constants */
	adr_l		x8, .Lsha2_rcon
	ld1		{ v0.4s- v3.4s}, [x8], #64
	ld1		{ v4.4s- v7.4s}, [x8], #64
	ld1		{ v8.4s-v11.4s}, [x8], #64
	ld1		{v12.4s-v15.4s}, [x8]

	/* load state */
	ld1		{dgav.4s, dgbv.4s}, [x0]

	/* load input */
0:	ld1		{v16.4s-v19.4s}, [x1], #64
	sub		x2, x2, #1

CPU_LE(	rev32		v16.16b, v16.16b	)
CPU_LE(	rev32		v17.16b, v17.16b	)
CPU_LE(	rev32		v18.16b, v18.16b	)
CPU_LE(	rev32		v19.16b, v19.16b	)

	add		t0.4s, v16.4s, v0.4s
	mov		dg0v.16b, dgav.16b
	mov		dg1v.16b, dgbv.16b

	add_update	0,  v1, 16, 17, 18, 19
	add_update	1,  v2, 17, 18, 19, 16
	add_update	0,  v3, 18, 19, 16, 17
	add_update	1,  v4, 19, 16, 17, 18

	add_update	0,  v5, 16, 17, 18, 19
	add_update	1,  v6, 17, 18, 19, 16
	add_update	0,  v7, 18, 19, 16, 17
	add_update	1,  v8, 19, 16, 17, 18

	add_update	0,  v9, 16, 17, 18, 19
	add_update	1, v10, 17, 18, 19, 16
	add_update	0, v11, 18, 19, 16, 17
	add_update	1, v12, 19, 16, 17, 18

	add_only	0, v13, 17
	add_only	1, v14, 18
	add_only	0, v15, 19
	add_only	1

	/* update state */
	add		dgav.4s, dgav.4s, dg0v.4s
	add		dgbv.4s, dgbv.4s, dg1v.4s

	/* return early if voluntary preemption is needed */
	cond_yield	1f, x5, x6

	/* handled all input blocks? */
	cbnz		x2, 0b

	/* store new state */
1:	st1		{dgav.4s, dgbv.4s}, [x0]
	mov		x0, x2
	ret
SYM_FUNC_END(__sha256_ce_transform)
