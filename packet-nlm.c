/* packet-nlm.c
 * Routines for nlm dissection
 *
 * $Id: packet-nlm.c,v 1.30 2002/10/23 21:17:02 guy Exp $
 *
 * Ethereal - Network traffic analyzer
 * By Gerald Combs <gerald@ethereal.com>
 * Copyright 1998 Gerald Combs
 *
 * Copied from packet-mount.c
 *
 * 2001-JAN  Ronnie Sahlberg <See AUTHORS for email>
 *  Updates to version 1 of the protocol.
 *  Added version 3 of the protocol.
 *  Added version 4 of the protocol.
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif



#include "packet-rpc.h"
#include "packet-nfs.h"
#include "packet-nlm.h"
#include "prefs.h"
#include <string.h>

/*
 * NFS Lock Manager protocol specs can only be found in actual
 * implementations or in the nice book:
 * Brent Callaghan: "NFS Illustrated", Addison-Wesley, ISBN 0-201-32570-5
 * which I use here as reference (BC).
 *
 * They can also be found if you go to
 *
 *	http://www.opengroup.org/publications/catalog/c702.htm
 *
 * and follow the links to the HTML version of the document.
 */

static int proto_nlm = -1;

static int hf_nlm_cookie = -1;
static int hf_nlm_block = -1;
static int hf_nlm_exclusive = -1;
static int hf_nlm_lock = -1;
static int hf_nlm_lock_caller_name = -1;
static int hf_nlm_lock_owner = -1;
static int hf_nlm_lock_svid = -1;
static int hf_nlm_lock_l_offset = -1;
static int hf_nlm_lock_l_offset64 = -1;
static int hf_nlm_lock_l_len = -1;
static int hf_nlm_lock_l_len64 = -1;
static int hf_nlm_reclaim = -1;
static int hf_nlm_stat = -1;
static int hf_nlm_state = -1;
static int hf_nlm_test_stat = -1;
static int hf_nlm_test_stat_stat = -1;
static int hf_nlm_holder = -1;
static int hf_nlm_share = -1;
static int hf_nlm_share_mode = -1;
static int hf_nlm_share_access = -1;
static int hf_nlm_share_name = -1;
static int hf_nlm_sequence = -1;
static int hf_nlm_request_in = -1;
static int hf_nlm_reply_in = -1;
static int hf_nlm_time = -1;

static gint ett_nlm = -1;
static gint ett_nlm_lock = -1;



/*
 * stuff to match MSG and RES packets for async NLM
 */

static gboolean nlm_match_msgres = FALSE;
static GHashTable *nlm_msg_res_unmatched = NULL;
static GHashTable *nlm_msg_res_matched = NULL;

/* XXX 	when matching the packets we should really check the conversation (only address
	NOT ports) and command type as well. I am lazy and thinks the cookie itself is
	good enough for now
*/
typedef struct _nlm_msg_res_unmatched_data {
	int req_frame;
	nstime_t ns;
	int cookie_len;
	char *cookie;
} nlm_msg_res_unmatched_data;

typedef struct _nlm_msg_res_matched_data {
	int req_frame;
	int rep_frame;
	nstime_t ns;
} nlm_msg_res_matched_data;

static gboolean
nlm_msg_res_unmatched_free_all(gpointer key_arg _U_, gpointer value, gpointer user_data _U_)
{
	nlm_msg_res_unmatched_data *umd = (nlm_msg_res_unmatched_data *)value;

	g_free(umd->cookie);
	g_free(umd);

	return TRUE;
}
static gboolean
nlm_msg_res_matched_free_all(gpointer key_arg _U_, gpointer value, gpointer user_data _U_)
{
	nlm_msg_res_matched_data *md = (nlm_msg_res_matched_data *)value;

	g_free(md);

	return TRUE;
}

static guint
nlm_msg_res_unmatched_hash(gconstpointer k)
{
	nlm_msg_res_unmatched_data *umd = (nlm_msg_res_unmatched_data *)k;
	guint8 hash=0;
	int i;

	for(i=0;i<umd->cookie_len;i++){
		hash^=umd->cookie[i];
	}

	return hash;
}
static guint
nlm_msg_res_matched_hash(gconstpointer k)
{
	guint hash = (guint)k;

	return hash;
}

static gint
nlm_msg_res_unmatched_equal(gconstpointer k1, gconstpointer k2)
{
	nlm_msg_res_unmatched_data *umd1 = (nlm_msg_res_unmatched_data *)k1;
	nlm_msg_res_unmatched_data *umd2 = (nlm_msg_res_unmatched_data *)k2;

	if(umd1->cookie_len!=umd2->cookie_len){
		return 0;
	}

	return( !memcmp(umd1->cookie, umd2->cookie, umd1->cookie_len));
}
static gint
nlm_msg_res_matched_equal(gconstpointer k1, gconstpointer k2)
{
	guint mk1 = (guint)k1;
	guint mk2 = (guint)k2;

	return( mk1==mk2 );
}

static void
nlm_msg_res_match_init(void)
{
	if(nlm_msg_res_unmatched != NULL){
		g_hash_table_foreach_remove(nlm_msg_res_unmatched,
				nlm_msg_res_unmatched_free_all, NULL);
	} else {
		nlm_msg_res_unmatched=g_hash_table_new(nlm_msg_res_unmatched_hash,
			nlm_msg_res_unmatched_equal);
	}

	if(nlm_msg_res_matched != NULL){
		g_hash_table_foreach_remove(nlm_msg_res_matched,
				nlm_msg_res_matched_free_all, NULL);
	} else {
		nlm_msg_res_matched=g_hash_table_new(nlm_msg_res_matched_hash,
			nlm_msg_res_matched_equal);
	}
}

static void
nlm_print_msgres_reply(packet_info *pinfo, proto_tree *tree, tvbuff_t *tvb)
{
	nlm_msg_res_matched_data *md;

	md=g_hash_table_lookup(nlm_msg_res_matched, (gconstpointer)pinfo->fd->num);
	if(md){
		nstime_t ns;
		proto_tree_add_uint(tree, hf_nlm_request_in, tvb, 0, 0, md->req_frame);
		ns.secs= pinfo->fd->abs_secs-md->ns.secs;
		ns.nsecs=pinfo->fd->abs_usecs*1000-md->ns.nsecs;
		if(ns.nsecs<0){
			ns.nsecs+=1000000000;
			ns.secs--;
		}
		proto_tree_add_time(tree, hf_nlm_time, tvb, 0, 0, &ns);

	}
}

static void
nlm_print_msgres_request(packet_info *pinfo, proto_tree *tree, tvbuff_t *tvb)
{
	nlm_msg_res_matched_data *md;

	md=g_hash_table_lookup(nlm_msg_res_matched, (gconstpointer)pinfo->fd->num);
	if(md){
		proto_tree_add_uint(tree, hf_nlm_reply_in, tvb, 0, 0, md->rep_frame);
	}
}
static void
nlm_match_fhandle_reply(packet_info *pinfo, proto_tree *tree)
{
	nlm_msg_res_matched_data *md;

	md=g_hash_table_lookup(nlm_msg_res_matched, (gconstpointer)pinfo->fd->num);
	if(md && md->rep_frame){
		nfs_fhandle_data_t *fhd;
		fhd=(nfs_fhandle_data_t *)g_hash_table_lookup(
			nfs_fhandle_frame_table,
			(gconstpointer)md->req_frame);
		if(fhd){
			dissect_fhandle_hidden(pinfo,
				tree, fhd);
		}
	}
}
static void
nlm_match_fhandle_request(packet_info *pinfo, proto_tree *tree)
{
	nlm_msg_res_matched_data *md;

	md=g_hash_table_lookup(nlm_msg_res_matched, (gconstpointer)pinfo->fd->num);
	if(md && md->rep_frame){
		nfs_fhandle_data_t *fhd;
		fhd=(nfs_fhandle_data_t *)g_hash_table_lookup(
			nfs_fhandle_frame_table,
			(gconstpointer)md->rep_frame);
		if(fhd){
			dissect_fhandle_hidden(pinfo,
				tree, fhd);
		}
	}
}

static void
nlm_register_unmatched_res(packet_info *pinfo, tvbuff_t *tvb, int offset)
{
	nlm_msg_res_unmatched_data umd;
	nlm_msg_res_unmatched_data *old_umd;

	umd.cookie_len=tvb_get_ntohl(tvb, offset);
	umd.cookie=(char *)tvb_get_ptr(tvb, offset+4, -1);

	/* have we seen this cookie before? */
	old_umd=g_hash_table_lookup(nlm_msg_res_unmatched, (gconstpointer)&umd);
	if(old_umd){
		nlm_msg_res_matched_data *md;

		md=g_malloc(sizeof(nlm_msg_res_matched_data));
		md->req_frame=old_umd->req_frame;
		md->rep_frame=pinfo->fd->num;
		md->ns=old_umd->ns;
		g_hash_table_insert(nlm_msg_res_matched, (gpointer)md->req_frame, (gpointer)md);
		g_hash_table_insert(nlm_msg_res_matched, (gpointer)md->rep_frame, (gpointer)md);

		g_hash_table_remove(nlm_msg_res_unmatched, (gconstpointer)old_umd);
		g_free(old_umd->cookie);
		g_free(old_umd);
	}
}

static void
nlm_register_unmatched_msg(packet_info *pinfo, tvbuff_t *tvb, int offset)
{
	nlm_msg_res_unmatched_data *umd;
	nlm_msg_res_unmatched_data *old_umd;

	/* allocate and build the unmatched structure for this request */
	umd=g_malloc(sizeof(nlm_msg_res_unmatched_data));
	umd->req_frame=pinfo->fd->num;
	umd->ns.secs=pinfo->fd->abs_secs;
	umd->ns.nsecs=pinfo->fd->abs_usecs*1000;
	umd->cookie_len=tvb_get_ntohl(tvb, offset);
	umd->cookie=g_malloc(umd->cookie_len);
	tvb_memcpy(tvb, (guint8 *)umd->cookie, offset+4, umd->cookie_len);

	/* remove any old duplicates */
	old_umd=g_hash_table_lookup(nlm_msg_res_unmatched, (gconstpointer)umd);
	if(old_umd){
		g_hash_table_remove(nlm_msg_res_unmatched, (gconstpointer)old_umd);
		g_free(old_umd->cookie);
		g_free(old_umd);
	}

	/* add new one */
	g_hash_table_insert(nlm_msg_res_unmatched, (gpointer)umd, (gpointer)umd);
}




static const value_string names_nlm_stats[] =
{
	/* NLM_GRANTED is the function number 5 and the state code 0.
	 * So we use for the state the postfix _S.
	 */
#define NLM_GRANTED_S		0
		{	NLM_GRANTED_S,	"NLM_GRANTED"	},
#define NLM_DENIED		1
		{	NLM_DENIED,	"NLM_DENIED"	},
#define NLM_DENIED_NOLOCKS	2
		{	NLM_DENIED_NOLOCKS,	"NLM_DENIED_NOLOCKS"	},
#define NLM_BLOCKED		3
		{	NLM_BLOCKED,	"NLM_BLOCKED"		},
#define NLM_DENIED_GRACE_PERIOD	4
		{	NLM_DENIED_GRACE_PERIOD,	"NLM_DENIED_GRACE_PERIOD"	},
#define NLM_DEADLCK		5
		{	NLM_DEADLCK,	"NLM_DEADLCK"	},
#define NLM_ROFS		6
		{	NLM_ROFS,	"NLM_ROFS"	},
#define NLM_STALE_FH		7
		{	NLM_STALE_FH,	"NLM_STALE_FH"	},
#define NLM_BIG			8
		{	NLM_BIG,	"NLM_BIG"	},
#define NLM_FAILED		9
		{	NLM_FAILED,	"NLM_FAILED"	},
		{	0,		NULL		}
};


static const value_string names_fsh_mode[] =
{
#define FSM_DN	0
		{	FSM_DN,		"deny none"	},
#define FSM_DR	1
		{	FSM_DR,		"deny read"	},
#define FSM_DW	2
		{	FSM_DW,		"deny write"	},
#define FSM_DRW	3
		{	FSM_DRW,	"deny read/write"	},

		{	0,		NULL	}
};


static const value_string names_fsh_access[] =
{
#define FSA_NONE	0
		{	FSA_NONE,	"no access"	},
#define FSA_R	1
		{	FSA_R,		"read-only"	},
#define FSA_W	2
		{	FSA_W,		"write-only"	},
#define FSA_RW	3
		{	FSA_RW,		"read/write"	},
		{	0,		NULL	}
};






/* **************************** */
/* generic dissecting functions */
/* **************************** */
static int
dissect_lock(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, int version, int offset)
{
	proto_item* lock_item = NULL;
	proto_tree* lock_tree = NULL;

	if (tree) {
		lock_item = proto_tree_add_item(tree, hf_nlm_lock, tvb,
				offset, -1, FALSE);
		if (lock_item)
			lock_tree = proto_item_add_subtree(lock_item, ett_nlm_lock);
	}

	offset = dissect_rpc_string(tvb,lock_tree,
			hf_nlm_lock_caller_name, offset, NULL);
	offset = dissect_nfs_fh3(tvb, offset, pinfo, lock_tree,"fh");

	offset = dissect_rpc_data(tvb, lock_tree, hf_nlm_lock_owner, offset);

	offset = dissect_rpc_uint32(tvb, lock_tree, hf_nlm_lock_svid, offset);

	if (version == 4) {
		offset = dissect_rpc_uint64(tvb, lock_tree, hf_nlm_lock_l_offset64, offset);
		offset = dissect_rpc_uint64(tvb, lock_tree, hf_nlm_lock_l_len64, offset);
	}
	else {
		offset = dissect_rpc_uint32(tvb, lock_tree, hf_nlm_lock_l_offset, offset);
		offset = dissect_rpc_uint32(tvb, lock_tree, hf_nlm_lock_l_len, offset);
	}

	return offset;
}


static int
dissect_nlm_test(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree, int version)
{
	if(nlm_match_msgres){
		rpc_call_info_value *rpc_call=pinfo->private_data;
		if(rpc_call->proc==6){	/* NLM_TEST_MSG */
			if( (!pinfo->fd->flags.visited) ){
				nlm_register_unmatched_msg(pinfo, tvb, offset);
			} else {
				nlm_print_msgres_request(pinfo, tree, tvb);
			}
			/* for the fhandle matching that finds both request and
			   response packet */
			if(nfs_fhandle_reqrep_matching){
				nlm_match_fhandle_request(pinfo, tree);
			}
		}
	}

	offset = dissect_rpc_data(tvb, tree, hf_nlm_cookie, offset);
	dissect_rpc_bool(tvb, tree, hf_nlm_exclusive, offset);
	offset += 4;
	offset = dissect_lock(tvb, pinfo, tree, version, offset);
	return offset;
}

static int
dissect_nlm_lock(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree,int version)
{
	if(nlm_match_msgres){
		rpc_call_info_value *rpc_call=pinfo->private_data;
		if(rpc_call->proc==7){	/* NLM_LOCK_MSG */
			if( (!pinfo->fd->flags.visited) ){
				nlm_register_unmatched_msg(pinfo, tvb, offset);
			} else {
				nlm_print_msgres_request(pinfo, tree, tvb);
			}
			/* for the fhandle matching that finds both request and
			   response packet */
			if(nfs_fhandle_reqrep_matching){
				nlm_match_fhandle_request(pinfo, tree);
			}
		}
	}

	offset = dissect_rpc_data(tvb, tree, hf_nlm_cookie, offset);
	offset = dissect_rpc_bool(tvb, tree, hf_nlm_block, offset);
	offset = dissect_rpc_bool(tvb, tree, hf_nlm_exclusive, offset);
	offset = dissect_lock(tvb, pinfo, tree, version, offset);
	offset = dissect_rpc_bool(tvb, tree, hf_nlm_reclaim, offset);
	offset = dissect_rpc_uint32(tvb, tree, hf_nlm_state, offset);
	return offset;
}

static int
dissect_nlm_cancel(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree,int version)
{
	if(nlm_match_msgres){
		rpc_call_info_value *rpc_call=pinfo->private_data;
		if(rpc_call->proc==8){	/* NLM_CANCEL_MSG */
			if( (!pinfo->fd->flags.visited) ){
				nlm_register_unmatched_msg(pinfo, tvb, offset);
			} else {
				nlm_print_msgres_request(pinfo, tree, tvb);
			}
			/* for the fhandle matching that finds both request and
			   response packet */
			if(nfs_fhandle_reqrep_matching){
				nlm_match_fhandle_request(pinfo, tree);
			}
		}
	}

	offset = dissect_rpc_data(tvb, tree, hf_nlm_cookie, offset);
	offset = dissect_rpc_bool(tvb, tree, hf_nlm_block, offset);
	offset = dissect_rpc_bool(tvb, tree, hf_nlm_exclusive, offset);
	offset = dissect_lock(tvb, pinfo, tree, version, offset);
	return offset;
}

static int
dissect_nlm_unlock(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree,int version)
{
	if(nlm_match_msgres){
		rpc_call_info_value *rpc_call=pinfo->private_data;
		if(rpc_call->proc==9){	/* NLM_UNLOCK_MSG */
			if( (!pinfo->fd->flags.visited) ){
				nlm_register_unmatched_msg(pinfo, tvb, offset);
			} else {
				nlm_print_msgres_request(pinfo, tree, tvb);
			}
			/* for the fhandle matching that finds both request and
			   response packet */
			if(nfs_fhandle_reqrep_matching){
				nlm_match_fhandle_request(pinfo, tree);
			}
		}
	}

	offset = dissect_rpc_data(tvb, tree, hf_nlm_cookie, offset);
	offset = dissect_lock(tvb, pinfo, tree, version, offset);
	return offset;
}

static int
dissect_nlm_granted(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree,int version)
{
	if(nlm_match_msgres){
		rpc_call_info_value *rpc_call=pinfo->private_data;
		if(rpc_call->proc==10){	/* NLM_GRANTED_MSG */
			if( (!pinfo->fd->flags.visited) ){
				nlm_register_unmatched_msg(pinfo, tvb, offset);
			} else {
				nlm_print_msgres_request(pinfo, tree, tvb);
			}
			/* for the fhandle matching that finds both request and
			   response packet */
			if(nfs_fhandle_reqrep_matching){
				nlm_match_fhandle_request(pinfo, tree);
			}
		}
	}

	offset = dissect_rpc_data(tvb, tree, hf_nlm_cookie, offset);
	offset = dissect_rpc_bool(tvb, tree, hf_nlm_exclusive, offset);
	offset = dissect_lock(tvb, pinfo, tree, version, offset);
	return offset;
}


static int
dissect_nlm_test_res(tvbuff_t *tvb, int offset, packet_info *pinfo _U_,
    proto_tree *tree,int version)
{
	proto_item* lock_item = NULL;
	proto_tree* lock_tree = NULL;

	if(nlm_match_msgres){
		rpc_call_info_value *rpc_call=pinfo->private_data;
		if(rpc_call->proc==11){	/* NLM_TEST_RES */
			if( (!pinfo->fd->flags.visited) ){
				nlm_register_unmatched_res(pinfo, tvb, offset);
			} else {
				nlm_print_msgres_reply(pinfo, tree, tvb);
			}
			/* for the fhandle matching that finds both request and
			   response packet */
			if(nfs_fhandle_reqrep_matching){
				nlm_match_fhandle_reply(pinfo, tree);
			}
		}
	}

	offset = dissect_rpc_data(tvb, tree, hf_nlm_cookie, offset);

	if (tree) {
		lock_item = proto_tree_add_item(tree, hf_nlm_test_stat, tvb,
				offset, -1, FALSE);
		if (lock_item)
			lock_tree = proto_item_add_subtree(lock_item,
				ett_nlm_lock);
	}

	offset = dissect_rpc_uint32(tvb, lock_tree, hf_nlm_test_stat_stat,
	    offset);

	/* last structure is optional, only supplied for stat==1 (LOCKED) */
	if(tvb_reported_length_remaining(tvb, offset) == 0){
		return offset;
	}

	if (tree) {
		lock_item = proto_tree_add_item(lock_tree, hf_nlm_holder, tvb,
				offset, -1, FALSE);
		if (lock_item)
			lock_tree = proto_item_add_subtree(lock_item,
				ett_nlm_lock);
	}

	offset = dissect_rpc_bool(tvb, lock_tree, hf_nlm_exclusive,
	    offset);
	offset = dissect_rpc_uint32(tvb, lock_tree, hf_nlm_lock_svid,
	    offset);
	offset = dissect_rpc_data(tvb, lock_tree, hf_nlm_lock_owner,
	    offset);

	if (version == 4) {
		offset = dissect_rpc_uint64(tvb, lock_tree,
		    hf_nlm_lock_l_offset64, offset);
		offset = dissect_rpc_uint64(tvb, lock_tree,
		    hf_nlm_lock_l_len64, offset);
	}
	else {
		offset = dissect_rpc_uint32(tvb, lock_tree,
		    hf_nlm_lock_l_offset, offset);
		offset = dissect_rpc_uint32(tvb, lock_tree,
		    hf_nlm_lock_l_len, offset);
	}

	return offset;
}


static int
dissect_nlm_share(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree,int version _U_)
{
	proto_item* lock_item = NULL;
	proto_tree* lock_tree = NULL;

	offset = dissect_rpc_data(tvb, tree, hf_nlm_cookie, offset);

	if (tree) {
		lock_item = proto_tree_add_item(tree, hf_nlm_share, tvb,
				offset, -1, FALSE);
		if (lock_item)
			lock_tree = proto_item_add_subtree(lock_item,
				ett_nlm_lock);
	}

	offset = dissect_rpc_string(tvb,lock_tree,
			hf_nlm_lock_caller_name, offset, NULL);

	offset = dissect_nfs_fh3(tvb, offset, pinfo, lock_tree, "fh");

	offset = dissect_rpc_data(tvb, lock_tree, hf_nlm_lock_owner, offset);

	offset = dissect_rpc_uint32(tvb, lock_tree, hf_nlm_share_mode, offset);
	offset = dissect_rpc_uint32(tvb, lock_tree, hf_nlm_share_access, offset);


	offset = dissect_rpc_bool(tvb, tree, hf_nlm_reclaim, offset);
	return offset;
}

static int
dissect_nlm_shareres(tvbuff_t *tvb, int offset, packet_info *pinfo _U_,
    proto_tree *tree, int version _U_)
{
	offset = dissect_rpc_data(tvb, tree, hf_nlm_cookie, offset);
	offset = dissect_rpc_uint32(tvb, tree, hf_nlm_stat, offset);
	offset = dissect_rpc_uint32(tvb, tree, hf_nlm_sequence, offset);
	return offset;
}

static int
dissect_nlm_freeall(tvbuff_t *tvb, int offset, packet_info *pinfo _U_,
    proto_tree *tree,int version _U_)
{
	offset = dissect_rpc_string(tvb,tree,
			hf_nlm_share_name, offset, NULL);

	offset = dissect_rpc_uint32(tvb, tree, hf_nlm_stat, offset);

	return offset;
}


/* RPC functions */


/* This function is identical for all NLM protocol versions (1-4)*/
static int
dissect_nlm_gen_reply(tvbuff_t *tvb, int offset, packet_info *pinfo _U_,
    proto_tree *tree)
{
	if(nlm_match_msgres){
		rpc_call_info_value *rpc_call=pinfo->private_data;
		if((rpc_call->proc==12)  /* NLM_LOCK_RES */
		|| (rpc_call->proc==13)  /* NLM_CANCEL_RES */
		|| (rpc_call->proc==14)  /* NLM_UNLOCK_RES */
		|| (rpc_call->proc==15) ){	/* NLM_GRENTED_RES */
			if( (!pinfo->fd->flags.visited) ){
				nlm_register_unmatched_res(pinfo, tvb, offset);
			} else {
				nlm_print_msgres_reply(pinfo, tree, tvb);
			}
			/* for the fhandle matching that finds both request and
			   response packet */
			if(nfs_fhandle_reqrep_matching){
				nlm_match_fhandle_reply(pinfo, tree);
			}
		}
	}

	offset = dissect_rpc_data(tvb, tree, hf_nlm_cookie, offset);
	offset = dissect_rpc_uint32(tvb, tree, hf_nlm_stat, offset);
	return offset;
}

static int
dissect_nlm1_test(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree)
{
	return dissect_nlm_test(tvb,offset,pinfo,tree,1);
}

static int
dissect_nlm4_test(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree)
{
	return dissect_nlm_test(tvb,offset,pinfo,tree,4);
}


static int
dissect_nlm1_lock(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree)
{
	return dissect_nlm_lock(tvb,offset,pinfo,tree,1);
}

static int
dissect_nlm4_lock(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree)
{
	return dissect_nlm_lock(tvb,offset,pinfo,tree,4);
}


static int
dissect_nlm1_cancel(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree)
{
	return dissect_nlm_cancel(tvb,offset,pinfo,tree,1);
}

static int
dissect_nlm4_cancel(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree)
{
	return dissect_nlm_cancel(tvb,offset,pinfo,tree,4);
}


static int
dissect_nlm1_unlock(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree)
{
	return dissect_nlm_unlock(tvb,offset,pinfo,tree,1);
}

static int
dissect_nlm4_unlock(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree)
{
	return dissect_nlm_unlock(tvb,offset,pinfo,tree,4);
}


static int
dissect_nlm1_granted(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree)
{
	return dissect_nlm_granted(tvb,offset,pinfo,tree,1);
}

static int
dissect_nlm4_granted(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree)
{
	return dissect_nlm_granted(tvb,offset,pinfo,tree,4);
}


static int
dissect_nlm1_test_res(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree)
{
	return dissect_nlm_test_res(tvb,offset,pinfo,tree,1);
}

static int
dissect_nlm4_test_res(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree)
{
	return dissect_nlm_test_res(tvb,offset,pinfo,tree,4);
}

static int
dissect_nlm3_share(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree)
{
	return dissect_nlm_share(tvb,offset,pinfo,tree,3);
}

static int
dissect_nlm4_share(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree)
{
	return dissect_nlm_share(tvb,offset,pinfo,tree,4);
}

static int
dissect_nlm3_shareres(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree)
{
	return dissect_nlm_shareres(tvb,offset,pinfo,tree,3);
}

static int
dissect_nlm4_shareres(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree)
{
	return dissect_nlm_shareres(tvb,offset,pinfo,tree,4);
}

static int
dissect_nlm3_freeall(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree)
{
	return dissect_nlm_freeall(tvb,offset,pinfo,tree,3);
}

static int
dissect_nlm4_freeall(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree)
{
	return dissect_nlm_freeall(tvb,offset,pinfo,tree,4);
}




/* proc number, "proc name", dissect_request, dissect_reply */
/* NULL as function pointer means: type of arguments is "void". */
/* NLM protocol version 1 */
static const vsff nlm1_proc[] = {
	{ NLM_NULL,		"NULL",
		NULL,				NULL },
	{ NLM_TEST,		"TEST",
		dissect_nlm1_test,		dissect_nlm1_test_res },
	{ NLM_LOCK,		"LOCK",
		dissect_nlm1_lock,		dissect_nlm_gen_reply },
	{ NLM_CANCEL,		"CANCEL",
		dissect_nlm1_cancel,		dissect_nlm_gen_reply },
	{ NLM_UNLOCK,		"UNLOCK",
		dissect_nlm1_unlock,		dissect_nlm_gen_reply },
	{ NLM_GRANTED,		"GRANTED",
		dissect_nlm1_granted,		dissect_nlm_gen_reply },
	{ NLM_TEST_MSG,		"TEST_MSG",
		dissect_nlm1_test,		NULL },
	{ NLM_LOCK_MSG,		"LOCK_MSG",
		dissect_nlm1_lock,		NULL },
	{ NLM_CANCEL_MSG,	"CANCEL_MSG",
		dissect_nlm1_cancel,		NULL },
	{ NLM_UNLOCK_MSG,	"UNLOCK_MSG",
		dissect_nlm1_unlock,		NULL },
	{ NLM_GRANTED_MSG,	"GRANTED_MSG",
		dissect_nlm1_granted,		NULL },
	{ NLM_TEST_RES,		"TEST_RES",
		dissect_nlm1_test_res,		NULL },
	{ NLM_LOCK_RES,		"LOCK_RES",
		dissect_nlm_gen_reply,		NULL },
	{ NLM_CANCEL_RES,	"CANCEL_RES",
		dissect_nlm_gen_reply,		NULL },
	{ NLM_UNLOCK_RES,	"UNLOCK_RES",
		dissect_nlm_gen_reply,		NULL },
	{ NLM_GRANTED_RES,	"GRANTED_RES",
		dissect_nlm_gen_reply,		NULL },
	{ 0,			NULL,
		NULL,				NULL }
};
/* end of NLM protocol version 1 */

/* NLM protocol version 2 */
static const vsff nlm2_proc[] = {
	{ NLM_NULL,		"NULL",
		NULL,				NULL },
	{ NLM_TEST,		"TEST",
		dissect_nlm1_test,		dissect_nlm1_test_res },
	{ NLM_LOCK,		"LOCK",
		dissect_nlm1_lock,		dissect_nlm_gen_reply },
	{ NLM_CANCEL,		"CANCEL",
		dissect_nlm1_cancel,		dissect_nlm_gen_reply },
	{ NLM_UNLOCK,		"UNLOCK",
		dissect_nlm1_unlock,		dissect_nlm_gen_reply },
	{ NLM_GRANTED,		"GRANTED",
		dissect_nlm1_granted,		dissect_nlm_gen_reply },
	{ NLM_TEST_MSG,		"TEST_MSG",
		dissect_nlm1_test,		NULL },
	{ NLM_LOCK_MSG,		"LOCK_MSG",
		dissect_nlm1_lock,		NULL },
	{ NLM_CANCEL_MSG,	"CANCEL_MSG",
		dissect_nlm1_cancel,		NULL },
	{ NLM_UNLOCK_MSG,	"UNLOCK_MSG",
		dissect_nlm1_unlock,		NULL },
	{ NLM_GRANTED_MSG,	"GRANTED_MSG",
		dissect_nlm1_granted,		NULL },
	{ NLM_TEST_RES,		"TEST_RES",
		dissect_nlm1_test_res,		NULL },
	{ NLM_LOCK_RES,		"LOCK_RES",
		dissect_nlm_gen_reply,		NULL },
	{ NLM_CANCEL_RES,	"CANCEL_RES",
		dissect_nlm_gen_reply,		NULL },
	{ NLM_UNLOCK_RES,	"UNLOCK_RES",
		dissect_nlm_gen_reply,		NULL },
	{ NLM_GRANTED_RES,	"GRANTED_RES",
		dissect_nlm_gen_reply,		NULL },
	{ 0,			NULL,
		NULL,				NULL }
};
/* end of NLM protocol version 2 */

/* NLM protocol version 3 */
static const vsff nlm3_proc[] = {
	{ NLM_NULL,		"NULL",
		NULL,				NULL },
	{ NLM_TEST,		"TEST",
		dissect_nlm1_test,		dissect_nlm1_test_res },
	{ NLM_LOCK,		"LOCK",
		dissect_nlm1_lock,		dissect_nlm_gen_reply },
	{ NLM_CANCEL,		"CANCEL",
		dissect_nlm1_cancel,		dissect_nlm_gen_reply },
	{ NLM_UNLOCK,		"UNLOCK",
		dissect_nlm1_unlock,		dissect_nlm_gen_reply },
	{ NLM_GRANTED,		"GRANTED",
		dissect_nlm1_granted,		dissect_nlm_gen_reply },
	{ NLM_TEST_MSG,		"TEST_MSG",
		dissect_nlm1_test,		NULL },
	{ NLM_LOCK_MSG,		"LOCK_MSG",
		dissect_nlm1_lock,		NULL },
	{ NLM_CANCEL_MSG,	"CANCEL_MSG",
		dissect_nlm1_cancel,		NULL },
	{ NLM_UNLOCK_MSG,	"UNLOCK_MSG",
		dissect_nlm1_unlock,		NULL },
	{ NLM_GRANTED_MSG,	"GRANTED_MSG",
		dissect_nlm1_granted,		NULL },
	{ NLM_TEST_RES,		"TEST_RES",
		dissect_nlm1_test_res,		NULL },
	{ NLM_LOCK_RES,		"LOCK_RES",
		dissect_nlm_gen_reply,		NULL },
	{ NLM_CANCEL_RES,	"CANCEL_RES",
		dissect_nlm_gen_reply,		NULL },
	{ NLM_UNLOCK_RES,	"UNLOCK_RES",
		dissect_nlm_gen_reply,		NULL },
	{ NLM_GRANTED_RES,	"GRANTED_RES",
		dissect_nlm_gen_reply,		NULL },
	{ NLM_SHARE,		"SHARE",
		dissect_nlm3_share,		dissect_nlm3_shareres },
	{ NLM_UNSHARE,		"UNSHARE",
		dissect_nlm3_share,		dissect_nlm3_shareres },
	{ NLM_NM_LOCK,		"NM_LOCK",
		dissect_nlm1_lock,		dissect_nlm_gen_reply },
	{ NLM_FREE_ALL,		"FREE_ALL",
		dissect_nlm3_freeall,		NULL },
	{ 0,			NULL,
		NULL,				NULL }
};
/* end of NLM protocol version 3 */


/* NLM protocol version 4 */
static const vsff nlm4_proc[] = {
	{ NLM_NULL,		"NULL",
		NULL,				NULL },
	{ NLM_TEST,		"TEST",
		dissect_nlm4_test,		dissect_nlm4_test_res },
	{ NLM_LOCK,		"LOCK",
		dissect_nlm4_lock,		dissect_nlm_gen_reply },
	{ NLM_CANCEL,		"CANCEL",
		dissect_nlm4_cancel,		dissect_nlm_gen_reply },
	{ NLM_UNLOCK,		"UNLOCK",
		dissect_nlm4_unlock,	 	dissect_nlm_gen_reply },
	{ NLM_GRANTED,		"GRANTED",
		dissect_nlm4_granted,		dissect_nlm_gen_reply },
	{ NLM_TEST_MSG,		"TEST_MSG",
		dissect_nlm4_test,		NULL },
	{ NLM_LOCK_MSG,		"LOCK_MSG",
		dissect_nlm4_lock,		NULL },
	{ NLM_CANCEL_MSG,	"CANCEL_MSG",
		dissect_nlm4_cancel,		NULL },
	{ NLM_UNLOCK_MSG,	"UNLOCK_MSG",
		dissect_nlm4_unlock,		NULL },
	{ NLM_GRANTED_MSG,	"GRANTED_MSG",
		dissect_nlm4_granted,		NULL },
	{ NLM_TEST_RES,		"TEST_RES",
		dissect_nlm4_test_res,		NULL },
	{ NLM_LOCK_RES,		"LOCK_RES",
		dissect_nlm_gen_reply,		NULL },
	{ NLM_CANCEL_RES,	"CANCEL_RES",
		dissect_nlm_gen_reply,		NULL },
	{ NLM_UNLOCK_RES,	"UNLOCK_RES",
		dissect_nlm_gen_reply,		NULL },
	{ NLM_GRANTED_RES,	"GRANTED_RES",
		dissect_nlm_gen_reply,		NULL },
	{ NLM_SHARE,		"SHARE",
		dissect_nlm4_share,		dissect_nlm4_shareres },
	{ NLM_UNSHARE,		"UNSHARE",
		dissect_nlm4_share,		dissect_nlm4_shareres },
	{ NLM_NM_LOCK,		"NM_LOCK",
		dissect_nlm4_lock,		dissect_nlm_gen_reply },
	{ NLM_FREE_ALL,		"FREE_ALL",
		dissect_nlm4_freeall,		NULL },
	{ 0,			NULL,
		NULL,				NULL }
};
/* end of NLM protocol version 4 */


static struct true_false_string yesno = { "Yes", "No" };


void
proto_register_nlm(void)
{
	static hf_register_info hf[] = {
		{ &hf_nlm_cookie, {
			"cookie", "nlm.cookie", FT_BYTES, BASE_DEC,
			NULL, 0, "cookie", HFILL }},
		{ &hf_nlm_block, {
			"block", "nlm.block", FT_BOOLEAN, BASE_NONE,
			&yesno, 0, "block", HFILL }},
		{ &hf_nlm_exclusive, {
			"exclusive", "nlm.exclusive", FT_BOOLEAN, BASE_NONE,
			&yesno, 0, "exclusive", HFILL }},
		{ &hf_nlm_lock, {
			"lock", "nlm.lock", FT_NONE, 0,
			NULL, 0, "lock", HFILL }},
		{ &hf_nlm_lock_caller_name, {
			"caller_name", "nlm.lock.caller_name", FT_STRING, BASE_NONE,
			NULL, 0, "caller_name", HFILL }},
		{ &hf_nlm_lock_owner, {
			"owner", "nlm.lock.owner", FT_BYTES, BASE_DEC,
			NULL, 0, "owner", HFILL }},
		{ &hf_nlm_lock_svid, {
			"svid", "nlm.lock.svid", FT_UINT32, BASE_DEC,
			NULL, 0, "svid", HFILL }},
		{ &hf_nlm_lock_l_offset64, {
			"l_offset", "nlm.lock.l_offset", FT_UINT64, BASE_DEC,
			NULL, 0, "l_offset", HFILL }},
		{ &hf_nlm_lock_l_offset, {
			"l_offset", "nlm.lock.l_offset", FT_UINT32, BASE_DEC,
			NULL, 0, "l_offset", HFILL }},
		{ &hf_nlm_lock_l_len64, {
			"l_len", "nlm.lock.l_len", FT_UINT64, BASE_DEC,
			NULL, 0, "l_len", HFILL }},
		{ &hf_nlm_lock_l_len, {
			"l_len", "nlm.lock.l_len", FT_UINT32, BASE_DEC,
			NULL, 0, "l_len", HFILL }},
		{ &hf_nlm_reclaim, {
			"reclaim", "nlm.reclaim", FT_BOOLEAN, BASE_NONE,
			&yesno, 0, "reclaim", HFILL }},
		{ &hf_nlm_state, {
			"state", "nlm.state", FT_UINT32, BASE_DEC,
			NULL, 0, "STATD state", HFILL }},
		{ &hf_nlm_stat, {
			"stat", "nlm.stat", FT_UINT32, BASE_DEC,
			VALS(names_nlm_stats), 0, "stat", HFILL }},
		{ &hf_nlm_test_stat, {
			"test_stat", "nlm.test_stat", FT_NONE, 0,
			NULL, 0, "test_stat", HFILL }},
		{ &hf_nlm_test_stat_stat, {
			"stat", "nlm.test_stat.stat", FT_UINT32, BASE_DEC,
			VALS(names_nlm_stats), 0, "stat", HFILL }},
		{ &hf_nlm_holder, {
			"holder", "nlm.holder", FT_NONE, 0,
			NULL, 0, "holder", HFILL }},
		{ &hf_nlm_share, {
			"share", "nlm.share", FT_NONE, 0,
			NULL, 0, "share", HFILL }},
		{ &hf_nlm_share_mode, {
			"mode", "nlm.share.mode", FT_UINT32, BASE_DEC,
			VALS(names_fsh_mode), 0, "mode", HFILL }},
		{ &hf_nlm_share_access, {
			"access", "nlm.share.access", FT_UINT32, BASE_DEC,
			VALS(names_fsh_access), 0, "access", HFILL }},
		{ &hf_nlm_share_name, {
			"name", "nlm.share.name", FT_STRING, BASE_NONE,
			NULL, 0, "name", HFILL }},
		{ &hf_nlm_sequence, {
			"sequence", "nlm.sequence", FT_INT32, BASE_DEC,
			NULL, 0, "sequence", HFILL }},
		{ &hf_nlm_request_in, {
			"Request MSG in", "nlm.msg_in", FT_UINT32, BASE_DEC,
			NULL, 0, "The RES packet is a response to the MSG in this packet", HFILL }},
		{ &hf_nlm_reply_in, {
			"Reply RES in", "nlm.res_in", FT_UINT32, BASE_DEC,
			NULL, 0, "The response to this MSG packet is in this packet", HFILL }},
		{ &hf_nlm_time, {
			"Time from request", "nlm.time", FT_RELATIVE_TIME, BASE_NONE,
			NULL, 0, "Time between Request and Reply for async NLM calls", HFILL }},

		};

	static gint *ett[] = {
		&ett_nlm,
		&ett_nlm_lock,
	};
	module_t *nlm_module;

	proto_nlm = proto_register_protocol("Network Lock Manager Protocol",
	    "NLM", "nlm");
	proto_register_field_array(proto_nlm, hf, array_length(hf));
	proto_register_subtree_array(ett, array_length(ett));

	nlm_module = prefs_register_protocol(proto_nlm, NULL);
	prefs_register_bool_preference(nlm_module, "nlm_msg_res_matching",
		"Match MSG/RES packets for async NLM",
		"Whether the dissector will track and match MSG and RES calls for asynchronous NLM",
		&nlm_match_msgres);
	register_init_routine(nlm_msg_res_match_init);
}

void
proto_reg_handoff_nlm(void)
{
	/* Register the protocol as RPC */
	rpc_init_prog(proto_nlm, NLM_PROGRAM, ett_nlm);
	/* Register the procedure tables */
	rpc_init_proc_table(NLM_PROGRAM, 1, nlm1_proc, -1);
	rpc_init_proc_table(NLM_PROGRAM, 2, nlm2_proc, -1);
	rpc_init_proc_table(NLM_PROGRAM, 3, nlm3_proc, -1);
	rpc_init_proc_table(NLM_PROGRAM, 4, nlm4_proc, -1);
}
