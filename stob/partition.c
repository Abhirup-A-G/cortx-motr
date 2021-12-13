/*
 * Copyright (c) 2013-2020 Seagate Technology LLC and/or its Affiliates
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * For any questions about this software or licensing,
 * please email opensource@seagate.com or cortx-questions@seagate.com.
 *
 */


#include "balloc/balloc.h"

#include "be/extmap.h"
#include "be/seg.h"
#include "be/seg0.h"		/* m0_be_0type */

#include "dtm/dtm.h"		/* m0_dtx */

#include "fid/fid.h"		/* m0_fid */

#include "lib/finject.h"
#include "lib/errno.h"
#include "lib/locality.h"	/* m0_locality0_get */
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/cksum_utils.h"
#define M0_TRACE_SUBSYSTEM M0_TRACE_SUBSYS_STOB
#include "lib/trace.h"		/* M0_LOG */

#include "addb2/addb2.h"
#include "module/instance.h"	/* m0_get */

#include "stob/partition.h"
#include "stob/addb2.h"
#include "stob/domain.h"
#include "stob/io.h"
#include "stob/module.h"	/* m0_stob_ad_module */
#include "stob/stob.h"
#include "stob/stob_internal.h"	/* m0_stob__fid_set */
#include "stob/type.h"		/* m0_stob_type */
#include "be/domain.h"
#include "be/partition_table.h"
#include <math.h>

/**
 * @addtogroup stobpart
 *
 * @{
 */

enum {
	PART_STOB_MAX_CHUNK_SIZE_IN_BITS = 64,
	PART_STOB_REF_CHUNK_COUNT        = 1024,
};
struct part_domain_cfg {
	struct m0_be_ptable_part_config part_config;
	struct m0_be_domain *be_domain;
};
static struct m0_stob_domain_ops stob_part_domain_ops;
static struct m0_stob_type_ops   stob_part_type_ops;
static struct m0_stob_ops        stob_part_ops;
struct part_stob_cfg {
	m0_bcount_t  psg_id;
	m0_bcount_t  psg_size_in_chunks;
};

static int stob_part_io_init(struct m0_stob *stob, struct m0_stob_io *io);
static int stob_part_punch(struct m0_stob *stob,
			   struct m0_indexvec *range,
			   struct m0_dtx *tx);

static struct m0_stob* stob_part_get_bstore(struct m0_stob_domain *dom);
static void stob_part_write_credit(const struct m0_stob_domain *dom,
				   const struct m0_stob_io     *iv,
				   struct m0_be_tx_credit      *accum)
{

}
static void stob_part_type_register(struct m0_stob_type *type)
{

}

static void stob_part_type_deregister(struct m0_stob_type *type)
{

}

/*
 * This function takes propose_chunk_size as input
 * and finds out the nearest power of 2 to this number
 * and returns that.
 */
static int align_chunk_size(m0_bcount_t proposed_chunk_size)
{
	int chunksize_in_bits;
	int i;

	for (i = 0; i < PART_STOB_MAX_CHUNK_SIZE_IN_BITS; ) {
		if (proposed_chunk_size > 0) {
			proposed_chunk_size >>= 1;
			i++;
		} else
			break;
	}
	M0_ASSERT(i < PART_STOB_MAX_CHUNK_SIZE_IN_BITS);
	chunksize_in_bits = i;
	return(chunksize_in_bits);
}

static int stob_part_domain_cfg_init_parse(const char *str_cfg_init,
					   void **cfg_init)
{
	int                              rc;
	char                            *devname;
	struct part_domain_cfg          *part_cfg;
	struct m0_be_ptable_part_config *cfg;
	m0_bcount_t                      size;
	m0_bcount_t                      proposed_chunk_size;  
	size = strlen(str_cfg_init) + 1;
	devname = (char *) m0_alloc(size);

	/* if we have reached here then devname and size
	 * are valid */

	part_cfg = (struct part_domain_cfg*)
		m0_alloc(sizeof( struct part_domain_cfg));
	if (part_cfg == NULL) {
		M0_LOG(M0_ERROR, "Failed allocate memory for part cfg");
		return -ENOMEM;
	}
	
	/* format = "be_domain_ptr /dev/sdc 20GB" */
	rc = sscanf(str_cfg_init, "%p %s %"SCNu64, (void **)&part_cfg->be_domain, devname, &size);
	if ( rc != 3 )
		return M0_RC(-EINVAL);

	cfg = &part_cfg->part_config;
	cfg->pc_dev_path_name = devname;
	cfg->pc_dev_size_in_bytes = size; 
	proposed_chunk_size = size / PART_STOB_REF_CHUNK_COUNT;
	cfg->pc_chunk_size_in_bits = align_chunk_size(proposed_chunk_size);
	cfg->pc_total_chunk_count = size >> cfg->pc_chunk_size_in_bits;
	*cfg_init = part_cfg;
	return 0;
}

static void stob_part_domain_cfg_init_free(void *cfg_init)
{

	struct part_domain_cfg *cfg = cfg_init;
	if( cfg != NULL ) {
                // fixme: commented the below line
		m0_free(cfg->part_config.pc_dev_path_name);
		m0_free(cfg);
	}
}

static int stob_part_domain_destroy(struct m0_stob_type *type,
				    const char *location_data)
{
	return 0;
}


static int stob_part_domain_cfg_create_parse(const char *str_cfg_create,
					     void **cfg_create)
{
	int                              rc;
	char                            *devname;
	struct part_domain_cfg          *part_cfg;
	struct m0_be_ptable_part_config *cfg;
	m0_bcount_t                      proposed_chunk_size;
	m0_bcount_t                      size;

	size = strlen(str_cfg_create) + 1;
	devname = (char *) m0_alloc(size);

	/* if we have reached here then devname and size
	 * are valid */

	part_cfg = (struct part_domain_cfg*)
		m0_alloc(sizeof( struct part_domain_cfg));
	if (part_cfg == NULL) {
		M0_LOG(M0_ERROR, "Failed allocate memory for part cfg");
		return -ENOMEM;
	}
	
	/* format = "be_domain_ptr /dev/sdc 20GB" */
	rc = sscanf(str_cfg_create, "%p %s %"SCNu64,(void **) &part_cfg->be_domain, devname, &size);
	if ( rc != 3 )
		return M0_RC(-EINVAL);

	cfg = &part_cfg->part_config;
	/* Seg0, seg1, log, data */
	cfg->pc_num_of_alloc_entries = 4;
	proposed_chunk_size = size / PART_STOB_REF_CHUNK_COUNT;
	cfg->pc_chunk_size_in_bits = align_chunk_size(proposed_chunk_size);
	cfg->pc_total_chunk_count = size >> cfg->pc_chunk_size_in_bits;

	M0_ALLOC_ARR(cfg->pc_part_alloc_info,
		     cfg->pc_num_of_alloc_entries);
	if (cfg->pc_part_alloc_info == NULL)
		return -ENOMEM;

	cfg->pc_part_alloc_info[0].ai_part_id = M0_BE_PTABLE_ENTRY_SEG0;
	cfg->pc_part_alloc_info[0].ai_def_size_in_chunks = 1;
	cfg->pc_part_alloc_info[1].ai_part_id = M0_BE_PTABLE_ENTRY_LOG;
	cfg->pc_part_alloc_info[1].ai_def_size_in_chunks = 1;
	cfg->pc_part_alloc_info[2].ai_part_id = M0_BE_PTABLE_ENTRY_SEG1;
	cfg->pc_part_alloc_info[2].ai_def_size_in_chunks =
		(cfg->pc_total_chunk_count * 10) / 100;
	cfg->pc_part_alloc_info[3].ai_part_id = M0_BE_PTABLE_ENTRY_BALLOC;
	cfg->pc_part_alloc_info[3].ai_def_size_in_chunks =
		(cfg->pc_total_chunk_count * 40) / 100;
	/* cfg->pc_dev_path_name = dom->bd_cfg.bc_seg0_cfg.bsc_stob_create_cfg;
	 * */
	cfg->pc_dev_path_name = devname;
	cfg->pc_dev_size_in_bytes = size; 
	*cfg_create = part_cfg;


	return 0;
}

static void stob_part_get_dom_key(uint64_t * dom_key)
{
	struct m0_be_ptable_part_tbl_info primary_part_info = {0};
	if(m0_be_ptable_get_part_info(&primary_part_info) == 0)
		*dom_key = primary_part_info.pti_key;	
}
static int stob_part_domain_init(struct m0_stob_type *type,
				 const char *location_data,
				 void *cfg_init,
				 struct m0_stob_domain **out)
{
	int rc;
	struct m0_stob_domain  *dom;
	uint8_t                 type_id;
	struct m0_fid           dom_id;
	struct part_domain_cfg *cfg = (struct part_domain_cfg *)cfg_init;
	uint64_t                dom_key = 0;
	struct m0_stob			*b_stob;
	rc = m0_be_ptable_create_init(cfg->be_domain,
				      false,
				      &cfg->part_config);
	if(rc == 0){
	
		M0_ALLOC_PTR(dom);
		if (dom == NULL)
			return M0_ERR(-ENOMEM);
			
		stob_part_get_dom_key(&dom_key);
		dom->sd_ops = &stob_part_domain_ops;
		dom->sd_type = (struct m0_stob_type*) &stob_part_type_ops;
		type_id = m0_stob_type_id_get(type);
		m0_stob_domain__dom_id_make(&dom_id, type_id, 0, dom_key);
		m0_stob_domain__id_set(dom, &dom_id);

		rc = m0_be_domain_stob_open(cfg->be_domain, M0_BE_PTABLE_PARTITION_TABLE,
				    cfg->part_config.pc_dev_path_name, &b_stob,
				    false);
		if(rc == 0){
			dom->sd_private = b_stob;
			*out = dom;
		}
	}
	return rc;
}

static void stob_part_domain_fini(struct m0_stob_domain *dom)
{
	if(dom->sd_private)
		m0_stob_put(dom->sd_private);
}

static void stob_part_domain_cfg_create_free(void *cfg_create)
{
	struct part_domain_cfg *cfg = cfg_create;
	if( cfg != NULL ) {
                // fixme: commented the below line
		m0_free((char *)cfg->part_config.pc_dev_path_name);
		m0_free(cfg);
	}
}

/* TODO Make cleanup on fail. */
static int stob_part_domain_create(struct m0_stob_type *type,
				   const char *location_data,
				   uint64_t sd_id,
				   void *cfg_create)
{

	struct part_domain_cfg     *cfg = (struct part_domain_cfg *)cfg_create;
	int                         rc;

	M0_ENTRY();
	cfg->part_config.pc_key = sd_id;
	rc = m0_be_ptable_create_init(cfg->be_domain,
				      true,
				      &cfg->part_config);
	return M0_RC(rc);
}


static struct m0_stob_part *stob_part_stob2part(const struct m0_stob *stob)
{
	return stob->so_private;
}

static struct m0_stob *stob_part_alloc(struct m0_stob_domain *dom,
				       const struct m0_fid *stob_fid)
{
	struct m0_stob_part *partstob;
	struct m0_stob      *stob;

	M0_ALLOC_PTR(partstob);
	stob = (partstob == NULL) ? NULL : &partstob->part_stob;
	if(stob != NULL)
		stob->so_private = partstob;
	return stob;
}

static void stob_part_free(struct m0_stob_domain *dom,
			   struct m0_stob *stob)
{
	struct m0_stob_part *partstob = stob_part_stob2part(stob);

	m0_free(partstob->part_table);
	m0_free(partstob);
}

static int stob_part_cfg_parse(const char *str_cfg_create, void **cfg_create)
{
	return 0;
}

static void stob_part_cfg_free(void *cfg_create)
{
}

static int stob_part_get_size(struct m0_stob_domain *dom, m0_bcount_t part_id, m0_bcount_t *part_size)
{
	int i;
	int rc = 0;
	struct m0_be_ptable_part_tbl_info pri_part_info;
	M0_ENTRY();
	M0_ASSERT(m0_be_ptable_get_part_info(&pri_part_info) == 0);
	for(i = 0; i < M0_BE_MAX_PARTITION_USERS; i++){
		if(pri_part_info.pti_part_alloc_info[i].ai_part_id == part_id){
			*part_size = pri_part_info.pti_part_alloc_info[i].ai_def_size_in_chunks;
			break;
		}
	}
	if(i == M0_BE_MAX_PARTITION_USERS)
		rc = -EINVAL;
	return M0_RC(rc);
}

static int stob_part_prepare_table(struct m0_stob *stob,
			  struct m0_stob_domain *dom,
			  const struct m0_fid *stob_fid)
{
	
	struct m0_stob_part   *partstob = stob_part_stob2part(stob);
	struct m0_be_ptable_part_tbl_info pt;
	m0_bcount_t                       primary_part_index;
	m0_bcount_t                       part_index;
	
	M0_PRE(partstob != NULL);
	partstob->part_id = stob_fid->f_key;
	if(stob_part_get_size(dom, partstob->part_id, &partstob->part_size_in_chunks) != 0 )
		return -EINVAL;

	M0_ALLOC_ARR(partstob->part_table,
		     partstob->part_size_in_chunks);
	if (partstob->part_table == NULL)
		return M0_ERR(-ENOMEM);

	if ( m0_be_ptable_get_part_info(&pt))
		M0_ASSERT(0);

	partstob->part_chunk_size_in_bits = pt.pti_chunk_size_in_bits;
	/**
	 * populate partition table
	 */
	part_index = 0;
	for (primary_part_index = 0;
	     primary_part_index <= pt.pti_dev_size_in_chunks;
	     primary_part_index++) {
		if (pt.pti_pri_part_info[primary_part_index].ppi_part_id ==
			    partstob->part_id)
			partstob->part_table[part_index++] = primary_part_index;
	}
	M0_ASSERT(part_index <= partstob->part_size_in_chunks);
	M0_LEAVE();
	return 0;
}

static int stob_part_init(struct m0_stob *stob,
			  struct m0_stob_domain *dom,
			  const struct m0_fid *stob_fid)
{
	struct m0_stob_part              *partstob = stob_part_stob2part(stob);
	
	stob->so_ops = &stob_part_ops;
	if(partstob->part_id == 0)
		return stob_part_prepare_table(stob, dom, stob_fid);
	
	return 0;
}

static void stob_part_fini(struct m0_stob *stob)
{
}

static void stob_part_create_credit(struct m0_stob_domain *dom,
				    struct m0_be_tx_credit *accum)
{
}

static int stob_part_create(struct m0_stob *stob,
			    struct m0_stob_domain *dom,
			    struct m0_dtx *dtx,
			    const struct m0_fid *stob_fid,
			    void *cfg)
{
	M0_ENTRY();	
        return stob_part_prepare_table(stob, dom, stob_fid);
}

static int stob_part_punch_credit(struct m0_stob *stob,
				  struct m0_indexvec *want,
				  struct m0_indexvec *got,
				  struct m0_be_tx_credit *accum)
{
	return M0_RC(0);
}

static void stob_part_destroy_credit(struct m0_stob *stob,
				     struct m0_be_tx_credit *accum)
{
}

static int stob_part_destroy(struct m0_stob *stob, struct m0_dtx *tx)
{
	return M0_RC(0);
}

static int stob_part_punch(struct m0_stob *stob,
			   struct m0_indexvec *range,
			   struct m0_dtx *tx)
{
	return M0_RC(0);
}

static uint32_t stob_part_block_shift(struct m0_stob *stob)
{
	return 0;
}

static int stob_part_fd(struct m0_stob *stob)
{
	struct m0_stob_domain *partdom =  m0_stob_dom_get(stob);
	struct m0_stob *b_stob;
	b_stob = stob_part_get_bstore(partdom);
	M0_ASSERT(b_stob != NULL && b_stob->so_ops != NULL && b_stob->so_ops->sop_fd != NULL);
	return b_stob->so_ops->sop_fd(b_stob);
}

static struct m0_stob_type_ops stob_part_type_ops = {
	.sto_register		     = &stob_part_type_register,
	.sto_deregister		     = &stob_part_type_deregister,
	.sto_domain_cfg_init_parse   = &stob_part_domain_cfg_init_parse,
	.sto_domain_cfg_init_free    = &stob_part_domain_cfg_init_free,
	.sto_domain_cfg_create_parse = &stob_part_domain_cfg_create_parse,
	.sto_domain_cfg_create_free  = &stob_part_domain_cfg_create_free,
	.sto_domain_init	     = &stob_part_domain_init,
	.sto_domain_create	     = &stob_part_domain_create,
	.sto_domain_destroy	     = &stob_part_domain_destroy,
};

static struct m0_stob_domain_ops stob_part_domain_ops = {
	.sdo_fini		= &stob_part_domain_fini,
	.sdo_stob_alloc	    	= &stob_part_alloc,
	.sdo_stob_free	    	= &stob_part_free,
	.sdo_stob_cfg_parse 	= &stob_part_cfg_parse,
	.sdo_stob_cfg_free  	= &stob_part_cfg_free,
	.sdo_stob_init	    	= &stob_part_init,
	.sdo_stob_create_credit	= &stob_part_create_credit,
	.sdo_stob_create	= &stob_part_create,
	.sdo_stob_write_credit	= &stob_part_write_credit,
};

static struct m0_stob_ops stob_part_ops = {
	.sop_fini            = &stob_part_fini,
	.sop_destroy_credit  = &stob_part_destroy_credit,
	.sop_destroy         = &stob_part_destroy,
	.sop_punch_credit    = &stob_part_punch_credit,
	.sop_punch           = &stob_part_punch,
	.sop_io_init         = &stob_part_io_init,
	.sop_block_shift     = &stob_part_block_shift,
	.sop_fd              = &stob_part_fd
};

const struct m0_stob_type m0_stob_part_type = {
	.st_ops  = &stob_part_type_ops,
	.st_fidt = {
		.ft_id   = STOB_TYPE_PARTITION,
		.ft_name = "partitionstob",
	},
};

static const struct m0_stob_io_op stob_part_io_op;

static bool stob_part_endio(struct m0_clink *link);
static void stob_part_io_release(struct m0_stob_part_io *pio);

static int stob_part_io_init(struct m0_stob *stob, struct m0_stob_io *io)
{
	struct m0_stob_part_io *pio;
	int                     rc;

	M0_PRE(io->si_state == SIS_IDLE);

	M0_ALLOC_PTR(pio);
	if (pio != NULL) {
		io->si_stob_private = pio;
		io->si_op = &stob_part_io_op;
		pio->pi_fore = io;
		m0_stob_io_init(&pio->pi_back);
		m0_clink_init(&pio->pi_clink, &stob_part_endio);
		m0_clink_add_lock(&pio->pi_back.si_wait, &pio->pi_clink);
		rc = 0;
	} else {
		rc = M0_ERR(-ENOMEM);
	}
	return M0_RC(rc);
}

static void stob_part_io_fini(struct m0_stob_io *io)
{
	struct m0_stob_part_io *pio = io->si_stob_private;
	stob_part_io_release(pio);
	m0_clink_del_lock(&pio->pi_clink);
	m0_clink_fini(&pio->pi_clink);
	m0_stob_io_fini(&pio->pi_back);
	m0_free(pio);
}

/**
   Releases vectors allocated for back IO.

   @note that back->si_stob.ov_vec.v_count is _not_ freed separately, as it is
   aliased to back->si_user.z_bvec.ov_vec.v_count.

   @see part_vec_alloc()
 */
static void stob_part_io_release(struct m0_stob_part_io *pio)
{
	struct m0_stob_io *back = &pio->pi_back;

	M0_ASSERT(back->si_user.ov_vec.v_count == back->si_stob.iv_vec.v_count);
	m0_free0(&back->si_user.ov_vec.v_count);
	back->si_stob.iv_vec.v_count = NULL;

	m0_free0(&back->si_user.ov_buf);
	m0_free0(&back->si_stob.iv_index);

	back->si_obj = NULL;
}


/**
   Allocates back IO buffers after number of fragments has been calculated.

   @see stob_part_io_release()
 */
static int stob_part_vec_alloc(struct m0_stob    *obj,
			       struct m0_stob_io *back,
			       uint32_t           frags)
{
	m0_bcount_t *counts;
	int          rc = 0;

	M0_ASSERT(back->si_user.ov_vec.v_count == NULL);

	if (frags > 0) {
		M0_ALLOC_ARR(counts, frags);
		back->si_user.ov_vec.v_count = counts;
		back->si_stob.iv_vec.v_count = counts;
		M0_ALLOC_ARR(back->si_user.ov_buf, frags);
		M0_ALLOC_ARR(back->si_stob.iv_index, frags);

		back->si_user.ov_vec.v_nr = frags;
		back->si_stob.iv_vec.v_nr = frags;

		if (counts == NULL || back->si_user.ov_buf == NULL ||
		    back->si_stob.iv_index == NULL) {
			m0_free(counts);
			m0_free(back->si_user.ov_buf);
			m0_free(back->si_stob.iv_index);
			rc = M0_ERR(-ENOMEM);
		}
	}
	return M0_RC(rc);
}

static m0_bcount_t stob_part_dev_offset_get(struct m0_stob_part *partstob,
					    m0_bcount_t user_byte_offset)
{
	m0_bcount_t   chunk_off_mask;
	m0_bcount_t   user_chunk_offset_index;
	m0_bcount_t   offset_within_chunk;
	m0_bcount_t   device_chunk_offset;
	m0_bcount_t   device_byte_offset;

	M0_ENTRY();
	M0_PRE(partstob != NULL);
	chunk_off_mask = (1 << partstob->part_chunk_size_in_bits) - 1;
	offset_within_chunk = user_byte_offset & chunk_off_mask;
	M0_LOG(M0_DEBUG, "relative offset in given chunk: %" PRIu64,
		offset_within_chunk);
	user_chunk_offset_index =
		(user_byte_offset >> partstob->part_chunk_size_in_bits);
	M0_LOG(M0_DEBUG, "table_index :%" PRIu64,
		user_chunk_offset_index);

	device_chunk_offset = partstob->part_table[user_chunk_offset_index];

	M0_LOG(M0_DEBUG, "device_chunk_offset: %" PRIu64,
		device_chunk_offset);
	device_byte_offset =
		( device_chunk_offset << partstob->part_chunk_size_in_bits ) +
		offset_within_chunk;
	M0_LOG(M0_DEBUG, "device offset in bytes: %" PRIu64,
		device_byte_offset);

	return(device_byte_offset);
}

/**
   Fills back IO request with device offset.
 */
static void stob_part_back_fill(struct m0_stob_io *io,
				struct m0_stob_io *back)
{
	uint32_t             idx;
	struct m0_stob      *stob = io->si_obj;
	struct m0_stob_part *partstob = stob_part_stob2part(stob);

	idx = 0;
	do {
		back->si_user.ov_vec.v_count[idx] =
			io->si_user.ov_vec.v_count[idx];
		back->si_user.ov_buf[idx] =
			io->si_user.ov_buf[idx];

		back->si_stob.iv_index[idx] =
			stob_part_dev_offset_get(partstob,
						 io->si_stob.iv_index[idx]);
		/**
		 * no need to update count again as it is aliases to
		 si_user.ov_vec.v_count, hence below statement is not required.
		 back->si_stob.iv_vec.v_count[idx] =
			io->si_stob.iv_vec.v_count[idx]; */

		idx++;
	} while (idx < io->si_stob.iv_vec.v_nr);
	back->si_user.ov_vec.v_nr = idx;
	back->si_stob.iv_vec.v_nr = idx;
}

/**
 * Constructs back IO for read.
 *
 * This is done in two passes:
 *
 *     - first, calculate number of fragments, taking holes into account. This
 *       pass iterates over user buffers list (src), target extents list (dst)
 *       and extents map (map). Once this pass is completed, back IO vectors can
 *       be allocated;
 *
 *     - then, iterate over the same sequences again. For holes, call memset()
 *       immediately, for other fragments, fill back IO vectors with the
 *       fragment description.
 *
 * @note assumes that allocation data can not change concurrently.
 *
 * @note memset() could become a bottleneck here.
 *
 * @note cursors and fragment sizes are measured in blocks.
 */
static int stob_part_read_prepare(struct m0_stob_io *io)
{
	struct m0_stob_io        *back;
	struct m0_stob_part_io   *pio = io->si_stob_private;
	int                       rc;

	M0_PRE(io->si_opcode == SIO_READ);

	back   = &pio->pi_back;
	rc = stob_part_vec_alloc(io->si_obj,
				 back,
				 io->si_stob.iv_vec.v_nr);
	if (rc != 0)
		return M0_RC(rc);

	stob_part_back_fill(io, back);

	return M0_RC(rc);
}


/**
 * Constructs back IO for write.
 *
 * - constructs back IO with translated device address;
 *  for now there is 1:1 mapping between the io extents and
 *  translated extents, this will work with static allocation
 *  where chuncks for same partition are adjacent in memory
 *  in future to support dynamic allocation need to device
 *  io extent further if it crosses chunk boundry
 *
 */
static int stob_part_write_prepare(struct m0_stob_io *io)
{
	struct m0_stob_io          *back;
	struct m0_stob_part_io     *pio = io->si_stob_private;
	int                         rc;

	M0_PRE(io->si_opcode == SIO_WRITE);
	M0_ADDB2_ADD(M0_AVI_STOB_IO_REQ, io->si_id, M0_AVI_PART_WR_PREPARE);
	M0_ENTRY("op=%d frags=%lu",
		 io->si_opcode,
		 (unsigned long)io->si_stob.iv_vec.v_nr);
	back = &pio->pi_back;

	rc = stob_part_vec_alloc(io->si_obj, back, io->si_stob.iv_vec.v_nr);
	if (rc == 0)
		stob_part_back_fill(io, back);
	return M0_RC(rc);
}

static int stob_part_io_launch_prepare(struct m0_stob_io *io)
{
	struct m0_stob_part_io     *pio  = io->si_stob_private;
	struct m0_stob_io          *back = &pio->pi_back;
	int                         rc;

	M0_PRE(io->si_stob.iv_vec.v_nr > 0);
	M0_PRE(!m0_vec_is_empty(&io->si_user.ov_vec));
	M0_PRE(io->si_state == SIS_PREPARED);

	/* prefix fragments execution mode is not yet supported */
	M0_PRE((io->si_flags & SIF_PREFIX) == 0);
	/* only read-write at the moment */
	M0_PRE(io->si_opcode == SIO_READ || io->si_opcode == SIO_WRITE);

	M0_ENTRY("op=%d, stob %p, stob_id="STOB_ID_F,
		 io->si_opcode, io->si_obj, STOB_ID_P(&io->si_obj->so_id));

	M0_ADDB2_ADD(M0_AVI_STOB_IO_REQ, io->si_id, M0_AVI_PART_PREPARE);

	back->si_opcode   = io->si_opcode;
	back->si_flags    = io->si_flags;
	back->si_fol_frag = io->si_fol_frag;
	back->si_id       = io->si_id;

	switch (io->si_opcode) {
	case SIO_READ:
		rc = stob_part_read_prepare(io);
		break;
	case SIO_WRITE:
		rc = stob_part_write_prepare(io);
		break;
	default:
		M0_IMPOSSIBLE("Invalid io type.");
	}

	return rc;
}

static struct m0_stob* stob_part_get_bstore(struct m0_stob_domain *dom)
{
	return dom->sd_private;	
}
/**
 * Launch asynchronous IO.
 *
 * Call ad_write_prepare() or ad_read_prepare() to do the bulk of work, then
 * launch back IO just constructed.
 */
static int stob_part_io_launch(struct m0_stob_io *io)
{
	struct m0_stob_part_io     *pio     = io->si_stob_private;
	struct m0_stob_io          *back    = &pio->pi_back;
	int                         rc      = 0;
	bool                        wentout = false;
	struct m0_stob_domain      *dom = m0_stob_dom_get(io->si_obj);

	M0_PRE(io->si_stob.iv_vec.v_nr > 0);
	M0_PRE(!m0_vec_is_empty(&io->si_user.ov_vec));
	M0_PRE(io->si_state == SIS_BUSY);

	/* prefix fragments execution mode is not yet supported */
	M0_PRE((io->si_flags & SIF_PREFIX) == 0);
	/* only read-write at the moment */
	M0_PRE(io->si_opcode == SIO_READ || io->si_opcode == SIO_WRITE);

	M0_ENTRY("op=%d stob_id="STOB_ID_F,
		 io->si_opcode, STOB_ID_P(&io->si_obj->so_id));
	M0_ADDB2_ADD(M0_AVI_STOB_IO_REQ, io->si_id, M0_AVI_AD_LAUNCH);


	if (back->si_stob.iv_vec.v_nr > 0) {
		/**
		 * Sorts index vecs in incremental order.
		 * @todo : Needs to check performance impact
		 *        of sorting each stobio on ad stob.
		 */
		M0_ADDB2_ADD(M0_AVI_STOB_IO_REQ, io->si_id,
			     M0_AVI_AD_SORT_START);
		m0_stob_iovec_sort(back);
		M0_ADDB2_ADD(M0_AVI_STOB_IO_REQ, io->si_id,
			     M0_AVI_AD_SORT_END);
		rc = m0_stob_io_prepare_and_launch(back, stob_part_get_bstore(dom),
						   io->si_tx, io->si_scope);
		wentout = rc == 0;
	} else {
		/*
		 * Back IO request was constructed OK, but is empty (all
		 * IO was satisfied from holes). Notify caller about
		 * completion.
		 */
		M0_ASSERT(io->si_opcode == SIO_READ);
		stob_part_endio(&pio->pi_clink);
	}

	if (!wentout)
		stob_part_io_release(pio);
	return M0_RC(rc);
}

static bool stob_part_endio(struct m0_clink *link)
{
	struct m0_stob_part_io *pio;
	struct m0_stob_io      *io;

	pio = container_of(link, struct m0_stob_part_io, pi_clink);
	io = pio->pi_fore;

	M0_ENTRY("op=%di, stob %p, stob_id="STOB_ID_F,
		 io->si_opcode, io->si_obj, STOB_ID_P(&io->si_obj->so_id));

	M0_ASSERT(io->si_state == SIS_BUSY);
	M0_ASSERT(pio->pi_back.si_state == SIS_IDLE);

	io->si_rc     = pio->pi_back.si_rc;
	io->si_count += pio->pi_back.si_count;
	io->si_state  = SIS_IDLE;
	M0_ADDB2_ADD(M0_AVI_STOB_IO_REQ, io->si_id, M0_AVI_AD_ENDIO);
	M0_ADDB2_ADD(M0_AVI_STOB_IO_END, FID_P(m0_stob_fid_get(io->si_obj)),
		     m0_time_sub(m0_time_now(), io->si_start),
		     io->si_rc, io->si_count, pio->pi_back.si_user.ov_vec.v_nr);
	stob_part_io_release(pio);
	m0_chan_broadcast_lock(&io->si_wait);
	return true;
}

static const struct m0_stob_io_op stob_part_io_op = {
	.sio_launch  = stob_part_io_launch,
	.sio_prepare = stob_part_io_launch_prepare,
	.sio_fini    = stob_part_io_fini,
};

/** @} end group stobpart */

#undef M0_TRACE_SUBSYSTEM
/*
 *  Local variables:
 *  c-indentation-style: "K&R"
 *  c-basic-offset: 8
 *  tab-width: 8
 *  fill-column: 80
 *  scroll-step: 1
 *  End:
 */