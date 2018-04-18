/*
 * Copyright 2017, Red Hat, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
*/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <inttypes.h>
#include <limits.h>

#include "ccan/list/list.h"

#include <scsi/scsi.h>

#include "libtcmu_log.h"
#include "libtcmu_common.h"
#include "libtcmu_priv.h"
#include "tcmur_device.h"
#include "target.h"
#include "alua.h"

static char *tcmu_get_alua_str_setting(struct alua_grp *group,
				       const char *setting)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), CFGFS_CORE"/%s/%s/alua/%s/%s",
		 group->dev->tcm_hba_name, group->dev->tcm_dev_name,
		 group->name, setting);
	return tcmu_get_cfgfs_str(path);
}

static int tcmu_get_alua_int_setting(struct alua_grp *group,
				     const char *setting)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), CFGFS_CORE"/%s/%s/alua/%s/%s",
		 group->dev->tcm_hba_name, group->dev->tcm_dev_name,
		 group->name, setting);
	return tcmu_get_cfgfs_int(path);
}

static int tcmu_set_alua_int_setting(struct alua_grp *group,
				     const char *setting, int val)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), CFGFS_CORE"/%s/%s/alua/%s/%s",
		 group->dev->tcm_hba_name, group->dev->tcm_dev_name,
		 group->name, setting);
	return tcmu_set_cfgfs_ul(path, val);
}

static void tcmu_release_tgt_ports(struct alua_grp *group)
{
	struct tgt_port *port, *port_next;

	list_for_each_safe(&group->tgt_ports, port, port_next, entry) {
		list_del(&port->entry);
		tcmu_free_tgt_port(port);
	}
}

static void tcmu_free_alua_grp(struct alua_grp *group)
{
	tcmu_release_tgt_ports(group);

	if (group->name)
		free(group->name);
	free(group);
}

static struct alua_grp *
tcmu_get_alua_grp(struct tcmu_device *dev, const char *name)
{
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);
	struct alua_grp *group;
	struct tgt_port *port;
	char *str_val, *orig_str_val, *member;
	int val;

	group = calloc(1, sizeof(*group));
	if (!group)
		return NULL;
	list_head_init(&group->tgt_ports);
	list_node_init(&group->entry);
	group->dev = dev;
	group->name = strdup(name);
	if (!group->name)
		goto free_group;

	val = tcmu_get_alua_int_setting(group, "alua_access_state");
	if (val < 0)
		goto free_group;
	group->state = val;

	val = tcmu_get_alua_int_setting(group, "alua_support_active_nonoptimized");
	if (val < 0)
		goto free_group;
	if (val)
		group->supported_states |= ALUA_SUP_NON_OPTIMIZED;

	val = tcmu_get_alua_int_setting(group, "alua_support_active_optimized");
	if (val < 0)
		goto free_group;
	if (val)
		group->supported_states |= ALUA_SUP_OPTIMIZED;

	val = tcmu_get_alua_int_setting(group, "alua_support_lba_dependent");
	if (val < 0)
		goto free_group;
	if (val)
		group->supported_states |= ALUA_SUP_LBA_DEPENDENT;

	val = tcmu_get_alua_int_setting(group, "alua_support_offline");
	if (val < 0)
		goto free_group;
	if (val)
		group->supported_states |= ALUA_SUP_OFFLINE;

	val = tcmu_get_alua_int_setting(group, "alua_support_standby");
	if (val < 0)
		goto free_group;
	if (val)
		group->supported_states |= ALUA_SUP_STANDBY;

	val = tcmu_get_alua_int_setting(group, "alua_support_transitioning");
	if (val < 0)
		goto free_group;
	if (val)
		group->supported_states |= ALUA_SUP_TRANSITIONING;

	val = tcmu_get_alua_int_setting(group, "alua_support_unavailable");
	if (val < 0)
		goto free_group;
	if (val)
		group->supported_states |= ALUA_SUP_UNAVAILABLE;

	val = tcmu_get_alua_int_setting(group, "implicit_trans_secs");
	if (val < 0)
		goto free_group;
	group->implicit_trans_secs = val;

	val = tcmu_get_alua_int_setting(group, "nonop_delay_msecs");
	if (val < 0)
		goto free_group;
	group->nonop_delay_msecs = val;

	val = tcmu_get_alua_int_setting(group, "trans_delay_msecs");
	if (val < 0)
		goto free_group;
	group->trans_delay_msecs = val;

	val = tcmu_get_alua_int_setting(group, "tg_pt_gp_id");
	if (val < 0)
		goto free_group;
	group->id = val;

	val = tcmu_get_alua_int_setting(group, "preferred");
	if (val < 0)
		goto free_group;
	group->pref = val ? true : false;

	str_val = tcmu_get_alua_str_setting(group, "alua_access_status");
	if (!str_val)
		goto free_group;

	if (!strcmp(str_val, "None"))
		group->status = ALUA_STAT_NONE;
	else if (!strcmp(str_val, "Altered by Explicit STPG"))
		group->status = ALUA_STAT_ALTERED_BY_EXPLICIT_STPG;
	else if (!strcmp(str_val, "Altered by Implicit ALUA"))
		group->status = ALUA_STAT_ALTERED_BY_IMPLICIT_ALUA;
	else {
		tcmu_dev_err(dev, "Invalid ALUA status %s", str_val);
		goto free_str_val;
	}
	free(str_val);

	str_val = tcmu_get_alua_str_setting(group, "alua_access_type");
	if (!str_val)
		goto free_group;

	if (!strcmp(str_val, "None")) {
		/*
		 * Assume user wanted to do active-active.
		 * We still want the initiator to use RTPG and we
		 * can manually change states, so report this as
		 * implicit.
		 */
		rdev->failover_type = TMCUR_DEV_FAILOVER_ALL_ACTIVE;

		group->tpgs = TPGS_ALUA_IMPLICIT;
	} else if (!strcmp(str_val, "Implicit")) {
		if (!failover_is_supported(dev)) {
			tcmu_dev_err(dev, "device failover is not supported with the alua access type: Implicit\n");
			goto free_str_val;
		}

		rdev->failover_type = TMCUR_DEV_FAILOVER_IMPLICIT;

		group->tpgs = TPGS_ALUA_IMPLICIT;
	} else if (!strcmp(str_val, "Explicit")) {
		/*
		 * kernel requires both implicit and explicit so we can
		 * update the state via configfs.
		 */
		tcmu_dev_warn(dev, "Unsupported alua_access_type: Explicit only failover not supported.\n");

		goto free_str_val;
	} else if (!strcmp(str_val, "Implicit and Explicit")) {
		if (!failover_is_supported(dev)) {
			tcmu_dev_err(dev, "device failover is not supported with the alua access type: Implicit and Explicit\n");
			goto free_str_val;
		}

		/*
		 * Only report explicit so initiator always sends STPG.
		 * We only need implicit enabled in the kernel so we can
		 * interact with the alua configfs interface.
		 */
		rdev->failover_type = TMCUR_DEV_FAILOVER_EXPLICIT;

		group->tpgs = TPGS_ALUA_EXPLICIT;
	} else {
		tcmu_dev_err(dev, "Invalid ALUA type %s", str_val);
		goto free_str_val;
	}
	free(str_val);

	str_val = orig_str_val = tcmu_get_alua_str_setting(group, "members");
	if (str_val) {
		while ((member = strsep(&str_val, "\n"))) {
			if (!strlen(member))
				continue;

			port = tcmu_get_tgt_port(member);
			if (!port) {
				free(orig_str_val);
				goto free_group;
			}
			port->grp = group;
			group->num_tgt_ports++;
			list_add_tail(&group->tgt_ports, &port->entry);
		}
	}

	free(orig_str_val);
	return group;

free_str_val:
	free(str_val);
free_group:
	tcmu_free_alua_grp(group);
	return NULL;
}

void tcmu_release_alua_grps(struct list_head *group_list)
{
	struct alua_grp *group, *group_next;

	list_for_each_safe(group_list, group, group_next, entry) {
		list_del(&group->entry);
		tcmu_free_alua_grp(group);
	}
}

static int alua_filter(const struct dirent *dir)
{
        return strcmp(dir->d_name, ".") && strcmp(dir->d_name, "..");
}

/**
 * tcmu_get_alua_grps: Fill group_list with the kernel's port groups.
 * @dev: device to get groups for.
 * @group_list: list allocated by the caller to add groups to.
 *
 * User must call tcmu_release_alua_grps when finished with the list of
 * groups.
 *
 * For now, we will only support ALUA if the user has defined groups.
 * tcmu ALUA support was added in 4.11, but not all fabric modules support
 * it. Depending on the kernel version and tools used we could have:
 *
 * 1. The default ALUA group, but empty members.
 * 2. The default ALUA group, and reading/writing to members will return
 *    a error or crash the kernel.
 * 3. The default ALUA group, and members set to it, but some fabric
 *    modules did not report the target port group/tag properly so
 *    we cannot match groups to ports.
 */
int tcmu_get_alua_grps(struct tcmu_device *dev,
			   struct list_head *group_list)
{
	struct alua_grp *group;
	struct dirent **namelist;
	char path[PATH_MAX];
	int i, n;
	int ret = 0;

	snprintf(path, sizeof(path), CFGFS_CORE"/%s/%s/alua",
		 dev->tcm_hba_name, dev->tcm_dev_name);
	n = scandir(path, &namelist, alua_filter, alphasort);
	if (n < 0) {
		tcmu_dev_err(dev, "Could not get ALUA dirs for %s:%s\n",
			     path, strerror(errno));
		return -errno;
	}

	for (i = 0; i < n; i++) {
		if (!strcmp(namelist[i]->d_name, "default_tg_pt_gp"))
			continue;

		group = tcmu_get_alua_grp(dev, namelist[i]->d_name);
		if (!group) {
			tcmu_dev_err(dev, "Could not get alua group %s.\n", namelist[i]->d_name);
			ret = -1;
			goto free_groups;
		}
		list_add_tail(group_list, &group->entry);
	}
	ret = 0;
	goto free_names;

free_groups:
	tcmu_release_alua_grps(group_list);
free_names:
	for (i = 0; i < n; i++)
		free(namelist[i]);
	free(namelist);
	return ret;
}

/*
 * tcmu does not pass up the target port that the command was
 * received on, so if a LUN is exported through multiple ports
 * in different ALUA target port group we do not know which group
 * to use.
 *
 * For now we support one target port group that contains all
 * enabled ports, or for HA configs one local target port group with
 * enabled ports and N remote port groups which are marked disabled
 * on the the local node.
 */
struct tgt_port *tcmu_get_enabled_port(struct list_head *group_list)
{
	struct alua_grp *group;
	struct tgt_port *port;

	list_for_each(group_list, group, entry) {
		list_for_each(&group->tgt_ports, port, entry) {
			if (port->enabled)
				return port;
		}
	}

	return NULL;
}

/*
 * alua_update_alua_state - update alua state based on lock state
 * @dev: tcmu_device to check
 * @group: ALUA group to check for
 * @enabled_port: port that is enabled on the local node.
 *
 * Some handlers are not able to do an async update state during STPG
 * handling so update it now. Returns -EAGAIN if the handler was not
 * able to check the state due to transport/device issue.
 */
static int alua_update_alua_state(struct tcmu_device *dev,
				  struct alua_grp *group,
				  struct tgt_port *enabled_port)
{
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);
	uint8_t alua_state;
	int retry = 0, ret, lock_state;

	if (rdev->failover_type != TMCUR_DEV_FAILOVER_EXPLICIT)
		return 0;

	lock_state = tcmu_update_dev_lock_state(dev);
	if (lock_state < 0)
		return 0;
retry:
	/* we only support standby and AO for now */
	switch (lock_state) {
	case TCMUR_DEV_LOCK_NO_HOLDERS:
		alua_state = ALUA_ACCESS_STATE_STANDBY;
		break;
	case TCMUR_DEV_LOCK_LOCKED:
		if (enabled_port->grp == group)
			alua_state =  ALUA_ACCESS_STATE_OPTIMIZED;
		else
			alua_state = ALUA_ACCESS_STATE_STANDBY;
		break;
	case TCMUR_DEV_LOCK_UNLOCKED:
		if (enabled_port->grp == group)
			alua_state = ALUA_ACCESS_STATE_STANDBY;
		else
			/*
			 * This only works for 2 nodes:
			 * Someone has the lock. It is not the local node and the group
			 * is for the remote node, so it must be AO.
			 *
			 * TODO:
			 * Next patch adds a callout so we can match remote
			 * groups to their state, support more than 2 nodes
			 * and we will not have to call this every loop.
			 */
			alua_state = ALUA_ACCESS_STATE_OPTIMIZED;
		break;
	case TCMUR_DEV_LOCK_FENCED:
		if (enabled_port->grp != group) {
			/* same as above */
			alua_state = ALUA_ACCESS_STATE_OPTIMIZED;
			break;
		}

		/*
		 * We do not need to reopen now, but will try to
		 * speed up the STPG handling later.
		 */
		alua_state = ALUA_ACCESS_STATE_STANDBY;
		/*
		 * This is safe without blocking/flushing because it
		 * is called from the main IO thread and will wait for
		 * commands started before it via the aio wait call.
		 */
		tcmu_dev_dbg(dev, "Reopen. Old ALUA state %u\n", group->state);
		ret = tcmu_reopen_dev(dev, false, 0);
		if (!ret && retry < 1) {
			retry++;
			goto retry;
		}
		/*
		 * If we cannot reopen the device to clear the fencing we
		 * will not be able to execute requests like RW and lock.
		 * Just drop the session until we can reopen.
		 */

		/* fallthrough */
	case TCMUR_DEV_LOCK_UNKNOWN:
	default:
		/*
		 * In spc4r37 and newer
		 * "5.15.2.7 Target port asymmetric access state reporting"
		 * states that the initiator should consider the info
		 * returned through our enabled port current for that
		 * enabled port. If a RTPG sent through another port
		 * returns different info, then the info for the enabled
		 * port returned through the enabled port should be
		 * considered current.
		 *
		 * ESX though assumes the all port info in a RTPG to be
		 * current so we drop the session here to prevent sending
		 * inconsistent info. We probably want to do this regardless
		 * of ESX, because that value is returned when the handler
		 * cannot connect to the cluster so all requests are
		 * going to fail.
		 */
		tcmu_notify_conn_lost(dev);
		/*
		 * To try and not return inconsistent info and not look
		 * like a hard device error, fail the command so it is
		 * retried and the retry will be handled like other commands
		 * during session level recovery.
		 */
		return -EAGAIN;
	}

	if (alua_state == group->state)
		return 0;

	group->state = alua_state;
	if (tcmu_set_alua_int_setting(group, "alua_access_state", alua_state)) {
		/*
		 * this should never happen so just log it.
		 * If it does we catch it in check state's lock state check
		 * or the blacklisting
		 */
		tcmu_dev_err(dev, "Could not change kernel state to %u\n",
			     alua_state);
	}

	return 0;
}

int tcmu_emulate_report_tgt_port_grps(struct tcmu_device *dev,
				      struct list_head *group_list,
				      struct tcmulib_cmd *cmd)
{
	struct alua_grp *group;
	struct tgt_port *port, *enabled_port;
	int ext_hdr = cmd->cdb[1] & 0x20;
	uint32_t off = 4, ret_data_len = 0, ret32;
	uint32_t alloc_len = tcmu_get_xfer_length(cmd->cdb);
	uint8_t *buf;
	int ret;

	enabled_port = tcmu_get_enabled_port(group_list);
	if (!enabled_port)
		/* unsupported config */
		return TCMU_NOT_HANDLED;

	if (alloc_len < 4)
		return tcmu_set_sense_data(cmd->sense_buf, ILLEGAL_REQUEST,
					   ASC_INVALID_FIELD_IN_CDB, NULL);

	buf = calloc(1, alloc_len);
	if (!buf)
		return tcmu_set_sense_data(cmd->sense_buf, HARDWARE_ERROR,
					   ASC_INTERNAL_TARGET_FAILURE, NULL);

	if (ext_hdr && alloc_len > 5) {
		buf[4] = 0x10;
		/*
		 * assume all groups will have the same value for now.
		 */
		group = list_first_entry(group_list, struct alua_grp,
					 entry);
		if (group)
			buf[5] = group->implicit_trans_secs;
		off = 8;
	}

	list_for_each(group_list, group, entry) {
		int next_off = off + 8 + (group->num_tgt_ports * 4);

		if (next_off > alloc_len) {
			ret_data_len += 8 + (group->num_tgt_ports * 4);
			continue;
		}

		if (group->pref)
			buf[off] = 0x80;

		if (alua_update_alua_state(dev, group, enabled_port) < 0) {
			ret = SAM_STAT_BUSY;
			goto free_buf;
		}

		buf[off++] |= group->state;
		buf[off++] |= group->supported_states;
		buf[off++] = (group->id >> 8) & 0xff;
		buf[off++] = group->id & 0xff;
		/* reserved */
		off++;
		buf[off++] = group->status;
		/* vendor specific */
		off++;
		buf[off++] = group->num_tgt_ports;

		ret_data_len += 8;

		list_for_each(&group->tgt_ports, port, entry) {
			/* reserved */
			off += 2;
			buf[off++] = (port->rel_port_id >> 8) & 0xff;
			buf[off++] = port->rel_port_id & 0xff;

			ret_data_len += 4;
		}

	}
	ret32 = htobe32(ret_data_len);
	memcpy(&buf[0], &ret32, 4);

	tcmu_memcpy_into_iovec(cmd->iovec, cmd->iov_cnt, buf, alloc_len);
	ret = SAM_STAT_GOOD;
free_buf:
	free(buf);
	return ret;
}

bool failover_is_supported(struct tcmu_device *dev)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);

	return !!rhandler->lock;
}

static void *alua_lock_thread_fn(void *arg)
{
	/* TODO: set UA based on bgly's patches */
	tcmu_acquire_dev_lock(arg, false);
	return NULL;
}

int alua_implicit_transition(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);
	pthread_attr_t attr;
	int ret = SAM_STAT_GOOD;

	pthread_mutex_lock(&rdev->state_lock);
	tcmu_dev_dbg(dev, "lock state %d\n", rdev->lock_state);
	if (rdev->lock_state == TCMUR_DEV_LOCK_LOCKED) {
		goto done;
	} else if (rdev->lock_state == TCMUR_DEV_LOCK_LOCKING) {
		ret = tcmu_set_sense_data(cmd->sense_buf, NOT_READY,
					  ASC_STATE_TRANSITION, NULL);
		goto done;
	}

	rdev->lock_state = TCMUR_DEV_LOCK_LOCKING;

	/*
	 * Make the lock_thread as detached to fix the memory leakage bug.
	 */
	pthread_attr_init (&attr);
	pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_DETACHED);

	/*
	 * The initiator is going to be queueing commands, so do this
	 * in the background to avoid command timeouts.
	 */
	if (pthread_create(&rdev->lock_thread, &attr, alua_lock_thread_fn,
			   dev)) {
		tcmu_dev_err(dev, "Could not start implicit transition thread:%s\n",
			     strerror(errno));
		rdev->lock_state = TCMUR_DEV_LOCK_UNLOCKED;
		ret = tcmu_set_sense_data(cmd->sense_buf, UNIT_ATTENTION,
					  ASC_STATE_TRANSITION_FAILED, NULL);
	} else {
		ret = tcmu_set_sense_data(cmd->sense_buf, NOT_READY,
					  ASC_STATE_TRANSITION, NULL);
	}

done:
	pthread_mutex_unlock(&rdev->state_lock);
	return ret;
}

static bool alua_check_sup_state(uint8_t state, uint8_t sup)
{
	switch (state) {
	case ALUA_ACCESS_STATE_OPTIMIZED:
		if (sup & ALUA_SUP_OPTIMIZED)
			return true;
		return false;
	case ALUA_ACCESS_STATE_NON_OPTIMIZED:
		if (sup & ALUA_SUP_NON_OPTIMIZED)
			return true;
		return false;
	case ALUA_ACCESS_STATE_STANDBY:
		if (sup & ALUA_SUP_STANDBY)
			return true;
		return false;
	case ALUA_ACCESS_STATE_UNAVAILABLE:
		if (sup & ALUA_SUP_UNAVAILABLE)
			return true;
		return false;
	case ALUA_ACCESS_STATE_OFFLINE:
		/*
		 * TODO: support secondary states
		 */
		return false;
	}

	return false;
}

static int tcmu_explicit_transition(struct alua_grp *group,
				    uint8_t new_state, uint8_t alua_status,
				    uint8_t *sense)
{
	struct tcmu_device *dev = group->dev;
	int ret;

	tcmu_dev_dbg(dev, "transition group %u new state %u old state %u sup 0x%x\n",
		     group->id, new_state, group->state, group->supported_states);

	if (!alua_check_sup_state(new_state, group->supported_states))
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_INVALID_FIELD_IN_PARAMETER_LIST,
					   NULL);

	switch (new_state) {
	case ALUA_ACCESS_STATE_OPTIMIZED:
		if (failover_is_supported(dev) &&
		    tcmu_acquire_dev_lock(dev, true)) {
			return tcmu_set_sense_data(sense, HARDWARE_ERROR,
						   ASC_STPG_CMD_FAILED, NULL);
		}
		break;
	case ALUA_ACCESS_STATE_NON_OPTIMIZED:
	case ALUA_ACCESS_STATE_UNAVAILABLE:
	case ALUA_ACCESS_STATE_OFFLINE:
		/* TODO we only support standby and AO */
		tcmu_dev_err(dev, "Igoring AO/unavail/offline\n");
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_INVALID_FIELD_IN_PARAMETER_LIST,
					   NULL);
	case ALUA_ACCESS_STATE_STANDBY:
		/*
		 * TODO: we only see this in verification tests.
		 * Add back unlock in final commit.
		 */
		tcmu_dev_err(dev, "Igoring standby\n");
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_INVALID_FIELD_IN_PARAMETER_LIST,
					   NULL);
	default:
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_INVALID_FIELD_IN_PARAMETER_LIST,
					   NULL);
	}

	ret = tcmu_set_alua_int_setting(group, "alua_access_state", new_state);
	if (ret) {
		tcmu_dev_err(dev, "Could not change kernel state to %u\n",
			     new_state);
		/*
		 * TODO drop the lock
		 */
		return tcmu_set_sense_data(sense, HARDWARE_ERROR,
					   ASC_STPG_CMD_FAILED, NULL);
	}

	ret = tcmu_set_alua_int_setting(group, "alua_access_status", alua_status);
	if (ret)
		/* Ignore. The RTPG status info will be off, but its not used */
		tcmu_dev_err(dev, "Could not set alua_access_status for group %s:%d\n",
			     group->name, group->id);

	group->state = new_state;
	group->status = alua_status;
	return SAM_STAT_GOOD;
}

int tcmu_emulate_set_tgt_port_grps(struct tcmu_device *dev,
				   struct list_head *group_list,
				   struct tcmulib_cmd *cmd)
{
	struct alua_grp *group;
	uint32_t off = 4, param_list_len = tcmu_get_xfer_length(cmd->cdb);
	uint16_t id, tmp_id;
	char *buf, new_state;
	int found, ret = SAM_STAT_GOOD;

	if (!tcmu_get_enabled_port(group_list))
		return TCMU_NOT_HANDLED;

	if (!param_list_len)
		return SAM_STAT_GOOD;

	buf = calloc(1, param_list_len);
	if (!buf)
		return tcmu_set_sense_data(cmd->sense_buf, HARDWARE_ERROR,
					   ASC_INTERNAL_TARGET_FAILURE, NULL);

	if (tcmu_memcpy_from_iovec(buf, param_list_len, cmd->iovec,
				   cmd->iov_cnt) != param_list_len) {
		ret = tcmu_set_sense_data(cmd->sense_buf, ILLEGAL_REQUEST,
					  ASC_PARAMETER_LIST_LENGTH_ERROR,
					  NULL);
		goto free_buf;
	}

	while (off < param_list_len) {
		new_state = buf[off++] & 0x0f;
		/* reserved */
		off++;
		memcpy(&tmp_id, &buf[off], sizeof(tmp_id));
		id = be16toh(tmp_id);
		off += 2;

		found = 0;
		list_for_each(group_list, group, entry) {
			if (group->id != id)
				continue;

			tcmu_dev_dbg(dev, "Got STPG for group %u\n", id);
			ret = tcmu_explicit_transition(group, new_state,
					ALUA_STAT_ALTERED_BY_EXPLICIT_STPG,
					cmd->sense_buf);
			if (ret != SAM_STAT_GOOD) {
				tcmu_dev_err(dev, "Failing STPG for group %d\n",
					      id);
				goto free_buf;
			}
			found = 1;
			break;
		}

		if (!found) {
			/*
			 * Could not find what error code to return in SCSI
			 * spec.
			 */
			tcmu_dev_err(dev, "Could not find group for %u for STPG\n",
				      id);
			ret = tcmu_set_sense_data(cmd->sense_buf,
						  HARDWARE_ERROR,
						  ASC_STPG_CMD_FAILED, NULL);
			break;
		}
	}

free_buf:
	free(buf);
	return ret;
}

int alua_check_state(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);

	if (!failover_is_supported(dev))
		return 0;

        if (rdev->failover_type == TMCUR_DEV_FAILOVER_EXPLICIT) {
		if (rdev->lock_state != TCMUR_DEV_LOCK_LOCKED) {
			tcmu_dev_dbg(dev, "device lock not held.\n");
			return tcmu_set_sense_data(cmd->sense_buf, NOT_READY,
						   ASC_PORT_IN_STANDBY,
						   NULL);
		}
	} else if (rdev->failover_type == TMCUR_DEV_FAILOVER_IMPLICIT) {
		return alua_implicit_transition(dev, cmd);
	}

	return 0;
}
