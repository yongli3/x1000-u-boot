/**
 * driver/nand_api.c
 *
 * Ingenic Nand Driver
 *
 **/
#include "nandinterface.h"
#include "vnandinfo.h"
#include "singlelist.h"
#include "blocklist.h"
#include "ppartition.h"
#include "pagelist.h"

#include <os_clib.h>
#include <speed_dug.h>
#include "nand_debug.h"
#include "nddata.h"
#include "nand_api.h"
#include "nand_io.h"
#include "nand_bch.h"
#include "nand_ops.h"
#include "nand_info.h"

//#define COMPAT_OLD_USB_BURN

/* global */
nand_data *nddata = NULL;
int nd_raw_boundary;

int (*__wait_rb_timeout) (rb_item *, int);
int (*__try_wait_rb) (rb_item *, int);
void (*__wp_enable) (int);
void (*__wp_disable) (int);
nand_flash * (*__get_nand_flash) (void);

extern int os_clib_init(os_clib *clib);

/*========================= NandInterface =========================*/
/**
 * if block is 'X', it is virtual
 *	     rb0   rb1   ...   rb(totalrbs)
 *     row ========================= (real start)
 *	   | raw |  X  | ... |  X  |
 *	   | pts |  X  | ... |  X  |
 *	   ------------------------- raw_boundary
 *	   |    zone               |
 *	   |    pts                |
 *	   ========================= rbblocks (real end)
 *	   :  X  : map : ... : map |
 *	   :  X  : ed  : ... : ed  |
 *	   ------------------------- (virtual end)
 *	   column
**/
static inline int is_blockid_virtual(ndpartition *npt, int blockid)
{
	int totalrbs = nddata->rbinfo->totalrbs;
	int chipprb = nddata->csinfo->totalchips / nddata->rbinfo->totalrbs;
	int column = blockid % totalrbs;
	int row = (npt->startblockid + blockid * npt->blockpvblock) / totalrbs;
	int raw_boundary = nddata->ptinfo->raw_boundary;
	int rbblocks = chipprb * nddata->cinfo->totalblocks;

	return ((column > 0) && (row < raw_boundary)) || ((column == 0) && (row >= rbblocks));
}

static int single_page_read(void *ppartition, int pageid, int offsetbyte, int bytecount, void *data)
{
	int ret;
	PageList ppl_node;
	PPartition *ppt = (PPartition *)ppartition;

	ndd_print(NDD_DEBUG,"......... Enter %s function %d line !\n",__func__,__LINE__);
	ppl_node.head.next = NULL;
	ppl_node.startPageID = pageid;
	ppl_node.OffsetBytes = offsetbyte;
	ppl_node.Bytes = bytecount;
	ppl_node.pData = data;

	speed_dug_begin(NDD_READ, &ppl_node);
	ret = nandops_read(nddata->ops_context, (ndpartition *)ppt->prData, &ppl_node);
	speed_dug_end(NDD_READ);

	if (ret == ECC_ERROR)
		ndd_dump_uncorrect_err(nddata, ppt, &ppl_node);

	ndd_print(NDD_DEBUG,"......... Out %s function %d line !\n",__func__,__LINE__);
	return ret;
}

static int single_page_write(void *ppartition, int pageid, int offsetbyte, int bytecount, void *data)
{
	int ret;
	PageList ppl_node;
	PPartition *ppt = (PPartition *)ppartition;

	//ndd_print(NDD_DEBUG,"......... Enter %s function %d line !\n",__func__,__LINE__);
	ppl_node.head.next = NULL;
	ppl_node.startPageID = pageid;
	ppl_node.OffsetBytes = offsetbyte;
	ppl_node.Bytes = bytecount;
	ppl_node.pData = data;

	__wp_disable(nddata->gpio_wp);
	ndd_dump_rewrite(nddata, ppt, &ppl_node);
	speed_dug_begin(NDD_WRITE, &ppl_node);
	ret = nandops_write(nddata->ops_context, (ndpartition *)ppt->prData, &ppl_node);
	speed_dug_end(NDD_WRITE);
	__wp_enable(nddata->gpio_wp);

	//ndd_print(NDD_DEBUG,"......... Out %s function %d line !\n",__func__,__LINE__);
	return ret;
}

static int multi_page_read(void *ppartition, PageList *pl)
{
	int ret;
	PPartition *ppt = (PPartition *)ppartition;
	ndpartition *npt = (ndpartition *)ppt->prData;

	speed_dug_begin(NDD_READ, pl);
	ret = nandops_read(nddata->ops_context, npt, pl);
	speed_dug_end(NDD_READ);

	if (ret == ECC_ERROR)
		ndd_dump_uncorrect_err(nddata, ppt, pl);

	return ret;
}

static int multi_page_write(void *ppartition, PageList *pl)
{
	int ret;
	PPartition *ppt = (PPartition *)ppartition;
	ndpartition *npt = (ndpartition *)ppt->prData;
	//ndd_print(NDD_DEBUG,"......... Enter %s function %d line !\n",__func__,__LINE__);

	//if(!strcmp(ppt->name,"ndsystem"))
        ndd_dump_write_reread_prepare(pl);

	__wp_disable(nddata->gpio_wp);
	ndd_dump_rewrite(nddata, ppt, pl);
	speed_dug_begin(NDD_WRITE, pl);
	ret = nandops_write(nddata->ops_context, npt, pl);
	speed_dug_end(NDD_WRITE);
	__wp_enable(nddata->gpio_wp);

	if (ret == SUCCESS)
	{
		//if(!strcmp(ppt->name,"ndsystem"))
			ndd_dump_write_reread_complete(nddata, ppt, pl);
	}
	//ndd_print(NDD_DEBUG,"......... Out %s function %d line !\n",__func__,__LINE__);
	return ret;
}

static int multi_block_erase(void* ppartition, BlockList *bl)
{
	int ret, cur = 0;
	unsigned int bitmap = 0xffffffff;
	struct singlelist *pos, *top = NULL, *tmp_node = NULL, *virtual_list = NULL, *normal_list = NULL;
	BlockList *bl_node;
	PPartition *ppt = (PPartition *)ppartition;
	ndpartition *npt = (ndpartition *)ppt->prData;

	singlelist_for_each(pos, &bl->head) {
		if (tmp_node) {
			tmp_node->next = NULL;
			if (!virtual_list)
				virtual_list = tmp_node;
			else
				singlelist_add_tail(virtual_list, tmp_node);
			bitmap &= ~(1 << (cur - 1));
			tmp_node = NULL;
		}
		bl_node = singlelist_entry(pos, BlockList, head);
		if (is_blockid_virtual(npt, bl_node->startBlock)) {
			ndd_debug("%s, pt[%s], block [%d] is virtual\n", __func__, npt->name, bl_node->startBlock);
			bl_node->retVal = 0; /* we return ok, but do nothing */
			singlelist_del(&bl->head, pos);
			tmp_node = pos;
		}
		cur++;
	}

	__wp_disable(nddata->gpio_wp);
	ret = nandops_erase(nddata->ops_context, npt, bl);
	__wp_enable(nddata->gpio_wp);

	if (virtual_list) {
		normal_list = &bl->head;
		while (normal_list || virtual_list) {
			for (cur = 0; cur < sizeof(unsigned int) * 8; cur++) {
				if ((bitmap & (1 << cur)) == 0) {
					if (!virtual_list)
						RETURN_ERR(ENAND, "virtual list not map bitmap\n");
					tmp_node = virtual_list;
					singlelist_del(virtual_list, tmp_node);
				} else {
					if (!normal_list)
						RETURN_ERR(ENAND,"normal list not map bitmap\n");
					tmp_node = normal_list;
					singlelist_del(normal_list, tmp_node);
				}
				tmp_node->next = NULL;
				if (!top)
					top = tmp_node;
				else
					singlelist_add(top, tmp_node);
			}
		}

		bl = singlelist_entry(top, BlockList, head);
	}

	//ndd_print(NDD_DEBUG,"......... Out %s function %d line !\n",__func__,__LINE__);
	return ret;
}

static int is_badblock(void *ppartition, int blockid)
{
	int ret;
	PPartition *ppt = (PPartition *)ppartition;
	ndpartition *npt = (ndpartition *)ppt->prData;

	//ndd_print(NDD_DEBUG,"......... Enter %s function %d line !\n",__func__,__LINE__);
	/* if block is virtual, it is bad block */
	if (is_blockid_virtual(npt, blockid)) {
		ndd_debug("%s, pt[%s], block [%d] is virtual\n", __func__, npt->name, blockid);
		return ENAND;
	}

	ret = nandops_isbadblk(nddata->ops_context, npt, blockid);

#ifdef CHECK_USED_BLOCK
	if ((ret == 0) && (blockid >= 0))
		ret = ndd_dump_check_used_block(nddata, ppt, blockid);
#endif

	//ndd_print(NDD_DEBUG,"......... Out %s function %d line !\n",__func__,__LINE__);
	return ret;
}

static int mark_badblock(void *ppartition, int blockid)
{
	int ret;
	PPartition *ppt = (PPartition *)ppartition;
	ndpartition *npt = (ndpartition *)ppt->prData;

//ndd_print(NDD_DEBUG,"......... Enter %s function %d line !\n",__func__,__LINE__);
	/* if block is virtual, mark it will do nothing */
	if (is_blockid_virtual(npt, blockid)) {
		ndd_debug("%s, pt[%s], block [%d] is virtual\n", __func__, npt->name, blockid);
		return 0;
	}

	__wp_disable(nddata->gpio_wp);
	ret = nandops_markbadblk(nddata->ops_context, npt, blockid);
	__wp_enable(nddata->gpio_wp);

	//ndd_print(NDD_DEBUG,"......... Out %s function %d line !\n",__func__,__LINE__);
	return ret;
}

static inline int get_ppt_startblockID(ndpartition *npt);
static inline int get_ppt_totalblocks(ndpartition *npt);
static int ioctl(enum ndd_cmd cmd, int args)
{
//ndd_print(NDD_DEBUG,"......... Enter %s function %d line !\n",__func__,__LINE__);
	switch (cmd) {
	case NDD_UPDATE_PT: {
		int pt_index;
		int ppt_orign_startblockid, ppt_orign_totalblocks;
		int npt_new_startblockid, npt_new_totalblocks;
		ndpartition *npta = nddata->ptinfo->pt;
		PPartition *ppt = (PPartition *)args;

		for (pt_index = 0; pt_index < nddata->ptinfo->ptcount; pt_index++) {
			if (!ndd_strcmp(ppt->name, npta[pt_index].name)) {
				/* startblockID */
				ppt_orign_startblockid = get_ppt_startblockID(npta + pt_index);
				if (ppt_orign_startblockid != ppt->startblockID) {
					npt_new_startblockid = npta[pt_index].startblockid +
						(ppt->startblockID - ppt_orign_startblockid) * npta[pt_index].blockpvblock;
					ndd_print(NDD_WARNING, "WARNING: npt[%s], startblockid [%d] modified to [%d]\n",
						  npta[pt_index].name, npta[pt_index].startblockid, npt_new_startblockid);
					npta[pt_index].startblockid = npt_new_startblockid;
					ppt->startblockID = npta[pt_index].startblockid;
				}
				/* totalblocks */
				ppt_orign_totalblocks = get_ppt_totalblocks(npta + pt_index);
				if (ppt_orign_totalblocks != ppt->totalblocks) {
					npt_new_totalblocks = ppt->totalblocks * npta[pt_index].blockpvblock;
					ndd_print(NDD_WARNING, "WARNING: npt[%s], totalblocks [%d] modified to [%d]\n",
						  npta[pt_index].name, npta[pt_index].totalblocks, npt_new_totalblocks);
					npta[pt_index].totalblocks = npt_new_totalblocks;
				}
			}
		}
		break;
	}
	default:
		ndd_print(NDD_WARNING, "WARNING, unknown ioctl command!\n");
	}

	//ndd_print(NDD_DEBUG,"......... Out %s function %d line !\n",__func__,__LINE__);
	return 0;
}

static inline const char* get_ppt_name(ndpartition *npt)
{
	return npt->name;
}
static inline int get_ppt_byteperpage(ndpartition *npt)
{
	return npt->pagesize;
}
static inline int get_ppt_pageperblock(ndpartition *npt)
{
	return npt->blockpvblock * npt->pagepblock;
}
static inline int get_ppt_startblockID(ndpartition *npt)
{
	return npt->startblockid;
}
static inline int get_ppt_totalblocks(ndpartition *npt)
{
	return npt->totalblocks / npt->blockpvblock;
}
static inline int get_ppt_PageCount(ndpartition *npt)
{
	return npt->totalblocks * npt->pagepblock;
}
static inline int get_ppt_startPage(ndpartition *npt)
{
	return npt->startblockid * npt->pagepblock;
}
static inline int get_ppt_mode(ndpartition *npt)
{
	return npt->nm_mode;
}
static inline void* get_ppt_prData(ndpartition *npt)
{
	return (void *)npt;
}
static inline int get_ppt_pagespergroup(ndpartition *npt)
{
	return npt->pagepblock * npt->blockpvblock * npt->vblockpgroup;
}
static inline int get_ppt_groupperzone(ndpartition *npt)
{
	return npt->groupspzone;
}
static inline int get_ppt_flags(ndpartition *npt)
{
	return npt->flags;
}

static inline int fill_ppartition(PPartition *ppt)
{
	int pt_index,i;
	unsigned int ptcount = nddata->ptinfo->ptcount;
	ndpartition *npt = nddata->ptinfo->pt;

	for (pt_index = 0; pt_index < ptcount; pt_index++) {
		/* fill PPartition */
		ppt[pt_index].name = get_ppt_name(npt + pt_index);
		ppt[pt_index].byteperpage = get_ppt_byteperpage(npt + pt_index);
		ppt[pt_index].pageperblock = get_ppt_pageperblock(npt + pt_index);
		ppt[pt_index].startblockID = get_ppt_startblockID(npt + pt_index);
		ppt[pt_index].totalblocks = get_ppt_totalblocks(npt + pt_index);
		ppt[pt_index].PageCount = get_ppt_PageCount(npt + pt_index);
		ppt[pt_index].badblockcount = 0;
		ppt[pt_index].hwsector = HW_SECTOR;
		ppt[pt_index].startPage = get_ppt_startPage(npt + pt_index);
		ppt[pt_index].mode = get_ppt_mode(npt + pt_index);
		ppt[pt_index].prData = get_ppt_prData(npt + pt_index);
		ppt[pt_index].badblock = NULL;
		ppt[pt_index].v2pp = NULL;
		ppt[pt_index].pagespergroup = get_ppt_pagespergroup(npt + pt_index);
		ppt[pt_index].groupperzone = get_ppt_groupperzone(npt + pt_index);
		ppt[pt_index].flags = get_ppt_flags(npt + pt_index);
		for (i = 0; i < npt[pt_index].ndparts_num; i++) {
			ppt[pt_index].parts[i].name = npt[pt_index].ndparts[i].name;
			ppt[pt_index].parts[i].startblockID = npt[pt_index].ndparts[i].startblockID;
			ppt[pt_index].parts[i].totalblocks = npt[pt_index].ndparts[i].totalblocks /
				npt[pt_index].blockpvblock;
		}
		ppt[pt_index].parts_num = npt[pt_index].ndparts_num;
		/* pt nderror */
		if ((ppt[pt_index].mode == ONCE_MANAGER) && (pt_index != 0))
			ppt[pt_index].badblockcount = ppt[pt_index - 1].totalblocks - ppt[pt_index].totalblocks;
	}

	return 0;
}

static int init_nand(void *vNand)
{
	int ret;
	PPartArray *ppta;
	PPartition *ppt;
	int ptcount = nddata->ptinfo->ptcount;
	int vpt_index = ptcount - 1;
	VNandManager *vnm =(VNandManager *)vNand;

	ppt = ndd_alloc(sizeof(PPartition) * ptcount);
	if (!ppt)
		GOTO_ERR(ndd_alloc_ppt);

	ppta = ndd_alloc(sizeof(PPartArray));
	if (!ppta)
		GOTO_ERR(ndd_alloc_ppta);

	/* get ppartitions */
	ret = fill_ppartition(ppt);
	if (ret)
		GOTO_ERR(fill_ppt);

	/* PPartArray */
	ppta->ptcount = ptcount - 1;
	ppta->ppt = ppt;

	/* VNandManager -> VNandInfo  ppta->ppt[nddata->ptcount] is 'ndvirtual' */
	vnm->info.startBlockID = ppta->ppt[vpt_index].startblockID;
	vnm->info.PagePerBlock = ppta->ppt[vpt_index].pageperblock;
	vnm->info.BytePerPage = ppta->ppt[vpt_index].byteperpage;
	vnm->info.TotalBlocks = ppta->ppt[vpt_index].totalblocks;
	vnm->info.MaxBadBlockCount = ppta->ppt[vpt_index].badblockcount;
	vnm->info.hwSector = ppta->ppt[vpt_index].hwsector;
	vnm->info.prData = (void *)&ppta->ppt[vpt_index];

	/* VNandManager -> PPartArray */
	vnm->pt = ppta;

	/* dump */
	ndd_dump_ppartition(ppta->ppt, ptcount);

	return 0;

ERR_LABLE(fill_ppt):
	ndd_free(ppta);
ERR_LABLE(ndd_alloc_ppta):
	ndd_free(ppt);
ERR_LABLE(ndd_alloc_ppt):
	return -1;
}

static int deinit_nand(void *vNand)
{
	VNandManager *vnm =(VNandManager *)vNand;

	if (vnm->pt) {
		if (vnm->pt->ppt)
			ndd_free(vnm->pt->ppt);
		ndd_free(vnm->pt);
	}

	return 0;
}

NandInterface nand_interface = {
	.iPageRead = single_page_read,
	.iPageWrite = single_page_write,
	.iMultiPageRead = multi_page_read,
	.iMultiPageWrite = multi_page_write,
	.iMultiBlockErase = multi_block_erase,
	.iIsBadBlock = is_badblock,
	.iMarkBadBlock = mark_badblock,
	.iIoctl = ioctl,
	.iInitNand = init_nand,
	.iDeInitNand = deinit_nand,
};

/*========================= NandDriver =========================*/
static inline int next_platpt_connected(unsigned int plat_index,
					plat_ptitem *plat_pt, unsigned int plat_ptcount)
{
	return (((plat_index + 1) < plat_ptcount) &&
		((plat_pt[plat_index].offset + plat_pt[plat_index].size) == plat_pt[plat_index + 1].offset));
}

static inline int get_group_vblocks(chip_info *cinfo, unsigned short blockpvlblock, unsigned int groupspzone)
{
	unsigned int vblocksize = (cinfo->pagesize * cinfo->ppblock) * blockpvlblock;

#ifdef COMPAT_OLD_USB_BURN
	return 4;
#endif
	/* group vblocks may be at 2 ~ 16 */
	if ((REF_ZONE_SIZE / groupspzone / vblocksize) < 2)
		return 2;
	else if ((REF_ZONE_SIZE / groupspzone / vblocksize) > 16)
		return 16;
	else
		return REF_ZONE_SIZE / groupspzone / vblocksize;
}

static inline unsigned int get_raw_bandary(plat_ptinfo *plat_ptinfo, chip_info *cinfo, unsigned short totalrbs)
{
	unsigned int raw_boundary = 0;
	unsigned char nm_mode, last_nm_mode = -1;
	unsigned short pt_index = plat_ptinfo->ptcount;
	plat_ptitem *plat_pt = plat_ptinfo->pt_table;

	while (pt_index--) {
		nm_mode = plat_pt[pt_index].nm_mode;
		if ((nm_mode != ZONE_MANAGER) && (last_nm_mode == ZONE_MANAGER)) {
			unsigned int blocksize = cinfo->pagesize * cinfo->ppblock;
			unsigned short alignunit = cinfo->planenum * totalrbs;
			unsigned int blockid = div_s64_32((plat_pt[pt_index + 1].offset + blocksize - 1), blocksize);
			raw_boundary = ((blockid + (alignunit - 1)) / alignunit) * alignunit;
			break;
		}
		last_nm_mode = nm_mode;
	}
	nd_raw_boundary = raw_boundary;
	return raw_boundary;
}

static inline int get_startblockid(plat_ptitem *plat_pt, chip_info *cinfo,
				   unsigned short totalrbs, unsigned int raw_boundary)
{
	unsigned int blocksize = cinfo->pagesize * cinfo->ppblock;
	unsigned int blockid = div_s64_32((plat_pt->offset + blocksize - 1), blocksize);

	if (plat_pt->nm_mode == ZONE_MANAGER) {
		unsigned short alignunit = cinfo->planenum * totalrbs;
		return (((blockid + (alignunit - 1)) / alignunit) * alignunit)
			+ (totalrbs > 1 ? (raw_boundary * (totalrbs - 1)) : 0);
	} else
		return blockid * totalrbs;
}

static inline int get_endblockid(plat_ptitem *plat_pt, chip_info *cinfo,
				 unsigned short totalchips, unsigned short totalrbs,
				 unsigned int raw_boundary)
{
	unsigned int blockid;
	unsigned long long offset;
	unsigned int blocksize = cinfo->pagesize * cinfo->ppblock;

	if (plat_pt->size == -1) {
		blockid = cinfo->totalblocks * totalchips;
		/* update last plat partition totalblocks
		   rb    rb
		   ------------- nand start
		   | raw | X X |
		   | pts | X X | here mark bad blocks, and mirror to 'vtl'
		   -------------
		   |    zone   |
		   |    pts    |
		   ------------- nand end
		   | X X | vtl |
		   | X X |     | here add to last zone pt
		   -------------
		*/
		blockid += (totalrbs > 1) ? (raw_boundary * totalrbs) : 0;
	} else {
		offset = plat_pt->offset + plat_pt->size;
		blockid = div_s64_32(offset, blocksize);
	}

	if (plat_pt->nm_mode == ZONE_MANAGER) {
		unsigned short alignunit = cinfo->planenum * totalrbs;
		return (blockid / alignunit * alignunit) - 1;
	} else
		return blockid * totalrbs - 1;
}

static inline int fill_ex_partition(ndpartition *pt, plat_ex_partition *plat_expta,
				    chip_info *cinfo, unsigned short totalrbs)
{
	int i = 0;
	int blockid, statblockID = 0, endblockID;
	unsigned int blocksize = cinfo->pagesize * cinfo->ppblock;

	while (plat_expta[i].size != 0) {
		pt->ndparts[i].startblockID = statblockID;
		if (plat_expta[i].size == -1)
			endblockID = pt->totalblocks - 1;
		else {
			blockid = div_s64_32(plat_expta[i].offset + plat_expta[i].size, blocksize);
			if (pt->nm_mode == ZONE_MANAGER) {
				unsigned short alignunit = cinfo->planenum * totalrbs;
				endblockID = (blockid / alignunit * alignunit) - 1;
			} else
				endblockID = blockid - 1;
		}
		pt->ndparts[i].totalblocks = endblockID - statblockID + 1;
		pt->ndparts[i].name = plat_expta[i].name;
		statblockID += pt->ndparts[i].totalblocks;
		i++;
	}
	pt->ndparts_num= i;

	return 0;
}

static pt_info* get_ptinfo(chip_info *cinfo, unsigned short totalchips,
			   unsigned short totalrbs, plat_ptinfo *plat_ptinfo)
{
	ndpartition *pt = NULL;
	pt_info *ptinfo = NULL;
	unsigned int endblockid;
	unsigned short plat_ptcount = plat_ptinfo->ptcount;
	unsigned short ptcount = plat_ptcount + REDUN_PT_NUM;
	plat_ptitem *plat_pt = plat_ptinfo->pt_table;
	unsigned short pt_index = plat_ptcount;
	unsigned short redunpt_start = plat_ptcount;

	ndd_dump_plat_partition(plat_ptinfo);

	ptinfo = ndd_alloc(sizeof(pt_info) + sizeof(ndpartition) * ptcount);
	if (!ptinfo)
		RETURN_ERR(NULL, "ndd_alloc memory for ptinfo error");
	else {
		ptinfo->ptcount = ptcount;
		ptinfo->pt = (ndpartition *)((unsigned char *)ptinfo + sizeof(pt_info));
	}

	ptinfo->raw_boundary = get_raw_bandary(plat_ptinfo, cinfo, totalrbs);
	pt = ptinfo->pt;

	while (pt_index--) {
		/* init ndpartition */
		pt[pt_index].name = plat_pt[pt_index].name;
		pt[pt_index].pagesize = cinfo->pagesize;
		pt[pt_index].pagepblock = cinfo->ppblock;
		pt[pt_index].startblockid = get_startblockid(&plat_pt[pt_index], cinfo,
							     totalrbs, ptinfo->raw_boundary);
		if (next_platpt_connected(pt_index, plat_pt, plat_ptcount))
			endblockid = pt[pt_index + 1].startblockid - 1;
		else
			endblockid = get_endblockid(&plat_pt[pt_index], cinfo, totalchips,
						    totalrbs, ptinfo->raw_boundary);
		pt[pt_index].totalblocks = endblockid - pt[pt_index].startblockid + 1;

		if (plat_pt[pt_index].nm_mode == ZONE_MANAGER) {
			pt[pt_index].groupspzone = totalrbs;
#ifdef COMPAT_OLD_USB_BURN
			if (!ndd_strcmp("ndsystem", pt[pt_index].name))
				pt[pt_index].blockpvblock = 1;
			else
				pt[pt_index].blockpvblock = cinfo->planenum;
#else
			pt[pt_index].blockpvblock = cinfo->planenum;
#endif
			pt[pt_index].vblockpgroup = get_group_vblocks(cinfo, cinfo->planenum, totalrbs);
#ifdef COMPAT_OLD_USB_BURN
			if (!ndd_strcmp("ndsystem", pt[pt_index].name))
				pt[pt_index].planes = 1;
			else
				pt[pt_index].planes = cinfo->planenum;
#else
			pt[pt_index].planes = cinfo->planenum;
#endif

			while (1) {
				/* check if pt has too few zones, we need to adjust blocks per zone */
				unsigned int zoneblocks = pt[pt_index].blockpvblock *
					pt[pt_index].vblockpgroup * pt[pt_index].groupspzone;
				unsigned int zonecount = pt[pt_index].totalblocks / zoneblocks;
				if (zonecount < ZONE_COUNT_LIMIT) {
					if (pt[pt_index].vblockpgroup > 1)
						pt[pt_index].vblockpgroup /= 2;
					else if (pt[pt_index].blockpvblock > 1) {
						pt[pt_index].blockpvblock /= 2;
						pt[pt_index].planes /=2;
					} else {
						ndd_print(NDD_WARNING, "WARNING: pt[%s] has too few blocks"
							  " as ZONE_MANAGER partition, totalblocks = %d\n",
							  pt[pt_index].name, pt[pt_index].totalblocks);
						break;
					}
				} else
					break;
			}
		} else {
			pt[pt_index].groupspzone = totalrbs;
			pt[pt_index].blockpvblock = 1;
			pt[pt_index].vblockpgroup = 1;
			pt[pt_index].planes = 1;
		}
		pt[pt_index].eccbit = cinfo->eccbit;
		pt[pt_index].ops_mode = plat_pt[pt_index].ops_mode;
		pt[pt_index].nm_mode = plat_pt[pt_index].nm_mode;
		pt[pt_index].copy_mode = DEFAULT_COPY_MODE;
		pt[pt_index].flags = plat_pt[pt_index].flags;
		fill_ex_partition(&pt[pt_index], plat_pt[pt_index].ex_partition, cinfo, totalrbs);
		pt[pt_index].handler = 0;
	}

	/* nderror */
	pt[redunpt_start].name = "nderror";
	pt[redunpt_start].pagesize = cinfo->pagesize;
	pt[redunpt_start].pagepblock = cinfo->ppblock;
	pt[redunpt_start].startblockid = pt[redunpt_start - 1].startblockid +
		pt[redunpt_start - 1].totalblocks;
	pt[redunpt_start].totalblocks = ERR_PT_TOTALBLOCKS;
	pt[redunpt_start].blockpvblock = 1;
	pt[redunpt_start].groupspzone = totalrbs; /* used for ppt aligned in libnm */
	pt[redunpt_start].vblockpgroup = 1;
	pt[redunpt_start].eccbit = cinfo->eccbit;
	pt[redunpt_start].planes = 1;
	pt[redunpt_start].ops_mode = CPU_OPS;
	pt[redunpt_start].nm_mode = ONCE_MANAGER;
	pt[redunpt_start].copy_mode = DEFAULT_COPY_MODE;
	pt[redunpt_start].flags = 0;
	pt[redunpt_start].handler = 0;

	/* ndvirtual */
	pt[redunpt_start + 1].name = "ndvirtual";
	pt[redunpt_start + 1].pagesize = cinfo->pagesize;
	pt[redunpt_start + 1].pagepblock = cinfo->ppblock;
	pt[redunpt_start + 1].startblockid = 0;
	pt[redunpt_start + 1].totalblocks = cinfo->totalblocks * totalchips;
	if (totalrbs > 1)
		pt[redunpt_start + 1].totalblocks += totalrbs * ptinfo->raw_boundary;
	pt[redunpt_start + 1].blockpvblock = 1;
	pt[redunpt_start + 1].groupspzone = totalrbs;
	pt[redunpt_start + 1].vblockpgroup = get_group_vblocks(cinfo, cinfo->planenum, totalrbs);
	pt[redunpt_start + 1].eccbit = cinfo->eccbit;
	pt[redunpt_start + 1].planes = 1;
	pt[redunpt_start + 1].ops_mode = CPU_OPS;//DEFAULT_OPS_MODE;
	pt[redunpt_start + 1].nm_mode = ZONE_MANAGER;
	pt[redunpt_start + 1].copy_mode = DEFAULT_COPY_MODE;
	pt[redunpt_start + 1].flags = 0;
	pt[redunpt_start + 1].handler = 0;

	ndd_dump_ptinfo(ptinfo);

	return ptinfo;
}

static void free_ptinfo(pt_info *ptinfo)
{
	ndd_free(ptinfo);
}

static cs_info* get_csinfo(nfi_base *base, rb_info *rbinfo)
{
	int cs_index;
	cs_info *csinfo = NULL;
	rb_item *rbitem = NULL;

	csinfo = ndd_alloc(sizeof(cs_info) + (sizeof(cs_item) * CS_PER_NFI));
	if (!csinfo)
		RETURN_ERR(NULL, "ndd_alloc memory for cs info error");
	else {
		csinfo->totalchips = 0;
		csinfo->csinfo_table = (cs_item *)((unsigned char *)csinfo + sizeof(cs_info));
	}

	for (cs_index = 0; cs_index < CS_PER_NFI; cs_index++) {
		rbitem = get_rbitem(base, cs_index, rbinfo);
		if (rbitem) {
			csinfo->csinfo_table[csinfo->totalchips].id = cs_index;
			csinfo->csinfo_table[csinfo->totalchips].rbitem = rbitem;
			csinfo->csinfo_table[csinfo->totalchips].iomem = base->cs_iomem[cs_index];
			csinfo->totalchips++;
		}
	}

	if (csinfo->totalchips < rbinfo->totalrbs) {
		ndd_free(csinfo);
		RETURN_ERR(NULL, "scand totalchips [%d] is less than totalrbs [%d]",
			   csinfo->totalchips, rbinfo->totalrbs);
	}

	ndd_dump_csinfo(csinfo);

	return csinfo;
}

static void free_csinfo(cs_info *csinfo)
{
	ndd_free(csinfo);
}

static int fill_cinfo(nfi_base *base, chip_info *cinfo, rb_info *rbinfo, const nand_flash *ndflash)
{
	/* init nddata->cinfo */
	cinfo->manuf = ndflash->id >> 8;
	cinfo->pagesize = ndflash->pagesize;
	cinfo->oobsize = ndflash->oobsize;
	cinfo->ppblock = ndflash->blocksize / ndflash->pagesize;
	cinfo->totalblocks = ndflash->maxvalidblocks / ndflash->chips;
	cinfo->totalpages = ndflash->maxvalidblocks * cinfo->ppblock / ndflash->chips;
	cinfo->planepdie = ndflash->planepdie;
	cinfo->totaldies = ndflash->diepchip;
	cinfo->origbadblkpos = ndflash->badblockpos;
	cinfo->badblkpos = ndflash->oobsize - 4;
	cinfo->eccpos = 0;
	cinfo->buswidth = ndflash->buswidth;
	cinfo->rowcycles = ndflash->rowcycles;
	cinfo->eccbit = ndflash->eccbit;
	cinfo->planenum = ndflash->realplanenum > 2 ? 2 : ndflash->realplanenum;
	cinfo->planeoffset = ndflash->planeoffset;
	cinfo->options = ndflash->options;

	/* if support read retry, get retry parm table */
	if (SUPPROT_READ_RETRY(cinfo)) {
		cinfo->retryparms = ndd_alloc(sizeof(retry_parms));
		if (!cinfo->retryparms)
			RETURN_ERR(ENAND, "can not ndd_alloc memory for retryparms");

		cinfo->retryparms->mode = READ_RETRY_MODE(cinfo);
		if (get_retry_parms(base, 0, rbinfo, cinfo->retryparms)) {
			ndd_free(cinfo->retryparms);
			RETURN_ERR(ENAND, "get retry data error");
		}
	} else
		cinfo->retryparms = NULL;

	cinfo->timing = &ndflash->timing;

	return 0;
}

static chip_info* get_chip_info(struct nand_api_osdependent *private,
				nfi_base *base, rb_info *rbinfo)
{
	int ret;
	chip_info *cinfo = NULL;
	const nand_flash *ndflash = NULL;
	nand_flash_id nand_fid = {0};

	cinfo = ndd_alloc(sizeof(chip_info));
	if (!cinfo)
		RETURN_ERR(NULL, "ndd_alloc memory for nand chip info error");

	if (__get_nand_flash) {
		ndflash = __get_nand_flash();
		if (!ndflash)
			GOTO_ERR(no_flash);
	} else {
		ret = get_nand_id(base, 0, rbinfo, &nand_fid);
		if (ret)
			GOTO_ERR(get_id);

		ndflash = get_nand_flash(&nand_fid);
		if (!ndflash)
			GOTO_ERR(no_flash);
	}

	ret = fill_cinfo(base, cinfo, rbinfo, ndflash);
	if (ret)
		GOTO_ERR(fill_cinfo);

	cinfo->drv_strength = private->drv_strength;
	cinfo->rb_pulldown = private->rb_pulldown;

	ndd_dump_chip_info(cinfo);

	return cinfo;

ERR_LABLE(get_id):
ERR_LABLE(fill_cinfo):
ERR_LABLE(no_flash):
	ndd_free(cinfo);
	return 0;
}

static void free_chip_info(chip_info *cinfo)
{
	ndd_free(cinfo);
}

static io_base* get_base(struct nand_api_osdependent *private)
{
	return private->base;
}

static rb_info* get_rbinfo(struct nand_api_osdependent *private)
{
	return private->rbinfo;
}


static int nandflash_setup(nfi_base *base, cs_info *csinfo, rb_info *rbinfo, chip_info *cinfo)
{
	int cs_index, cs_id, ret = 0;
	unsigned short totalchips = csinfo->totalchips;
	cs_item *csinfo_table = csinfo->csinfo_table;

	for (cs_index = 0; cs_index < totalchips; cs_index++) {
		cs_id = csinfo_table[cs_index].id;
		ret = nand_set_features(base, cs_id, rbinfo, cinfo);
		if (ret)
			break;
	}

	return ret;
}

int wait_rb_timeout(int cs_index, int timeout)
{
	int ret = SUCCESS;

	cs_item *csitem = &(nddata->csinfo->csinfo_table[cs_index]);

	ret = __wait_rb_timeout(csitem->rbitem, timeout);
	if (ret < 0)
		ret = TIMEOUT;
	else
		ret = SUCCESS;

	return ret;
}

int nand_api_init(struct nand_api_osdependent *private)
{
	int ret;

	os_clib_init(&(private->clib));
	//ndd_dump_status();

	__wait_rb_timeout = private->wait_rb_timeout;
	__try_wait_rb = private->try_wait_rb;
	__wp_enable = private->wp_enable;
	__wp_disable = private->wp_disable;
	__get_nand_flash = private->get_nand_flash;

	nddata = ndd_alloc(sizeof(nand_data));
	if (!nddata)
		RETURN_ERR(ENAND, "ndd_alloc memory for nddata error");

	nddata->eccsize = DEFAULT_ECCSIZE;
	nddata->spl_eccsize = SPL_ECCSIZE;
	nddata->gpio_wp = private->gpio_wp;

	nddata->rbinfo = get_rbinfo(private);
	if (!nddata->rbinfo)
		GOTO_ERR(get_rbinfo);

	nddata->base = get_base(private);
	if (!nddata->base)
		GOTO_ERR(get_base);

	ndd_debug("\nget nand chip info:\n");
	nddata->cinfo = get_chip_info(private, &(nddata->base->nfi), nddata->rbinfo);
	if (!nddata->cinfo)
		GOTO_ERR(get_cinfo);

	ndd_debug("\nget csinfo:\n");
	nddata->csinfo = get_csinfo(&(nddata->base->nfi), nddata->rbinfo);
	if (!nddata->csinfo)
		GOTO_ERR(get_csinfo);
	nddata->wait_rb = wait_rb_timeout;

	ndd_debug("\nnand flash setup:\n");
	ret = nandflash_setup(&(nddata->base->nfi), nddata->csinfo, nddata->rbinfo, nddata->cinfo);
	if (ret)
		ndd_print(NDD_WARNING, "WARNING: setup_nandflash faild!\n");

	ndd_debug("\nget ndpartition:\n");
	nddata->ptinfo = get_ptinfo(nddata->cinfo, nddata->csinfo->totalchips,
				    nddata->rbinfo->totalrbs, private->plat_ptinfo);
	if (!nddata->ptinfo)
		GOTO_ERR(get_ptinfo);

	ndd_debug("\nnand ops init:\n");
	nddata->ops_context = nandops_init(nddata);
	if (!nddata->ops_context)
		GOTO_ERR(ops_init);

	ndd_print(NDD_DEBUG,"\nregister NandManger:\n");
	Register_NandDriver(&nand_interface);

	ndd_debug("nand driver init ok!\n");
	return 0;

ERR_LABLE(ops_init):
	free_ptinfo(nddata->ptinfo);
ERR_LABLE(get_ptinfo):
//ERR_LABLE(setup_nandflash):
	free_csinfo(nddata->csinfo);
ERR_LABLE(get_csinfo):
	free_chip_info(nddata->cinfo);
ERR_LABLE(get_cinfo):
ERR_LABLE(get_base):
ERR_LABLE(get_rbinfo):
	ndd_free(nddata);

	return -1;
}

int nand_api_suspend(void)
{
	int ret;

	ret = nandops_suspend(nddata->ops_context);
	if (ret)
		RETURN_ERR(ret, "nand ops suspend error!");

	ret = nand_io_suspend();
	if (ret)
		RETURN_ERR(ret, "nand io suspend error!");

	ret = nand_bch_suspend();
	if (ret)
		RETURN_ERR(ret, "nand bch suspend error!");

	return 0;
}

int nand_api_resume(void)
{
	int ret;

	ret = nand_io_resume();
	if (ret)
		RETURN_ERR(ret, "nand io resume error!");

	ret = nand_bch_resume();
	if (ret)
		RETURN_ERR(ret, "nand bch resume error!");

	ret = nandops_resume(nddata->ops_context);
	if (ret)
		RETURN_ERR(ret, "nand ops resume error!");

	return 0;
}