#include <memory/allocator/page.h>
#include <memory/page_flags.h>
#include <memory/gfp.h>
#include <page.h>
#include <slabcache.h>
#include <list.h>
#include <align.h>
#include <print.h>
#include <string.h>

struct slab_cache *slab_cache;

static inline void add_slab_to_partial_list(struct slab_cache *s,
					struct page *page)
{
	list_add(&page->slab_list, &s->partial);
}

static inline void remove_slab_from_partial_list(struct slab_cache *s,
						struct page *page)
{
	list_del(&page->slab_list);
}

static inline void add_slab_to_full_list(struct slab_cache *s,
					struct page *page)
{
	list_add(&page->slab_list, &s->full);
}

static inline void remove_slab_from_full_list(struct slab_cache *s,
					struct page *page)
{
	list_del(&page->slab_list);
}

static inline unsigned int oo_order(struct order_objects oo)
{
	return oo.order & OO_MASK;
}

static inline unsigned int oo_objects(order_objects_t oo)
{
	return oo.objects & OO_MASK;
}

static inline order_objects_t oo_make(unsigned int order, unsigned int size)
{
	order_objects_t oo;

	oo.order = order;
	oo.objects = (PAGE_SIZE << order) / size;

	return oo;
}

static inline void *get_freepointer(struct slab_cache *s, void *object)
{
	return (void *)*(unsigned long *)(object + s->offset);
}

static inline void set_freepointer(struct slab_cache *s, void *object, void *fp)
{
	unsigned long freeptr_addr = (unsigned long)object + s->offset;

	*(void **)freeptr_addr = fp;
}

/*
 * Get a partial slab, and then remove slab from the partial list,
 * freeze it and return the pointer to the freelist.
 */
static void *get_partial(struct slab_cache *s, gfp_t gfp_mask)
{
	struct page *page;
	void *freelist = NULL;

	list_for_each_entry(page, &s->partial, slab_list) {
		freelist = page->freelist;

		if (freelist) {
			s->page = page;

			remove_slab_from_partial_list(s, page);
			s->nr_partial--;

			break;
		}
	}

	return freelist;
}

/*
 * allocate new slab from page allocator
 */
static struct page *allocate_slab(struct slab_cache *s, gfp_t gfp_mask)
{
	order_objects_t oo = s->oo;
	unsigned int order = oo_order(oo);
	unsigned int objects = oo_objects(oo);
	struct page *page;
	void *start, *next;
	int idx;

	page = __alloc_pages(gfp_mask, order);
	set_page_slab(page);

	start = page_address(page);
	page->freelist = start;
	page->slab_cache = s;
	page->inuse = 0;
	INIT_LIST_HEAD(&page->slab_list);

	for (idx = 0; idx < (objects - 1); idx++) {
		next = (void *)((unsigned long)start + s->size);
		set_freepointer(s, start, next);
		start = next;
	}
	set_freepointer(s, start, NULL);

	return page;
}


/*
 * Slow path. When the lockless freelist is empty.
 *
 * Processing is still very fast if has partial freelist. In that case
 * we simply take over the partial freelist as the lockless freelist,
 * and then take the first element of the freelist as the object to allocate now
 * and move the rest of the freelist to the lockless freelist.
 *
 * And if we were unable to get a new slab from the partial slab lists then
 * we need to allocate a new slab. This is the slowest path since it involves
 * a call to the page allocator and the setup of a new slab.
 */
static void *__slab_cache_alloc(struct slab_cache *s, gfp_t gfp_mask)
{
	void *freelist;
	struct page *page;

	page = s->page;
	if (page) {
		/*
		 * move slab to full list in slab_cache
		 */
		add_slab_to_full_list(s, page);

		s->page = NULL;
	}

	/*
	 * try get slab from partial list in slab cache
	 */
	freelist = get_partial(s, gfp_mask);
	if (freelist)
		goto FREELIST;

	/*
	 * allocate new slab
	 */
	page = allocate_slab(s, gfp_mask);
	if (page) {
		freelist = page->freelist;
		page->freelist = NULL;
		s->page = page;
	}

	if (!freelist) {
		//slab_out_of_memory(s, gfp_mask, node);
		return NULL;
	}

FREELIST:
	return freelist;
}

/**
 * allocate an object from slab cache @s
 * @s: slab cache pointer of want to allocate object
 * @gfp_mask: gfp flags
 *
 * The fastpath works by first checking if the lockless freelist can be used.
 * If not then __slab_cache_alloc is called for slow processing.
 *
 * Otherwise we can simply pick the next object from the lockless free list.
 */
void *slab_cache_alloc(struct slab_cache *s, gfp_t gfp_mask)
{
	void *object;

	object = s->freelist;
	if (!object) {
		object = __slab_cache_alloc(s, gfp_mask);
	}

	s->freelist = get_freepointer(s, object);
	s->page->inuse++;

	if (gfp_mask & __GFP_ZERO) {
		memset(object, 0, s->object_size);
	}

	return object;
}

void *slab_cache_zalloc(struct slab_cache *s, gfp_t gfp_mask)
{
        return slab_cache_alloc(s, gfp_mask | __GFP_ZERO);
}

static inline struct slab_cache *cache_from_obj(void *object)
{
	struct page *page;

	page = virt_to_page(object);

	return page->slab_cache;
}

static void discard_slab(struct slab_cache *s, struct page *page)
{
	int order = s->oo.order;

	clear_page_slab(page);
	__free_pages(page, order);
}

/*
 * Slow path handling.
 *
 * This may still be called fast in objects from full list,
 * just free object and move slab which full list to partial list.
 *
 * If object from partial list, total partial slab not used after free object
 * and partial list number less than SLAB_CACHE_MIN_PARTIAL,
 * so free this partial slab to page allocator.
 */
static void __slab_cache_free(struct slab_cache *s, struct page *page,
				void *object)

{
	void *prior_object;

	prior_object = page->freelist;
	set_freepointer(s, object, prior_object);
	page->freelist = object;
	page->inuse--;

	/*
	 * @page from full list
	 */
	if (!prior_object) {
		remove_slab_from_full_list(s, page);

		s->nr_partial++;
		add_slab_to_partial_list(s, page);

		return;
	}

	/*
	 * @page from partial list
	 */
	if(!page->inuse && s->nr_partial > SLAB_CACHE_MIN_PARTIAL) {
		remove_slab_from_partial_list(s, page);
		s->nr_partial--;

		discard_slab(s, page);
	}
}

/**
 * free an @object to slab cache @s
 * @s: slab cache pointer of want to free object
 * @object: object of want to free
 *
 * The fastpath is only possible if we are freeing object from the current slab.
 * Otherwise, go into the slowpath __slab_cache_free() to free this slab.
 */
void slab_cache_free(struct slab_cache *s, void *object)
{
	struct slab_cache *slabcache;
	struct page *page;

	slabcache = cache_from_obj(object);
	if (s != slabcache) {
		pr_warn("The parameter may be wrong. check please!\n");
		s = slabcache;
	}

	page = virt_to_page(object);
	if (page == s->page) {
		set_freepointer(s, object, s->freelist);
		s->freelist = object;
		page->inuse--;
	} else {
		__slab_cache_free(s, page, object);
	}
}

static inline int calculate_order(unsigned int size)
{
	int order = 0;

	return order;
}

static int calculate_sizes(struct slab_cache *s, int forced_order)
{
	unsigned int size = s->size;
	unsigned int order;

	size = ALIGN(size, s->align);
	s->size = size;

	if (forced_order >= 0)
		order = forced_order;
	else
		order = calculate_order(size);

	if ((int)order < 0)
		return 0;

	s->oo = oo_make(order, size);

	return !!oo_objects(s->oo);
}

int __slab_cache_create(struct slab_cache *s,
			const char *name, unsigned int object_size,
			unsigned int align, unsigned long flags)
{
	s->name = name;
	s->align = align;
	s->flags = flags;
	s->size = s->object_size = object_size;

	s->offset = 0;
	s->freelist = NULL;
	s->page = NULL;

	if (!calculate_sizes(s, -1))
		return -1;

	INIT_LIST_HEAD(&s->partial);
	INIT_LIST_HEAD(&s->full);

	return 0;
}

/**
 * allocate an object of slab cache type from @slab_cahe
 * @name: A string to identify this cache.
 * @object_size: The size of objects to be created in this cache.
 * @align: The required alignment for the objects.
 * @flags: slab cache flags
 *
 * Return: a pointer to the slab cache on success, NULL on failure.
 */
struct slab_cache *slab_cache_create(const char *name, unsigned int object_size,
				unsigned int align, unsigned long flags)
{
	struct slab_cache *s;
	int err;

	s = slab_cache_zalloc(slab_cache, GFP_KERNEL);
	if (!s)
		return s;

	err = __slab_cache_create(s, name, object_size, align, flags);
	if (err)
		slab_cache_free(slab_cache, s);

	return s;
}

/**
 * free an object of slab cache type to @slab_cahe
 * @s: slab cache pointer of want to free
 */
void slab_cache_destroy(struct slab_cache *s)
{
	if(s->page->inuse) {
		pr_debug("s->page in use\n");
	}

	discard_slab(s, s->page);

	if(!list_empty(&s->partial)) {
		pr_debug("list s->partial not empty\n");
	}

	if(!list_empty(&s->full)) {
		pr_debug("list s->full not empty\n");
	}

	slab_cache_free(slab_cache, s);
}

/*
 * Create a cache during boot when no slab services are available yet
 */
static void create_boot_cache(struct slab_cache *s, const char *name,
			unsigned int object_size, unsigned long flags)
{
	int err;

	err = __slab_cache_create(s, name, object_size,
				sizeof(unsigned long), flags);
	if (err)
		pr_error("Creation of slab cache %s, object size=%u failed.\n",
			name, object_size);
}

/*
 * Used for early slab_cache structures that were allocated using the page
 * allocator. Allocate them properly then fix up the pointers that may be
 * pointing to the wrong slab_cache structure.
 */
static struct slab_cache *bootstrap(struct slab_cache *static_cache)
{
	struct slab_cache *s;
	struct page *page;

	s = slab_cache_zalloc(slab_cache, GFP_KERNEL);

	memcpy(s, static_cache, slab_cache->object_size);

	page = virt_to_page(s);
	page->slab_cache = s;

	INIT_LIST_HEAD(&s->partial);
	INIT_LIST_HEAD(&s->full);

	return s;
}

/**
 * initialization slab cache allocator
 */
void slab_cache_allocator_init(void)
{
	static struct slab_cache boot_slab_cache;

	slab_cache = &boot_slab_cache;

	create_boot_cache(slab_cache, "slab_cache", sizeof(struct slab_cache),
			SLAB_HWCACHE_ALIGN);

	slab_cache = bootstrap(&boot_slab_cache);
}
