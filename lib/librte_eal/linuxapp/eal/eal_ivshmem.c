/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef RTE_LIBRTE_IVSHMEM /* hide it from coverage */

#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <string.h>
#include <sys/queue.h>
#include <libudev.h>
#include <signal.h>
#include <fcntl.h>

#include <rte_log.h>
#include <rte_pci.h>
#include <rte_memory.h>
#include <rte_eal.h>
#include <rte_eal_memconfig.h>
#include <rte_string_fns.h>
#include <rte_errno.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <rte_common.h>
#include <rte_ivshmem.h>

#include "eal_internal_cfg.h"
#include "eal_private.h"

#define RTE_LIBRTE_IVSHMEM_DEBUG

#define PCI_VENDOR_ID_IVSHMEM 0x1AF4
#define PCI_DEVICE_ID_IVSHMEM 0x1110
#define PCI_ID_IVSHMEM "1AF4:1110"

#define IVSHMEM_MAGIC 0x0BADC0DE

#define IVSHMEM_RESOURCE_PATH "/sys/bus/pci/devices/%04x:%02x:%02x.%x/resource2"
#define IVSHMEM_CONFIG_PATH "/var/run/.%s_ivshmem_config"

#define PHYS 0x1
#define VIRT 0x2
#define IOREMAP 0x4
#define FULL (PHYS|VIRT|IOREMAP)

#define METADATA_SIZE_ALIGNED \
	(RTE_ALIGN_CEIL(sizeof(struct rte_ivshmem_metadata),pagesz))

#define CONTAINS(x,y)\
	(((y).addr_64 >= (x).addr_64) && ((y).addr_64 < (x).addr_64 + (x).len))

#define DIM(x) (sizeof(x)/sizeof(x[0]))

struct ivshmem_pci_device {
	char path[PATH_MAX];
	phys_addr_t ioremap_addr;
};

/* data type to store in config */
struct ivshmem_segment {
	struct rte_ivshmem_metadata_entry entry;
	uint64_t align;
	char path[PATH_MAX];
	/* 1 if already considered in the mapping process */
	uint8_t processed;
};
struct ivshmem_shared_config {
	struct ivshmem_segment segment[RTE_MAX_MEMSEG];
	uint32_t segment_idx;
	struct ivshmem_pci_device pci_devs[RTE_LIBRTE_IVSHMEM_MAX_PCI_DEVS];
	uint32_t pci_devs_idx;
};
static struct ivshmem_shared_config * ivshmem_config = NULL;
static int memseg_idx;
static int pagesz;

static struct udev * udev = NULL;
static struct udev_monitor * udev_monitor = NULL;

/* Tailq heads to add rings to */
TAILQ_HEAD(rte_ring_list, rte_tailq_entry);

/*
 * Utility functions
 */

static int
is_ivshmem_device(struct rte_pci_device * dev)
{
	return (dev->id.vendor_id == PCI_VENDOR_ID_IVSHMEM
			&& dev->id.device_id == PCI_DEVICE_ID_IVSHMEM);
}

static void *
map_metadata(int fd, uint64_t len)
{
	size_t metadata_len = sizeof(struct rte_ivshmem_metadata);
	size_t aligned_len = METADATA_SIZE_ALIGNED;

	return mmap(NULL, metadata_len, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, len - aligned_len);
}

static void
unmap_metadata(void * ptr)
{
	munmap(ptr, sizeof(struct rte_ivshmem_metadata));
}

static int
has_ivshmem_metadata(int fd, uint64_t len)
{
	struct rte_ivshmem_metadata metadata;
	void * ptr;

	ptr = map_metadata(fd, len);

	if (ptr == MAP_FAILED)
		return -1;

	metadata = *(struct rte_ivshmem_metadata*) (ptr);

	unmap_metadata(ptr);

	return metadata.magic_number == IVSHMEM_MAGIC;
}

static void
remove_segment(struct ivshmem_segment * ms, int len, int idx)
{
	int i;

	for (i = idx; i < len - 1; i++)
		memcpy(&ms[i], &ms[i+1], sizeof(struct ivshmem_segment));
	memset(&ms[len-1], 0, sizeof(struct ivshmem_segment));
}

static int
overlap(const struct rte_memzone * mz1, const struct rte_memzone * mz2)
{
	uint64_t start1, end1, start2, end2;
	uint64_t p_start1, p_end1, p_start2, p_end2;
	uint64_t i_start1, i_end1, i_start2, i_end2;
	int result = 0;

	/* gather virtual addresses */
	start1 = mz1->addr_64;
	end1 = mz1->addr_64 + mz1->len;
	start2 = mz2->addr_64;
	end2 = mz2->addr_64 + mz2->len;

	/* gather physical addresses */
	p_start1 = mz1->phys_addr;
	p_end1 = mz1->phys_addr + mz1->len;
	p_start2 = mz2->phys_addr;
	p_end2 = mz2->phys_addr + mz2->len;

	/* gather ioremap addresses */
	i_start1 = mz1->ioremap_addr;
	i_end1 = mz1->ioremap_addr + mz1->len;
	i_start2 = mz2->ioremap_addr;
	i_end2 = mz2->ioremap_addr + mz2->len;

	/* check for overlap in virtual addresses */
	if (start1 >= start2 && start1 < end2)
		result |= VIRT;
	if (start2 >= start1 && start2 < end1)
		result |= VIRT;

	/* check for overlap in physical addresses */
	if (p_start1 >= p_start2 && p_start1 < p_end2)
		result |= PHYS;
	if (p_start2 >= p_start1 && p_start2 < p_end1)
		result |= PHYS;

	/* check for overlap in ioremap addresses */
	if (i_start1 >= i_start2 && i_start1 < i_end2)
		result |= IOREMAP;
	if (i_start2 >= i_start1 && i_start2 < i_end1)
		result |= IOREMAP;

	return result;
}

static int
adjacent(const struct rte_memzone * mz1, const struct rte_memzone * mz2)
{
	uint64_t start1, end1, start2, end2;
	uint64_t p_start1, p_end1, p_start2, p_end2;
	uint64_t i_start1, i_end1, i_start2, i_end2;
	int result = 0;

	/* gather virtual addresses */
	start1 = mz1->addr_64;
	end1 = mz1->addr_64 + mz1->len;
	start2 = mz2->addr_64;
	end2 = mz2->addr_64 + mz2->len;

	/* gather physical addresses */
	p_start1 = mz1->phys_addr;
	p_end1 = mz1->phys_addr + mz1->len;
	p_start2 = mz2->phys_addr;
	p_end2 = mz2->phys_addr + mz2->len;

	/* gather ioremap addresses */
	i_start1 = mz1->ioremap_addr;
	i_end1 = mz1->ioremap_addr + mz1->len;
	i_start2 = mz2->ioremap_addr;
	i_end2 = mz2->ioremap_addr + mz2->len;

	/* check if segments are virtually adjacent */
	if (start1 == end2)
		result |= VIRT;
	if (start2 == end1)
		result |= VIRT;

	/* check if segments are physically adjacent */
	if (p_start1 == p_end2)
		result |= PHYS;
	if (p_start2 == p_end1)
		result |= PHYS;

	/* check if segments are ioremap-adjacent */
	if (i_start1 == i_end2)
		result |= IOREMAP;
	if (i_start2 == i_end1)
		result |= IOREMAP;

	return result;
}

static int
has_adjacent_segments(struct ivshmem_segment * ms, int len)
{
	int i, j, a;

	for (i = 0; i < len; i++)
		for (j = i + 1; j < len; j++) {
			a = adjacent(&ms[i].entry.mz, &ms[j].entry.mz);

			/* check if segments are adjacent virtually and/or physically but
			 * not ioremap (since that would indicate that they are from
			 * different PCI devices and thus don't need to be concatenated.
			 */
			if ((a & (VIRT|PHYS)) > 0 && (a & IOREMAP) == 0)
				return 1;
		}
	return 0;
}

static int
has_overlapping_segments(struct ivshmem_segment * ms, int len)
{
	int i, j;

	for (i = 0; i < len; i++)
		for (j = i + 1; j < len; j++)
			if (overlap(&ms[i].entry.mz, &ms[j].entry.mz))
				return 1;
	return 0;
}

static int
seg_compare(const void * a, const void * b)
{
	const struct ivshmem_segment * s1 = (const struct ivshmem_segment*) a;
	const struct ivshmem_segment * s2 = (const struct ivshmem_segment*) b;

	/* move unallocated zones to the end */
	if (s1->entry.mz.addr == NULL && s2->entry.mz.addr == NULL)
		return 0;
	if (s1->entry.mz.addr == 0)
		return 1;
	if (s2->entry.mz.addr == 0)
		return -1;

	return s1->entry.mz.phys_addr > s2->entry.mz.phys_addr;
}

#ifdef RTE_LIBRTE_IVSHMEM_DEBUG
static void
entry_dump(struct rte_ivshmem_metadata_entry *e)
{
	RTE_LOG(DEBUG, EAL, "\tvirt: %p-%p\n", e->mz.addr,
			RTE_PTR_ADD(e->mz.addr, e->mz.len));
	RTE_LOG(DEBUG, EAL, "\tphys: 0x%" PRIx64 "-0x%" PRIx64 "\n",
			e->mz.phys_addr,
			e->mz.phys_addr + e->mz.len);
	RTE_LOG(DEBUG, EAL, "\tio: 0x%" PRIx64 "-0x%" PRIx64 "\n",
			e->mz.ioremap_addr,
			e->mz.ioremap_addr + e->mz.len);
	RTE_LOG(DEBUG, EAL, "\tlen: 0x%" PRIx64 "\n", e->mz.len);
	RTE_LOG(DEBUG, EAL, "\toff: 0x%" PRIx64 "\n", e->offset);
}
#endif


static void
unmap_memseg(struct ivshmem_segment * seg)
{
	uint64_t align, len, addr;
	int retval;
	RTE_LOG(DEBUG, EAL, "unmap_memseg: \n");
	RTE_LOG(DEBUG, EAL, "addr: 0x%" PRIx64 "len: 0x%" PRIx64 "\n", 
		seg->entry.mz.addr_64, seg->entry.mz.len);
	
	/* work out alignments */
	align = seg->entry.mz.addr_64 - 
		RTE_ALIGN_FLOOR(seg->entry.mz.addr_64, 0x1000);
	len = RTE_ALIGN_CEIL(seg->entry.mz.len + align, 0x1000);
	
	addr = seg->entry.mz.addr_64 - align;
	
	RTE_LOG(DEBUG, EAL, "addr: 0x%" PRIx64 "len: 0x%" PRIx64 "\n", 
		addr, len);
	
	retval = munmap((void *) addr, len);	///XXX: Be carefull with the casting
	if(retval != 0)
		RTE_LOG(DEBUG, EAL, "Error munmap: (%s)\n", strerror(errno));
}

/*
 * Actual useful code
 */

/* read through metadata mapped from the IVSHMEM device */
static int
read_metadata(char * path, int path_len, int fd, uint64_t flen)
{
	struct rte_ivshmem_metadata metadata;
	struct rte_ivshmem_metadata_entry * entry;
	int idx, i;
	void * ptr;
	int unmap = 0;	/* 1 is the metadata file request unmapping */

	ptr = map_metadata(fd, flen);

	if (ptr == MAP_FAILED)
		return -1;

	metadata = *(struct rte_ivshmem_metadata*) (ptr);

	unmap_metadata(ptr);

	RTE_LOG(DEBUG, EAL, "Parsing metadata for \"%s\"\n", metadata.name);

	if (strncmp(metadata.name, IVSHMEM_REMAP_PREFIX,
		sizeof(IVSHMEM_REMAP_PREFIX) - 1) == 0)
	{
		RTE_LOG(DEBUG, EAL, "Metadata file request unmap\n");
		unmap = 1;
	}

	idx = ivshmem_config->segment_idx;

	for (i = 0; i < RTE_LIBRTE_IVSHMEM_MAX_ENTRIES &&
		idx <= RTE_MAX_MEMSEG; i++) {

		if (idx == RTE_MAX_MEMSEG) {
			RTE_LOG(ERR, EAL, "Not enough memory segments!\n");
			return -1;
		}

		entry = &metadata.entry[i];

		/* stop on uninitialized memzone */
		if (entry->mz.len == 0)
			break;

		/* copy metadata entry */
		memcpy(&ivshmem_config->segment[idx].entry, entry,
				sizeof(struct rte_ivshmem_metadata_entry));

		/* copy path */
		snprintf(ivshmem_config->segment[idx].path, path_len, "%s", path);

		if(unmap)
		{
			unmap_memseg(&ivshmem_config->segment[idx]);
		}

		idx++;
	}
	ivshmem_config->segment_idx = idx;

	return 0;
}

/* check through each segment and look for adjacent or overlapping ones. */
static int
cleanup_segments(struct ivshmem_segment * ms, int tbl_len)
{
	struct ivshmem_segment * s, * tmp;
	int i, j, concat, seg_adjacent, seg_overlapping;
	uint64_t start1, start2, end1, end2, p_start1, p_start2, i_start1, i_start2;

	qsort(ms, tbl_len, sizeof(struct ivshmem_segment),
				seg_compare);

	while (has_overlapping_segments(ms, tbl_len) ||
			has_adjacent_segments(ms, tbl_len)) {

		for (i = 0; i < tbl_len; i++) {
			s = &ms[i];

			concat = 0;

			for (j = i + 1; j < tbl_len; j++) {
				tmp = &ms[j];

				/* check if this segment is overlapping with existing segment,
				 * or is adjacent to existing segment */
				seg_overlapping = overlap(&s->entry.mz, &tmp->entry.mz);
				seg_adjacent = adjacent(&s->entry.mz, &tmp->entry.mz);

				/* check if segments fully overlap or are fully adjacent */
				if ((seg_adjacent == FULL) || (seg_overlapping == FULL)) {

#ifdef RTE_LIBRTE_IVSHMEM_DEBUG
					RTE_LOG(DEBUG, EAL, "Concatenating segments\n");
					RTE_LOG(DEBUG, EAL, "Segment %i:\n", i);
					entry_dump(&s->entry);
					RTE_LOG(DEBUG, EAL, "Segment %i:\n", j);
					entry_dump(&tmp->entry);
#endif

					start1 = s->entry.mz.addr_64;
					start2 = tmp->entry.mz.addr_64;
					p_start1 = s->entry.mz.phys_addr;
					p_start2 = tmp->entry.mz.phys_addr;
					i_start1 = s->entry.mz.ioremap_addr;
					i_start2 = tmp->entry.mz.ioremap_addr;
					end1 = s->entry.mz.addr_64 + s->entry.mz.len;
					end2 = tmp->entry.mz.addr_64 + tmp->entry.mz.len;

					/* settle for minimum start address and maximum length */
					s->entry.mz.addr_64 = RTE_MIN(start1, start2);
					s->entry.mz.phys_addr = RTE_MIN(p_start1, p_start2);
					s->entry.mz.ioremap_addr = RTE_MIN(i_start1, i_start2);
					s->entry.offset = RTE_MIN(s->entry.offset, tmp->entry.offset);
					s->entry.mz.len = RTE_MAX(end1, end2) - s->entry.mz.addr_64;
					concat = 1;

#ifdef RTE_LIBRTE_IVSHMEM_DEBUG
					RTE_LOG(DEBUG, EAL, "Resulting segment:\n");
					entry_dump(&s->entry);

#endif
				}
				/* if segments not fully overlap, we have an error condition.
				 * adjacent segments can coexist.
				 */
				else if (seg_overlapping > 0) {
					RTE_LOG(ERR, EAL, "Segments %i and %i overlap!\n", i, j);
#ifdef RTE_LIBRTE_IVSHMEM_DEBUG
					RTE_LOG(DEBUG, EAL, "Segment %i:\n", i);
					entry_dump(&s->entry);
					RTE_LOG(DEBUG, EAL, "Segment %i:\n", j);
					entry_dump(&tmp->entry);
#endif
					return -1;
				}
				if (concat)
					break;
			}
			/* if we concatenated, remove segment at j */
			if (concat) {
				remove_segment(ms, tbl_len, j);
				tbl_len--;
				break;
			}
		}
	}

	return tbl_len;
}

static int
create_shared_config(void)
{
	char path[PATH_MAX];
	int fd;

	/* build ivshmem config file path */
	snprintf(path, sizeof(path), IVSHMEM_CONFIG_PATH,
			internal_config.hugefile_prefix);

	fd = open(path, O_CREAT | O_RDWR, 0600);

	if (fd < 0) {
		RTE_LOG(ERR, EAL, "Could not open %s: %s\n", path, strerror(errno));
		return -1;
	}

	/* try ex-locking first - if the file is locked, we have a problem */
	if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
		RTE_LOG(ERR, EAL, "Locking %s failed: %s\n", path, strerror(errno));
		close(fd);
		return -1;
	}

	if (ftruncate(fd, sizeof(struct ivshmem_shared_config)) < 0) {
		RTE_LOG(ERR, EAL, "ftruncate failed: %s\n", strerror(errno));
		return -1;
	}

	ivshmem_config = mmap(NULL, sizeof(struct ivshmem_shared_config),
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if (ivshmem_config == MAP_FAILED)
		return -1;

	memset(ivshmem_config, 0, sizeof(struct ivshmem_shared_config));

	/* change the exclusive lock we got earlier to a shared lock */
	if (flock(fd, LOCK_SH | LOCK_NB) == -1) {
		RTE_LOG(ERR, EAL, "Locking %s failed: %s \n", path, strerror(errno));
		return -1;
	}

	close(fd);

	return 0;
}

/* open shared config file and, if present, map the config.
 * having no config file is not an error condition, as we later check if
 * ivshmem_config is NULL (if it is, that means nothing was mapped). */
static int
open_shared_config(void)
{
	char path[PATH_MAX];
	int fd;

	/* build ivshmem config file path */
	snprintf(path, sizeof(path), IVSHMEM_CONFIG_PATH,
			internal_config.hugefile_prefix);

	fd = open(path, O_RDONLY);

	/* if the file doesn't exist, just return success */
	if (fd < 0 && errno == ENOENT)
		return 0;
	/* else we have an error condition */
	else if (fd < 0) {
		RTE_LOG(ERR, EAL, "Could not open %s: %s\n",
				path, strerror(errno));
		return -1;
	}

	/* try ex-locking first - if the lock *does* succeed, this means it's a
	 * stray config file, so it should be deleted.
	 */
	if (flock(fd, LOCK_EX | LOCK_NB) != -1) {

		/* if we can't remove the file, something is wrong */
		if (unlink(path) < 0) {
			RTE_LOG(ERR, EAL, "Could not remove %s: %s\n", path,
					strerror(errno));
			return -1;
		}

		/* release the lock */
		flock(fd, LOCK_UN);
		close(fd);

		/* return success as having a stray config file is equivalent to not
		 * having config file at all.
		 */
		return 0;
	}

	ivshmem_config = mmap(NULL, sizeof(struct ivshmem_shared_config),
			PROT_READ, MAP_SHARED, fd, 0);

	if (ivshmem_config == MAP_FAILED)
		return -1;

	/* place a shared lock on config file */
	if (flock(fd, LOCK_SH | LOCK_NB) == -1) {
		RTE_LOG(ERR, EAL, "Locking %s failed: %s \n", path, strerror(errno));
		return -1;
	}

	close(fd);

	return 0;
}

static int
map_one_segment(struct ivshmem_segment * seg, struct rte_memseg * ms, 
		int fd_zero) 
{
	void * base_addr;
	int fd;
	
	ms->addr_64 = seg->entry.mz.addr_64;
	ms->hugepage_sz = seg->entry.mz.hugepage_sz;
	ms->len = seg->entry.mz.len;
	ms->nchannel = rte_memory_get_nchannel();
	ms->nrank = rte_memory_get_nrank();
	ms->phys_addr = seg->entry.mz.phys_addr;
	ms->ioremap_addr = seg->entry.mz.ioremap_addr;
	ms->socket_id = seg->entry.mz.socket_id;

	base_addr = mmap(ms->addr, ms->len,
			PROT_READ | PROT_WRITE, MAP_PRIVATE, fd_zero, 0);

	if (base_addr == MAP_FAILED || base_addr != ms->addr) {
		RTE_LOG(ERR, EAL, "Cannot map /dev/zero!\n");
		return -1;
	}

	fd = open(seg->path, O_RDWR);

	if (fd < 0) {
		RTE_LOG(ERR, EAL, "Cannot open %s: %s\n", seg->path,
				strerror(errno));
		return -1;
	}

	munmap(ms->addr, ms->len);

	RTE_LOG(DEBUG, EAL, "Mapping Segment: \n");
	RTE_LOG(DEBUG, EAL, "\tAddr: %p\n", ms->addr);
	RTE_LOG(DEBUG, EAL, "\tLen: 0x%" PRIx64 "\n", ms->len);
	RTE_LOG(DEBUG, EAL, "\tOff: 0x%" PRIx64 "\n", seg->entry.offset);
	RTE_LOG(DEBUG, EAL, "\tAlign: 0x%" PRIx64 "\n", seg->align);

	base_addr = mmap(ms->addr, ms->len,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			seg->entry.offset);


	if (base_addr == MAP_FAILED || base_addr != ms->addr) {
		RTE_LOG(ERR, EAL, "Cannot map segment into memory: "
				"expected %p got %p (%s)\n", ms->addr, base_addr,
				strerror(errno));
		return -1;
	}

	close(fd);
	
	RTE_LOG(DEBUG, EAL, "Memory segment mapped: %p (len %" PRIx64 ") at "
			"offset 0x%" PRIx64 "\n",
			ms->addr, ms->len, seg->entry.offset);

	/* put the pointers back into their real positions using original
	 * alignment */
	ms->addr_64 += seg->align;
	ms->phys_addr += seg->align;
	ms->ioremap_addr += seg->align;
	ms->len -= seg->align;
	
	return 0;
}

/*
 * This function does the following:
 *
 * 1) Builds a table of ivshmem_segments with proper offset alignment
 * 2) Cleans up that table so that we don't have any overlapping or adjacent
 *    memory segments
 * 3) Creates memsegs from this table and maps them into memory.
 */
static inline int
map_all_segments(void)
{
	struct ivshmem_segment ms_tbl[RTE_MAX_MEMSEG];
	struct ivshmem_pci_device * pci_dev;
	struct rte_mem_config * mcfg;
	struct ivshmem_segment * seg;
	int fd_zero;
	unsigned i, j, k;
	struct rte_memzone mz;
	struct rte_memseg ms;
	uint64_t align, len;
	phys_addr_t ioremap_addr;

	ioremap_addr = 0;

	memset(ms_tbl, 0, sizeof(ms_tbl));
	memset(&mz, 0, sizeof(struct rte_memzone));
	memset(&ms, 0, sizeof(struct rte_memseg));

	/* first, build a table of memsegs to map, to avoid failed mmaps due to
	 * overlaps
	 */
	for (i = 0, k = 0; i < ivshmem_config->segment_idx && i <= RTE_MAX_MEMSEG; i++) {
		if (i == RTE_MAX_MEMSEG) {
			RTE_LOG(ERR, EAL, "Too many segments requested!\n");
			return -1;
		}

		seg = &ivshmem_config->segment[i];

		if(seg->processed)
			continue;

		/* copy segment to table */
		memcpy(&ms_tbl[k], seg, sizeof(struct ivshmem_segment));

		/* find ioremap addr */
		for (j = 0; j < DIM(ivshmem_config->pci_devs); j++) {
			pci_dev = &ivshmem_config->pci_devs[j];
			if (!strncmp(pci_dev->path, seg->path, sizeof(pci_dev->path))) {
				ioremap_addr = pci_dev->ioremap_addr;
				break;
			}
		}
		if (ioremap_addr == 0) {
			RTE_LOG(ERR, EAL, "Cannot find ioremap addr!\n");
			return -1;
		}

		/* work out alignments */
		align = seg->entry.mz.addr_64 -
				RTE_ALIGN_FLOOR(seg->entry.mz.addr_64, 0x1000);
		len = RTE_ALIGN_CEIL(seg->entry.mz.len + align, 0x1000);

		/* save original alignments */
		ms_tbl[k].align = align;

		/* create a memory zone */
		mz.addr_64 = seg->entry.mz.addr_64 - align;
		mz.len = len;
		mz.hugepage_sz = seg->entry.mz.hugepage_sz;
		mz.phys_addr = seg->entry.mz.phys_addr - align;

		/* find true physical address */
		mz.ioremap_addr = ioremap_addr + seg->entry.offset - align;

		ms_tbl[k].entry.offset = seg->entry.offset - align;

		memcpy(&ms_tbl[k].entry.mz, &mz, sizeof(struct rte_memzone));

		/* the segment was already processed */
		seg->processed = 1;
		k++;
	}

	/* clean up the segments */
	memseg_idx = cleanup_segments(ms_tbl, k);

	if (memseg_idx < 0)
		return -1;

	mcfg = rte_eal_get_configuration()->mem_config;

	fd_zero = open("/dev/zero", O_RDWR);

	if (fd_zero < 0) {
		RTE_LOG(ERR, EAL, "Cannot open /dev/zero: %s\n", strerror(errno));
		return -1;
	}

	/* look for the last free memseg */
	for (j = 0; j <= RTE_MAX_MEMSEG; j++)
	{
		if(i == RTE_MAX_MEMSEG)
		{
			RTE_LOG(ERR, EAL, "Not enough memory segments!\n");
			return -1;
		}

		if (mcfg->memseg[j].addr == NULL)
			break;
	}

	/* create memsegs and put them into DPDK memory */
	for (i = 0; i < (unsigned) memseg_idx; i++) {

		seg = &ms_tbl[i];

		map_one_segment(seg, &ms, fd_zero);

		/* copy memseg starting from the last free one */
		memcpy(&mcfg->memseg[j + i], &ms,
				sizeof(struct rte_memseg));

		RTE_LOG(DEBUG, EAL, "IVSHMEM segment found, size: 0x%lx\n",
				ms.len);
	}

	return 0;
}

/* this happens at a later stage, after general EAL memory initialization */
int
rte_eal_ivshmem_obj_init(void)
{
	struct rte_ring_list* ring_list = NULL;
	struct rte_mem_config * mcfg;
	struct ivshmem_segment * seg;
	struct rte_memzone * mz;
	struct rte_ring * r;
	struct rte_tailq_entry *te;
	unsigned i, ms, idx;
	uint64_t offset;

	/* secondary process would not need any object discovery - it'll all
	 * already be in shared config */
	if (rte_eal_process_type() != RTE_PROC_PRIMARY || ivshmem_config == NULL)
		return 0;

	/* check that we have an initialised ring tail queue */
	ring_list = RTE_TAILQ_LOOKUP(RTE_TAILQ_RING_NAME, rte_ring_list);
	if (ring_list == NULL) {
		RTE_LOG(ERR, EAL, "No rte_ring tailq found!\n");
		return -1;
	}

	mcfg = rte_eal_get_configuration()->mem_config;

	/* create memzones */
	for (i = 0; i < ivshmem_config->segment_idx && i <= RTE_MAX_MEMZONE; i++) {

		seg = &ivshmem_config->segment[i];

		/* avoid duplicating memzones */
		if(rte_memzone_lookup(seg->entry.mz.name) != NULL)
			continue;

		/* add memzone */
		if (mcfg->memzone_cnt == RTE_MAX_MEMZONE) {
			RTE_LOG(ERR, EAL, "No more memory zones available!\n");
			return -1;
		}

		idx = mcfg->memzone_cnt;

		RTE_LOG(DEBUG, EAL, "Found memzone: '%s' at %p (len 0x%" PRIx64 ")\n",
				seg->entry.mz.name, seg->entry.mz.addr, seg->entry.mz.len);

		memcpy(&mcfg->memzone[idx], &seg->entry.mz,
				sizeof(struct rte_memzone));

		/* find ioremap address */
		for (ms = 0; ms <= RTE_MAX_MEMSEG; ms++) {
			if (ms == RTE_MAX_MEMSEG) {
				RTE_LOG(ERR, EAL, "Physical address of segment not found!\n");
				return -1;
			}
			if (CONTAINS(mcfg->memseg[ms], mcfg->memzone[idx])) {
				offset = mcfg->memzone[idx].addr_64 -
								mcfg->memseg[ms].addr_64;
				mcfg->memzone[idx].ioremap_addr = mcfg->memseg[ms].ioremap_addr +
						offset;
				break;
			}
		}

		mcfg->memzone_cnt++;
	}

	/* find rings */
	for (i = 0; i < mcfg->memzone_cnt; i++) {
		mz = &mcfg->memzone[i];

		/* avoid looking in memzones that are not ivshmem */
		if(mz->ioremap_addr == 0)
			continue;

		/* check if memzone has a ring prefix */
		if (strncmp(mz->name, RTE_RING_MZ_PREFIX,
				sizeof(RTE_RING_MZ_PREFIX) - 1) != 0)
			continue;

		r = (struct rte_ring*) (mz->addr_64);

		/* avoid inserting duplicated rings */
		if(rte_ring_lookup(r->name) != NULL)
			continue;

		rte_rwlock_write_lock(RTE_EAL_TAILQ_RWLOCK);

		te = rte_zmalloc("RING_TAILQ_ENTRY", sizeof(*te), 0);
		if (te == NULL) {
			RTE_LOG(ERR, EAL, "Cannot allocate ring tailq entry!\n");
			return -1;
		}

		te->data = (void *) r;

		TAILQ_INSERT_TAIL(ring_list, te, next);

		rte_rwlock_write_unlock(RTE_EAL_TAILQ_RWLOCK);

		RTE_LOG(DEBUG, EAL, "Found ring: '%s' at %p\n", r->name, mz->addr);
	}


#ifdef RTE_LIBRTE_IVSHMEM_DEBUG
	rte_memzone_dump(stdout);
	rte_ring_list_dump(stdout);
#endif

	return 0;
}

static int
pci_dev_already_saved(char * path)
{
	uint32_t i;

	if(!ivshmem_config)
		return 0;	///XXX: what could be a better error message?

	/* look for all the registered devices comparing with it */
	for(i = 0; i < ivshmem_config->pci_devs_idx; i++)
		if(!strncmp(path, ivshmem_config->pci_devs[i].path, 
			sizeof(ivshmem_config->pci_devs[i].path)))
			return 1;

	return 0;
}

static int
ivshmem_read_devices(void)
{
	struct rte_pci_device * dev;
	struct rte_pci_resource * res;
	int fd, ret;
	char path[PATH_MAX];

	/* initialize everything to 0 */
	memset(path, 0, sizeof(path));

	pagesz = getpagesize();

	RTE_LOG(DEBUG, EAL, "Searching for IVSHMEM devices...\n");
	
	TAILQ_FOREACH(dev, &pci_device_list, next) {

		if (is_ivshmem_device(dev)) {

			/* IVSHMEM memory is always on BAR2 */
			res = &dev->mem_resource[2];

			/* if we don't have a BAR2 */
			if (res->len == 0)
				continue;

			/* construct pci device path */
			snprintf(path, sizeof(path), IVSHMEM_RESOURCE_PATH,
					dev->addr.domain, dev->addr.bus, dev->addr.devid,
					dev->addr.function);

			/* if it is already saved */
			if(pci_dev_already_saved(path))
			{
				RTE_LOG(DEBUG, EAL, "Skipping existing IVSHMEM device\n");
				continue;
			}

			/* try to find memseg */
			fd = open(path, O_RDWR);
			if (fd < 0) {
				RTE_LOG(ERR, EAL, "Could not open %s\n", path);
				return -1;
			}

			/* check if it's a DPDK IVSHMEM device */
			ret = has_ivshmem_metadata(fd, res->len);

			/* is DPDK device */
			if (ret == 1) {

				/* config file creation is deferred until the first
				 * DPDK device is found. then, it has to be created
				 * only once. */
				if (ivshmem_config == NULL &&
						create_shared_config() < 0) {
					RTE_LOG(ERR, EAL, "Could not create IVSHMEM config!\n");
					close(fd);
					return -1;
				}

				if (read_metadata(path, sizeof(path), fd, res->len) < 0) {
					RTE_LOG(ERR, EAL, "Could not read metadata from"
							" device %02x:%02x.%x!\n", dev->addr.bus,
							dev->addr.devid, dev->addr.function);
					close(fd);
					return -1;
				}

				if (ivshmem_config->pci_devs_idx == RTE_LIBRTE_IVSHMEM_MAX_PCI_DEVS) {
					RTE_LOG(WARNING, EAL,
							"IVSHMEM PCI device limit exceeded. Increase "
							"CONFIG_RTE_LIBRTE_IVSHMEM_MAX_PCI_DEVS  in "
							"your config file.\n");
					break;
				}

				RTE_LOG(INFO, EAL, "Found IVSHMEM device %02x:%02x.%x\n",
						dev->addr.bus, dev->addr.devid, dev->addr.function);

				ivshmem_config->pci_devs[ivshmem_config->pci_devs_idx].ioremap_addr = res->phys_addr;
				snprintf(ivshmem_config->pci_devs[ivshmem_config->pci_devs_idx].path,
						sizeof(ivshmem_config->pci_devs[ivshmem_config->pci_devs_idx].path),
						"%s", path);

				ivshmem_config->pci_devs_idx++;
			}
			/* failed to read */
			else if (ret < 0) {
				RTE_LOG(ERR, EAL, "Could not read IVSHMEM device: %s\n",
						strerror(errno));
				close(fd);
				return -1;
			}
			/* not a DPDK device */
			else
				RTE_LOG(DEBUG, EAL, "Skipping non-DPDK IVSHMEM device\n");

			/* close the BAR fd */
			close(fd);
		}
	}

	return 0;
}

static void
hotplug_handler(int d)
{
	const char *str;
	struct udev_device * dev;

	(void) d;

	if(udev_monitor == NULL)
	{
		RTE_LOG(ERR, EAL, "udev_monitor is NULL\n");
		return;
	}
	
	dev = udev_monitor_receive_device(udev_monitor);
	if(dev == NULL)
	{
		RTE_LOG(ERR, EAL, "dev is NULL\n");
		return;
	}
	str = udev_device_get_property_value(dev, "PCI_ID");
	
	if(str == NULL)
		return;
	
	if(!strcmp(str, PCI_ID_IVSHMEM))
	{
		///XXX: block signal.
		//RTE_LOG(DEBUG, EAL, "Ivshmem device: %s\n", udev_device_get_action(dev));
		str = udev_device_get_action(dev);
		if(!strcmp(str, "add"))
		{
			/* new ivshmem device  was found */
			if (rte_eal_pci_scan() < 0)
			{
				RTE_LOG(ERR, EAL, "Cannot scan PCI\n");	
				return; 
			}
			
			if (ivshmem_read_devices() < 0)
			{
				RTE_LOG(ERR, EAL, "Cannot init IVSHMEM\n");	
				return; 
			}
			
			if (map_all_segments() < 0)
			{
				RTE_LOG(ERR, EAL, "Cannot init IVSHMEM\n");	
				return; 
			}
			
			if (rte_eal_ivshmem_obj_init() < 0)
			{
				RTE_LOG(ERR, EAL, "Cannot init IVSHMEM OBJ\n");	
				return; 
			}
		}
	}
}

/*
 * enables or disables the hotplugging of ivshmem devices
 * if a device is hotppluged when hotppluging is disable then the 
 * request will be processed when hotpugging will be enable
 */
static void
set_hotplug_enable(int en)
{
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGIO);
	if(en)
		pthread_sigmask(SIG_UNBLOCK, &set, NULL);
	else
		pthread_sigmask(SIG_BLOCK, &set, NULL);
}

void rte_eal_ivshmem_enable_hotplug(void);

void rte_eal_ivshmem_enable_hotplug(void)
{
	return set_hotplug_enable(1);
}

void rte_eal_ivshmem_disable_hotplug(void);

void rte_eal_ivshmem_disable_hotplug(void)
{
	return set_hotplug_enable(0);
}

/* initialize ivshmem structures */
int rte_eal_ivshmem_init(void)
{
	int fd_mon;
	
	if (rte_eal_process_type() == RTE_PROC_SECONDARY) {
		if (open_shared_config() < 0) {
			RTE_LOG(ERR, EAL, "Could not open IVSHMEM config!\n");
			return -1;
		}
	}
	else {
		/* 
		 * install a udev_monitor to detect when new ivshmem device
		 * is connected or disconnedted (not supported yet)
		 */
		udev = udev_new();
		if(!udev)
		{
			RTE_LOG(ERR, EAL, "udev_new failed, ivshmem hotplug will"
				"be disabled! \n");
			goto read_devices;
		}

		udev_monitor = udev_monitor_new_from_netlink(udev, "udev");
		RTE_LOG(DEBUG, EAL, "udev_monitor: %p\n", udev_monitor);
		if(!udev_monitor)
		{
			RTE_LOG(ERR, EAL, "udev_monitor can not be instantiated,"
				"ivshmem hotplug will be disabled! \n");
			goto read_devices;
		}

		fd_mon = udev_monitor_get_fd(udev_monitor);		///Check error!
		
		if(udev_monitor_filter_add_match_subsystem_devtype(udev_monitor, 
			"pci", NULL) < 0)
		{
			RTE_LOG(ERR, EAL, "Error adding filter to udev_monitor.\n");
			goto read_devices;
		}

		if (udev_monitor_enable_receiving(udev_monitor) < 0) 
		{
			RTE_LOG(ERR, EAL, "Error enabling udev_monitor..\n");
			goto read_devices;
		}

		/* 
		 * setup a signal in a way that each time a device is connected
		 * the handler is called in an automatic way 
		 */
		signal(SIGIO, &hotplug_handler);
		if(fcntl(fd_mon, F_SETOWN, getpid()) != 0)
		{
			RTE_LOG(ERR, EAL, "Error fcntcl.\n");
			goto read_devices;
		}
			
		int oflags = fcntl(fd_mon, F_GETFL);
		if(fcntl(fd_mon, F_SETFL, oflags | FASYNC) != 0)
		{
			RTE_LOG(ERR, EAL, "Error fcntcl.\n");
			goto read_devices;
		}
		
read_devices:
		if(ivshmem_read_devices() < 0) {
			//RTE_LOG(ERR, EAL, "Could not read IVSHMEM devices!\n");
			return -1;
		}
	}

	if(ivshmem_config != NULL)
	{
		if (map_all_segments() < 0) {
			RTE_LOG(ERR, EAL, "Mapping IVSHMEM segments failed!\n");
			return -1;
		}
	} else {
		RTE_LOG(DEBUG, EAL, "No IVSHMEM configuration found! \n");
	}

	return 0;
}

#endif
