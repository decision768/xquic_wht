/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _XQC_SCHEDULER_ORIGINAL_MINRTT_H_INCLUDED_
#define _XQC_SCHEDULER_ORIGINAL_MINRTT_H_INCLUDED_

#include <xquic/xquic.h>

#define XQC_ORIGINAL_MINRTT_INITIAL_RTT_US 250000
#define XQC_ORIGINAL_MINRTT_PTO_THRESHOLD  2

extern const xqc_scheduler_callback_t xqc_original_minrtt_scheduler_cb;

/* Restore the fixed upstream baseline only when this scheduler is selected. */
void xqc_original_minrtt_apply_settings(xqc_conn_settings_t *settings);

#endif /* _XQC_SCHEDULER_ORIGINAL_MINRTT_H_INCLUDED_ */
