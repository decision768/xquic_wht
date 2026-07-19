#include <CUnit/CUnit.h>
#include <limits.h>

#include "xqc_scheduler_diag_test.h"
#include "src/transport/scheduler/xqc_scheduler_common.h"
#include "src/transport/xqc_send_ctl.h"

void
xqc_test_scheduler_cwnd_classifier(void)
{
    CU_ASSERT_EQUAL(xqc_scheduler_classify_cwnd_block(
        10000, 9500, 0, 1200, 1), XQC_SCHED_BLOCK_REAL_INFLIGHT);

    CU_ASSERT_EQUAL(xqc_scheduler_classify_cwnd_block(
        10000, 7000, 2500, 1200, 1),
        XQC_SCHED_BLOCK_SCHEDULED_RESERVATION);

    CU_ASSERT_EQUAL(xqc_scheduler_classify_cwnd_block(
        10000, 6000, 2000, 1200, 1), XQC_SCHED_BLOCK_NONE);

    CU_ASSERT_EQUAL(xqc_scheduler_classify_cwnd_block(
        10000, 9500, 5000, 1200, 0), XQC_SCHED_BLOCK_NONE);

    CU_ASSERT_EQUAL(xqc_send_ctl_calc_effective_cwnd(10000, 0), 10000);
    CU_ASSERT_EQUAL(xqc_send_ctl_calc_effective_cwnd(10000, 8000), 8000);
    CU_ASSERT_EQUAL(xqc_send_ctl_calc_effective_cwnd(UINT64_MAX, 0),
        UINT_MAX);

    uint64_t effective_cwnd = xqc_send_ctl_calc_effective_cwnd(10000, 8000);
    CU_ASSERT_EQUAL(xqc_scheduler_classify_cwnd_block(
        effective_cwnd, 7000, 0, 1200, 1),
        XQC_SCHED_BLOCK_REAL_INFLIGHT);

    CU_ASSERT_EQUAL(xqc_scheduler_classify_cwnd_block(
        10000, 8800, 0, 1200, 1), XQC_SCHED_BLOCK_NONE);

    CU_ASSERT_EQUAL(xqc_scheduler_classify_cwnd_block(
        10000, UINT64_MAX, UINT64_MAX, 1200, 1),
        XQC_SCHED_BLOCK_REAL_INFLIGHT);
}
