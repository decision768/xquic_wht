#ifndef XQC_OBSERVER_H
#define XQC_OBSERVER_H
#include <xquic/xquic.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Sink must copy a record before returning. Called only on the engine thread.
 * Returns nonzero if the record was dropped. No callback may reenter XQUIC. */
typedef int (*xqc_observer_sink_pt)(void *, int, const char *);
typedef struct xqc_observer_s xqc_observer_t;
typedef struct xqc_observer_stream_s xqc_observer_stream_t;
XQC_EXPORT_PUBLIC_API int xqc_observer_attach(xqc_engine_t *, const xqc_cid_t *,
    const char *, const char *, xqc_observer_sink_pt, void *);
XQC_EXPORT_PUBLIC_API void xqc_observer_sample(xqc_engine_t *, const xqc_cid_t *,
    const char *, uint64_t, uint64_t, uint64_t);
XQC_EXPORT_PUBLIC_API void xqc_observer_bind(xqc_engine_t *, const xqc_cid_t *,
    uint64_t, const char *, int, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
XQC_EXPORT_PUBLIC_API void xqc_observer_path_interface(xqc_engine_t *, const xqc_cid_t *, uint64_t, const char *);
XQC_EXPORT_PUBLIC_API void xqc_observer_body(xqc_engine_t *, const xqc_cid_t *,
    uint64_t, uint64_t);
XQC_EXPORT_PUBLIC_API void xqc_observer_phase(xqc_engine_t *, const xqc_cid_t *,
    const char *, uint64_t, uint64_t, uint64_t);
XQC_EXPORT_PUBLIC_API void xqc_observer_detach(xqc_engine_t *, const xqc_cid_t *);
#ifdef __cplusplus
}
#endif
#endif
