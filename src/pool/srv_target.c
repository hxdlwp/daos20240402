/**
 * (C) Copyright 2016 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */
/**
 * ds_pool: Target Operations
 *
 * This file contains the server API methods and the RPC handlers that are both
 * related target states.
 *
 * Data structures used here:
 *
 *                 Pool           Container
 *
 *         Global  ds_pool
 *                 ds_pool_hdl
 *
 *   Thread-local  ds_pool_child  ds_cont
 *                                ds_cont_hdl
 */

#include <daos_srv/pool.h>

#include <daos/pool_map.h>
#include <daos/rpc.h>
#include <daos_srv/daos_mgmt_srv.h>
#include <daos_srv/vos.h>
#include "rpc.h"
#include "srv_internal.h"

/* ds_pool_child **************************************************************/

struct ds_pool_child *
ds_pool_child_lookup(const uuid_t uuid)
{
	struct ds_pool_child   *child;
	struct dsm_tls	       *tls = dsm_tls_get();

	daos_list_for_each_entry(child, &tls->dt_pool_list, spc_list) {
		if (uuid_compare(uuid, child->spc_uuid) == 0) {
			child->spc_ref++;
			return child;
		}
	}
	return NULL;
}

void
ds_pool_child_put(struct ds_pool_child *child)
{
	D_ASSERTF(child->spc_ref > 0, "%d\n", child->spc_ref);
	child->spc_ref--;
	if (child->spc_ref == 0) {
		D_DEBUG(DF_DSMS, DF_UUID": destroying\n",
			DP_UUID(child->spc_uuid));
		D_ASSERT(daos_list_empty(&child->spc_list));
		vos_pool_close(child->spc_hdl);
		D_FREE_PTR(child);
	}
}

void
ds_pool_child_purge(struct dsm_tls *tls)
{
	struct ds_pool_child   *child;
	struct ds_pool_child   *n;

	daos_list_for_each_entry_safe(child, n, &tls->dt_pool_list, spc_list) {
		D_ASSERTF(child->spc_ref == 1, DF_UUID": %d\n",
			  DP_UUID(child->spc_uuid), child->spc_ref);
		daos_list_del_init(&child->spc_list);
		ds_pool_child_put(child);
	}
}

struct pool_child_lookup_arg {
	void	       *pla_uuid;
	uint32_t	pla_map_version;
};

/*
 * Called via dss_collective() to create and add the ds_pool_child object for
 * one thread. This opens the matching VOS pool.
 */
static int
pool_child_add_one(void *varg)
{
	struct pool_child_lookup_arg   *arg = varg;
	struct dsm_tls		       *tls = dsm_tls_get();
	struct ds_pool_child	       *child;
	struct dss_module_info	       *info = dss_get_module_info();
	char			       *path;
	int				rc;

	child = ds_pool_child_lookup(arg->pla_uuid);
	if (child != NULL) {
		ds_pool_child_put(child);
		return 0;
	}

	D_DEBUG(DF_DSMS, DF_UUID": creating\n", DP_UUID(arg->pla_uuid));

	D_ALLOC_PTR(child);
	if (child == NULL)
		return -DER_NOMEM;

	rc = ds_mgmt_tgt_file(arg->pla_uuid, VOS_FILE, &info->dmi_tid, &path);
	if (rc != 0) {
		D_FREE_PTR(child);
		return rc;
	}

	rc = vos_pool_open(path, arg->pla_uuid, &child->spc_hdl);

	free(path);

	if (rc != 0) {
		D_FREE_PTR(child);
		return rc;
	}

	uuid_copy(child->spc_uuid, arg->pla_uuid);
	child->spc_map_version = arg->pla_map_version;
	child->spc_ref = 1;
	daos_list_add(&child->spc_list, &tls->dt_pool_list);
	return 0;
}

/*
 * Called via dss_collective() to delete the ds_pool_child object for one
 * thread. If nobody else is referencing this object, then its VOS pool handle
 * is closed and the object itself is freed.
 */
static int
pool_child_delete_one(void *uuid)
{
	struct ds_pool_child *child;

	child = ds_pool_child_lookup(uuid);
	if (child == NULL)
		return 0;

	daos_list_del_init(&child->spc_list);
	ds_pool_child_put(child);

	ds_pool_child_put(child);
	return 0;
}

/* ds_pool ********************************************************************/

static struct daos_lru_cache *pool_cache;

static inline struct ds_pool *
pool_obj(struct daos_llink *llink)
{
	return container_of(llink, struct ds_pool, sp_entry);
}

static int
pool_alloc_ref(void *key, unsigned int ksize, void *varg,
	       struct daos_llink **link)
{
	struct ds_pool_create_arg      *arg = varg;
	struct ds_pool		       *pool;
	struct pool_child_lookup_arg	collective_arg;
	int				rc;
	int				rc_tmp;

	if (arg == NULL)
		D_GOTO(err, rc = -DER_NONEXIST);

	D_DEBUG(DF_DSMS, DF_UUID": creating\n", DP_UUID(key));

	D_ALLOC_PTR(pool);
	if (pool == NULL)
		D_GOTO(err, rc = -DER_NOMEM);

	rc = ABT_rwlock_create(&pool->sp_lock);
	if (rc != ABT_SUCCESS)
		D_GOTO(err_pool, rc = dss_abterr2der(rc));

	uuid_copy(pool->sp_uuid, key);
	pool->sp_map_version = arg->pca_map_version;

	if (arg->pca_map_buf != NULL) {
		rc = pool_map_create(arg->pca_map_buf, arg->pca_map_version,
				     &pool->sp_map);
		if (rc != 0)
			D_GOTO(err_lock, rc);
	}

	collective_arg.pla_uuid = key;
	collective_arg.pla_map_version = arg->pca_map_version;

	rc = dss_collective(pool_child_add_one, &collective_arg);
	D_ASSERTF(rc == 0, "%d\n", rc);

	if (arg->pca_create_group) {
		D_ASSERT(pool->sp_map != NULL);
		rc = ds_pool_group_create(key, pool->sp_map, &pool->sp_group);
		if (rc != 0)
			D_GOTO(err_collective, rc);
	}

	*link = &pool->sp_entry;
	return 0;

err_collective:
	rc_tmp = dss_collective(pool_child_delete_one, key);
	D_ASSERTF(rc_tmp == 0, "%d\n", rc_tmp);
	if (arg->pca_map_buf != NULL)
		pool_map_destroy(pool->sp_map);
err_lock:
	ABT_rwlock_free(&pool->sp_lock);
err_pool:
	D_FREE_PTR(pool);
err:
	return rc;
}

static void
pool_free_ref(struct daos_llink *llink)
{
	struct ds_pool *pool = pool_obj(llink);
	int		rc;

	D_DEBUG(DF_DSMS, DF_UUID": freeing\n", DP_UUID(pool->sp_uuid));

	if (pool->sp_group != NULL) {
		rc = ds_pool_group_destroy(pool->sp_uuid, pool->sp_group);
		if (rc != 0)
			D_ERROR(DF_UUID": failed to destroy pool group %s: "
				"%d\n", DP_UUID(pool->sp_uuid),
				pool->sp_group->cg_grpid, rc);
	}

	rc = dss_collective(pool_child_delete_one, pool->sp_uuid);
	D_ASSERTF(rc == 0, "%d\n", rc);

	if (pool->sp_map != NULL)
		pool_map_destroy(pool->sp_map);

	ABT_rwlock_free(&pool->sp_lock);
	D_FREE_PTR(pool);
}

static bool
pool_cmp_keys(const void *key, unsigned int ksize, struct daos_llink *llink)
{
	struct ds_pool *pool = pool_obj(llink);

	return uuid_compare(key, pool->sp_uuid) == 0;
}

static struct daos_llink_ops pool_cache_ops = {
	.lop_alloc_ref	= pool_alloc_ref,
	.lop_free_ref	= pool_free_ref,
	.lop_cmp_keys	= pool_cmp_keys
};

int
ds_pool_cache_init(void)
{
	return daos_lru_cache_create(-1 /* bits */, DHASH_FT_NOLOCK /* feats */,
				     &pool_cache_ops, &pool_cache);
}

void
ds_pool_cache_fini(void)
{
	daos_lru_cache_destroy(pool_cache);
}

/*
 * If "arg == NULL", then this is assumed to be a pure lookup. In this case,
 * -DER_NONEXIST is returned if the ds_pool object does not exist in the cache.
 * A group is only created if "arg->pca_create_group != 0".
 */
int
ds_pool_lookup_create(const uuid_t uuid, struct ds_pool_create_arg *arg,
		      struct ds_pool **pool)
{
	struct daos_llink      *llink;
	int			rc;

	D_ASSERT(arg == NULL || !arg->pca_create_group ||
		 arg->pca_map_buf != NULL);

	rc = daos_lru_ref_hold(pool_cache, (void *)uuid, sizeof(uuid_t),
			       arg, &llink);
	if (rc != 0) {
		if (arg == NULL && rc == -DER_NONEXIST)
			D_DEBUG(DF_DSMS, DF_UUID": pure lookup failed: %d\n",
				DP_UUID(uuid), rc);
		else
			D_ERROR(DF_UUID": failed to lookup%s pool: %d\n",
				DP_UUID(uuid), arg == NULL ? "" : "/create",
				rc);
		return rc;
	}

	*pool = pool_obj(llink);
	return 0;
}

struct ds_pool *
ds_pool_lookup(const uuid_t uuid)
{
	struct ds_pool *pool;
	int		rc;

	rc = ds_pool_lookup_create(uuid, NULL /* arg */, &pool);
	if (rc != 0)
		pool = NULL;
	return pool;
}

void
ds_pool_put(struct ds_pool *pool)
{
	daos_lru_ref_release(pool_cache, &pool->sp_entry);
}

/* ds_pool_hdl ****************************************************************/

static struct dhash_table *pool_hdl_hash;

static inline struct ds_pool_hdl *
pool_hdl_obj(daos_list_t *rlink)
{
	return container_of(rlink, struct ds_pool_hdl, sph_entry);
}

static bool
pool_hdl_key_cmp(struct dhash_table *htable, daos_list_t *rlink,
		 const void *key, unsigned int ksize)
{
	struct ds_pool_hdl *hdl = pool_hdl_obj(rlink);

	D_ASSERTF(ksize == sizeof(uuid_t), "%u\n", ksize);
	return uuid_compare(hdl->sph_uuid, key) == 0;
}

static void
pool_hdl_rec_addref(struct dhash_table *htable, daos_list_t *rlink)
{
	pool_hdl_obj(rlink)->sph_ref++;
}

static bool
pool_hdl_rec_decref(struct dhash_table *htable, daos_list_t *rlink)
{
	struct ds_pool_hdl *hdl = pool_hdl_obj(rlink);

	D_ASSERTF(hdl->sph_ref > 0, "%d\n", hdl->sph_ref);
	hdl->sph_ref--;
	return hdl->sph_ref == 0;
}

static void
pool_hdl_rec_free(struct dhash_table *htable, daos_list_t *rlink)
{
	struct ds_pool_hdl *hdl = pool_hdl_obj(rlink);

	D_DEBUG(DF_DSMS, DF_UUID": freeing "DF_UUID"\n",
		DP_UUID(hdl->sph_pool->sp_uuid), DP_UUID(hdl->sph_uuid));
	D_ASSERT(dhash_rec_unlinked(&hdl->sph_entry));
	D_ASSERTF(hdl->sph_ref == 0, "%d\n", hdl->sph_ref);
	ds_pool_put(hdl->sph_pool);
	D_FREE_PTR(hdl);
}

static dhash_table_ops_t pool_hdl_hash_ops = {
	.hop_key_cmp	= pool_hdl_key_cmp,
	.hop_rec_addref	= pool_hdl_rec_addref,
	.hop_rec_decref	= pool_hdl_rec_decref,
	.hop_rec_free	= pool_hdl_rec_free
};

int
ds_pool_hdl_hash_init(void)
{
	return dhash_table_create(0 /* feats */, 4 /* bits */, NULL /* priv */,
				  &pool_hdl_hash_ops, &pool_hdl_hash);
}

void
ds_pool_hdl_hash_fini(void)
{
	/* Currently, we use "force" to purge all ds_pool_hdl objects. */
	dhash_table_destroy(pool_hdl_hash, true /* force */);
}

static int
pool_hdl_add(struct ds_pool_hdl *hdl)
{
	return dhash_rec_insert(pool_hdl_hash, hdl->sph_uuid,
				sizeof(uuid_t), &hdl->sph_entry,
				true /* exclusive */);
}

static void
pool_hdl_delete(struct ds_pool_hdl *hdl)
{
	bool deleted;

	deleted = dhash_rec_delete(pool_hdl_hash, hdl->sph_uuid,
				   sizeof(uuid_t));
	D_ASSERT(deleted == true);
}

struct ds_pool_hdl *
ds_pool_hdl_lookup(const uuid_t uuid)
{
	daos_list_t *rlink;

	rlink = dhash_rec_find(pool_hdl_hash, uuid, sizeof(uuid_t));
	if (rlink == NULL)
		return NULL;

	return pool_hdl_obj(rlink);
}

void
ds_pool_hdl_put(struct ds_pool_hdl *hdl)
{
	dhash_rec_decref(pool_hdl_hash, &hdl->sph_entry);
}

int
ds_pool_tgt_connect_handler(crt_rpc_t *rpc)
{
	struct pool_tgt_connect_in     *in = crt_req_get(rpc);
	struct pool_tgt_connect_out    *out = crt_reply_get(rpc);
	struct ds_pool		       *pool;
	struct ds_pool_hdl	       *hdl;
	struct ds_pool_create_arg	arg;
	int				rc;

	D_DEBUG(DF_DSMS, DF_UUID": handling rpc %p: hdl="DF_UUID"\n",
		DP_UUID(in->tci_uuid), rpc, DP_UUID(in->tci_hdl));

	hdl = ds_pool_hdl_lookup(in->tci_hdl);
	if (hdl != NULL) {
		if (hdl->sph_capas == in->tci_capas) {
			D_DEBUG(DF_DSMS, DF_UUID": found compatible pool "
				"handle: hdl="DF_UUID" capas="DF_U64"\n",
				DP_UUID(in->tci_uuid), DP_UUID(in->tci_hdl),
				hdl->sph_capas);
			rc = 0;
		} else {
			D_ERROR(DF_UUID": found conflicting pool handle: hdl="
				DF_UUID" capas="DF_U64"\n",
				DP_UUID(in->tci_uuid), DP_UUID(in->tci_hdl),
				hdl->sph_capas);
			rc = -DER_EXIST;
		}
		ds_pool_hdl_put(hdl);
		D_GOTO(out, rc);
	}

	D_ALLOC_PTR(hdl);
	if (hdl == NULL)
		D_GOTO(out, rc = -DER_NOMEM);

	arg.pca_map_buf = NULL;
	arg.pca_map_version = in->tci_map_version;
	arg.pca_create_group = 0;

	rc = ds_pool_lookup_create(in->tci_uuid, &arg, &pool);
	if (rc != 0) {
		D_FREE_PTR(hdl);
		D_GOTO(out, rc);
	}

	uuid_copy(hdl->sph_uuid, in->tci_hdl);
	hdl->sph_capas = in->tci_capas;
	hdl->sph_pool = pool;

	rc = pool_hdl_add(hdl);
	if (rc != 0) {
		ds_pool_put(pool);
		D_GOTO(out, rc);
	}

out:
	out->tco_rc = (rc == 0 ? 0 : 1);
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d (%d)\n",
		DP_UUID(in->tci_uuid), rpc, out->tco_rc, rc);
	return crt_reply_send(rpc);
}

int
ds_pool_tgt_connect_aggregator(crt_rpc_t *source, crt_rpc_t *result, void *priv)
{
	struct pool_tgt_connect_out    *out_source = crt_reply_get(source);
	struct pool_tgt_connect_out    *out_result = crt_reply_get(result);

	out_result->tco_rc += out_source->tco_rc;
	return 0;
}

int
ds_pool_tgt_disconnect_handler(crt_rpc_t *rpc)
{
	struct pool_tgt_disconnect_in  *in = crt_req_get(rpc);
	struct pool_tgt_disconnect_out *out = crt_reply_get(rpc);
	uuid_t			       *hdl_uuids = in->tdi_hdls.da_arrays;
	int				i;
	int				rc;

	if (in->tdi_hdls.da_count == 0)
		D_GOTO(out, rc = 0);

	if (in->tdi_hdls.da_arrays == NULL)
		D_GOTO(out, rc = -DER_INVAL);

	D_DEBUG(DF_DSMS, DF_UUID": handling rpc %p: hdls[0]="DF_UUID" nhdls="
		DF_U64"\n", DP_UUID(in->tdi_uuid), rpc, DP_UUID(hdl_uuids),
		in->tdi_hdls.da_count);

	for (i = 0; i < in->tdi_hdls.da_count; i++) {
		struct ds_pool_hdl *hdl;

		hdl = ds_pool_hdl_lookup(hdl_uuids[i]);
		if (hdl == NULL) {
			D_DEBUG(DF_DSMS, DF_UUID": handle "DF_UUID
				" does not exist\n", DP_UUID(in->tdi_uuid),
				DP_UUID(hdl_uuids[i]));
			continue;
		}
		pool_hdl_delete(hdl);
		ds_pool_hdl_put(hdl);
	}

	rc = 0;
out:
	out->tdo_rc = (rc == 0 ? 0 : 1);
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d (%d)\n",
		DP_UUID(in->tdi_uuid), rpc, out->tdo_rc, rc);
	return crt_reply_send(rpc);
}

int
ds_pool_tgt_disconnect_aggregator(crt_rpc_t *source, crt_rpc_t *result,
					void *priv)
{
	struct pool_tgt_disconnect_out *out_source = crt_reply_get(source);
	struct pool_tgt_disconnect_out *out_result = crt_reply_get(result);

	out_result->tdo_rc += out_source->tdo_rc;
	return 0;
}

/*
 * Called via dss_collective() to update the pool map version in the
 * ds_pool_child object.
 */
static int
update_child_map_version(void *vin)
{
	struct pool_tgt_update_map_in  *in = vin;
	struct ds_pool_child	       *child;

	child = ds_pool_child_lookup(in->tui_uuid);
	if (child == NULL)
		return -DER_NONEXIST;

	D_DEBUG(DF_DSMS, DF_UUID": changing cached map version: %u -> %u\n",
		DP_UUID(child->spc_uuid), child->spc_map_version,
		in->tui_map_version);

	child->spc_map_version = in->tui_map_version;

	ds_pool_child_put(child);
	return 0;
}

int
ds_pool_tgt_update_map_handler(crt_rpc_t *rpc)
{
	struct pool_tgt_update_map_in  *in = crt_req_get(rpc);
	struct pool_tgt_update_map_out *out = crt_reply_get(rpc);
	struct ds_pool		       *pool;
	uint32_t			map_version_old;
	int				rc = 0;

	D_DEBUG(DF_DSMS, DF_UUID": handling rpc %p: version=%u\n",
		DP_UUID(in->tui_uuid), rpc, in->tui_map_version);

	pool = ds_pool_lookup(in->tui_uuid);
	if (pool == NULL)
		D_GOTO(out, rc = -DER_NONEXIST);

	rc = dss_collective(update_child_map_version, in);
	D_ASSERTF(rc == 0, "%d\n", rc);

	ABT_rwlock_wrlock(pool->sp_lock);
	map_version_old = pool->sp_map_version;
	pool->sp_map_version = in->tui_map_version;
	ABT_rwlock_unlock(pool->sp_lock);

	D_DEBUG(DF_DSMS, DF_UUID": changed cached map version: %u -> %u\n",
		DP_UUID(in->tui_uuid), map_version_old, in->tui_map_version);

	ds_pool_put(pool);
out:
	out->tuo_rc = (rc == 0 ? 0 : 1);
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d (%d)\n",
		DP_UUID(in->tui_uuid), rpc, out->tuo_rc, rc);
	return crt_reply_send(rpc);
}

int
ds_pool_tgt_update_map_aggregator(crt_rpc_t *source, crt_rpc_t *result,
				  void *priv)
{
	struct pool_tgt_update_map_out *out_source = crt_reply_get(source);
	struct pool_tgt_update_map_out *out_result = crt_reply_get(result);

	out_result->tuo_rc += out_source->tuo_rc;
	return 0;
}
