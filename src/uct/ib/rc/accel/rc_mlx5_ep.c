/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2014.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#include "rc_mlx5.h"

#if HAVE_DECL_IBV_CMD_MODIFY_QP
#include <infiniband/driver.h>
#endif
#include <uct/ib/mlx5/ib_mlx5_log.h>
#include <ucs/arch/cpu.h>
#include <ucs/sys/compiler.h>
#include <arpa/inet.h> /* For htonl */

/*
 *
 * Helper function for buffer-copy post.
 * Adds the descriptor to the callback queue.
 */
static UCS_F_ALWAYS_INLINE void
uct_rc_mlx5_txqp_bcopy_post(uct_rc_iface_t *iface, uct_rc_txqp_t *txqp, uct_ib_mlx5_txwq_t *txwq,
                            unsigned opcode, unsigned length,
                            /* RDMA */ uint64_t rdma_raddr, uct_rkey_t rdma_rkey,
                            uint8_t fm_ce_se, uint32_t imm_val_be,
                            uct_rc_iface_send_desc_t *desc, const void *buffer,
                            uct_ib_log_sge_t *log_sge)
{
    desc->super.sn = txwq->sw_pi;
    uct_rc_mlx5_txqp_dptr_post(iface, IBV_QPT_RC, txqp, txwq,
                               opcode, buffer, length, &desc->lkey,
                               rdma_raddr, uct_ib_md_direct_rkey(rdma_rkey),
                               0, 0, 0, 0,
                               NULL, NULL, 0, fm_ce_se, imm_val_be, INT_MAX, log_sge);
    uct_rc_txqp_add_send_op(txqp, &desc->super);
}

/*
 * Helper function for zero-copy post.
 * Adds user completion to the callback queue.
 */
static UCS_F_ALWAYS_INLINE ucs_status_t
uct_rc_mlx5_ep_zcopy_post(uct_rc_mlx5_ep_t *ep,
                          unsigned opcode, const uct_iov_t *iov, size_t iovcnt,
                          /* SEND */ uint8_t am_id, const void *am_hdr, unsigned am_hdr_len,
                          /* RDMA */ uint64_t rdma_raddr, uct_rkey_t rdma_rkey,
                          /* TAG  */ uct_tag_t tag, uint32_t app_ctx, uint32_t ib_imm_be,
                          int force_sig, uct_completion_t *comp)
{
    uct_rc_iface_t *iface  = ucs_derived_of(ep->super.super.super.iface,
                                            uct_rc_iface_t);
    uint16_t sn;

    UCT_RC_CHECK_RES(iface, &ep->super);

    sn = ep->tx.wq.sw_pi;
    uct_rc_mlx5_txqp_dptr_post_iov(iface, IBV_QPT_RC,
                                   &ep->super.txqp, &ep->tx.wq,
                                   opcode, iov, iovcnt,
                                   am_id, am_hdr, am_hdr_len,
                                   rdma_raddr, uct_ib_md_direct_rkey(rdma_rkey),
                                   tag, app_ctx, ib_imm_be,
                                   NULL, NULL, 0,
                                   (comp == NULL) ? force_sig : MLX5_WQE_CTRL_CQ_UPDATE,
                                   UCT_IB_MAX_ZCOPY_LOG_SGE(&iface->super));

    uct_rc_txqp_add_send_comp(iface, &ep->super.txqp, comp, sn);
    return UCS_INPROGRESS;
}

static ucs_status_t UCS_F_ALWAYS_INLINE
uct_rc_mlx5_ep_put_short_inline(uct_ep_h tl_ep, const void *buffer, unsigned length,
                                uint64_t remote_addr, uct_rkey_t rkey)
{
    uct_rc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_rc_iface_t);
    uct_rc_mlx5_ep_t *ep  = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);

    UCT_RC_MLX5_CHECK_PUT_SHORT(length, 0);
    UCT_RC_CHECK_RES(iface, &ep->super);

    uct_rc_mlx5_txqp_inline_post(iface, IBV_QPT_RC,
                                 &ep->super.txqp, &ep->tx.wq,
                                 MLX5_OPCODE_RDMA_WRITE,
                                 buffer, length, 0, 0, 0,
                                 remote_addr, uct_ib_md_direct_rkey(rkey),
                                 NULL, NULL, 0, 0, INT_MAX);
    UCT_TL_EP_STAT_OP(&ep->super.super, PUT, SHORT, length);
    return UCS_OK;
}

static ucs_status_t UCS_F_ALWAYS_INLINE
uct_rc_mlx5_ep_am_short_inline(uct_ep_h tl_ep, uint8_t id, uint64_t hdr,
                               const void *payload, unsigned length)
{
    uct_rc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_rc_iface_t);
    uct_rc_mlx5_ep_t *ep  = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);

    UCT_RC_MLX5_CHECK_AM_SHORT(id, length, 0);

    UCT_RC_CHECK_RES(iface, &ep->super);
    UCT_RC_CHECK_FC(iface, &ep->super, id);

    uct_rc_mlx5_txqp_inline_post(iface, IBV_QPT_RC,
                                 &ep->super.txqp, &ep->tx.wq,
                                 MLX5_OPCODE_SEND,
                                 payload, length,
                                 id, hdr, 0,
                                 0, 0,
                                 NULL, NULL, 0,
                                 MLX5_WQE_CTRL_SOLICITED,
                                 INT_MAX);
    UCT_TL_EP_STAT_OP(&ep->super.super, AM, SHORT, sizeof(hdr) + length);
    UCT_RC_UPDATE_FC(iface, &ep->super, id);
    return UCS_OK;
}

#if HAVE_IBV_EXP_DM
static ucs_status_t UCS_F_ALWAYS_INLINE
uct_rc_mlx5_ep_short_dm(uct_rc_mlx5_ep_t *ep, uct_rc_mlx5_dm_copy_data_t *cache,
                        size_t hdr_len, const void *payload, unsigned length,
                        unsigned opcode, uint8_t fm_ce_se,
                        uint64_t rdma_raddr, uct_rkey_t rdma_rkey)
{
    uct_rc_mlx5_iface_t *iface = ucs_derived_of(ep->super.super.super.iface, uct_rc_mlx5_iface_t);
    uct_rc_iface_t *rc_iface   = &iface->super;
    uct_rc_iface_send_desc_t *desc;
    void *buffer;
    ucs_status_t status;
    uct_ib_log_sge_t log_sge;

    status = uct_rc_mlx5_common_dm_make_data(&iface->mlx5_common, &iface->super,
                                             cache, hdr_len, payload, length, &desc,
                                             &buffer, &log_sge);
    if (ucs_unlikely(UCS_STATUS_IS_ERR(status))) {
        return status;
    }

    uct_rc_mlx5_txqp_bcopy_post(rc_iface, &ep->super.txqp, &ep->tx.wq,
                                opcode, hdr_len + length,
                                rdma_raddr, rdma_rkey, fm_ce_se,
                                0, desc, buffer,
                                log_sge.num_sge ? &log_sge : NULL);
    return UCS_OK;
}
#endif

ucs_status_t
uct_rc_mlx5_ep_put_short(uct_ep_h tl_ep, const void *buffer, unsigned length,
                         uint64_t remote_addr, uct_rkey_t rkey)
{
#if HAVE_IBV_EXP_DM
    uct_rc_mlx5_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_rc_mlx5_iface_t);
    uct_rc_iface_t *rc_iface   = &iface->super;
    uct_rc_mlx5_ep_t *ep       = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);
    ucs_status_t status;

    if (ucs_likely((length <= UCT_IB_MLX5_PUT_MAX_SHORT(0)) ||
                   !iface->mlx5_common.dm.dm)) {
#endif
        return uct_rc_mlx5_ep_put_short_inline(tl_ep, buffer, length, remote_addr, rkey);
#if HAVE_IBV_EXP_DM
    }

    UCT_CHECK_LENGTH(length, 0, iface->mlx5_common.dm.seg_len, "put_short");
    UCT_RC_CHECK_RES(rc_iface, &ep->super);
    status =  uct_rc_mlx5_ep_short_dm(ep, NULL, 0, buffer, length,
                                      MLX5_OPCODE_RDMA_WRITE,
                                      MLX5_WQE_CTRL_CQ_UPDATE,
                                      remote_addr, rkey);
    if (UCS_STATUS_IS_ERR(status)) {
        return status;
    }

    UCT_TL_EP_STAT_OP(&ep->super.super, PUT, SHORT, length);
    return UCS_OK;
#endif
}

ssize_t uct_rc_mlx5_ep_put_bcopy(uct_ep_h tl_ep, uct_pack_callback_t pack_cb,
                                 void *arg, uint64_t remote_addr, uct_rkey_t rkey)
{
    uct_rc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_rc_iface_t);
    uct_rc_mlx5_ep_t *ep  = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);
    uct_rc_iface_send_desc_t *desc;
    size_t length;

    UCT_RC_CHECK_RES(iface, &ep->super);
    UCT_RC_IFACE_GET_TX_PUT_BCOPY_DESC(iface, &iface->tx.mp,
                                       desc, pack_cb, arg, length);

    uct_rc_mlx5_txqp_bcopy_post(iface, &ep->super.txqp, &ep->tx.wq,
                                MLX5_OPCODE_RDMA_WRITE, length, remote_addr,
                                rkey, MLX5_WQE_CTRL_CQ_UPDATE, 0, desc, desc + 1,
                                NULL);
    UCT_TL_EP_STAT_OP(&ep->super.super, PUT, BCOPY, length);
    return length;
}

ucs_status_t uct_rc_mlx5_ep_put_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov, size_t iovcnt,
                                      uint64_t remote_addr, uct_rkey_t rkey,
                                      uct_completion_t *comp)
{
    uct_ib_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_ib_iface_t);
    uct_rc_mlx5_ep_t *ep  = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);
    ucs_status_t status;

    UCT_CHECK_IOV_SIZE(iovcnt, uct_ib_iface_get_max_iov(iface),
                       "uct_rc_mlx5_ep_put_zcopy");
    UCT_CHECK_LENGTH(uct_iov_total_length(iov, iovcnt), 0, UCT_IB_MAX_MESSAGE_SIZE,
                     "put_zcopy");

    status = uct_rc_mlx5_ep_zcopy_post(ep, MLX5_OPCODE_RDMA_WRITE, iov, iovcnt,
                                       0, NULL, 0, remote_addr, rkey, 0ul, 0, 0,
                                       MLX5_WQE_CTRL_CQ_UPDATE, comp);
    UCT_TL_EP_STAT_OP_IF_SUCCESS(status, &ep->super.super, PUT, ZCOPY,
                                 uct_iov_total_length(iov, iovcnt));
    return status;
}

ucs_status_t uct_rc_mlx5_ep_get_bcopy(uct_ep_h tl_ep,
                                      uct_unpack_callback_t unpack_cb,
                                      void *arg, size_t length,
                                      uint64_t remote_addr, uct_rkey_t rkey,
                                      uct_completion_t *comp)
{
    uct_rc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_rc_iface_t);
    uct_rc_mlx5_ep_t *ep  = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);
    uct_rc_iface_send_desc_t *desc;

    UCT_CHECK_LENGTH(length, 0, iface->super.config.seg_size, "get_bcopy");
    UCT_RC_CHECK_RES(iface, &ep->super);
    UCT_RC_IFACE_GET_TX_GET_BCOPY_DESC(iface, &iface->tx.mp, desc,
                                       unpack_cb, comp, arg, length);

    uct_rc_mlx5_txqp_bcopy_post(iface, &ep->super.txqp, &ep->tx.wq,
                                MLX5_OPCODE_RDMA_READ, length, remote_addr,
                                rkey, MLX5_WQE_CTRL_CQ_UPDATE, 0, desc, desc + 1,
                                NULL);
    UCT_TL_EP_STAT_OP(&ep->super.super, GET, BCOPY, length);
    return UCS_INPROGRESS;
}

ucs_status_t uct_rc_mlx5_ep_get_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov, size_t iovcnt,
                                      uint64_t remote_addr, uct_rkey_t rkey,
                                      uct_completion_t *comp)
{
    uct_ib_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_ib_iface_t);
    uct_rc_mlx5_ep_t *ep  = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);
    ucs_status_t status;

    UCT_CHECK_IOV_SIZE(iovcnt, uct_ib_iface_get_max_iov(iface),
                       "uct_rc_mlx5_ep_get_zcopy");
    UCT_CHECK_LENGTH(uct_iov_total_length(iov, iovcnt),
                     iface->config.max_inl_resp + 1, UCT_IB_MAX_MESSAGE_SIZE,
                     "get_zcopy");

    status = uct_rc_mlx5_ep_zcopy_post(ep, MLX5_OPCODE_RDMA_READ, iov, iovcnt,
                                       0, NULL, 0, remote_addr, rkey, 0ul, 0, 0,
                                       MLX5_WQE_CTRL_CQ_UPDATE, comp);
    UCT_TL_EP_STAT_OP_IF_SUCCESS(status, &ep->super.super, GET, ZCOPY,
                                 uct_iov_total_length(iov, iovcnt));
    return status;
}

ucs_status_t
uct_rc_mlx5_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t hdr,
                        const void *payload, unsigned length)
{
#if HAVE_IBV_EXP_DM
    uct_rc_mlx5_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_rc_mlx5_iface_t);
    uct_rc_iface_t *rc_iface   = &iface->super;
    uct_rc_mlx5_ep_t *ep       = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);
    ucs_status_t status;
    uct_rc_mlx5_dm_copy_data_t cache;

    if (ucs_likely((sizeof(uct_rc_am_short_hdr_t) + length <= UCT_IB_MLX5_AM_MAX_SHORT(0)) ||
                   !iface->mlx5_common.dm.dm)) {
#endif
        return uct_rc_mlx5_ep_am_short_inline(tl_ep, id, hdr, payload, length);
#if HAVE_IBV_EXP_DM
    }

    UCT_CHECK_LENGTH(length + sizeof(uct_rc_am_short_hdr_t), 0,
                     iface->mlx5_common.dm.seg_len, "am_short");
    UCT_CHECK_AM_ID(id);
    UCT_RC_CHECK_RES(rc_iface, &ep->super);
    UCT_RC_CHECK_FC(rc_iface, &ep->super, id);

    uct_rc_am_hdr_fill(&cache.am_hdr.rc_hdr, id);
    cache.am_hdr.am_hdr = hdr;

    status = uct_rc_mlx5_ep_short_dm(ep, &cache, sizeof(cache.am_hdr), payload, length,
                                     MLX5_OPCODE_SEND,
                                     MLX5_WQE_CTRL_SOLICITED | MLX5_WQE_CTRL_CQ_UPDATE,
                                     0, 0);
    if (UCS_STATUS_IS_ERR(status)) {
        return status;
    }

    UCT_TL_EP_STAT_OP(&ep->super.super, AM, SHORT, sizeof(cache.am_hdr) + length);
    UCT_RC_UPDATE_FC(rc_iface, &ep->super, id);
    return UCS_OK;
#endif
}

ssize_t uct_rc_mlx5_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                                uct_pack_callback_t pack_cb, void *arg,
                                unsigned flags)
{
    uct_rc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_rc_iface_t);
    uct_rc_mlx5_ep_t *ep  = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);
    uct_rc_iface_send_desc_t *desc;
    size_t length;

    UCT_CHECK_AM_ID(id);
    UCT_RC_CHECK_RES(iface, &ep->super);
    UCT_RC_CHECK_FC(iface, &ep->super, id);
    UCT_RC_IFACE_GET_TX_AM_BCOPY_DESC(iface, &iface->tx.mp, desc,
                                      id, pack_cb, arg, &length);

    uct_rc_mlx5_txqp_bcopy_post(iface, &ep->super.txqp, &ep->tx.wq,
                                MLX5_OPCODE_SEND, sizeof(uct_rc_hdr_t) + length,
                                0, 0, MLX5_WQE_CTRL_SOLICITED, 0, desc, desc + 1,
                                NULL);
    UCT_TL_EP_STAT_OP(&ep->super.super, AM, BCOPY, length);
    UCT_RC_UPDATE_FC(iface, &ep->super, id);
    return length;
}

ucs_status_t uct_rc_mlx5_ep_am_zcopy(uct_ep_h tl_ep, uint8_t id, const void *header,
                                     unsigned header_length, const uct_iov_t *iov,
                                     size_t iovcnt, unsigned flags,
                                     uct_completion_t *comp)
{
    uct_rc_mlx5_ep_t *ep  = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);
    uct_rc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_rc_iface_t);
    ucs_status_t status;

    UCT_CHECK_IOV_SIZE(iovcnt, UCT_IB_MLX5_AM_ZCOPY_MAX_IOV,
                       "uct_rc_mlx5_ep_am_zcopy");
    UCT_RC_MLX5_CHECK_AM_ZCOPY(id, header_length, uct_iov_total_length(iov, iovcnt),
                               iface->super.config.seg_size, 0);
    UCT_RC_CHECK_FC(iface, &ep->super, id);

    status = uct_rc_mlx5_ep_zcopy_post(ep, MLX5_OPCODE_SEND, iov, iovcnt,
                                       id, header, header_length, 0, 0, 0ul, 0, 0,
                                       MLX5_WQE_CTRL_SOLICITED, comp);
    if (ucs_likely(status >= 0)) {
        UCT_TL_EP_STAT_OP(&ep->super.super, AM, ZCOPY,
                          header_length + uct_iov_total_length(iov, iovcnt));
        UCT_RC_UPDATE_FC(iface, &ep->super, id);
    }
    return status;
}

static UCS_F_ALWAYS_INLINE void
uct_rc_mlx5_ep_atomic_post(uct_rc_mlx5_ep_t *ep, unsigned opcode,
                           uct_rc_iface_send_desc_t *desc, unsigned length,
                           uint64_t remote_addr, uct_rkey_t rkey,
                           uint64_t compare_mask, uint64_t compare,
                           uint64_t swap_mask, uint64_t swap_add, int signal)
{
    uct_rc_iface_t *iface  = ucs_derived_of(ep->super.super.super.iface,
                                            uct_rc_iface_t);
    uint32_t ib_rkey       = uct_ib_resolve_atomic_rkey(rkey, ep->super.atomic_mr_offset,
                                                        &remote_addr);

    desc->super.sn = ep->tx.wq.sw_pi;
    uct_rc_mlx5_txqp_dptr_post(iface, IBV_QPT_RC,
                               &ep->super.txqp, &ep->tx.wq,
                               opcode, desc + 1, length, &desc->lkey,
                               remote_addr, ib_rkey,
                               compare_mask, compare, swap_mask, swap_add,
                               NULL, NULL, 0, signal, 0, INT_MAX, NULL);

    UCT_TL_EP_STAT_ATOMIC(&ep->super.super);
    uct_rc_txqp_add_send_op(&ep->super.txqp, &desc->super);
}

static UCS_F_ALWAYS_INLINE ucs_status_t
uct_rc_mlx5_ep_atomic_fop(uct_rc_mlx5_ep_t *ep, int opcode, void *result, int ext,
                          unsigned length, uint64_t remote_addr, uct_rkey_t rkey,
                          uint64_t compare_mask, uint64_t compare,
                          uint64_t swap_mask, uint64_t swap_add, uct_completion_t *comp)
{
    uct_rc_mlx5_iface_t *iface = ucs_derived_of(ep->super.super.super.iface,
                                                uct_rc_mlx5_iface_t);
    uct_rc_iface_send_desc_t *desc;

    UCT_RC_CHECK_RES(&iface->super, &ep->super);
    UCT_RC_IFACE_GET_TX_ATOMIC_FETCH_DESC(&iface->super, &iface->mlx5_common.tx.atomic_desc_mp,
                                          desc, uct_rc_iface_atomic_handler(&iface->super, ext,
                                                                            length),
                                          result, comp);
    uct_rc_mlx5_ep_atomic_post(ep, opcode, desc, length, remote_addr, rkey,
                               compare_mask, compare, swap_mask, swap_add,
                               MLX5_WQE_CTRL_CQ_UPDATE);
    return UCS_INPROGRESS;
}

static ucs_status_t UCS_F_ALWAYS_INLINE
uct_rc_mlx5_ep_atomic_op_post(uct_ep_h tl_ep, unsigned opcode, unsigned size,
                              uint64_t value, uint64_t remote_addr, uct_rkey_t rkey)
{
    uct_rc_mlx5_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_rc_mlx5_iface_t);
    uct_rc_mlx5_ep_t *ep       = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);
    uct_rc_iface_send_desc_t *desc;
    int op;
    uint64_t compare_mask;
    uint64_t compare;
    uint64_t swap_mask;
    uint64_t swap;
    int      ext; /* not used here */
    ucs_status_t status;

    UCT_RC_CHECK_RES(&iface->super, &ep->super);
    UCT_RC_MLX5_CHECK_ATOMIC_OPS(opcode, size, UCT_RC_MLX5_ATOMIC_OPS);

    status = uct_rc_mlx5_iface_common_atomic_data(opcode, size, value, &op, &compare_mask,
                                                  &compare, &swap_mask, &swap, &ext);
    if (ucs_unlikely(UCS_STATUS_IS_ERR(status))) {
        return status;
    }

    UCT_RC_IFACE_GET_TX_ATOMIC_DESC(&iface->super, &iface->mlx5_common.tx.atomic_desc_mp, desc);

    uct_rc_mlx5_ep_atomic_post(ep, op, desc, size, remote_addr, rkey,
                               compare_mask, compare, swap_mask, swap, 0);
    return UCS_OK;
}

static ucs_status_t UCS_F_ALWAYS_INLINE
uct_rc_mlx5_ep_atomic_fop_post(uct_ep_h tl_ep, unsigned opcode, unsigned size,
                               uint64_t value, void *result,
                               uint64_t remote_addr, uct_rkey_t rkey,
                               uct_completion_t *comp)
{
    uct_rc_mlx5_ep_t *ep = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);
    int op;
    uint64_t compare_mask;
    uint64_t compare;
    uint64_t swap_mask;
    uint64_t swap;
    int      ext;
    ucs_status_t status;

    UCT_RC_MLX5_CHECK_ATOMIC_OPS(opcode, size, UCT_RC_MLX5_ATOMIC_FOPS);

    status = uct_rc_mlx5_iface_common_atomic_data(opcode, size, value, &op, &compare_mask,
                                                  &compare, &swap_mask, &swap, &ext);
    if (ucs_unlikely(UCS_STATUS_IS_ERR(status))) {
        return status;
    }

    return uct_rc_mlx5_ep_atomic_fop(ep, op, result, ext, size, remote_addr, rkey,
                                     compare_mask, compare, swap_mask, swap, comp);
}

ucs_status_t uct_rc_mlx5_ep_atomic32_post(uct_ep_h ep, unsigned opcode, uint32_t value,
                                          uint64_t remote_addr, uct_rkey_t rkey)
{
    return uct_rc_mlx5_ep_atomic_op_post(ep, opcode, sizeof(value), value, remote_addr, rkey);
}

ucs_status_t uct_rc_mlx5_ep_atomic64_post(uct_ep_h ep, unsigned opcode, uint64_t value,
                                          uint64_t remote_addr, uct_rkey_t rkey)
{
    return uct_rc_mlx5_ep_atomic_op_post(ep, opcode, sizeof(value), value, remote_addr, rkey);
}

ucs_status_t uct_rc_mlx5_ep_atomic64_fetch(uct_ep_h ep, uct_atomic_op_t opcode,
                                           uint64_t value, uint64_t *result,
                                           uint64_t remote_addr, uct_rkey_t rkey,
                                           uct_completion_t *comp)
{
    return uct_rc_mlx5_ep_atomic_fop_post(ep, opcode, sizeof(value), value, result,
                                          remote_addr, rkey, comp);
}

ucs_status_t uct_rc_mlx5_ep_atomic32_fetch(uct_ep_h ep, uct_atomic_op_t opcode,
                                           uint32_t value, uint32_t *result,
                                           uint64_t remote_addr, uct_rkey_t rkey,
                                           uct_completion_t *comp)
{
    return uct_rc_mlx5_ep_atomic_fop_post(ep, opcode, sizeof(value), value, result,
                                          remote_addr, rkey, comp);
}

ucs_status_t uct_rc_mlx5_ep_atomic_cswap64(uct_ep_h tl_ep, uint64_t compare, uint64_t swap,
                                           uint64_t remote_addr, uct_rkey_t rkey,
                                           uint64_t *result, uct_completion_t *comp)
{
    return uct_rc_mlx5_ep_atomic_fop(ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t),
                                     MLX5_OPCODE_ATOMIC_CS, result, 0, sizeof(uint64_t),
                                     remote_addr, rkey, 0, htobe64(compare), -1, htobe64(swap),
                                     comp);
}

ucs_status_t uct_rc_mlx5_ep_atomic_cswap32(uct_ep_h tl_ep, uint32_t compare, uint32_t swap,
                                           uint64_t remote_addr, uct_rkey_t rkey,
                                           uint32_t *result, uct_completion_t *comp)
{
    return uct_rc_mlx5_ep_atomic_fop(ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t),
                                     MLX5_OPCODE_ATOMIC_MASKED_CS, result, 1,
                                     sizeof(uint32_t), remote_addr, rkey, UCS_MASK(32),
                                     htonl(compare), -1, htonl(swap), comp);
}

ucs_status_t uct_rc_mlx5_ep_flush(uct_ep_h tl_ep, unsigned flags,
                                  uct_completion_t *comp)
{
    uct_rc_mlx5_ep_t *ep = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);
    uct_rc_mlx5_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_rc_mlx5_iface_t);
    ucs_status_t status;
    uint16_t sn;

    status = uct_rc_ep_flush(&ep->super, ep->tx.wq.bb_max, flags);
    if (status != UCS_INPROGRESS) {
        return status;
    }

    if (uct_rc_txqp_unsignaled(&ep->super.txqp) != 0) {
        sn = ep->tx.wq.sw_pi;
        UCT_RC_CHECK_RES(&iface->super, &ep->super);
        uct_rc_mlx5_txqp_inline_post(&iface->super, IBV_QPT_RC,
                                     &ep->super.txqp, &ep->tx.wq,
                                     MLX5_OPCODE_NOP, NULL, 0,
                                     0, 0, 0,
                                     0, 0,
                                     NULL, NULL, 0, 0,
                                     INT_MAX);
    } else {
        sn = ep->tx.wq.sig_pi;
    }

    uct_rc_txqp_add_send_comp(&iface->super, &ep->super.txqp, comp, sn);
    UCT_TL_EP_STAT_FLUSH_WAIT(&ep->super.super);
    return UCS_INPROGRESS;
}

ucs_status_t uct_rc_mlx5_ep_fc_ctrl(uct_ep_t *tl_ep, unsigned op,
                                    uct_rc_fc_request_t *req)
{
    uct_rc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_rc_iface_t);
    uct_rc_mlx5_ep_t *ep  = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);

    /* In RC only PURE grant is sent as a separate message. Other FC
     * messages are bundled with AM. */
    ucs_assert(op == UCT_RC_EP_FC_PURE_GRANT);

    UCT_RC_CHECK_RES(iface, &ep->super);
    uct_rc_mlx5_txqp_inline_post(iface, IBV_QPT_RC,
                                 &ep->super.txqp, &ep->tx.wq,
                                 MLX5_OPCODE_SEND|UCT_RC_MLX5_OPCODE_FLAG_RAW,
                                 NULL, 0,
                                 UCT_RC_EP_FC_PURE_GRANT, 0, 0,
                                 0, 0,
                                 NULL, NULL, 0, 0,
                                 INT_MAX);
    return UCS_OK;
}

#if IBV_EXP_HW_TM

static ucs_status_t UCS_F_ALWAYS_INLINE
uct_rc_mlx5_ep_tag_eager_short_inline(uct_ep_h tl_ep, uct_tag_t tag,
                                      const void *data, size_t length)
{
    uct_rc_mlx5_ep_t *ep  = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);
    uct_rc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_rc_iface_t);

    UCT_CHECK_LENGTH(length + sizeof(struct ibv_exp_tmh), 0,
                     UCT_IB_MLX5_AM_MAX_SHORT(0), "tag_short");
    UCT_RC_CHECK_RES(iface, &ep->super);

    uct_rc_mlx5_txqp_tag_inline_post(iface, IBV_QPT_RC, &ep->super.txqp,
                                     &ep->tx.wq, MLX5_OPCODE_SEND, data, length,
                                     NULL, tag, 0, IBV_EXP_TMH_EAGER, 0, NULL,
                                     NULL, 0, NULL, 0, MLX5_WQE_CTRL_SOLICITED);

    UCT_TL_EP_STAT_OP(&ep->super.super, TAG, SHORT, length);

    return UCS_OK;
}

ucs_status_t uct_rc_mlx5_ep_tag_eager_short(uct_ep_h tl_ep, uct_tag_t tag,
                                            const void *data, size_t length)
{
#if HAVE_IBV_EXP_DM
    uct_rc_mlx5_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_rc_mlx5_iface_t);
    uct_rc_iface_t *rc_iface   = &iface->super;
    uct_rc_mlx5_ep_t *ep       = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);
    uct_rc_mlx5_dm_copy_data_t cache;
    ucs_status_t status;

    if (ucs_likely((sizeof(struct ibv_exp_tmh) + length <= UCT_IB_MLX5_AM_MAX_SHORT(0)) ||
                   !iface->mlx5_common.dm.dm)) {
#endif
        return uct_rc_mlx5_ep_tag_eager_short_inline(tl_ep, tag, data, length);
#if HAVE_IBV_EXP_DM
    }

    UCT_CHECK_LENGTH(length + sizeof(struct ibv_exp_tmh), 0,
                     iface->mlx5_common.dm.seg_len, "tag_short");
    UCT_RC_CHECK_RES(rc_iface, &ep->super);

    uct_rc_mlx5_fill_tmh(ucs_unaligned_ptr(&cache.tm_hdr), tag, 0, IBV_EXP_TMH_EAGER);

    status = uct_rc_mlx5_ep_short_dm(ep, &cache, sizeof(cache.tm_hdr), data, length,
                                   MLX5_OPCODE_SEND,
                                   MLX5_WQE_CTRL_SOLICITED | MLX5_WQE_CTRL_CQ_UPDATE,
                                   0, 0);
    if (!UCS_STATUS_IS_ERR(status)) {
        UCT_TL_EP_STAT_OP(&ep->super.super, TAG, SHORT, length);
    }

    return status;
#endif
}

ssize_t uct_rc_mlx5_ep_tag_eager_bcopy(uct_ep_h tl_ep, uct_tag_t tag,
                                       uint64_t imm,
                                       uct_pack_callback_t pack_cb,
                                       void *arg, unsigned flags)
{
    uct_rc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_rc_iface_t);
    uct_rc_mlx5_ep_t *ep  = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);
    uct_rc_iface_send_desc_t *desc;
    uint32_t app_ctx, ib_imm;
    int opcode;
    size_t length;

    UCT_RC_CHECK_RES(iface, &ep->super);

    UCT_RC_IFACE_FILL_TM_IMM(imm, app_ctx, ib_imm, opcode, MLX5_OPCODE_SEND,
                             _IMM);

    UCT_RC_MLX5_IFACE_GET_TM_BCOPY_DESC(iface, &iface->tx.mp, desc, tag,
                                        app_ctx, pack_cb, arg, length);

    uct_rc_mlx5_txqp_bcopy_post(iface, &ep->super.txqp, &ep->tx.wq,
                                opcode, sizeof(struct ibv_exp_tmh) + length,
                                0, 0, MLX5_WQE_CTRL_SOLICITED, ib_imm,
                                desc, desc + 1, NULL);

    UCT_TL_EP_STAT_OP(&ep->super.super, TAG, BCOPY, length);

    return length;
}

ucs_status_t uct_rc_mlx5_ep_tag_eager_zcopy(uct_ep_h tl_ep, uct_tag_t tag,
                                            uint64_t imm, const uct_iov_t *iov,
                                            size_t iovcnt, unsigned flags,
                                            uct_completion_t *comp)
{
    uct_rc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_rc_iface_t);
    uct_rc_mlx5_ep_t *ep  = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);
    uint32_t app_ctx, ib_imm;
    int opcode;

    UCT_CHECK_IOV_SIZE(iovcnt, UCT_RC_MLX5_TM_EAGER_ZCOPY_MAX_IOV(0),
                       "uct_rc_mlx5_ep_tag_eager_zcopy");
    UCT_RC_CHECK_ZCOPY_DATA(sizeof(struct ibv_exp_tmh),
                            uct_iov_total_length(iov, iovcnt),
                            iface->super.config.seg_size);

    UCT_RC_IFACE_FILL_TM_IMM(imm, app_ctx, ib_imm, opcode, MLX5_OPCODE_SEND,
                             _IMM);

    UCT_TL_EP_STAT_OP(&ep->super.super, TAG, ZCOPY,
                      uct_iov_total_length(iov, iovcnt));

    return uct_rc_mlx5_ep_zcopy_post(ep, opcode|UCT_RC_MLX5_OPCODE_FLAG_TM,
                                     iov, iovcnt, 0, "", 0, 0, 0,
                                     tag, app_ctx, ib_imm,
                                     MLX5_WQE_CTRL_SOLICITED, comp);
}

ucs_status_ptr_t uct_rc_mlx5_ep_tag_rndv_zcopy(uct_ep_h tl_ep, uct_tag_t tag,
                                               const void *header,
                                               unsigned header_length,
                                               const uct_iov_t *iov,
                                               size_t iovcnt, unsigned flags,
                                               uct_completion_t *comp)
{
    uct_rc_mlx5_ep_t *ep  = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);
    uct_rc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_rc_iface_t);
    unsigned tm_hdr_len   = sizeof(struct ibv_exp_tmh) +
                            sizeof(struct ibv_exp_tmh_rvh);
    uint32_t op_index;

    UCT_RC_IFACE_CHECK_RNDV_PARAMS(iovcnt, header_length, tm_hdr_len,
                                   UCT_IB_MLX5_AM_MAX_SHORT(0),
                                   iface->tm.max_rndv_data +
                                   UCT_RC_IFACE_TMH_PRIV_LEN);
    UCT_RC_IFACE_CHECK_RES_PTR(iface, &ep->super);

    op_index = uct_rc_iface_tag_get_op_id(iface, comp);

    uct_rc_mlx5_txqp_tag_inline_post(iface, IBV_QPT_RC, &ep->super.txqp,
                                     &ep->tx.wq, MLX5_OPCODE_SEND, header,
                                     header_length, iov, tag, op_index,
                                     IBV_EXP_TMH_RNDV, 0, NULL, NULL, 0,
                                     NULL, 0, MLX5_WQE_CTRL_SOLICITED);

    return (ucs_status_ptr_t)((uint64_t)op_index);
}

ucs_status_t uct_rc_mlx5_ep_tag_rndv_request(uct_ep_h tl_ep, uct_tag_t tag,
                                             const void* header,
                                             unsigned header_length,
                                             unsigned flags)
{
    uct_rc_mlx5_ep_t *ep  = ucs_derived_of(tl_ep, uct_rc_mlx5_ep_t);
    uct_rc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_rc_iface_t);

    UCT_CHECK_LENGTH(header_length + sizeof(struct ibv_exp_tmh), 0,
                     UCT_IB_MLX5_AM_MAX_SHORT(0), "tag_rndv_request");
    UCT_RC_CHECK_RES(iface, &ep->super);

    uct_rc_mlx5_txqp_tag_inline_post(iface, IBV_QPT_RC, &ep->super.txqp,
                                     &ep->tx.wq, MLX5_OPCODE_SEND_IMM, header,
                                     header_length, NULL, tag, 0,
                                     IBV_EXP_TMH_EAGER, 0, NULL, NULL, 0,
                                     NULL, 0, MLX5_WQE_CTRL_SOLICITED);
    return UCS_OK;
}

#endif /* IBV_EXP_HW_TM */


UCS_CLASS_INIT_FUNC(uct_rc_mlx5_ep_t, uct_iface_h tl_iface)
{
    uct_rc_mlx5_iface_t *iface = ucs_derived_of(tl_iface, uct_rc_mlx5_iface_t);
    ucs_status_t status;

    UCS_CLASS_CALL_SUPER_INIT(uct_rc_ep_t, &iface->super);

    status = uct_ib_mlx5_txwq_init(iface->super.super.super.worker,
                                   iface->tx.mmio_mode, &self->tx.wq,
                                   self->super.txqp.qp);
    if (status != UCS_OK) {
        ucs_error("Failed to get mlx5 QP information");
        return status;
    }

    self->qp_num       = self->super.txqp.qp->qp_num;
    self->tx.wq.bb_max = ucs_min(self->tx.wq.bb_max, iface->tx.bb_max);
    uct_rc_txqp_available_set(&self->super.txqp, self->tx.wq.bb_max);
    return UCS_OK;
}

static void uct_rc_mlx5_ep_clean_qp(uct_rc_mlx5_ep_t *ep, struct ibv_qp *qp)
{
    uct_rc_mlx5_iface_t *iface = ucs_derived_of(ep->super.super.super.iface,
                                                uct_rc_mlx5_iface_t);

    /* Make the HW generate CQEs for all in-progress SRQ receives from the QP,
     * so we clean them all before ibv_modify_qp() can see them.
     */
#if HAVE_DECL_IBV_CMD_MODIFY_QP
    struct ibv_qp_attr qp_attr;
    struct ibv_modify_qp cmd;
    int ret;

    /* Bypass mlx5 driver, and go directly to command interface, to avoid
     * cleaning the CQ in mlx5 driver
     */
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_RESET;
    ret = ibv_cmd_modify_qp(qp, &qp_attr, IBV_QP_STATE, &cmd, sizeof(cmd));
    if (ret) {
        ucs_warn("modify qp 0x%x to RESET failed: %m", qp->qp_num);
    }
#else
    (void)uct_ib_modify_qp(qp, IBV_QPS_ERR);
#endif

    iface->super.rx.srq.available += uct_rc_mlx5_iface_commom_clean(
            &iface->mlx5_common.cq[UCT_IB_DIR_RX],
            &iface->mlx5_common.rx.srq, qp->qp_num);

    /* Synchronize CQ index with the driver, since it would remove pending
     * completions for this QP (both send and receive) during ibv_destroy_qp().
     */
    uct_rc_mlx5_iface_common_update_cqs_ci(&iface->mlx5_common, &iface->super.super);
    (void)uct_ib_modify_qp(qp, IBV_QPS_RESET);
    uct_rc_mlx5_iface_common_sync_cqs_ci(&iface->mlx5_common, &iface->super.super);
}

static UCS_CLASS_CLEANUP_FUNC(uct_rc_mlx5_ep_t)
{
    uct_rc_mlx5_iface_t *iface = ucs_derived_of(self->super.super.super.iface,
                                                uct_rc_mlx5_iface_t);

    uct_ib_mlx5_txwq_cleanup(&self->tx.wq);
    uct_rc_mlx5_ep_clean_qp(self, self->super.txqp.qp);
#if IBV_EXP_HW_TM
    if (UCT_RC_IFACE_TM_ENABLED(&iface->super)) {
        uct_rc_mlx5_ep_clean_qp(self, self->super.tm_qp);
    }
#endif

    /* Return all credits if user do flush(UCT_FLUSH_FLAG_CANCEL) before
     * ep_destroy.
     */
    uct_rc_txqp_available_add(&self->super.txqp,
                              self->tx.wq.bb_max -
                              uct_rc_txqp_available(&self->super.txqp));

    uct_ib_mlx5_srq_cleanup(&iface->mlx5_common.rx.srq, iface->super.rx.srq.srq);
}

UCS_CLASS_DEFINE(uct_rc_mlx5_ep_t, uct_rc_ep_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_rc_mlx5_ep_t, uct_ep_t, uct_iface_h);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_rc_mlx5_ep_t, uct_ep_t);
