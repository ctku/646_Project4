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
static int core;

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
		c1->size =  cache_usize;        /* cache size */
		c1->associativity = cache_assoc;                              /* cache associativity */
		c1->n_sets = (c1->size / cache_block_size) / cache_assoc;;     /* number of cache sets */
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
extern void data_copy_cache2mem(int *dirty, int type);
void inst_copy_mem2cache(int *old_tag, int new_tag)
{
	mesi_cache_stat[core].demand_fetches += (cache_block_size>>2);
	*old_tag = new_tag;
}

void inst_load_hit()
{
	// do nothing
}

void inst_load_miss(int empty, int replace, int old_dirty, int *new_dirty, int *old_tag, int new_tag)
{
	mesi_cache_stat[core].misses ++;
	if (!empty) {
		if (cache_writeback && old_dirty)
			data_copy_cache2mem(new_dirty, CB_1LINE);
		if (replace)
			mesi_cache_stat[core].replacements ++;
	}
	inst_copy_mem2cache(old_tag, new_tag);
}
/************************************************************/

/************************************************************/
void data_copy_mem2cache(int *old_tag, int new_tag)
{
	mesi_cache_stat[core].demand_fetches += (cache_block_size>>2);
	*old_tag = new_tag;
}

void data_copy_cache2mem(int *dirty, int type)
{
	int size = (type == CB_1WORD) ? 1 : (cache_block_size>>2);

	mesi_cache_stat[core].copies_back += size;
	*dirty = 0;
}

void data_load_hit()
{
	// do nothing
}

void data_load_miss(int empty, int replace, int old_dirty, int *new_dirty, int *old_tag, int new_tag)
{
	mesi_cache_stat[core].misses ++;
	if (!empty) {
		if (cache_writeback && old_dirty)
			data_copy_cache2mem(new_dirty, CB_1LINE);
		if (replace)
			mesi_cache_stat[core].replacements ++;
	}
	data_copy_mem2cache(old_tag, new_tag);
}

void data_write_hit(int *dirty)
{
	// write through always generate 1 word to CB stats for DATA_STORE
	if (!cache_writeback) {
		data_copy_cache2mem(dirty, CB_1WORD);
	}

	if (cache_writeback) {
		*dirty = 1;
	}
}

void data_write_miss(int empty, int replace, int old_dirty, int *new_dirty, int *old_tag, int new_tag)
{
	// write through always generate 1 word to CB stats for DATA_STORE
	if (!cache_writeback) {
		int dummy;
		data_copy_cache2mem(&dummy, CB_1WORD);
	}

	mesi_cache_stat[core].misses ++;
	if (cache_writealloc) {
		if (!empty) {
			if (cache_writeback && old_dirty)
				data_copy_cache2mem(new_dirty, CB_1LINE);
			if (replace)
				mesi_cache_stat[core].replacements ++;
		}
		data_copy_mem2cache(old_tag, new_tag);
		// then CPU write to cache again
		if (cache_writeback) {
			*new_dirty = 1;
		}
	} else {
		// copy data from cpu to mem, no replacement
		int dummy;
		data_copy_cache2mem(&dummy, CB_1WORD);
	}
}
/************************************************************/

/************************************************************/
void perform_access(unsigned addr, unsigned access_type, unsigned pid)
{
	/* handle an access to the cache */
	int i, c1_nontag_bits = 0, c1_tag = 0, c1_idx = 0, c1_no = 0;
	Pcache_line c1_line;
	Pcache_line new_node;
	cache *c1 = &mesi_cache[core];

	c1_nontag_bits = ceil(LOG2_FL(c1->n_sets)) + LOG2(cache_block_size);
	c1_tag = addr >> c1_nontag_bits;
	c1_idx =  ((addr & c1->index_mask) >> c1->index_mask_offset) % c1->n_sets;
	c1_line = (Pcache_line)c1->LRU_head[c1_idx];
	c1_no = c1->set_contents[c1_idx];

	/* update access */
	switch (access_type) {
	case TRACE_LOAD:
		mesi_cache_stat[core].accesses ++;
		if (c1_no == 0) {
			// Miss
			new_node = (Pcache_line)malloc(sizeof(cache_line));
			memset(new_node, 0, sizeof(cache_line));
			insert(&c1->LRU_head[c1_idx], &c1->LRU_tail[c1_idx], new_node);
			c1->set_contents[c1_idx] ++;
			data_load_miss(1, 0, new_node->dirty, &new_node->dirty, &new_node->tag, c1_tag);
		} else {
			Pcache_line cur = (Pcache_line)c1->LRU_head[c1_idx];
			int found = 0;
			while (cur) {
				if (c1_tag == cur->tag) {
					found = 1;
					break;
				}
				cur = cur->LRU_next;
			}
			if (!found) {
				// Miss
				if (c1_no < cache_assoc) {
					// Insertable
					new_node = (Pcache_line)malloc(sizeof(cache_line));
					memset(new_node, 0, sizeof(cache_line));
					insert(&c1->LRU_head[c1_idx], &c1->LRU_tail[c1_idx], new_node);
					c1->set_contents[c1_idx] ++;
					data_load_miss(0, 0, new_node->dirty, &new_node->dirty, &new_node->tag, c1_tag);
				} else {
					// Not Insertable, need replace LRU
					// delete(tail)
					int old_dirty = c1->LRU_tail[c1_idx]->dirty;
					delete(&c1->LRU_head[c1_idx], &c1->LRU_tail[c1_idx], c1->LRU_tail[c1_idx]);
					if (old_dirty==1)
						old_dirty = old_dirty;
					c1->set_contents[c1_idx] --;
					// insert(new)
					new_node = (Pcache_line)malloc(sizeof(cache_line));
					memset(new_node, 0, sizeof(cache_line));
					insert(&c1->LRU_head[c1_idx], &c1->LRU_tail[c1_idx], new_node);
					c1->set_contents[c1_idx] ++;
					data_load_miss(0, 1, old_dirty, &new_node->dirty, &new_node->tag, c1_tag);
				}
			} else {
				// Hit
				if (c1_no > 1) {
					// switch nodes to maintain order of LRU
					delete(&c1->LRU_head[c1_idx], &c1->LRU_tail[c1_idx], cur);
					insert(&c1->LRU_head[c1_idx], &c1->LRU_tail[c1_idx], cur);
				}
				data_load_hit();
			}
		}
		break;
	case TRACE_STORE:
		mesi_cache_stat[core].accesses ++;
		if (cache_writealloc) {
			if (c1_no == 0) {
				// Miss
				new_node = (Pcache_line)malloc(sizeof(cache_line));
				memset(new_node, 0, sizeof(cache_line));
				insert(&c1->LRU_head[c1_idx], &c1->LRU_tail[c1_idx], new_node);
				c1->set_contents[c1_idx] ++;
				data_write_miss(1, 0, new_node->dirty, &new_node->dirty, &new_node->tag, c1_tag);
			} else {
				Pcache_line cur = (Pcache_line)c1->LRU_head[c1_idx];
				int found = 0;
				while (cur) {
					if (c1_tag == cur->tag) {
						found = 1;
						break;
					}
					cur = cur->LRU_next;
				}
				if (!found) {
					// Miss
					if (c1_no < cache_assoc) {
						// Insertable
						new_node = (Pcache_line)malloc(sizeof(cache_line));
						memset(new_node, 0, sizeof(cache_line));
						insert(&c1->LRU_head[c1_idx], &c1->LRU_tail[c1_idx], new_node);
						c1->set_contents[c1_idx] ++;
						data_write_miss(0, 0, new_node->dirty, &new_node->dirty, &new_node->tag, c1_tag);
					} else {
						// Not Insertable, need replace LRU
						// delete(tail)
						int old_dirty = c1->LRU_tail[c1_idx]->dirty;
						delete(&c1->LRU_head[c1_idx], &c1->LRU_tail[c1_idx], c1->LRU_tail[c1_idx]);
						c1->set_contents[c1_idx] --;
						// insert(new)
						new_node = (Pcache_line)malloc(sizeof(cache_line));
						memset(new_node, 0, sizeof(cache_line));
						insert(&c1->LRU_head[c1_idx], &c1->LRU_tail[c1_idx], new_node);
						c1->set_contents[c1_idx] ++;
						data_write_miss(0, 1, old_dirty, &new_node->dirty, &new_node->tag, c1_tag);
					}
				} else {
					// Hit
					if (c1_no > 1) {
						// switch nodes to maintain order of LRU
						delete(&c1->LRU_head[c1_idx], &c1->LRU_tail[c1_idx], cur);
						insert(&c1->LRU_head[c1_idx], &c1->LRU_tail[c1_idx], cur);
					}
					data_write_hit(&cur->dirty);
				}
			}
		} else {
			// Write non allocate
			Pcache_line cur = (Pcache_line)c1->LRU_head[c1_idx];
			int found = 0, dummy = 0;
			while (cur) {
				if (c1_tag == cur->tag) {
					found = 1;
					break;
				}
				cur = cur->LRU_next;
			}
			// no cache will be modified
			if (found) {
				if (c1_no > 1) {
					// switch nodes to maintain order of LRU
					delete(&c1->LRU_head[c1_idx], &c1->LRU_tail[c1_idx], cur);
					insert(&c1->LRU_head[c1_idx], &c1->LRU_tail[c1_idx], cur);
				}
				data_write_hit(&cur->dirty);
			} else {
				data_write_miss(0, 0, 0, &dummy, &dummy, 0);
			}
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
	cache *c1 = &mesi_cache[core];

	for (i=0; i<c1->n_sets; i++) {
		Pcache_line cur = c1->LRU_head[i];
		while (cur) {
			if (cur->dirty)
				data_copy_cache2mem(&c1->LRU_head[i]->dirty, CB_1LINE);
			cur = cur->LRU_next;
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
