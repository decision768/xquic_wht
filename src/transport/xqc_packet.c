/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include <stdio.h>
#include <xquic/xquic.h>
#include "src/transport/xqc_packet.h"
#include "src/transport/xqc_packet_out.h"
#include "src/transport/xqc_conn.h"
#include "src/common/xqc_algorithm.h"
#include "src/common/utils/vint/xqc_variable_len_int.h"
#include "src/transport/xqc_defs.h"
#include "src/transport/xqc_send_ctl.h"
#include "src/transport/xqc_recv_record.h"
#include "src/transport/xqc_packet_parser.h"
#include "src/transport/xqc_utils.h"
#include "src/transport/xqc_engine.h"
#include "src/tls/xqc_tls.h"

#ifndef XQC_TARGET_DEFERRED_FRAME_APPLY
#define XQC_TARGET_DEFERRED_FRAME_APPLY 1
#endif



static const char * const pkt_type_2_str[XQC_PTYPE_NUM] = {
    [XQC_PTYPE_INIT]                = "INIT",
    [XQC_PTYPE_0RTT]                = "0RTT",
    [XQC_PTYPE_HSK]                 = "HSK",
    [XQC_PTYPE_RETRY]               = "RETRY",
    [XQC_PTYPE_SHORT_HEADER]        = "SHORT_HEADER",
    [XQC_PTYPE_VERSION_NEGOTIATION] = "VERSION_NEGOTIATION",
};

const char *
xqc_pkt_type_2_str(xqc_pkt_type_t pkt_type)
{
    return pkt_type_2_str[pkt_type];
}

xqc_encrypt_level_t
xqc_packet_type_to_enc_level(xqc_pkt_type_t pkt_type)
{
    switch (pkt_type) {
    case XQC_PTYPE_INIT:
        return XQC_ENC_LEV_INIT;
    case XQC_PTYPE_0RTT:
        return XQC_ENC_LEV_0RTT;
    case XQC_PTYPE_HSK:
        return XQC_ENC_LEV_HSK;
    case XQC_PTYPE_SHORT_HEADER:
        return XQC_ENC_LEV_1RTT;
    default:
        return XQC_ENC_LEV_INIT;
    }
}

xqc_pkt_num_space_t
xqc_packet_type_to_pns(xqc_pkt_type_t pkt_type)
{
    switch (pkt_type) {
    case XQC_PTYPE_INIT:
        return XQC_PNS_INIT;
    case XQC_PTYPE_0RTT:
        return XQC_PNS_APP_DATA;
    case XQC_PTYPE_HSK:
        return XQC_PNS_HSK;
    case XQC_PTYPE_SHORT_HEADER:
        return XQC_PNS_APP_DATA;
    default:
        return XQC_PNS_N;
    }
}

xqc_pkt_type_t
xqc_state_to_pkt_type(xqc_connection_t *conn)
{
    switch (conn->conn_state) {
    case XQC_CONN_STATE_CLIENT_INIT:
    case XQC_CONN_STATE_CLIENT_INITIAL_SENT:
    case XQC_CONN_STATE_CLIENT_INITIAL_RECVD:
    case XQC_CONN_STATE_SERVER_INIT:
    case XQC_CONN_STATE_SERVER_INITIAL_RECVD:
    case XQC_CONN_STATE_SERVER_INITIAL_SENT:
        return XQC_PTYPE_INIT;
    case XQC_CONN_STATE_CLIENT_HANDSHAKE_RECVD:
    case XQC_CONN_STATE_CLIENT_HANDSHAKE_SENT:
    case XQC_CONN_STATE_SERVER_HANDSHAKE_SENT:
    case XQC_CONN_STATE_SERVER_HANDSHAKE_RECVD:
        return XQC_PTYPE_HSK;
    default:
        return XQC_PTYPE_SHORT_HEADER;
    }
}

uint8_t
xqc_packet_need_decrypt(xqc_packet_t *pkt)
{
    /* packets don't need decryption */
    return xqc_has_packet_number(pkt);
}

/* 
 * a client MUST discard Initial keys when it first sends a Handshake packet 
 * and a server MUST discard Initial keys when it first successfully processes a Handshake packet
 * we should ignore initial packet if xqc_conn_check_initial_packet_from_cur_state return XQC_FALSE
 */
static inline xqc_int_t
xqc_conn_check_initial_packet_from_cur_state(xqc_conn_state_t cur_state)
{
    switch(cur_state) {
        case XQC_CONN_STATE_CLIENT_INIT:
        case XQC_CONN_STATE_CLIENT_INITIAL_SENT:
        case XQC_CONN_STATE_CLIENT_INITIAL_RECVD:
        case XQC_CONN_STATE_SERVER_INIT:
        case XQC_CONN_STATE_SERVER_INITIAL_RECVD:
        case XQC_CONN_STATE_SERVER_INITIAL_SENT:
        case XQC_CONN_STATE_SERVER_HANDSHAKE_SENT:
        case XQC_CONN_STATE_SERVER_HANDSHAKE_RECVD:
            return XQC_TRUE;
        default:
            return XQC_FALSE;

    }
    return XQC_TRUE;
}


xqc_int_t
xqc_conn_deferred_rx_init(xqc_connection_t *conn)
{
#if XQC_TARGET_DEFERRED_FRAME_APPLY
    if (conn->conn_type != XQC_CONN_TYPE_CLIENT) {
        return XQC_OK;
    }

    conn->deferred_rx_pool = xqc_calloc(XQC_DEFERRED_RX_WORK_CAPACITY,
                                        sizeof(xqc_deferred_rx_work_t));
    if (conn->deferred_rx_pool == NULL) {
        return -XQC_EMALLOC;
    }

    for (uint32_t i = 0; i < XQC_DEFERRED_RX_WORK_CAPACITY; ++i) {
        conn->deferred_rx_pool[i].next = conn->deferred_rx_free;
        conn->deferred_rx_free = &conn->deferred_rx_pool[i];
    }
    conn->deferred_rx_enabled = XQC_TRUE;
    if (conn->target_rx_diag.enabled) {
        printf("[diag_rx_work_pool] enabled=1 capacity=%u slot_bytes=%zu"
               " total_bytes=%zu\n",
               XQC_DEFERRED_RX_WORK_CAPACITY,
               sizeof(xqc_deferred_rx_work_t),
               sizeof(xqc_deferred_rx_work_t)
                   * XQC_DEFERRED_RX_WORK_CAPACITY);
    }
#endif
    return XQC_OK;
}


void
xqc_conn_deferred_rx_destroy(xqc_connection_t *conn)
{
    if (conn->deferred_rx_pool != NULL) {
        xqc_free(conn->deferred_rx_pool);
    }
    conn->deferred_rx_pool = NULL;
    conn->deferred_rx_free = NULL;
    conn->deferred_rx_pending_head = NULL;
    conn->deferred_rx_pending_tail = NULL;
    conn->deferred_rx_pending_count = 0;
    conn->deferred_rx_enabled = XQC_FALSE;
}


static xqc_deferred_rx_work_t *
xqc_conn_deferred_rx_acquire(xqc_connection_t *conn)
{
    xqc_deferred_rx_work_t *work = conn->deferred_rx_free;
    if (work != NULL) {
        conn->deferred_rx_free = work->next;
        xqc_memzero(work, sizeof(*work));
    }
    return work;
}


static void
xqc_conn_deferred_rx_release(xqc_connection_t *conn,
    xqc_deferred_rx_work_t *work)
{
    work->next = conn->deferred_rx_free;
    conn->deferred_rx_free = work;
}


static void
xqc_conn_deferred_rx_enqueue(xqc_connection_t *conn,
    xqc_deferred_rx_work_t *work)
{
    work->next = NULL;
    if (conn->deferred_rx_pending_tail != NULL) {
        conn->deferred_rx_pending_tail->next = work;

    } else {
        conn->deferred_rx_pending_head = work;
    }
    conn->deferred_rx_pending_tail = work;
    conn->deferred_rx_pending_count++;
}


xqc_int_t
xqc_conn_apply_deferred_rx_frames(xqc_connection_t *conn)
{
    while (conn->deferred_rx_pending_head != NULL) {
        xqc_deferred_rx_work_t *work = conn->deferred_rx_pending_head;
        conn->deferred_rx_pending_head = work->next;
        if (conn->deferred_rx_pending_head == NULL) {
            conn->deferred_rx_pending_tail = NULL;
        }
        conn->deferred_rx_pending_count--;

        if (work->conn_generation != conn->rx_generation) {
            xqc_conn_deferred_rx_release(conn, work);
            continue;
        }

        xqc_usec_t start = XQC_TARGET_RX_DIAG_START(conn);
        xqc_int_t ret = xqc_process_frames(conn, &work->packet_in);
        XQC_TARGET_RX_DIAG_ADD(conn, frame_process_us, start);
        if (conn->target_rx_diag.enabled) {
            conn->target_rx_diag.deferred_rx_applied++;
        }
        xqc_conn_deferred_rx_release(conn, work);

        if (ret != XQC_OK) {
            if (conn->target_rx_diag.enabled) {
                conn->target_rx_diag.deferred_rx_apply_error++;
            }
            return ret;
        }
    }

    return XQC_OK;
}

xqc_int_t
xqc_packet_parse_single(xqc_connection_t *c, xqc_packet_in_t *packet_in)
{
    unsigned char *pos = packet_in->pos;
    xqc_int_t ret = XQC_ERROR;

    if (XQC_BUFF_LEFT_SIZE(pos, packet_in->last) == 0) {
        xqc_log(c->log, XQC_LOG_ERROR,
                "|xqc_packet_parse_short_header error:%d|", ret);
        return -XQC_EILLPKT;
    }

    /* short header */
    if (XQC_PACKET_IS_SHORT_HEADER(pos)) {
        ret = xqc_packet_parse_short_header(c, packet_in);
        if (ret != XQC_OK) {
            xqc_log(c->log, XQC_LOG_ERROR,
                    "|xqc_packet_parse_short_header error:%d|", ret);
            return ret;
        }

        /* check handshake */
        if (!xqc_conn_check_handshake_completed(c)) {
            /* handshake not completed, buffer packets */
            xqc_log(c->log, XQC_LOG_WARN,
                    "|delay|buff 1RTT packet before handshake completed|");
            xqc_conn_buff_undecrypt_packet_in(packet_in, c, XQC_ENC_LEV_1RTT);
            return -XQC_EWAITING;
        }

    } else if (XQC_PACKET_IS_LONG_HEADER(pos)) {    /* long header */
        /* buffer packets if key is not ready */
        if (XQC_PACKET_LONG_HEADER_GET_TYPE(packet_in->pos) == XQC_PTYPE_0RTT) {
            c->conn_flag |= XQC_CONN_FLAG_HAS_0RTT;

            if (!xqc_tls_is_key_ready(c->tls, XQC_ENC_LEV_0RTT, XQC_KEY_TYPE_RX_READ)) {
                /* buffer packets */
                xqc_log(c->log, XQC_LOG_INFO, "|delay|buff 0RTT before 0rtt_key_ready|");
                xqc_conn_buff_undecrypt_packet_in(packet_in, c, XQC_ENC_LEV_0RTT);
                return -XQC_EWAITING;
            }

        } else if (XQC_PACKET_LONG_HEADER_GET_TYPE(packet_in->pos) == XQC_PTYPE_HSK
                   && !xqc_tls_is_key_ready(c->tls, XQC_ENC_LEV_HSK, XQC_KEY_TYPE_RX_READ))
        {
            /* buffer packets */
            xqc_log(c->log, XQC_LOG_INFO, "|delay|buff HSK before hs_rx_key_ready|");
            xqc_conn_buff_undecrypt_packet_in(packet_in, c, XQC_ENC_LEV_HSK);
            return -XQC_EWAITING;
        } else if (XQC_PTYPE_INIT == XQC_PACKET_LONG_HEADER_GET_TYPE(packet_in->pos)) {
            if (XQC_UNLIKELY(xqc_conn_check_initial_packet_from_cur_state(c->conn_state) == XQC_FALSE)) {
                xqc_log(c->log, XQC_LOG_INFO, "|initial packet should be discarded"
                        "|curr_stat:%d|", c->conn_state);
                return -XQC_EIGNORE_PKT;
            }
        }

        /* parse packet */
        ret = xqc_packet_parse_long_header(c, packet_in);
        if (XQC_OK != ret) {
            xqc_log(c->log, XQC_LOG_ERROR,
                    "|xqc_packet_parse_long_header error:%d|", ret);
            return ret;
        }

    } else {
        xqc_log(c->log, XQC_LOG_INFO, "unknown packet type, first byte[%d], "
                "skip all buf, skip length: %d", pos[0], packet_in->last - packet_in->pos);
        return -XQC_EIGNORE_PKT;
    }

    return ret;
}

xqc_int_t
xqc_packet_decrypt_single(xqc_connection_t *c, xqc_packet_in_t *packet_in)
{
    xqc_int_t ret = XQC_OK;

    /* 
     * remember the last position of udp packet, as the last pointer
     * of packet_in will be changed during processing QUIC packets 
     */
    unsigned char *last = packet_in->last;

    /* decrypt packet */
    xqc_usec_t target_stage_start = XQC_TARGET_RX_DIAG_START(c);
    ret = xqc_packet_decrypt(c, packet_in);
    XQC_TARGET_RX_DIAG_ADD(c, packet_decrypt_us, target_stage_start);
    if (ret == XQC_OK) {
        xqc_path_ctx_t *path = xqc_conn_find_path_by_path_id(
            c, packet_in->pi_path_id);
        if (path != NULL) {
            xqc_send_ctl_diag_record_auth(path->path_send_ctl,
                packet_in->pi_pkt.pkt_pns, packet_in->pkt_recv_time);
        }
        /* process frames */
        xqc_log(c->log, XQC_LOG_DEBUG, "|pkt_type:%s|pkt_num:%ui|",
                xqc_pkt_type_2_str(packet_in->pi_pkt.pkt_type), packet_in->pi_pkt.pkt_num);

        xqc_bool_t frames_deferred = XQC_FALSE;
#if XQC_TARGET_DEFERRED_FRAME_APPLY
        if (c->deferred_rx_enabled
            && packet_in->pi_pkt.pkt_type == XQC_PTYPE_SHORT_HEADER
            && c->conn_state < XQC_CONN_STATE_CLOSING)
        {
            xqc_deferred_rx_work_t *work = xqc_conn_deferred_rx_acquire(c);
            if (work == NULL) {
                if (c->target_rx_diag.enabled) {
                    c->target_rx_diag.deferred_rx_pool_exhausted++;
                    c->target_rx_diag.deferred_rx_fallback++;
                }
                ret = xqc_conn_apply_deferred_rx_frames(c);
                if (ret != XQC_OK) {
                    return ret;
                }

            } else {
                xqc_frame_type_bit_t frame_types = 0;
                xqc_bool_t deferable = XQC_FALSE;
                uint32_t frame_count = 0;
                target_stage_start = XQC_TARGET_RX_DIAG_START(c);
                ret = xqc_scan_frames(packet_in->pos,
                    packet_in->last - packet_in->pos, work->frames,
                    XQC_RX_FRAME_DESC_CAPACITY, &frame_count, &frame_types,
                    &deferable);
                if (target_stage_start != 0) {
                    c->target_rx_diag.frame_scan_count++;
                    XQC_TARGET_RX_DIAG_ADD(c, frame_scan_us,
                                           target_stage_start);
                }
                if (ret != XQC_OK) {
                    xqc_conn_deferred_rx_release(c, work);
                    return ret;
                }

                if (deferable && path != NULL
                    && packet_in->decode_payload_len <= sizeof(work->decrypted))
                {
                    work->conn_generation = c->rx_generation;
                    work->key_generation = c->key_generation;
                    work->path_generation = path != NULL
                        ? path->rx_generation : 0;
                    work->packet_in = *packet_in;
                    xqc_init_list_head(&work->packet_in.pi_list);
                    xqc_memcpy(work->decrypted, packet_in->decode_payload,
                               packet_in->decode_payload_len);
                    work->packet_in.buf = work->decrypted;
                    work->packet_in.decode_payload = work->decrypted;
                    work->packet_in.decode_payload_size =
                        sizeof(work->decrypted);
                    work->packet_in.pos = work->decrypted;
                    work->packet_in.last = work->decrypted
                        + packet_in->decode_payload_len;
                    work->packet_in.pi_frame_types = frame_types;
                    work->frame_count = frame_count;

                    /* Publish the work before the caller records ACK state. */
                    xqc_conn_deferred_rx_enqueue(c, work);
                    packet_in->pi_frame_types = frame_types;
                    packet_in->pos = packet_in->decode_payload
                        + packet_in->decode_payload_len;
                    frames_deferred = XQC_TRUE;
                    if (c->target_rx_diag.enabled) {
                        c->target_rx_diag.deferred_rx_queued++;
                    }

                } else {
                    xqc_conn_deferred_rx_release(c, work);
                    if (c->target_rx_diag.enabled) {
                        c->target_rx_diag.deferred_rx_fallback++;
                    }
                    /* Preserve arrival order before running a serial frame. */
                    ret = xqc_conn_apply_deferred_rx_frames(c);
                    if (ret != XQC_OK) {
                        return ret;
                    }
                }
            }
        }
#endif

        if (!frames_deferred) {
            target_stage_start = XQC_TARGET_RX_DIAG_START(c);
            ret = xqc_process_frames(c, packet_in);
            XQC_TARGET_RX_DIAG_ADD(c, frame_process_us, target_stage_start);
            if (ret != XQC_OK) {
                xqc_log(c->log, XQC_LOG_ERROR,
                        "|xqc_process_frames error|%d|", ret);
                return ret;
            }
        }

    } else {
        if (ret == -XQC_TLS_DATA_REJECT) {
            xqc_log(c->log, XQC_LOG_DEBUG, "|decrypt early data reject, continue|");
            ret = -XQC_EIGNORE_PKT;

        } else {
            xqc_log_event(c->log, TRA_PACKET_DROPPED, "decrypt data error", ret, 
                xqc_pkt_type_2_str(packet_in->pi_pkt.pkt_type), packet_in->pi_pkt.pkt_num);
            c->packet_dropped_count ++;
            ret = -XQC_EDECRYPT;
            /* don't close connection, just drop the packet */
        }
        return ret;
    }

    /* restore the udp packet's end */
    packet_in->last = last;
    return ret;
}

xqc_int_t
xqc_packet_process_single(xqc_connection_t *c,
    xqc_packet_in_t *packet_in)
{
    xqc_int_t ret = XQC_ERROR;

    /* parse packet */
    xqc_usec_t target_parse_start = XQC_TARGET_RX_DIAG_START(c);
    ret = xqc_packet_parse_single(c, packet_in);
    XQC_TARGET_RX_DIAG_ADD(c, packet_parse_us, target_parse_start);
    if (XQC_OK != ret) {
        return ret;
    }

    /* those packets with no packet number, don't need to be decrypt or put into CC */
    if (!xqc_packet_need_decrypt(&packet_in->pi_pkt)) {
        return XQC_OK;
    }

    /* decrypt packet */
    ret = xqc_packet_decrypt_single(c, packet_in);
    if (ret != XQC_OK) {
        return ret;
    }

    return XQC_OK;
}




