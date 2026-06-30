/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM block_iobuf

#if !defined(_TRACE_BLOCK_IOBUF_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_BLOCK_IOBUF_H

#include <linux/blkdev.h>
#include <linux/tracepoint.h>

TRACE_EVENT(block_iobuf_pool_create,

	TP_PROTO(struct request_queue *q, unsigned int order,
		 unsigned int folio_size, unsigned int reasons,
		 unsigned int min_nr),

	TP_ARGS(q, order, folio_size, reasons, min_nr),

	TP_STRUCT__entry(
		__field(dev_t,		dev)
		__field(unsigned int,	order)
		__field(unsigned int,	folio_size)
		__field(unsigned int,	reasons)
		__field(unsigned int,	min_nr)
	),

	TP_fast_assign(
		__entry->dev		= q->disk ? disk_devt(q->disk) : 0;
		__entry->order		= order;
		__entry->folio_size	= folio_size;
		__entry->reasons	= reasons;
		__entry->min_nr		= min_nr;
	),

	TP_printk("%d,%d order=%u folio_size=%u reasons=0x%x min_nr=%u",
		  MAJOR(__entry->dev), MINOR(__entry->dev),
		  __entry->order, __entry->folio_size,
		  __entry->reasons, __entry->min_nr)
);

TRACE_EVENT(block_iobuf_alloc,

	TP_PROTO(struct request_queue *q, unsigned int order,
		 gfp_t gfp, bool success),

	TP_ARGS(q, order, gfp, success),

	TP_STRUCT__entry(
		__field(dev_t,		dev)
		__field(unsigned int,	order)
		__field(gfp_t,		gfp)
		__field(bool,		success)
	),

	TP_fast_assign(
		__entry->dev		= q->disk ? disk_devt(q->disk) : 0;
		__entry->order		= order;
		__entry->gfp		= gfp;
		__entry->success	= success;
	),

	TP_printk("%d,%d order=%u gfp=0x%x success=%d",
		  MAJOR(__entry->dev), MINOR(__entry->dev),
		  __entry->order, __entry->gfp, __entry->success)
);

TRACE_EVENT(block_iobuf_free,

	TP_PROTO(struct request_queue *q, unsigned int order),

	TP_ARGS(q, order),

	TP_STRUCT__entry(
		__field(dev_t,		dev)
		__field(unsigned int,	order)
	),

	TP_fast_assign(
		__entry->dev		= q->disk ? disk_devt(q->disk) : 0;
		__entry->order		= order;
	),

	TP_printk("%d,%d order=%u",
		  MAJOR(__entry->dev), MINOR(__entry->dev), __entry->order)
);

TRACE_EVENT(block_iobuf_fallback,

	TP_PROTO(struct request_queue *q, const char *reason),

	TP_ARGS(q, reason),

	TP_STRUCT__entry(
		__field(dev_t,		dev)
		__string(reason,	reason)
	),

	TP_fast_assign(
		__entry->dev		= q->disk ? disk_devt(q->disk) : 0;
		__assign_str(reason);
	),

	TP_printk("%d,%d reason=%s",
		  MAJOR(__entry->dev), MINOR(__entry->dev),
		  __get_str(reason))
);

TRACE_EVENT(block_iobuf_bounce_submit,

	TP_PROTO(struct request_queue *q, size_t bytes,
		 unsigned int nr_folios, unsigned int op),

	TP_ARGS(q, bytes, nr_folios, op),

	TP_STRUCT__entry(
		__field(dev_t,		dev)
		__field(size_t,		bytes)
		__field(unsigned int,	nr_folios)
		__field(unsigned int,	op)
	),

	TP_fast_assign(
		__entry->dev		= q->disk ? disk_devt(q->disk) : 0;
		__entry->bytes		= bytes;
		__entry->nr_folios	= nr_folios;
		__entry->op		= op;
	),

	TP_printk("%d,%d bytes=%zu nr_folios=%u op=%u",
		  MAJOR(__entry->dev), MINOR(__entry->dev),
		  __entry->bytes, __entry->nr_folios, __entry->op)
);

#endif /* _TRACE_BLOCK_IOBUF_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
