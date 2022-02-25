#include <memory/allocator/slabcache.h>
#include <print.h>
#include <string.h>

void test_alloc_8bytes(void)
{
	struct slab_cache *s;
	unsigned int object_size;
	unsigned int align;
	unsigned long *buf;

	pr_info("Enter %s\n", __func__);

	object_size = sizeof(unsigned long);
	align = 8;
	pr_info("\tslab cache create, object size %d, align %d\n",
		object_size, align);
	s = slab_cache_create("unsigned long", object_size, align, 0);

	pr_info("\tslab cache alloc\n");
	buf = slab_cache_alloc(s, 0);
	*buf = 0x123456;
	pr_info("\tbuf address %p, buf values 0x%lx\n", buf, *buf);

	pr_info("\tslab cache free\n");
	slab_cache_free(s, buf);

	pr_info("\tslab cache destroy\n");
	slab_cache_destroy(s);

	pr_info("Exit %s\n", __func__);
}

struct structA {
	char name[10];
	int id;
};

void test_alloc_structA(void)
{
	struct slab_cache *s;
	unsigned int object_size;
	unsigned int align;
	struct structA *buf;

	pr_info("Enter %s\n", __func__);

	object_size = sizeof(struct structA);
	align = 8;
	pr_info("\tslab cache create, object size %d, align %d\n",
		object_size, align);
	s = slab_cache_create("struct structA", object_size, align, 0);

	pr_info("\tslab cache alloc\n");
	buf = slab_cache_alloc(s, 0);
	strcpy(buf->name, "test");
	buf->id = 0xaa;
	pr_info("\tbuf address %p, buf->name %s, buf->id 0x%x\n",
		buf, buf->name, buf->id);

	pr_info("\tslab cache free\n");
	slab_cache_free(s, buf);

	pr_info("\tslab cache destroy\n");
	slab_cache_destroy(s);

	pr_info("Exit %s\n", __func__);
}

int slab_cache_allocator_test(void)
{
	pr_info("\nslab cache allocator init\n");
	slab_cache_allocator_init();

	test_alloc_8bytes();
	test_alloc_structA();

	return 0;
}
