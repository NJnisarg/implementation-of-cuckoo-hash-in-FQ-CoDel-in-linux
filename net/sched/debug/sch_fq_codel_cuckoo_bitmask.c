// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Fair Queue CoDel discipline
 *
 *  Copyright (C) 2012,2015 Eric Dumazet <edumazet@google.com>
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/in.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/skbuff.h>
#include <linux/jhash.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <net/netlink.h>
#include <net/pkt_sched.h>
#include <net/pkt_cls.h>
#include <net/codel.h>
#include <net/codel_impl.h>
#include <net/codel_qdisc.h>

/*	Fair Queue CoDel.
 *
 * Principles :
 * Packets are classified (internal classifier or external) on flows.
 * This is a Stochastic model (as we use a hash, several flows
 *			       might be hashed on same slot)
 * Each flow has a CoDel managed queue.
 * Flows are linked onto two (Round Robin) lists,
 * so that new flows have priority on old ones.
 *
 * For a given flow, packets are not reordered (CoDel uses a FIFO)
 * head drops only.
 * ECN capability is on by default.
 * Low memory footprint (64 bytes per flow)
 */

struct fq_codel_flow {
	struct sk_buff	  *head;
	struct sk_buff	  *tail;
	struct list_head  flowchain;
	int		  deficit;
	u32		  dropped; /* number of drops (or ECN marks) on this flow */
	struct codel_vars cvars;
}; /* please try to keep this structure <= 64 bytes */

struct fq_codel_sched_data {
	struct tcf_proto __rcu *filter_list; /* optional external classifier */
	struct tcf_block *block;
	// $$
	u16     *hashtable;      /* The hashtable holding the indexes into the flow table */
	u32		*random_seed;	/* Array of size 2 that will hold 2 random seeds for hash1 and hash2 */
	u32     *empty_flow_mask;    /* The bitmask array to maintain the empty flows */
	u32     flow_mask_index;     /* The 2 level index to find out the element that has atleast one empty flow. More like a lookup */
	struct fq_codel_flow *flows;	/* Flows table [flows_cnt] */
	u32		*backlogs;	/* backlog table [flows_cnt] */
	u32		flows_cnt;	/* number of flows */
	u32		quantum;	/* psched_mtu(qdisc_dev(sch)); */
	u32		drop_batch_size;
	u32		memory_limit;
	struct codel_params cparams;
	struct codel_stats cstats;
	u32		memory_usage;
	u32		drop_overmemory;
	u32		drop_overlimit;
	u32		new_flow_count;

	struct list_head new_flows;	/* list of new flows */
	struct list_head old_flows;	/* list of old flows */
};

static void print_internal_info(const struct fq_codel_sched_data *q)
{
	printk(KERN_EMERG "FQ_CODEL: PRINTING INTERNAL INFORMATION \n");

	u32 i;

	printk(KERN_EMERG "FQ_CODEL: PRINTING HASHTABLE \n");
	for(i=0;i<2*q->flows_cnt;i++)
	{
		printk(KERN_EMERG "idx:%d, val:%d\n", i,q->hashtable[i]);
	}

	printk(KERN_EMERG "FQ_CODEL: PRINTING BITMASK INDEX\n");
	printk(KERN_EMERG "idx:%d, val:%d\n", i,q->flow_mask_index);

	printk(KERN_EMERG "FQ_CODEL: PRINTING BITMASK ARRAY\n");
	for(i=0;i<32;i++)
	{
		printk(KERN_EMERG "idx:%d, val:%d\n", i,q->empty_flow_mask[i]);
	}
}

// $$
/*
 * This function simply gives you the empty flow.
 * It does not flip the bit to mark it as non-empty.
 * A separate function handles the bit flip
 * It is 0-indexed
 */
static unsigned int get_next_empty_flow(const struct fq_codel_sched_data *q)
{
	printk(KERN_EMERG "FQ_CODEL: ENTERING GET NEXT EMPTY FLOW \n");
	if(ffs(q->flow_mask_index)==0)
		return 0;
    u8 right_most_set_zone = 32 - ffs(q->flow_mask_index);
	printk(KERN_EMERG "FQ_CODEL: Right most set zone: %d \n", right_most_set_zone);
    return right_most_set_zone*32 + (32-ffs(q->empty_flow_mask[right_most_set_zone]));
}

// $$
/*
 * This function does the actual marking of flow as empty.
 */
static void mark_flow_as_empty(struct fq_codel_sched_data *q, int idx)
{
    // Setting a bit will mark the flow as empty
	printk(KERN_EMERG "FQ_CODEL: ENTERING MARK FLOW EMPTY \n");
    q->flow_mask_index |= (1 << (32 - (idx/32+1)));
    q->empty_flow_mask[idx/32] |= (1 << (32 - (idx%32+1)));
}

// $$
/*
 * This function does the actual marking of flow as non-empty.
 */
static void mark_flow_as_non_empty(struct fq_codel_sched_data *q, int idx)
{
    // Clearing a bit will mark the flow as occupied
	printk(KERN_EMERG "FQ_CODEL: ENTERING MARK FLOW NON EMPTY \n");
    q->empty_flow_mask[idx/32] &= ~(1 << (32 - (idx%32+1)));
    if(q->empty_flow_mask[idx/32]==0)
        q->flow_mask_index &= ~(1 << (32 - (idx/32+1)));
}

static unsigned int fq_codel_hash(const struct fq_codel_sched_data *q,
				  struct sk_buff *skb)
{
	printk(KERN_EMERG "FQ_CODEL: ENTERING HASH \n");
	return reciprocal_scale(skb_get_hash(skb), q->flows_cnt);
}

// $$
static unsigned int fq_codel_hash_modified(const struct fq_codel_sched_data *q,
                                  struct sk_buff *skb, int table_num)
{
	printk(KERN_EMERG "FQ_CODEL: ENTERING HASH MODIFIED\n");
    return q->flows_cnt*table_num + reciprocal_scale(skb_get_hash_perturb(skb,q->random_seed[table_num]), q->flows_cnt);
}

// $$
static void cuckoo_rehash(const struct fq_codel_sched_data *q,
                   struct sk_buff *skb, int value_to_insert)
{

	printk(KERN_EMERG "FQ_CODEL: ENTERING CUCKOO REHASH \n");

    int temp_index,i;
    for(i=0;i<(q->flows_cnt);i++){

        temp_index = fq_codel_hash_modified(q,skb,0) - 1;
        if(q->hashtable[temp_index]==0){
            q->hashtable[temp_index]=value_to_insert;
            return;
        }
        else
            swap(value_to_insert,q->hashtable[temp_index]);

        //No. of iterations increased by 1
        i++;
        if(i>=(q->flows_cnt))
            break;

        skb = (q->flows[value_to_insert-1].head);
		if(skb==NULL)
			return;
        temp_index = fq_codel_hash_modified(q,skb,1) - 1;
        if(q->hashtable[temp_index]==0){
            q->hashtable[temp_index]=value_to_insert;
            return;
        }
        else
            swap(value_to_insert,q->hashtable[temp_index]);

        skb = (q->flows[value_to_insert-1].head);
		if(skb==NULL)
			return;
    }
}

// $$
static unsigned int fq_codel_cuckoo_hash(const struct fq_codel_sched_data *q,
                                         struct sk_buff *skb)
{
    /*
     * First calculate the hash1 and hash2 values.
     */

	printk(KERN_EMERG "FQ_CODEL: ENTERING CUCKOO HASH \n");

    unsigned int hash1 = fq_codel_hash_modified(q,skb,0);
    unsigned int hash2 = fq_codel_hash_modified(q,skb,1);
	printk(KERN_EMERG "FQ_CODEL: values of hash1 and hash2: %d %d \n", hash1, hash2);

    int idx, idx2;

    if(q->hashtable[hash1]==0 && q->hashtable[hash2]==0)
    {
		printk(KERN_EMERG "FQ_CODEL:0 0 ==> BOTH SLOTS EMPTY, h1:%d h2:%d \n", hash1, hash2);
        q->hashtable[hash1] = get_next_empty_flow(q) + 1;
		printk(KERN_EMERG "FQ_CODEL: value in hashtable1 on slot %d is %d \n", hash1, q->hashtable[hash1]);
        return q->hashtable[hash1];
    }

    if(q->hashtable[hash1] != 0 && q->hashtable[hash2]==0)
    {
		printk(KERN_EMERG "FQ_CODEL:1 0 ==> H1 Non empty and H2 empty, h1:%d h2:%d \n", hash1, hash2);
        idx = q->hashtable[hash1] - 1;

		if(q->flows[idx].head==NULL)
			return q->hashtable[hash1];

        if(skb_get_hash(q->flows[idx].head)==skb_get_hash(skb))
            return q->hashtable[hash1];

		q->hashtable[hash2] = get_next_empty_flow(q) + 1;
		return q->hashtable[hash2];
    }

    if(q->hashtable[hash1] == 0 && q->hashtable[hash2] != 0)
    {
		printk(KERN_EMERG "FQ_CODEL:0 1 ==> H1 empty and H2 non empty, h1:%d h2:%d \n", hash1, hash2);
        idx = q->hashtable[hash2] - 1;

		if(q->flows[idx].head==NULL)
			return q->hashtable[hash2];

        if(skb_get_hash(q->flows[idx].head)==skb_get_hash(skb))
            return q->hashtable[hash2];
        
		q->hashtable[hash1] = get_next_empty_flow(q) + 1;
		return q->hashtable[hash1];
        
    }

	printk(KERN_EMERG "FQ_CODEL:1 1 ==> Both Non empty, h1:%d h2:%d \n", hash1, hash2);
    idx = q->hashtable[hash1] - 1;
    idx2 = q->hashtable[hash2] - 1;

	if(q->flows[idx].head==NULL)
		return q->hashtable[hash1];
	
	if(q->flows[idx2].head==NULL)
		return q->hashtable[hash2];

    if(skb_get_hash(q->flows[idx].head)==skb_get_hash(skb))
        return q->hashtable[hash1];
    if(skb_get_hash(q->flows[idx2].head)==skb_get_hash(skb))
        return q->hashtable[hash2];

    /*
     * If none of the above things prevail, then we have to allocate a new physical flow from the flows table to this packet.
     * We will put it at hashtable[hash1] location.
     * Then we will carry out rehashing of the other values in cuckoo fashion.
     */
    unsigned int value_to_insert = get_next_empty_flow(q) + 1;

    // Write that loop here to rehash stuff in the hashtable.
    // Remember rehashing is simply moving the flows table indexes around in our hashtable. We are touching no flows here.
    cuckoo_rehash(q,skb,value_to_insert);

    return value_to_insert;

	// If you don't want to rehash and let the collision happen
	// return q->hashtable[hash1];
}


static unsigned int fq_codel_classify(struct sk_buff *skb, struct Qdisc *sch,
				      int *qerr)
{
	printk(KERN_EMERG "FQ_CODEL: ENTERING CLASSIFY \n");

	struct fq_codel_sched_data *q = qdisc_priv(sch);
	struct tcf_proto *filter;
	struct tcf_result res;
	int result;

	if (TC_H_MAJ(skb->priority) == sch->handle &&
	    TC_H_MIN(skb->priority) > 0 &&
	    TC_H_MIN(skb->priority) <= q->flows_cnt)
		return TC_H_MIN(skb->priority);

	filter = rcu_dereference_bh(q->filter_list);
	if (!filter)
	{
		// $$
		u32 num = fq_codel_cuckoo_hash(q, skb);
		printk(KERN_EMERG "FQ_CODEL: The value returned by cuckoo hash:%d \n",num);
	    return num;
	}
	*qerr = NET_XMIT_SUCCESS | __NET_XMIT_BYPASS;
	result = tcf_classify(skb, filter, &res, false);
	if (result >= 0) {
#ifdef CONFIG_NET_CLS_ACT
		switch (result) {
		case TC_ACT_STOLEN:
		case TC_ACT_QUEUED:
		case TC_ACT_TRAP:
			*qerr = NET_XMIT_SUCCESS | __NET_XMIT_STOLEN;
			/* fall through */
		case TC_ACT_SHOT:
			return 0;
		}
#endif
		if (TC_H_MIN(res.classid) <= q->flows_cnt)
			return TC_H_MIN(res.classid);
	}
	return 0;
}

/* helper functions : might be changed when/if skb use a standard list_head */

/* remove one skb from head of slot queue */
static inline struct sk_buff *dequeue_head(struct fq_codel_flow *flow)
{
	printk(KERN_EMERG "FQ_CODEL: DEQUEUE HEAD \n");
	struct sk_buff *skb = flow->head;

	flow->head = skb->next;
	skb_mark_not_on_list(skb);
	return skb;
}

/* add skb to flow queue (tail add) */
static inline void flow_queue_add(struct fq_codel_flow *flow,
				  struct sk_buff *skb)
{
	printk(KERN_EMERG "FQ_CODEL: FLOW QUEUE ADD \n");
	if (flow->head == NULL)
		flow->head = skb;
	else
		flow->tail->next = skb;
	flow->tail = skb;
	skb->next = NULL;
}

static unsigned int fq_codel_drop(struct Qdisc *sch, unsigned int max_packets,
				  struct sk_buff **to_free)
{
	printk(KERN_EMERG "FQ_CODEL: FQ CODEL DROP \n");

	struct fq_codel_sched_data *q = qdisc_priv(sch);
	struct sk_buff *skb;
	unsigned int maxbacklog = 0, idx = 0, i, len;
	struct fq_codel_flow *flow;
	unsigned int threshold;
	unsigned int mem = 0;

	/* Queue is full! Find the fat flow and drop packet(s) from it.
	 * This might sound expensive, but with 1024 flows, we scan
	 * 4KB of memory, and we dont need to handle a complex tree
	 * in fast path (packet queue/enqueue) with many cache misses.
	 * In stress mode, we'll try to drop 64 packets from the flow,
	 * amortizing this linear lookup to one cache line per drop.
	 */
	for (i = 0; i < q->flows_cnt; i++) {
		if (q->backlogs[i] > maxbacklog) {
			maxbacklog = q->backlogs[i];
			idx = i;
		}
	}

	/* Our goal is to drop half of this fat flow backlog */
	threshold = maxbacklog >> 1;

	flow = &q->flows[idx];
	len = 0;
	i = 0;
	do {
		skb = dequeue_head(flow);
		len += qdisc_pkt_len(skb);
		mem += get_codel_cb(skb)->mem_usage;
		__qdisc_drop(skb, to_free);
	} while (++i < max_packets && len < threshold);

	flow->dropped += i;
	q->backlogs[idx] -= len;
	q->memory_usage -= mem;
	sch->qstats.drops += i;
	sch->qstats.backlog -= len;
	sch->q.qlen -= i;
	return idx;
}

static int fq_codel_enqueue(struct sk_buff *skb, struct Qdisc *sch,
			    struct sk_buff **to_free)
{
	printk(KERN_EMERG "FQ_CODEL: ENTERING ENQUEUE \n");

	struct fq_codel_sched_data *q = qdisc_priv(sch);

	unsigned int idx, prev_backlog, prev_qlen;
	struct fq_codel_flow *flow;
	int uninitialized_var(ret);
	unsigned int pkt_len;
	bool memory_limited;

	idx = fq_codel_classify(skb, sch, &ret);
	if (idx == 0) {
		if (ret & __NET_XMIT_BYPASS)
			qdisc_qstats_drop(sch);
		__qdisc_drop(skb, to_free);
		return ret;
	}
	idx--;
	printk(KERN_EMERG "FQ_CODEL: The value returned by classify:%d \n",idx);

	codel_set_enqueue_time(skb);
	flow = &q->flows[idx];
	flow_queue_add(flow, skb);
	q->backlogs[idx] += qdisc_pkt_len(skb);
	qdisc_qstats_backlog_inc(sch, skb);
	// $$
	mark_flow_as_non_empty(q,idx);

	if (list_empty(&flow->flowchain)) {
		list_add_tail(&flow->flowchain, &q->new_flows);
		q->new_flow_count++;
		flow->deficit = q->quantum;
		flow->dropped = 0;
	}
	get_codel_cb(skb)->mem_usage = skb->truesize;
	q->memory_usage += get_codel_cb(skb)->mem_usage;
	memory_limited = q->memory_usage > q->memory_limit;
	if (++sch->q.qlen <= sch->limit && !memory_limited)
		return NET_XMIT_SUCCESS;

	prev_backlog = sch->qstats.backlog;
	prev_qlen = sch->q.qlen;

	/* save this packet length as it might be dropped by fq_codel_drop() */
	pkt_len = qdisc_pkt_len(skb);
	/* fq_codel_drop() is quite expensive, as it performs a linear search
	 * in q->backlogs[] to find a fat flow.
	 * So instead of dropping a single packet, drop half of its backlog
	 * with a 64 packets limit to not add a too big cpu spike here.
	 */
	ret = fq_codel_drop(sch, q->drop_batch_size, to_free);

	prev_qlen -= sch->q.qlen;
	prev_backlog -= sch->qstats.backlog;
	q->drop_overlimit += prev_qlen;
	if (memory_limited)
		q->drop_overmemory += prev_qlen;

	/* As we dropped packet(s), better let upper stack know this.
	 * If we dropped a packet for this flow, return NET_XMIT_CN,
	 * but in this case, our parents wont increase their backlogs.
	 */
	if (ret == idx) {
		qdisc_tree_reduce_backlog(sch, prev_qlen - 1,
					  prev_backlog - pkt_len);
		return NET_XMIT_CN;
	}
	qdisc_tree_reduce_backlog(sch, prev_qlen, prev_backlog);
	return NET_XMIT_SUCCESS;
}

/* This is the specific function called from codel_dequeue()
 * to dequeue a packet from queue. Note: backlog is handled in
 * codel, we dont need to reduce it here.
 */
static struct sk_buff *dequeue_func(struct codel_vars *vars, void *ctx)
{

	printk(KERN_EMERG "FQ_CODEL: DEQUEUE_FUNC \n");

	struct Qdisc *sch = ctx;
	struct fq_codel_sched_data *q = qdisc_priv(sch);
	struct fq_codel_flow *flow;
	struct sk_buff *skb = NULL;

	flow = container_of(vars, struct fq_codel_flow, cvars);
	if (flow->head) {
		skb = dequeue_head(flow);
		q->backlogs[flow - q->flows] -= qdisc_pkt_len(skb);
		q->memory_usage -= get_codel_cb(skb)->mem_usage;
		sch->q.qlen--;
		sch->qstats.backlog -= qdisc_pkt_len(skb);
	}
	return skb;
}

static void drop_func(struct sk_buff *skb, void *ctx)
{
	printk(KERN_EMERG "FQ_CODEL: DROP FUNC \n");

	struct Qdisc *sch = ctx;

	kfree_skb(skb);
	qdisc_qstats_drop(sch);
}

static struct sk_buff *fq_codel_dequeue(struct Qdisc *sch)
{

	struct fq_codel_sched_data *q = qdisc_priv(sch);
	struct sk_buff *skb;
	struct fq_codel_flow *flow;
	struct list_head *head;
	u32 prev_drop_count, prev_ecn_mark;

begin:
	head = &q->new_flows;
	if (list_empty(head)) {
		head = &q->old_flows;
		if (list_empty(head))
			return NULL;
	}
	flow = list_first_entry(head, struct fq_codel_flow, flowchain);

	if (flow->deficit <= 0) {
		flow->deficit += q->quantum;
		list_move_tail(&flow->flowchain, &q->old_flows);
		goto begin;
	}

	prev_drop_count = q->cstats.drop_count;
	prev_ecn_mark = q->cstats.ecn_mark;

	skb = codel_dequeue(sch, &sch->qstats.backlog, &q->cparams,
			    &flow->cvars, &q->cstats, qdisc_pkt_len,
			    codel_get_enqueue_time, drop_func, dequeue_func);

	flow->dropped += q->cstats.drop_count - prev_drop_count;
	flow->dropped += q->cstats.ecn_mark - prev_ecn_mark;

	if (!skb) {
		/* force a pass through old_flows to prevent starvation */
		if ((head == &q->new_flows) && !list_empty(&q->old_flows))
			list_move_tail(&flow->flowchain, &q->old_flows);
		else
			list_del_init(&flow->flowchain);
		goto begin;
	}
	printk(KERN_EMERG "FQ_CODEL: ENTERING DEQUEUE NOT NULL \n");
	qdisc_bstats_update(sch, skb);
	flow->deficit -= qdisc_pkt_len(skb);
	/* We cant call qdisc_tree_reduce_backlog() if our qlen is 0,
	 * or HTB crashes. Defer it for next round.
	 */
	if (q->cstats.drop_count && sch->q.qlen) {
		qdisc_tree_reduce_backlog(sch, q->cstats.drop_count,
					  q->cstats.drop_len);
		q->cstats.drop_count = 0;
		q->cstats.drop_len = 0;
	}

	// $$
	/*
	* Marking the flow as empty and setting the hashtable entry to 0
	*/
	if(flow->head==NULL)
	{
		printk(KERN_EMERG "Going to mark the flow as empty \n");
		int empty_id = (flow - &q->flows[0]);
		printk(KERN_EMERG "The empty ID: %d \n", empty_id);
		mark_flow_as_empty(q, empty_id);
		printk(KERN_EMERG "Marked the flow as empty! \n");
		
		int h1 = fq_codel_hash_modified(q,skb,0);
		int h2 = fq_codel_hash_modified(q,skb,1);

		printk(KERN_EMERG "h1: %d and h2:%d and table[h1]:%d and table[h2]:%d \n", h1,h2, q->hashtable[h1], q->hashtable[h2]);
		if(q->hashtable[h1]==(empty_id+1))
		{
			printk(KERN_EMERG "Went to h1 \n");
			q->hashtable[h1] = 0;
		}
		if(q->hashtable[h2]==(empty_id+1))
		{
			printk(KERN_EMERG "Went to h2 \n");
			q->hashtable[h2] = 0;
		}
	}

	printk(KERN_EMERG "SKB that was dequeued:%p \n", skb);
	return skb;
}

static void fq_codel_flow_purge(struct fq_codel_flow *flow)
{

	printk(KERN_EMERG "FQ_CODEL: FLOW PURGE \n");

	rtnl_kfree_skbs(flow->head, flow->tail);
	flow->head = NULL;
}

static void fq_codel_reset(struct Qdisc *sch)
{

	printk(KERN_EMERG "FQ_CODEL: ENTERING FQ_CODEL_RESET \n");

	struct fq_codel_sched_data *q = qdisc_priv(sch);
	int i;

	INIT_LIST_HEAD(&q->new_flows);
	INIT_LIST_HEAD(&q->old_flows);
	for (i = 0; i < q->flows_cnt; i++) {
		struct fq_codel_flow *flow = q->flows + i;

		fq_codel_flow_purge(flow);
		INIT_LIST_HEAD(&flow->flowchain);
		codel_vars_init(&flow->cvars);
	}
	memset(q->backlogs, 0, q->flows_cnt * sizeof(u32));
	// $$
    memset(q->hashtable, 0, 2 * q->flows_cnt * sizeof(u16));
    memset(q->empty_flow_mask, 1, 32 * sizeof(u32));
    memset(&q->flow_mask_index, 1, sizeof(u32));

	sch->q.qlen = 0;
	sch->qstats.backlog = 0;
	q->memory_usage = 0;
}

static const struct nla_policy fq_codel_policy[TCA_FQ_CODEL_MAX + 1] = {
	[TCA_FQ_CODEL_TARGET]	= { .type = NLA_U32 },
	[TCA_FQ_CODEL_LIMIT]	= { .type = NLA_U32 },
	[TCA_FQ_CODEL_INTERVAL]	= { .type = NLA_U32 },
	[TCA_FQ_CODEL_ECN]	= { .type = NLA_U32 },
	[TCA_FQ_CODEL_FLOWS]	= { .type = NLA_U32 },
	[TCA_FQ_CODEL_QUANTUM]	= { .type = NLA_U32 },
	[TCA_FQ_CODEL_CE_THRESHOLD] = { .type = NLA_U32 },
	[TCA_FQ_CODEL_DROP_BATCH_SIZE] = { .type = NLA_U32 },
	[TCA_FQ_CODEL_MEMORY_LIMIT] = { .type = NLA_U32 },
};

static int fq_codel_change(struct Qdisc *sch, struct nlattr *opt,
			   struct netlink_ext_ack *extack)
{

	printk(KERN_EMERG "FQ_CODEL: ENTERING FQ_CODEL_CHANGE \n");

	struct fq_codel_sched_data *q = qdisc_priv(sch);
	struct nlattr *tb[TCA_FQ_CODEL_MAX + 1];
	int err;

	if (!opt)
		return -EINVAL;

	err = nla_parse_nested_deprecated(tb, TCA_FQ_CODEL_MAX, opt,
					  fq_codel_policy, NULL);
	if (err < 0)
		return err;
	if (tb[TCA_FQ_CODEL_FLOWS]) {
		if (q->flows)
			return -EINVAL;
		q->flows_cnt = nla_get_u32(tb[TCA_FQ_CODEL_FLOWS]);
		if (!q->flows_cnt ||
		    q->flows_cnt > 65536)
			return -EINVAL;
	}
	sch_tree_lock(sch);

	if (tb[TCA_FQ_CODEL_TARGET]) {
		u64 target = nla_get_u32(tb[TCA_FQ_CODEL_TARGET]);

		q->cparams.target = (target * NSEC_PER_USEC) >> CODEL_SHIFT;
	}

	if (tb[TCA_FQ_CODEL_CE_THRESHOLD]) {
		u64 val = nla_get_u32(tb[TCA_FQ_CODEL_CE_THRESHOLD]);

		q->cparams.ce_threshold = (val * NSEC_PER_USEC) >> CODEL_SHIFT;
	}

	if (tb[TCA_FQ_CODEL_INTERVAL]) {
		u64 interval = nla_get_u32(tb[TCA_FQ_CODEL_INTERVAL]);

		q->cparams.interval = (interval * NSEC_PER_USEC) >> CODEL_SHIFT;
	}

	if (tb[TCA_FQ_CODEL_LIMIT])
		sch->limit = nla_get_u32(tb[TCA_FQ_CODEL_LIMIT]);

	if (tb[TCA_FQ_CODEL_ECN])
		q->cparams.ecn = !!nla_get_u32(tb[TCA_FQ_CODEL_ECN]);

	if (tb[TCA_FQ_CODEL_QUANTUM])
		q->quantum = max(256U, nla_get_u32(tb[TCA_FQ_CODEL_QUANTUM]));

	if (tb[TCA_FQ_CODEL_DROP_BATCH_SIZE])
		q->drop_batch_size = min(1U, nla_get_u32(tb[TCA_FQ_CODEL_DROP_BATCH_SIZE]));

	if (tb[TCA_FQ_CODEL_MEMORY_LIMIT])
		q->memory_limit = min(1U << 31, nla_get_u32(tb[TCA_FQ_CODEL_MEMORY_LIMIT]));

	while (sch->q.qlen > sch->limit ||
	       q->memory_usage > q->memory_limit) {
		printk(KERN_EMERG "FQ_CODEL: DEQUEUE BEING CALLED FROM FQ_CODEL_CHANGE \n");
		struct sk_buff *skb = fq_codel_dequeue(sch);

		q->cstats.drop_len += qdisc_pkt_len(skb);
		rtnl_kfree_skbs(skb, skb);
		q->cstats.drop_count++;
	}
	qdisc_tree_reduce_backlog(sch, q->cstats.drop_count, q->cstats.drop_len);
	q->cstats.drop_count = 0;
	q->cstats.drop_len = 0;

	sch_tree_unlock(sch);
	return 0;
}

static void fq_codel_destroy(struct Qdisc *sch)
{
	printk(KERN_EMERG "FQ_CODEL: ENTERING FQ_CODEL_DESTROY \n");

	struct fq_codel_sched_data *q = qdisc_priv(sch);

	tcf_block_put(q->block);
	kvfree(q->backlogs);
	kvfree(q->flows);
}

static int fq_codel_init(struct Qdisc *sch, struct nlattr *opt,
			 struct netlink_ext_ack *extack)
{

	printk(KERN_EMERG "FQ_CODEL: ENTERING INIT \n");

	struct fq_codel_sched_data *q = qdisc_priv(sch);
	int i;
	int err;

	sch->limit = 10*1024;
	q->flows_cnt = 1024;
	q->memory_limit = 32 << 20; /* 32 MBytes */
	q->drop_batch_size = 64;
	q->quantum = psched_mtu(qdisc_dev(sch));
	INIT_LIST_HEAD(&q->new_flows);
	INIT_LIST_HEAD(&q->old_flows);
	codel_params_init(&q->cparams);
	codel_stats_init(&q->cstats);
	q->cparams.ecn = true;
	q->cparams.mtu = psched_mtu(qdisc_dev(sch));

	if (opt) {
		err = fq_codel_change(sch, opt, extack);
		if (err)
			goto init_failure;
	}

	err = tcf_block_get(&q->block, &q->filter_list, sch, extack);
	if (err)
		goto init_failure;

	if (!q->flows) {
		q->flows = kvcalloc(q->flows_cnt,
				    sizeof(struct fq_codel_flow),
				    GFP_KERNEL);
		if (!q->flows) {
			err = -ENOMEM;
			goto init_failure;
		}
		q->backlogs = kvcalloc(q->flows_cnt, sizeof(u32), GFP_KERNEL);
		if (!q->backlogs) {
			err = -ENOMEM;
			goto alloc_failure;
		}
		// $$
		/*
		 * Allocation of memory for the hashtable
		 */
        q->hashtable = kvcalloc(2*q->flows_cnt, sizeof(u16), GFP_KERNEL);
        if (!q->hashtable) {
            err = -ENOMEM;
            goto alloc_failure;
        }
		// $$
		/*
		 * Allocation of memory for the random_seed
		 */
        q->random_seed = kvcalloc(2, sizeof(32), GFP_KERNEL);
        if (!q->random_seed) {
            err = -ENOMEM;
            goto alloc_failure;
        }
		q->random_seed[0] = get_random_u32();
		q->random_seed[1] = get_random_u32();
        // $$
        /* We have 1024 flows. Hence 32*32 = 1024 bits allocated */
        q->empty_flow_mask = kvcalloc(32, sizeof(u32), GFP_KERNEL);
        memset(q->empty_flow_mask, 1, 32 * sizeof(u32));

        // $$
        /* Memset the flow_mask_index to 1 */
        memset(&q->flow_mask_index, 1, sizeof(u32));

		for (i = 0; i < q->flows_cnt; i++) {
			struct fq_codel_flow *flow = q->flows + i;

			INIT_LIST_HEAD(&flow->flowchain);
			codel_vars_init(&flow->cvars);
		}
	}

	printk(KERN_EMERG "FQ_CODEL: EXITING INIT \n");

	if (sch->limit >= 1)
		sch->flags |= TCQ_F_CAN_BYPASS;
	else
		sch->flags &= ~TCQ_F_CAN_BYPASS;
	return 0;

alloc_failure:
	kvfree(q->flows);
	q->flows = NULL;
init_failure:
	q->flows_cnt = 0;
	return err;
}

static int fq_codel_dump(struct Qdisc *sch, struct sk_buff *skb)
{

	printk(KERN_EMERG "FQ_CODEL: ENTERING FQ_CODEL_DUMP \n");

	struct fq_codel_sched_data *q = qdisc_priv(sch);
	struct nlattr *opts;

	opts = nla_nest_start_noflag(skb, TCA_OPTIONS);
	if (opts == NULL)
		goto nla_put_failure;

	if (nla_put_u32(skb, TCA_FQ_CODEL_TARGET,
			codel_time_to_us(q->cparams.target)) ||
	    nla_put_u32(skb, TCA_FQ_CODEL_LIMIT,
			sch->limit) ||
	    nla_put_u32(skb, TCA_FQ_CODEL_INTERVAL,
			codel_time_to_us(q->cparams.interval)) ||
	    nla_put_u32(skb, TCA_FQ_CODEL_ECN,
			q->cparams.ecn) ||
	    nla_put_u32(skb, TCA_FQ_CODEL_QUANTUM,
			q->quantum) ||
	    nla_put_u32(skb, TCA_FQ_CODEL_DROP_BATCH_SIZE,
			q->drop_batch_size) ||
	    nla_put_u32(skb, TCA_FQ_CODEL_MEMORY_LIMIT,
			q->memory_limit) ||
	    nla_put_u32(skb, TCA_FQ_CODEL_FLOWS,
			q->flows_cnt))
		goto nla_put_failure;

	if (q->cparams.ce_threshold != CODEL_DISABLED_THRESHOLD &&
	    nla_put_u32(skb, TCA_FQ_CODEL_CE_THRESHOLD,
			codel_time_to_us(q->cparams.ce_threshold)))
		goto nla_put_failure;

	return nla_nest_end(skb, opts);

nla_put_failure:
	return -1;
}

static int fq_codel_dump_stats(struct Qdisc *sch, struct gnet_dump *d)
{
	printk(KERN_EMERG "FQ_CODEL: ENTERING FQ_CODEL_DUMP_STATS \n");

	struct fq_codel_sched_data *q = qdisc_priv(sch);
	struct tc_fq_codel_xstats st = {
		.type				= TCA_FQ_CODEL_XSTATS_QDISC,
	};
	struct list_head *pos;

	st.qdisc_stats.maxpacket = q->cstats.maxpacket;
	st.qdisc_stats.drop_overlimit = q->drop_overlimit;
	st.qdisc_stats.ecn_mark = q->cstats.ecn_mark;
	st.qdisc_stats.new_flow_count = q->new_flow_count;
	st.qdisc_stats.ce_mark = q->cstats.ce_mark;
	st.qdisc_stats.memory_usage  = q->memory_usage;
	st.qdisc_stats.drop_overmemory = q->drop_overmemory;

	sch_tree_lock(sch);
	list_for_each(pos, &q->new_flows)
		st.qdisc_stats.new_flows_len++;

	list_for_each(pos, &q->old_flows)
		st.qdisc_stats.old_flows_len++;
	sch_tree_unlock(sch);

	return gnet_stats_copy_app(d, &st, sizeof(st));
}

static struct Qdisc *fq_codel_leaf(struct Qdisc *sch, unsigned long arg)
{
	return NULL;
}

static unsigned long fq_codel_find(struct Qdisc *sch, u32 classid)
{
	return 0;
}

static unsigned long fq_codel_bind(struct Qdisc *sch, unsigned long parent,
			      u32 classid)
{
	return 0;
}

static void fq_codel_unbind(struct Qdisc *q, unsigned long cl)
{
}

static struct tcf_block *fq_codel_tcf_block(struct Qdisc *sch, unsigned long cl,
					    struct netlink_ext_ack *extack)
{
	printk(KERN_EMERG "FQ_CODEL: ENTERING TCF_BLOCK \n");
	struct fq_codel_sched_data *q = qdisc_priv(sch);

	if (cl)
		return NULL;
	return q->block;
}

static int fq_codel_dump_class(struct Qdisc *sch, unsigned long cl,
			  struct sk_buff *skb, struct tcmsg *tcm)
{
	tcm->tcm_handle |= TC_H_MIN(cl);
	return 0;
}

static int fq_codel_dump_class_stats(struct Qdisc *sch, unsigned long cl,
				     struct gnet_dump *d)
{

	printk(KERN_EMERG "FQ_CODEL: ENTERING FQ_CODEL_DUMP_CLASS_STATS \n");

	struct fq_codel_sched_data *q = qdisc_priv(sch);
	u32 idx = cl - 1;
	struct gnet_stats_queue qs = { 0 };
	struct tc_fq_codel_xstats xstats;

	if (idx < q->flows_cnt) {
		const struct fq_codel_flow *flow = &q->flows[idx];
		const struct sk_buff *skb;

		memset(&xstats, 0, sizeof(xstats));
		xstats.type = TCA_FQ_CODEL_XSTATS_CLASS;
		xstats.class_stats.deficit = flow->deficit;
		xstats.class_stats.ldelay =
			codel_time_to_us(flow->cvars.ldelay);
		xstats.class_stats.count = flow->cvars.count;
		xstats.class_stats.lastcount = flow->cvars.lastcount;
		xstats.class_stats.dropping = flow->cvars.dropping;
		if (flow->cvars.dropping) {
			codel_tdiff_t delta = flow->cvars.drop_next -
					      codel_get_time();

			xstats.class_stats.drop_next = (delta >= 0) ?
				codel_time_to_us(delta) :
				-codel_time_to_us(-delta);
		}
		if (flow->head) {
			sch_tree_lock(sch);
			skb = flow->head;
			while (skb) {
				qs.qlen++;
				skb = skb->next;
			}
			sch_tree_unlock(sch);
		}
		qs.backlog = q->backlogs[idx];
		qs.drops = flow->dropped;
	}
	if (gnet_stats_copy_queue(d, NULL, &qs, qs.qlen) < 0)
		return -1;
	if (idx < q->flows_cnt)
		return gnet_stats_copy_app(d, &xstats, sizeof(xstats));
	return 0;
}

static void fq_codel_walk(struct Qdisc *sch, struct qdisc_walker *arg)
{
	printk(KERN_EMERG "FQ_CODEL: ENTERING FQ_CODEL_WALK \n");
	struct fq_codel_sched_data *q = qdisc_priv(sch);
	unsigned int i;

	if (arg->stop)
		return;

	for (i = 0; i < q->flows_cnt; i++) {
		if (list_empty(&q->flows[i].flowchain) ||
		    arg->count < arg->skip) {
			arg->count++;
			continue;
		}
		if (arg->fn(sch, i + 1, arg) < 0) {
			arg->stop = 1;
			break;
		}
		arg->count++;
	}
}

static const struct Qdisc_class_ops fq_codel_class_ops = {
	.leaf		=	fq_codel_leaf,
	.find		=	fq_codel_find,
	.tcf_block	=	fq_codel_tcf_block,
	.bind_tcf	=	fq_codel_bind,
	.unbind_tcf	=	fq_codel_unbind,
	.dump		=	fq_codel_dump_class,
	.dump_stats	=	fq_codel_dump_class_stats,
	.walk		=	fq_codel_walk,
};

static struct Qdisc_ops fq_codel_qdisc_ops __read_mostly = {
	.cl_ops		=	&fq_codel_class_ops,
	.id		=	"fq_codel",
	.priv_size	=	sizeof(struct fq_codel_sched_data),
	.enqueue	=	fq_codel_enqueue,
	.dequeue	=	fq_codel_dequeue,
	.peek		=	qdisc_peek_dequeued,
	.init		=	fq_codel_init,
	.reset		=	fq_codel_reset,
	.destroy	=	fq_codel_destroy,
	.change		=	fq_codel_change,
	.dump		=	fq_codel_dump,
	.dump_stats =	fq_codel_dump_stats,
	.owner		=	THIS_MODULE,
};

static int __init fq_codel_module_init(void)
{
	return register_qdisc(&fq_codel_qdisc_ops);
}

static void __exit fq_codel_module_exit(void)
{
	unregister_qdisc(&fq_codel_qdisc_ops);
}

module_init(fq_codel_module_init)
module_exit(fq_codel_module_exit)
MODULE_AUTHOR("Eric Dumazet");
MODULE_LICENSE("GPL");
