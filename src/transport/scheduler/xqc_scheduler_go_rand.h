/**
 * Copyright 2009 The Go Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license. See
 * THIRD_PARTY_LICENSES.md.
 */

#ifndef _XQC_SCHEDULER_GO_RAND_H_INCLUDED_
#define _XQC_SCHEDULER_GO_RAND_H_INCLUDED_

#include <stdint.h>

#define XQC_GO_RAND_STATE_LEN 607

typedef struct {
    int       tap;
    int       feed;
    uint64_t  vec[XQC_GO_RAND_STATE_LEN];
} xqc_go_rand_t;

void xqc_go_rand_seed(xqc_go_rand_t *rng, int64_t seed);

uint32_t xqc_go_rand_intn_100(xqc_go_rand_t *rng);

#endif
