#include <stdio.h>
#include <assert.h>

#include <linux/scatterlist.h>

#define MAX_PAGES (64)

static unsigned
_set_pages(struct page **pages, const unsigned *array, unsigned num)
{
	unsigned int i;

	assert(num < MAX_PAGES);

	for (i = 0; i < num; i++)
		pages[i] = (struct page *)(unsigned long)
			   ((1 + array[i]) * PAGE_SIZE);

	return num;
}

#define set_pages(p, a) _set_pages((p), (a), sizeof(a) / sizeof(a[0]))

#define check_and_free(_st, _ret, _nents) \
{ \
	assert((_ret) == 0); \
	assert((_st)->nents == _nents); \
	assert((_st)->orig_nents == _nents); \
	sg_free_table(_st); \
}

static int
alloc_tbl(struct sg_table *st, struct page **pages, unsigned nr_pages,
	  unsigned offset, unsigned size, unsigned max)
{
	return __sg_alloc_table_from_pages(st, pages, nr_pages, offset, size,
					   max, GFP_KERNEL);
}

int main(void)
{
	const unsigned int sgmax = SCATTERLIST_MAX_SEGMENT;
	struct page *pages[MAX_PAGES];
	struct sg_table st;
	int ret;

	ret = set_pages(pages, ((unsigned []){ 0 }));
	ret = alloc_tbl(&st, pages, ret, 0, ret * PAGE_SIZE, PAGE_SIZE + 1);
	assert(ret == -EINVAL);

	ret = set_pages(pages, ((unsigned []){ 0 }));
	ret = alloc_tbl(&st, pages, ret, 0, ret * PAGE_SIZE, 0);
	assert(ret == -EINVAL);

	ret = set_pages(pages, ((unsigned []){ 0 }));
	ret = alloc_tbl(&st, pages, ret, 0, ret * PAGE_SIZE, sgmax);
	check_and_free(&st, ret, 1);

	ret = set_pages(pages, ((unsigned []){ 0 }));
	ret = alloc_tbl(&st, pages, ret, 0, ret, sgmax);
	check_and_free(&st, ret, 1);

	ret = set_pages(pages, ((unsigned []){ 0, 1 }));
	ret = alloc_tbl(&st, pages, ret, 0, ret * PAGE_SIZE, sgmax);
	check_and_free(&st, ret, 1);

	ret = set_pages(pages, ((unsigned []){ 0, 2 }));
	ret = alloc_tbl(&st, pages, ret, 0, ret * PAGE_SIZE, sgmax);
	check_and_free(&st, ret, 2);

	ret = set_pages(pages, ((unsigned []){ 0, 1, 3 }));
	ret = alloc_tbl(&st, pages, ret, 0, ret * PAGE_SIZE, sgmax);
	check_and_free(&st, ret, 2);

	ret = set_pages(pages, ((unsigned []){ 0, 1, 3, 4 }));
	ret = alloc_tbl(&st, pages, ret, 0, ret * PAGE_SIZE, sgmax);
	check_and_free(&st, ret, 2);

	ret = set_pages(pages, ((unsigned []){ 0, 1, 3, 4, 5 }));
	ret = alloc_tbl(&st, pages, ret, 0, ret * PAGE_SIZE, sgmax);
	check_and_free(&st, ret, 2);

	ret = set_pages(pages, ((unsigned []){ 0, 1, 3, 4, 6 }));
	ret = alloc_tbl(&st, pages, ret, 0, ret * PAGE_SIZE, sgmax);
	check_and_free(&st, ret, 3);

	ret = set_pages(pages, ((unsigned []){ 0, 2, 4, 6, 8 }));
	ret = alloc_tbl(&st, pages, ret, 0, ret * PAGE_SIZE, sgmax);
	check_and_free(&st, ret, 5);

	ret = set_pages(pages, ((unsigned []){ 0, 1, 2, 3, 4 }));
	ret = alloc_tbl(&st, pages, ret, 0, ret * PAGE_SIZE, sgmax);
	check_and_free(&st, ret, 1);

	ret = set_pages(pages, ((unsigned []){ 0, 1, 2, 3, 4 }));
	ret = alloc_tbl(&st, pages, ret, 0, ret * PAGE_SIZE, 2 * PAGE_SIZE);
	check_and_free(&st, ret, 3);

	ret = set_pages(pages, ((unsigned []){ 0, 1, 2, 3, 4, 5}));
	ret = alloc_tbl(&st, pages, ret, 0, ret * PAGE_SIZE, 2 * PAGE_SIZE);
	check_and_free(&st, ret, 3);

	ret = set_pages(pages, ((unsigned []){ 0, 2, 3, 4, 5, 6}));
	ret = alloc_tbl(&st, pages, ret, 0, ret * PAGE_SIZE, 2 * PAGE_SIZE);
	check_and_free(&st, ret, 4);

	ret = set_pages(pages, ((unsigned []){ 0, 1, 3, 4, 5, 6}));
	ret = alloc_tbl(&st, pages, ret, 0, ret * PAGE_SIZE, 2 * PAGE_SIZE);
	check_and_free(&st, ret, 3);

	return 0;
}
