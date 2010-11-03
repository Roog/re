/**
 * @file fb.c Real-time Transport Control Protocol (RTCP)-Based Feedback
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re_types.h>
#include <re_fmt.h>
#include <re_mem.h>
#include <re_mbuf.h>
#include <re_list.h>
#include <re_sys.h>
#include <re_sa.h>
#include <re_rtp.h>
#include "rtcp.h"


#define DEBUG_MODULE "rtcp_pb"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


enum {
	GNACK_SIZE = 4,
	SLI_SIZE   = 4
};


/* Encode functions */


int rtcp_rtpfb_gnack_encode(struct mbuf *mb, uint16_t pid, uint16_t blp)
{
	int err;
	err  = mbuf_write_u16(mb, htons(pid));
	err |= mbuf_write_u16(mb, htons(blp));
	return err;
}


int rtcp_psfb_sli_encode(struct mbuf *mb, uint16_t first, uint16_t number,
			 uint8_t picid)
{
	const uint32_t v = first<<19 | number<<6 | picid;
	return mbuf_write_u32(mb, htonl(v));
}


/* Decode functions */


int rtcp_rtpfb_decode(struct mbuf *mb, struct rtcp_msg *msg)
{
	size_t i, sz;

	if (!msg)
		return EINVAL;

	switch (msg->hdr.count) {

	case RTCP_RTPFB_GNACK:
		sz = msg->r.fb.n * sizeof(*msg->r.fb.fci.gnackv);
		msg->r.fb.fci.gnackv = mem_alloc(sz, NULL);
		if (!msg->r.fb.fci.gnackv)
			return ENOMEM;

		if (mbuf_get_left(mb) < msg->r.fb.n * GNACK_SIZE)
			return EBADMSG;
		for (i=0; i<msg->r.fb.n; i++) {
			msg->r.fb.fci.gnackv[i].pid = ntohs(mbuf_read_u16(mb));
			msg->r.fb.fci.gnackv[i].blp = ntohs(mbuf_read_u16(mb));
		}
		break;

	default:
		DEBUG_NOTICE("unknown RTPFB fmt %d\n", msg->hdr.count);
		break;
	}

	return 0;
}


int rtcp_psfb_decode(struct mbuf *mb, struct rtcp_msg *msg)
{
	size_t i, sz;

	if (!msg)
		return EINVAL;

	switch (msg->hdr.count) {

	case RTCP_PSFB_PLI:
		/* no params */
		break;

	case RTCP_PSFB_SLI:
		sz = msg->r.fb.n * sizeof(*msg->r.fb.fci.sliv);
		msg->r.fb.fci.sliv = mem_alloc(sz, NULL);
		if (!msg->r.fb.fci.sliv)
			return ENOMEM;

		if (mbuf_get_left(mb) < msg->r.fb.n * SLI_SIZE)
			return EBADMSG;
		for (i=0; i<msg->r.fb.n; i++) {
			const uint32_t v = ntohl(mbuf_read_u32(mb));

			msg->r.fb.fci.sliv[i].first  = v>>19 & 0x1fff;
			msg->r.fb.fci.sliv[i].number = v>> 6 & 0x1fff;
			msg->r.fb.fci.sliv[i].picid  = v>> 0 & 0x003f;
		}
		break;

	default:
		DEBUG_NOTICE("unknown PSFB fmt %d\n", msg->hdr.count);
		break;
	}

	return 0;
}
