/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2018 Intel Corporation
 */

#include <stdlib.h>
#include <string.h>

#include <rte_common.h>
#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_ip.h>


#include "rte_table_action.h"

#define rte_htons rte_cpu_to_be_16
#define rte_htonl rte_cpu_to_be_32

#define rte_ntohs rte_be_to_cpu_16
#define rte_ntohl rte_be_to_cpu_32

/**
 * RTE_TABLE_ACTION_FWD
 */
#define fwd_data rte_pipeline_table_entry

static int
fwd_apply(struct fwd_data *data,
	struct rte_table_action_fwd_params *p)
{
	data->action = p->action;

	if (p->action == RTE_PIPELINE_ACTION_PORT)
		data->port_id = p->id;

	if (p->action == RTE_PIPELINE_ACTION_TABLE)
		data->table_id = p->id;

	return 0;
}

/**
 * RTE_TABLE_ACTION_MTR
 */
static int
mtr_cfg_check(struct rte_table_action_mtr_config *mtr)
{
	if ((mtr->alg == RTE_TABLE_ACTION_METER_SRTCM) ||
		((mtr->n_tc != 1) && (mtr->n_tc != 4)) ||
		(mtr->n_bytes_enabled != 0))
		return -ENOTSUP;
	return 0;
}

#define MBUF_SCHED_QUEUE_TC_COLOR(queue, tc, color)        \
	((uint16_t)((((uint64_t)(queue)) & 0x3) |          \
	((((uint64_t)(tc)) & 0x3) << 2) |                  \
	((((uint64_t)(color)) & 0x3) << 4)))

#define MBUF_SCHED_COLOR(sched, color)                     \
	(((sched) & (~0x30LLU)) | ((color) << 4))

struct mtr_trtcm_data {
	struct rte_meter_trtcm trtcm;
	uint64_t stats[e_RTE_METER_COLORS];
} __attribute__((__packed__));

#define MTR_TRTCM_DATA_METER_PROFILE_ID_GET(data)          \
	(((data)->stats[e_RTE_METER_GREEN] & 0xF8LLU) >> 3)

static void
mtr_trtcm_data_meter_profile_id_set(struct mtr_trtcm_data *data,
	uint32_t profile_id)
{
	data->stats[e_RTE_METER_GREEN] &= ~0xF8LLU;
	data->stats[e_RTE_METER_GREEN] |= (profile_id % 32) << 3;
}

#define MTR_TRTCM_DATA_POLICER_ACTION_DROP_GET(data, color)\
	(((data)->stats[(color)] & 4LLU) >> 2)

#define MTR_TRTCM_DATA_POLICER_ACTION_COLOR_GET(data, color)\
	((enum rte_meter_color)((data)->stats[(color)] & 3LLU))

static void
mtr_trtcm_data_policer_action_set(struct mtr_trtcm_data *data,
	enum rte_meter_color color,
	enum rte_table_action_policer action)
{
	if (action == RTE_TABLE_ACTION_POLICER_DROP) {
		data->stats[color] |= 4LLU;
	} else {
		data->stats[color] &= ~7LLU;
		data->stats[color] |= color & 3LLU;
	}
}

static uint64_t
mtr_trtcm_data_stats_get(struct mtr_trtcm_data *data,
	enum rte_meter_color color)
{
	return data->stats[color] >> 8;
}

static void
mtr_trtcm_data_stats_reset(struct mtr_trtcm_data *data,
	enum rte_meter_color color)
{
	data->stats[color] &= 0xFFLU;
}

#define MTR_TRTCM_DATA_STATS_INC(data, color)              \
	((data)->stats[(color)] += (1LLU << 8))

static size_t
mtr_data_size(struct rte_table_action_mtr_config *mtr)
{
	return mtr->n_tc * sizeof(struct mtr_trtcm_data);
}

struct dscp_table_entry_data {
	enum rte_meter_color color;
	uint16_t tc;
	uint16_t queue_tc_color;
};

struct dscp_table_data {
	struct dscp_table_entry_data entry[64];
};

struct meter_profile_data {
	struct rte_meter_trtcm_profile profile;
	uint32_t profile_id;
	int valid;
};

static struct meter_profile_data *
meter_profile_data_find(struct meter_profile_data *mp,
	uint32_t mp_size,
	uint32_t profile_id)
{
	uint32_t i;

	for (i = 0; i < mp_size; i++) {
		struct meter_profile_data *mp_data = &mp[i];

		if (mp_data->valid && (mp_data->profile_id == profile_id))
			return mp_data;
	}

	return NULL;
}

static struct meter_profile_data *
meter_profile_data_find_unused(struct meter_profile_data *mp,
	uint32_t mp_size)
{
	uint32_t i;

	for (i = 0; i < mp_size; i++) {
		struct meter_profile_data *mp_data = &mp[i];

		if (!mp_data->valid)
			return mp_data;
	}

	return NULL;
}

static int
mtr_apply_check(struct rte_table_action_mtr_params *p,
	struct rte_table_action_mtr_config *cfg,
	struct meter_profile_data *mp,
	uint32_t mp_size)
{
	uint32_t i;

	if (p->tc_mask > RTE_LEN2MASK(cfg->n_tc, uint32_t))
		return -EINVAL;

	for (i = 0; i < RTE_TABLE_ACTION_TC_MAX; i++) {
		struct rte_table_action_mtr_tc_params *p_tc = &p->mtr[i];
		struct meter_profile_data *mp_data;

		if ((p->tc_mask & (1LLU << i)) == 0)
			continue;

		mp_data = meter_profile_data_find(mp,
			mp_size,
			p_tc->meter_profile_id);
		if (!mp_data)
			return -EINVAL;
	}

	return 0;
}

static int
mtr_apply(struct mtr_trtcm_data *data,
	struct rte_table_action_mtr_params *p,
	struct rte_table_action_mtr_config *cfg,
	struct meter_profile_data *mp,
	uint32_t mp_size)
{
	uint32_t i;
	int status;

	/* Check input arguments */
	status = mtr_apply_check(p, cfg, mp, mp_size);
	if (status)
		return status;

	/* Apply */
	for (i = 0; i < RTE_TABLE_ACTION_TC_MAX; i++) {
		struct rte_table_action_mtr_tc_params *p_tc = &p->mtr[i];
		struct mtr_trtcm_data *data_tc = &data[i];
		struct meter_profile_data *mp_data;

		if ((p->tc_mask & (1LLU << i)) == 0)
			continue;

		/* Find profile */
		mp_data = meter_profile_data_find(mp,
			mp_size,
			p_tc->meter_profile_id);
		if (!mp_data)
			return -EINVAL;

		memset(data_tc, 0, sizeof(*data_tc));

		/* Meter object */
		status = rte_meter_trtcm_config(&data_tc->trtcm,
			&mp_data->profile);
		if (status)
			return status;

		/* Meter profile */
		mtr_trtcm_data_meter_profile_id_set(data_tc,
			mp_data - mp);

		/* Policer actions */
		mtr_trtcm_data_policer_action_set(data_tc,
			e_RTE_METER_GREEN,
			p_tc->policer[e_RTE_METER_GREEN]);

		mtr_trtcm_data_policer_action_set(data_tc,
			e_RTE_METER_YELLOW,
			p_tc->policer[e_RTE_METER_YELLOW]);

		mtr_trtcm_data_policer_action_set(data_tc,
			e_RTE_METER_RED,
			p_tc->policer[e_RTE_METER_RED]);
	}

	return 0;
}

static __rte_always_inline uint64_t
pkt_work_mtr(struct rte_mbuf *mbuf,
	struct mtr_trtcm_data *data,
	struct dscp_table_data *dscp_table,
	struct meter_profile_data *mp,
	uint64_t time,
	uint32_t dscp,
	uint16_t total_length)
{
	uint64_t drop_mask, sched;
	uint64_t *sched_ptr = (uint64_t *) &mbuf->hash.sched;
	struct dscp_table_entry_data *dscp_entry = &dscp_table->entry[dscp];
	enum rte_meter_color color_in, color_meter, color_policer;
	uint32_t tc, mp_id;

	tc = dscp_entry->tc;
	color_in = dscp_entry->color;
	data += tc;
	mp_id = MTR_TRTCM_DATA_METER_PROFILE_ID_GET(data);
	sched = *sched_ptr;

	/* Meter */
	color_meter = rte_meter_trtcm_color_aware_check(
		&data->trtcm,
		&mp[mp_id].profile,
		time,
		total_length,
		color_in);

	/* Stats */
	MTR_TRTCM_DATA_STATS_INC(data, color_meter);

	/* Police */
	drop_mask = MTR_TRTCM_DATA_POLICER_ACTION_DROP_GET(data, color_meter);
	color_policer =
		MTR_TRTCM_DATA_POLICER_ACTION_COLOR_GET(data, color_meter);
	*sched_ptr = MBUF_SCHED_COLOR(sched, color_policer);

	return drop_mask;
}

/**
 * RTE_TABLE_ACTION_TM
 */
static int
tm_cfg_check(struct rte_table_action_tm_config *tm)
{
	if ((tm->n_subports_per_port == 0) ||
		(rte_is_power_of_2(tm->n_subports_per_port) == 0) ||
		(tm->n_subports_per_port > UINT16_MAX) ||
		(tm->n_pipes_per_subport == 0) ||
		(rte_is_power_of_2(tm->n_pipes_per_subport) == 0))
		return -ENOTSUP;

	return 0;
}

struct tm_data {
	uint16_t queue_tc_color;
	uint16_t subport;
	uint32_t pipe;
} __attribute__((__packed__));

static int
tm_apply_check(struct rte_table_action_tm_params *p,
	struct rte_table_action_tm_config *cfg)
{
	if ((p->subport_id >= cfg->n_subports_per_port) ||
		(p->pipe_id >= cfg->n_pipes_per_subport))
		return -EINVAL;

	return 0;
}

static int
tm_apply(struct tm_data *data,
	struct rte_table_action_tm_params *p,
	struct rte_table_action_tm_config *cfg)
{
	int status;

	/* Check input arguments */
	status = tm_apply_check(p, cfg);
	if (status)
		return status;

	/* Apply */
	data->queue_tc_color = 0;
	data->subport = (uint16_t) p->subport_id;
	data->pipe = p->pipe_id;

	return 0;
}

static __rte_always_inline void
pkt_work_tm(struct rte_mbuf *mbuf,
	struct tm_data *data,
	struct dscp_table_data *dscp_table,
	uint32_t dscp)
{
	struct dscp_table_entry_data *dscp_entry = &dscp_table->entry[dscp];
	struct tm_data *sched_ptr = (struct tm_data *) &mbuf->hash.sched;
	struct tm_data sched;

	sched = *data;
	sched.queue_tc_color = dscp_entry->queue_tc_color;
	*sched_ptr = sched;
}

/**
 * Action profile
 */
static int
action_valid(enum rte_table_action_type action)
{
	switch (action) {
	case RTE_TABLE_ACTION_FWD:
	case RTE_TABLE_ACTION_MTR:
	case RTE_TABLE_ACTION_TM:
		return 1;
	default:
		return 0;
	}
}


#define RTE_TABLE_ACTION_MAX                      64

struct ap_config {
	uint64_t action_mask;
	struct rte_table_action_common_config common;
	struct rte_table_action_mtr_config mtr;
	struct rte_table_action_tm_config tm;
};

static size_t
action_cfg_size(enum rte_table_action_type action)
{
	switch (action) {
	case RTE_TABLE_ACTION_MTR:
		return sizeof(struct rte_table_action_mtr_config);
	case RTE_TABLE_ACTION_TM:
		return sizeof(struct rte_table_action_tm_config);
	default:
		return 0;
	}
}

static void*
action_cfg_get(struct ap_config *ap_config,
	enum rte_table_action_type type)
{
	switch (type) {
	case RTE_TABLE_ACTION_MTR:
		return &ap_config->mtr;

	case RTE_TABLE_ACTION_TM:
		return &ap_config->tm;

	default:
		return NULL;
	}
}

static void
action_cfg_set(struct ap_config *ap_config,
	enum rte_table_action_type type,
	void *action_cfg)
{
	void *dst = action_cfg_get(ap_config, type);

	if (dst)
		memcpy(dst, action_cfg, action_cfg_size(type));

	ap_config->action_mask |= 1LLU << type;
}

struct ap_data {
	size_t offset[RTE_TABLE_ACTION_MAX];
	size_t total_size;
};

static size_t
action_data_size(enum rte_table_action_type action,
	struct ap_config *ap_config)
{
	switch (action) {
	case RTE_TABLE_ACTION_FWD:
		return sizeof(struct fwd_data);

	case RTE_TABLE_ACTION_MTR:
		return mtr_data_size(&ap_config->mtr);

	case RTE_TABLE_ACTION_TM:
		return sizeof(struct tm_data);

	default:
		return 0;
	}
}


static void
action_data_offset_set(struct ap_data *ap_data,
	struct ap_config *ap_config)
{
	uint64_t action_mask = ap_config->action_mask;
	size_t offset;
	uint32_t action;

	memset(ap_data->offset, 0, sizeof(ap_data->offset));

	offset = 0;
	for (action = 0; action < RTE_TABLE_ACTION_MAX; action++)
		if (action_mask & (1LLU << action)) {
			ap_data->offset[action] = offset;
			offset += action_data_size((enum rte_table_action_type)action,
				ap_config);
		}

	ap_data->total_size = offset;
}

struct rte_table_action_profile {
	struct ap_config cfg;
	struct ap_data data;
	int frozen;
};

struct rte_table_action_profile *
rte_table_action_profile_create(struct rte_table_action_common_config *common)
{
	struct rte_table_action_profile *ap;

	/* Check input arguments */
	if (common == NULL)
		return NULL;

	/* Memory allocation */
	ap = calloc(1, sizeof(struct rte_table_action_profile));
	if (ap == NULL)
		return NULL;

	/* Initialization */
	memcpy(&ap->cfg.common, common, sizeof(*common));

	return ap;
}


int
rte_table_action_profile_action_register(struct rte_table_action_profile *profile,
	enum rte_table_action_type type,
	void *action_config)
{
	int status;

	/* Check input arguments */
	if ((profile == NULL) ||
		profile->frozen ||
		(action_valid(type) == 0) ||
		(profile->cfg.action_mask & (1LLU << type)) ||
		((action_cfg_size(type) == 0) && action_config) ||
		(action_cfg_size(type) && (action_config == NULL)))
		return -EINVAL;

	switch (type) {
	case RTE_TABLE_ACTION_MTR:
		status = mtr_cfg_check(action_config);
		break;

	case RTE_TABLE_ACTION_TM:
		status = tm_cfg_check(action_config);
		break;

	default:
		status = 0;
		break;
	}

	if (status)
		return status;

	/* Action enable */
	action_cfg_set(&profile->cfg, type, action_config);

	return 0;
}

int
rte_table_action_profile_freeze(struct rte_table_action_profile *profile)
{
	if (profile->frozen)
		return -EBUSY;

	profile->cfg.action_mask |= 1LLU << RTE_TABLE_ACTION_FWD;
	action_data_offset_set(&profile->data, &profile->cfg);
	profile->frozen = 1;

	return 0;
}

int
rte_table_action_profile_free(struct rte_table_action_profile *profile)
{
	if (profile == NULL)
		return 0;

	free(profile);
	return 0;
}

/**
 * Action
 */
#define METER_PROFILES_MAX                                 32

struct rte_table_action {
	struct ap_config cfg;
	struct ap_data data;
	struct dscp_table_data dscp_table;
	struct meter_profile_data mp[METER_PROFILES_MAX];
};

struct rte_table_action *
rte_table_action_create(struct rte_table_action_profile *profile,
	uint32_t socket_id)
{
	struct rte_table_action *action;

	/* Check input arguments */
	if ((profile == NULL) ||
		(profile->frozen == 0))
		return NULL;

	/* Memory allocation */
	action = rte_zmalloc_socket(NULL,
		sizeof(struct rte_table_action),
		RTE_CACHE_LINE_SIZE,
		socket_id);
	if (action == NULL)
		return NULL;

	/* Initialization */
	memcpy(&action->cfg, &profile->cfg, sizeof(profile->cfg));
	memcpy(&action->data, &profile->data, sizeof(profile->data));

	return action;
}

static __rte_always_inline void *
action_data_get(void *data,
	struct rte_table_action *action,
	enum rte_table_action_type type)
{
	size_t offset = action->data.offset[type];
	uint8_t *data_bytes = data;

	return &data_bytes[offset];
}

int
rte_table_action_apply(struct rte_table_action *action,
	void *data,
	enum rte_table_action_type type,
	void *action_params)
{
	void *action_data;

	/* Check input arguments */
	if ((action == NULL) ||
		(data == NULL) ||
		(action_valid(type) == 0) ||
		((action->cfg.action_mask & (1LLU << type)) == 0) ||
		(action_params == NULL))
		return -EINVAL;

	/* Data update */
	action_data = action_data_get(data, action, type);

	switch (type) {
	case RTE_TABLE_ACTION_FWD:
		return fwd_apply(action_data,
			action_params);

	case RTE_TABLE_ACTION_MTR:
		return mtr_apply(action_data,
			action_params,
			&action->cfg.mtr,
			action->mp,
			RTE_DIM(action->mp));

	case RTE_TABLE_ACTION_TM:
		return tm_apply(action_data,
			action_params,
			&action->cfg.tm);

	default:
		return -EINVAL;
	}
}

int
rte_table_action_dscp_table_update(struct rte_table_action *action,
	uint64_t dscp_mask,
	struct rte_table_action_dscp_table *table)
{
	uint32_t i;

	/* Check input arguments */
	if ((action == NULL) ||
		((action->cfg.action_mask & ((1LLU << RTE_TABLE_ACTION_MTR) |
		(1LLU << RTE_TABLE_ACTION_TM))) == 0) ||
		(dscp_mask == 0) ||
		(table == NULL))
		return -EINVAL;

	for (i = 0; i < RTE_DIM(table->entry); i++) {
		struct dscp_table_entry_data *data =
			&action->dscp_table.entry[i];
		struct rte_table_action_dscp_table_entry *entry =
			&table->entry[i];
		uint16_t queue_tc_color =
			MBUF_SCHED_QUEUE_TC_COLOR(entry->tc_queue_id,
				entry->tc_id,
				entry->color);

		if ((dscp_mask & (1LLU << i)) == 0)
			continue;

		data->color = entry->color;
		data->tc = entry->tc_id;
		data->queue_tc_color = queue_tc_color;
	}

	return 0;
}

int
rte_table_action_meter_profile_add(struct rte_table_action *action,
	uint32_t meter_profile_id,
	struct rte_table_action_meter_profile *profile)
{
	struct meter_profile_data *mp_data;
	uint32_t status;

	/* Check input arguments */
	if ((action == NULL) ||
		((action->cfg.action_mask & (1LLU << RTE_TABLE_ACTION_MTR)) == 0) ||
		(profile == NULL))
		return -EINVAL;

	if (profile->alg != RTE_TABLE_ACTION_METER_TRTCM)
		return -ENOTSUP;

	mp_data = meter_profile_data_find(action->mp,
		RTE_DIM(action->mp),
		meter_profile_id);
	if (mp_data)
		return -EEXIST;

	mp_data = meter_profile_data_find_unused(action->mp,
		RTE_DIM(action->mp));
	if (!mp_data)
		return -ENOSPC;

	/* Install new profile */
	status = rte_meter_trtcm_profile_config(&mp_data->profile,
		&profile->trtcm);
	if (status)
		return status;

	mp_data->profile_id = meter_profile_id;
	mp_data->valid = 1;

	return 0;
}

int
rte_table_action_meter_profile_delete(struct rte_table_action *action,
	uint32_t meter_profile_id)
{
	struct meter_profile_data *mp_data;

	/* Check input arguments */
	if ((action == NULL) ||
		((action->cfg.action_mask & (1LLU << RTE_TABLE_ACTION_MTR)) == 0))
		return -EINVAL;

	mp_data = meter_profile_data_find(action->mp,
		RTE_DIM(action->mp),
		meter_profile_id);
	if (!mp_data)
		return 0;

	/* Uninstall profile */
	mp_data->valid = 0;

	return 0;
}

int
rte_table_action_meter_read(struct rte_table_action *action,
	void *data,
	uint32_t tc_mask,
	struct rte_table_action_mtr_counters *stats,
	int clear)
{
	struct mtr_trtcm_data *mtr_data;
	uint32_t i;

	/* Check input arguments */
	if ((action == NULL) ||
		((action->cfg.action_mask & (1LLU << RTE_TABLE_ACTION_MTR)) == 0) ||
		(data == NULL) ||
		(tc_mask > RTE_LEN2MASK(action->cfg.mtr.n_tc, uint32_t)))
		return -EINVAL;

	mtr_data = action_data_get(data, action, RTE_TABLE_ACTION_MTR);

	/* Read */
	if (stats) {
		for (i = 0; i < RTE_TABLE_ACTION_TC_MAX; i++) {
			struct rte_table_action_mtr_counters_tc *dst =
				&stats->stats[i];
			struct mtr_trtcm_data *src = &mtr_data[i];

			if ((tc_mask & (1 << i)) == 0)
				continue;

			dst->n_packets[e_RTE_METER_GREEN] =
				mtr_trtcm_data_stats_get(src, e_RTE_METER_GREEN);

			dst->n_packets[e_RTE_METER_YELLOW] =
				mtr_trtcm_data_stats_get(src, e_RTE_METER_YELLOW);

			dst->n_packets[e_RTE_METER_RED] =
				mtr_trtcm_data_stats_get(src, e_RTE_METER_RED);

			dst->n_packets_valid = 1;
			dst->n_bytes_valid = 0;
		}

		stats->tc_mask = tc_mask;
	}

	/* Clear */
	if (clear)
		for (i = 0; i < RTE_TABLE_ACTION_TC_MAX; i++) {
			struct mtr_trtcm_data *src = &mtr_data[i];

			if ((tc_mask & (1 << i)) == 0)
				continue;

			mtr_trtcm_data_stats_reset(src, e_RTE_METER_GREEN);
			mtr_trtcm_data_stats_reset(src, e_RTE_METER_YELLOW);
			mtr_trtcm_data_stats_reset(src, e_RTE_METER_RED);
		}


	return 0;
}

static __rte_always_inline uint64_t
pkt_work(struct rte_mbuf *mbuf,
	struct rte_pipeline_table_entry *table_entry,
	uint64_t time,
	struct rte_table_action *action,
	struct ap_config *cfg)
{
	uint64_t drop_mask = 0;

	uint32_t ip_offset = action->cfg.common.ip_offset;
	void *ip = RTE_MBUF_METADATA_UINT32_PTR(mbuf, ip_offset);

	uint32_t dscp;
	uint16_t total_length;

	if (cfg->common.ip_version) {
		struct ipv4_hdr *hdr = ip;

		dscp = hdr->type_of_service >> 2;
		total_length = rte_ntohs(hdr->total_length);
	} else {
		struct ipv6_hdr *hdr = ip;

		dscp = (rte_ntohl(hdr->vtc_flow) & 0x0F600000) >> 18;
		total_length =
			rte_ntohs(hdr->payload_len) + sizeof(struct ipv6_hdr);
	}

	if (cfg->action_mask & (1LLU << RTE_TABLE_ACTION_MTR)) {
		void *data =
			action_data_get(table_entry, action, RTE_TABLE_ACTION_MTR);

		drop_mask |= pkt_work_mtr(mbuf,
			data,
			&action->dscp_table,
			action->mp,
			time,
			dscp,
			total_length);
	}

	if (cfg->action_mask & (1LLU << RTE_TABLE_ACTION_TM)) {
		void *data =
			action_data_get(table_entry, action, RTE_TABLE_ACTION_TM);

		pkt_work_tm(mbuf,
			data,
			&action->dscp_table,
			dscp);
	}

	return drop_mask;
}

static __rte_always_inline uint64_t
pkt4_work(struct rte_mbuf **mbufs,
	struct rte_pipeline_table_entry **table_entries,
	uint64_t time,
	struct rte_table_action *action,
	struct ap_config *cfg)
{
	uint64_t drop_mask0 = 0;
	uint64_t drop_mask1 = 0;
	uint64_t drop_mask2 = 0;
	uint64_t drop_mask3 = 0;

	struct rte_mbuf *mbuf0 = mbufs[0];
	struct rte_mbuf *mbuf1 = mbufs[1];
	struct rte_mbuf *mbuf2 = mbufs[2];
	struct rte_mbuf *mbuf3 = mbufs[3];

	struct rte_pipeline_table_entry *table_entry0 = table_entries[0];
	struct rte_pipeline_table_entry *table_entry1 = table_entries[1];
	struct rte_pipeline_table_entry *table_entry2 = table_entries[2];
	struct rte_pipeline_table_entry *table_entry3 = table_entries[3];

	uint32_t ip_offset = action->cfg.common.ip_offset;
	void *ip0 = RTE_MBUF_METADATA_UINT32_PTR(mbuf0, ip_offset);
	void *ip1 = RTE_MBUF_METADATA_UINT32_PTR(mbuf1, ip_offset);
	void *ip2 = RTE_MBUF_METADATA_UINT32_PTR(mbuf2, ip_offset);
	void *ip3 = RTE_MBUF_METADATA_UINT32_PTR(mbuf3, ip_offset);

	uint32_t dscp0, dscp1, dscp2, dscp3;
	uint16_t total_length0, total_length1, total_length2, total_length3;

	if (cfg->common.ip_version) {
		struct ipv4_hdr *hdr0 = ip0;
		struct ipv4_hdr *hdr1 = ip1;
		struct ipv4_hdr *hdr2 = ip2;
		struct ipv4_hdr *hdr3 = ip3;

		dscp0 = hdr0->type_of_service >> 2;
		dscp1 = hdr1->type_of_service >> 2;
		dscp2 = hdr2->type_of_service >> 2;
		dscp3 = hdr3->type_of_service >> 2;

		total_length0 = rte_ntohs(hdr0->total_length);
		total_length1 = rte_ntohs(hdr1->total_length);
		total_length2 = rte_ntohs(hdr2->total_length);
		total_length3 = rte_ntohs(hdr3->total_length);
	} else {
		struct ipv6_hdr *hdr0 = ip0;
		struct ipv6_hdr *hdr1 = ip1;
		struct ipv6_hdr *hdr2 = ip2;
		struct ipv6_hdr *hdr3 = ip3;

		dscp0 = (rte_ntohl(hdr0->vtc_flow) & 0x0F600000) >> 18;
		dscp1 = (rte_ntohl(hdr1->vtc_flow) & 0x0F600000) >> 18;
		dscp2 = (rte_ntohl(hdr2->vtc_flow) & 0x0F600000) >> 18;
		dscp3 = (rte_ntohl(hdr3->vtc_flow) & 0x0F600000) >> 18;

		total_length0 =
			rte_ntohs(hdr0->payload_len) + sizeof(struct ipv6_hdr);
		total_length1 =
			rte_ntohs(hdr1->payload_len) + sizeof(struct ipv6_hdr);
		total_length2 =
			rte_ntohs(hdr2->payload_len) + sizeof(struct ipv6_hdr);
		total_length3 =
			rte_ntohs(hdr3->payload_len) + sizeof(struct ipv6_hdr);
	}

	if (cfg->action_mask & (1LLU << RTE_TABLE_ACTION_MTR)) {
		void *data0 =
			action_data_get(table_entry0, action, RTE_TABLE_ACTION_MTR);
		void *data1 =
			action_data_get(table_entry1, action, RTE_TABLE_ACTION_MTR);
		void *data2 =
			action_data_get(table_entry2, action, RTE_TABLE_ACTION_MTR);
		void *data3 =
			action_data_get(table_entry3, action, RTE_TABLE_ACTION_MTR);

		drop_mask0 |= pkt_work_mtr(mbuf0,
			data0,
			&action->dscp_table,
			action->mp,
			time,
			dscp0,
			total_length0);

		drop_mask1 |= pkt_work_mtr(mbuf1,
			data1,
			&action->dscp_table,
			action->mp,
			time,
			dscp1,
			total_length1);

		drop_mask2 |= pkt_work_mtr(mbuf2,
			data2,
			&action->dscp_table,
			action->mp,
			time,
			dscp2,
			total_length2);

		drop_mask3 |= pkt_work_mtr(mbuf3,
			data3,
			&action->dscp_table,
			action->mp,
			time,
			dscp3,
			total_length3);
	}

	if (cfg->action_mask & (1LLU << RTE_TABLE_ACTION_TM)) {
		void *data0 =
			action_data_get(table_entry0, action, RTE_TABLE_ACTION_TM);
		void *data1 =
			action_data_get(table_entry1, action, RTE_TABLE_ACTION_TM);
		void *data2 =
			action_data_get(table_entry2, action, RTE_TABLE_ACTION_TM);
		void *data3 =
			action_data_get(table_entry3, action, RTE_TABLE_ACTION_TM);

		pkt_work_tm(mbuf0,
			data0,
			&action->dscp_table,
			dscp0);

		pkt_work_tm(mbuf1,
			data1,
			&action->dscp_table,
			dscp1);

		pkt_work_tm(mbuf2,
			data2,
			&action->dscp_table,
			dscp2);

		pkt_work_tm(mbuf3,
			data3,
			&action->dscp_table,
			dscp3);
	}

	return drop_mask0 |
		(drop_mask1 << 1) |
		(drop_mask2 << 2) |
		(drop_mask3 << 3);
}

static __rte_always_inline int
ah(struct rte_pipeline *p,
	struct rte_mbuf **pkts,
	uint64_t pkts_mask,
	struct rte_pipeline_table_entry **entries,
	struct rte_table_action *action,
	struct ap_config *cfg)
{
	uint64_t pkts_drop_mask = 0;
	uint64_t time = 0;

	if (cfg->action_mask & (1LLU << RTE_TABLE_ACTION_MTR))
		time = rte_rdtsc();

	if ((pkts_mask & (pkts_mask + 1)) == 0) {
		uint64_t n_pkts = __builtin_popcountll(pkts_mask);
		uint32_t i;

		for (i = 0; i < (n_pkts & (~0x3LLU)); i += 4) {
			uint64_t drop_mask;

			drop_mask = pkt4_work(&pkts[i],
				&entries[i],
				time,
				action,
				cfg);

			pkts_drop_mask |= drop_mask << i;
		}

		for ( ; i < n_pkts; i++) {
			uint64_t drop_mask;

			drop_mask = pkt_work(pkts[i],
				entries[i],
				time,
				action,
				cfg);

			pkts_drop_mask |= drop_mask << i;
		}
	} else
		for ( ; pkts_mask; ) {
			uint32_t pos = __builtin_ctzll(pkts_mask);
			uint64_t pkt_mask = 1LLU << pos;
			uint64_t drop_mask;

			drop_mask = pkt_work(pkts[pos],
				entries[pos],
				time,
				action,
				cfg);

			pkts_mask &= ~pkt_mask;
			pkts_drop_mask |= drop_mask << pos;
		}

	rte_pipeline_ah_packet_drop(p, pkts_drop_mask);

	return 0;
}

static int
ah_default(struct rte_pipeline *p,
	struct rte_mbuf **pkts,
	uint64_t pkts_mask,
	struct rte_pipeline_table_entry **entries,
	void *arg)
{
	struct rte_table_action *action = arg;

	return ah(p,
		pkts,
		pkts_mask,
		entries,
		action,
		&action->cfg);
}

static rte_pipeline_table_action_handler_hit
ah_selector(struct rte_table_action *action)
{
	if (action->cfg.action_mask == (1LLU << RTE_TABLE_ACTION_FWD))
		return NULL;

	return ah_default;
}

int
rte_table_action_table_params_get(struct rte_table_action *action,
	struct rte_pipeline_table_params *params)
{
	rte_pipeline_table_action_handler_hit f_action_hit;
	uint32_t total_size;

	/* Check input arguments */
	if ((action == NULL) ||
		(params == NULL))
		return -EINVAL;

	f_action_hit = ah_selector(action);
	total_size = rte_align32pow2(action->data.total_size);

	/* Fill in params */
	params->f_action_hit = f_action_hit;
	params->f_action_miss = NULL;
	params->arg_ah = (f_action_hit) ? action : NULL;
	params->action_data_size = total_size -
		sizeof(struct rte_pipeline_table_entry);

	return 0;
}

int
rte_table_action_free(struct rte_table_action *action)
{
	if (action == NULL)
		return 0;

	rte_free(action);

	return 0;
}