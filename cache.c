#include <stdio.h>
#include <math.h>

#include "cache.h"
#include "main.h"

/* cache configuration parameters */
static int cache_usize = DEFAULT_CACHE_SIZE;
static int cache_block_size = DEFAULT_CACHE_BLOCK_SIZE;
static int words_per_block = DEFAULT_CACHE_BLOCK_SIZE / WORD_SIZE;
static int cache_assoc = DEFAULT_CACHE_ASSOC;
static int cache_writeback = DEFAULT_CACHE_WRITEBACK;
static int cache_writealloc = DEFAULT_CACHE_WRITEALLOC;
static int num_core = DEFAULT_NUM_CORE;

/* cache model data structures */
/* max of 8 cores */
static cache mesi_cache[8];
static cache_stat mesi_cache_stat[8];

/************************************************************/
void set_cache_param(param, value)
  int param;
  int value;
{
  switch (param) {
  case NUM_CORE:
    num_core = value;
    break;
  case CACHE_PARAM_BLOCK_SIZE:
    cache_block_size = value;
    words_per_block = value / WORD_SIZE;
    break;
  case CACHE_PARAM_USIZE:
    cache_usize = value;
    break;
  case CACHE_PARAM_ASSOC:
    cache_assoc = value;
    break;
  default:
    printf("error set_cache_param: bad parameter value\n");
    exit(-1);
  }
}
/************************************************************/

/************************************************************/
void init_cache()
{
	/* initialize the cache, and cache statistics data structures */
	cache_line *line;
	int i, nontag_bits;
	cache *c1;
	
	for (i=0; i<num_core; i++) {
		c1 = &mesi_cache[i];
		c1->size = cache_usize;                                       /* cache size */
		c1->associativity = cache_assoc;                              /* cache associativity */
		c1->n_sets = (c1->size / cache_block_size) / cache_assoc;     /* number of cache sets */
		nontag_bits = LOG2(c1->n_sets) + LOG2(cache_block_size);
		c1->index_mask = (((2 << nontag_bits) - 1) >> LOG2(cache_block_size)) << LOG2(cache_block_size);/* mask to find cache index */
		c1->index_mask_offset = LOG2(cache_block_size);               /* number of zero bits in mask */
		c1->LRU_head = (Pcache_line *)malloc(sizeof(Pcache_line)*c1->n_sets);
		c1->LRU_tail = (Pcache_line *)malloc(sizeof(Pcache_line)*c1->n_sets);
		memset(c1->LRU_head, 0, sizeof(Pcache_line)*c1->n_sets);
		memset(c1->LRU_tail, 0, sizeof(Pcache_line)*c1->n_sets);
		c1->set_contents = (int *)malloc(sizeof(int)*c1->n_sets);
		memset(c1->set_contents, 0, sizeof(int)*c1->n_sets);
	}
}
/************************************************************/

/************************************************************/
Pcache_line insert_node(int pid, int idx, int tag)
{
	cache *c = &mesi_cache[pid];
	Pcache_line cur;

	// Insertable
	cur = (Pcache_line)malloc(sizeof(cache_line));
	memset(cur, 0, sizeof(cache_line));
	cur->tag = tag;
	insert(&c->LRU_head[idx], &c->LRU_tail[idx], cur);
	c->set_contents[idx] ++;

	return cur;
}

void delete_node(int pid, int idx)
{
	cache *c = &mesi_cache[pid];

	// delete(tail)
	delete(&c->LRU_head[idx], &c->LRU_tail[idx], c->LRU_tail[idx]);
	c->set_contents[idx] --;
}

void maintain_node(int pid, int idx, Pcache_line cline)
{
	cache *c = &mesi_cache[pid];

	// switch nodes to maintain order of LRU
	delete(&c->LRU_head[idx], &c->LRU_tail[idx], cline);
	insert(&c->LRU_head[idx], &c->LRU_tail[idx], cline);
}
/************************************************************/

/************************************************************/
void remote_load_miss(int local_pid, int local_idx, int local_tag, int *satisfied_by_cache)
{
	Pcache_line cur;
	int remote_pid, j, found = 0;

	*satisfied_by_cache = 0;
	for (remote_pid=0; remote_pid<num_core; remote_pid++) {
		if (remote_pid == local_pid)
			continue;

		cur = (Pcache_line)mesi_cache[remote_pid].LRU_head[local_idx];
		while (cur) {
			if ((cur->tag == local_tag) && (cur->state != INVALID)) {
				*satisfied_by_cache = 1;
				// (3) maintain remote cache state
				if (cur->state == MODIFIED)
					mesi_cache_stat[remote_pid].copies_back += (cache_block_size>>2);
				if ((cur->state == EXCLUSIVE) || (cur->state == SHARED) || (cur->state == MODIFIED))
					cur->state = SHARED;
				break;
			}
			cur = cur->LRU_next;
		}
	}
}

void cpu_load_miss(int local_pid, int local_idx, int local_tag, int local_no)
{
	Pcache_line cur;
	int state, satisfied_by_cache;

	mesi_cache_stat[local_pid].misses ++;
	
	if (local_no < cache_assoc) {
		// Insertable
		state = INVALID;
		cur = insert_node(local_pid, local_idx, local_tag);
	} else {
		// Not Insertable, need replace LRU
		state = mesi_cache[local_pid].LRU_tail[local_idx]->state;
        // delete(tail)
        delete_node(local_pid, local_idx);
        // insert(new)
        cur = insert_node(local_pid, local_idx, local_tag);
        mesi_cache_stat[local_pid].replacements ++;
	}

	mesi_cache_stat[local_pid].broadcasts ++;
	mesi_cache_stat[local_pid].demand_fetches += (cache_block_size>>2);
	remote_load_miss(local_pid, local_idx, local_tag, &satisfied_by_cache);

	// (1) maintain local cache state (special case, handle local after knowing remote)
	if (state == MODIFIED)
		mesi_cache_stat[local_pid].copies_back += (cache_block_size>>2);

	if (satisfied_by_cache) {
		// INVALID -> SHARED
		cur->state = SHARED;
	} else {
		// INVALID -> EXCLUSIVE
		cur->state = EXCLUSIVE;
	}
}

void cpu_load_hit(Pcache_line local_cline, int local_pid, int local_idx, int local_no)
{
	if (local_no > 1) {
		// switch nodes to maintain order of LRU
		maintain_node(local_pid, local_idx, local_cline);
	}

	// (2) do nothing
}

void remote_write_miss(int local_pid, int local_idx, int local_tag)
{
	Pcache_line cur;
	int remote_pid;
	for (remote_pid=0; remote_pid<num_core; remote_pid++) {
		if (remote_pid == local_pid)
			continue;
		
		cur = (Pcache_line)mesi_cache[remote_pid].LRU_head[local_idx];
		while (cur) {
			if (local_tag == cur->tag) {
				// (6) maintain remote cache state
				cur->state = INVALID;
				break;
			}
			cur = cur->LRU_next;
		}
	}
}

void cpu_write_miss(int local_pid, int local_idx, int local_tag, int local_no)
{
	Pcache_line cur;
	int state;

	mesi_cache_stat[local_pid].misses ++;
	
	if (local_no < cache_assoc) {
		// Insertable
		state = INVALID;
		cur = insert_node(local_pid, local_idx, local_tag);
	} else {
		// Not Insertable, need replace LRU
		state = mesi_cache[local_pid].LRU_tail[local_idx]->state;
        // delete(tail)
        delete_node(local_pid, local_idx);
        // insert(new)
        cur = insert_node(local_pid, local_idx, local_tag);
        mesi_cache_stat[local_pid].replacements ++;
	}

	mesi_cache_stat[local_pid].broadcasts ++;
	mesi_cache_stat[local_pid].demand_fetches += (cache_block_size>>2);
	remote_write_miss(local_pid, local_idx, local_tag);

	if (state == MODIFIED)
		mesi_cache_stat[local_pid].copies_back += (cache_block_size>>2);

	// (4)
	cur->state = MODIFIED;
}

void remote_write_hit(int local_pid, int local_idx, int local_tag)
{
	Pcache_line cur;
	int remote_pid;
	for (remote_pid=0; remote_pid<num_core; remote_pid++) {
		if (remote_pid == local_pid)
			continue;

		cur = (Pcache_line)mesi_cache[remote_pid].LRU_head[local_idx];
		while (cur) {
			if (local_tag == cur->tag) {
				// (7) maintain remote cache state
				if (cur->state == SHARED)
					cur->state = INVALID;
				else if (cur->state == MODIFIED)
					printf("Impossible MODIFIED happened!!!\n");
				else if (cur->state == EXCLUSIVE)
					printf("Impossible EXCLUSIVE happened!!!\n");
				break;
			}
			cur = cur->LRU_next;
		}
	}
}

void cpu_write_hit(Pcache_line local_cline, int local_pid, int local_idx, int local_tag, int local_no)
{
	if (local_no > 1) {
		// switch nodes to maintain order of LRU
		maintain_node(local_pid, local_idx, local_cline);
	}

	if (local_cline->state == SHARED) {
		mesi_cache_stat[local_pid].broadcasts ++;
		remote_write_hit(local_pid, local_idx, local_tag);
	}

	// (5)
	local_cline->state = MODIFIED;
}
/************************************************************/

/************************************************************/
void perform_access(unsigned addr, unsigned access_type, unsigned pid)
{
	/* handle an access to the cache */
	int i, c1_nontag_bits = 0, c1_tag = 0, c1_idx = 0, c1_no = 0, found;
	Pcache_line c1_line;
	Pcache_line cur;
	cache *c1 = &mesi_cache[pid];

	c1_nontag_bits = ceil(LOG2_FL(c1->n_sets)) + LOG2(cache_block_size);
	c1_tag = addr >> c1_nontag_bits;
	c1_idx =  ((addr & c1->index_mask) >> c1->index_mask_offset) % c1->n_sets;
	c1_line = (Pcache_line)c1->LRU_head[c1_idx];
	c1_no = c1->set_contents[c1_idx];

	/* update access */
	switch (access_type) {
	case TRACE_LOAD:
		mesi_cache_stat[pid].accesses ++;

		// check if tag is matched
		cur = (Pcache_line)c1->LRU_head[c1_idx];
		found = 0;
		while (cur) {
			if (c1_tag == cur->tag) {
				found = 1;
				break;
			}
			cur = cur->LRU_next;
		}
		if (!found) {
			// Read Miss (tag mismatch)
			cpu_load_miss(pid, c1_idx, c1_tag, c1_no);
		} else if (found && cur && cur->state == INVALID) {
			// Read Miss (tag match + wrong state)
			cpu_load_miss(pid, c1_idx, c1_tag, c1_no);
		} else {
			// Read Hit (tag match + correct state)
			cpu_load_hit(cur, pid, c1_idx, c1_no);
		}
		break;
	case TRACE_STORE:
		mesi_cache_stat[pid].accesses ++;

		// check if tag is matched
		cur = (Pcache_line)c1->LRU_head[c1_idx];
		found = 0;
		while (cur) {
			if (c1_tag == cur->tag) {
				found = 1;
				break;
			}
			cur = cur->LRU_next;
		}
		if (!found) {
			// Write Miss (tag mismatch)
			cpu_write_miss(pid, c1_idx, c1_tag, c1_no);
		} else if (found && cur && cur->state == INVALID) {
			// Write Miss (tag match + wrong state)
			cpu_write_miss(pid, c1_idx, c1_tag, c1_no);
		} else {
			// Write Hit (tag match + correct state)
			cpu_write_hit(cur, pid, c1_idx, c1_tag, c1_no);
		}
		break;
	}
}
/************************************************************/

/************************************************************/
void flush()
{
	/* flush the cache */
	int i, j;

	for (j=0; j<num_core; j++) {
		cache *c1 = &mesi_cache[j];

		for (i=0; i<c1->n_sets; i++) {
			Pcache_line cur = c1->LRU_head[i];
			while (cur) {
				if (cur->state == MODIFIED) {
					mesi_cache_stat[j].copies_back += (cache_block_size>>2);
					cur->state = INVALID;
				}
				cur = cur->LRU_next;
			}
		}
	}
}
/************************************************************/

/************************************************************/
void delete(head, tail, item)
  Pcache_line *head, *tail;
  Pcache_line item;
{
  if (item->LRU_prev) {
    item->LRU_prev->LRU_next = item->LRU_next;
  } else {
    /* item at head */
    *head = item->LRU_next;
  }

  if (item->LRU_next) {
    item->LRU_next->LRU_prev = item->LRU_prev;
  } else {
    /* item at tail */
    *tail = item->LRU_prev;
  }
}
/************************************************************/

/************************************************************/
/* inserts at the head of the list */
void insert(head, tail, item)
  Pcache_line *head, *tail;
  Pcache_line item;
{
  item->LRU_next = *head;
  item->LRU_prev = (Pcache_line)NULL;

  if (item->LRU_next)
    item->LRU_next->LRU_prev = item;
  else
    *tail = item;

  *head = item;
}
/************************************************************/

/************************************************************/
void dump_settings()
{
  printf("Cache Settings:\n");
  printf("\tSize: \t%d\n", cache_usize);
  printf("\tAssociativity: \t%d\n", cache_assoc);
  printf("\tBlock size: \t%d\n", cache_block_size);
}
/************************************************************/

/************************************************************/
void print_stats()
{
  int i;
  int demand_fetches = 0;
  int copies_back = 0;
  int broadcasts = 0;

  printf("*** CACHE STATISTICS ***\n");

  for (i = 0; i < num_core; i++) {
    printf("  CORE %d\n", i);
    printf("  accesses:  %d\n", mesi_cache_stat[i].accesses);
    printf("  misses:    %d\n", mesi_cache_stat[i].misses);
    printf("  miss rate: %f (%f)\n", 
	   (float)mesi_cache_stat[i].misses / (float)mesi_cache_stat[i].accesses,
	   1.0 - (float)mesi_cache_stat[i].misses / (float)mesi_cache_stat[i].accesses);
    printf("  replace:   %d\n", mesi_cache_stat[i].replacements);
  }

  printf("\n");
  printf("  TRAFFIC\n");
  for (i = 0; i < num_core; i++) {
    demand_fetches += mesi_cache_stat[i].demand_fetches;
    copies_back += mesi_cache_stat[i].copies_back;
    broadcasts += mesi_cache_stat[i].broadcasts;
  }
  printf("  demand fetch (words): %d\n", demand_fetches);
  /* number of broadcasts */
  printf("  broadcasts:           %d\n", broadcasts);
  printf("  copies back (words):  %d\n", copies_back);
}
/************************************************************/
