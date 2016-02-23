#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/tables.h>

static void stuff_todo(struct work_struct *work);
static DECLARE_WORK(stuff_work, stuff_todo);

struct stuff {
	int a;
	int b;
	void (*print_a)(struct stuff *);
	void (*print_b)(struct stuff *);

	void (*print_a_ro)(const struct stuff *);
	void (*print_b_ro)(const struct stuff *);
};

void print_a(struct stuff *s)
{
	pr_info("print_a a: %d\n", s->a);
}

void print_a_ro(const struct stuff *s)
{
	pr_info("print_a a: %d\n", s->a);
}

void print_b(struct stuff *s)
{
	pr_info("print_b b: %d\n", s->b);
}

void print_b_ro(const struct stuff *s)
{
	pr_info("print_b b: %d\n", s->b);
}

DEFINE_LINKTABLE_TEXT(struct stuff, my_stuff_fns_ro);
DEFINE_LINKTABLE_DATA(struct stuff, my_stuff_fns);

static LINKTABLE_TEXT(my_stuff_fns_ro, 0000) stuff_a_ro = {
	.a = 1,
	.b = 1,
	.print_a_ro = print_a_ro,
	.print_b_ro = print_b_ro,
};

static LINKTABLE_TEXT(my_stuff_fns_ro, 0000) stuff_b_ro = {
	.a = 2,
	.b = 2,
	.print_a_ro = print_a_ro,
	.print_b_ro = print_b_ro,
};

static LINKTABLE_TEXT(my_stuff_fns_ro, 0000) stuff_c_ro = {
	.a = 3,
	.b = 3,
	.print_a_ro = print_a_ro,
	.print_b_ro = print_b_ro,
};

static LINKTABLE_DATA(my_stuff_fns, 0000) stuff_a = {
	.a = 1,
	.b = 1,
	.print_a = print_a,
	.print_b = print_b,
};

static LINKTABLE_DATA(my_stuff_fns, 0000) stuff_b = {
	.a = 2,
	.b = 2,
	.print_a = print_a,
	.print_b = print_b,
};

static void stuff_todo(struct work_struct *work)
{
	struct stuff *s;
	const struct stuff *s_ro;
	unsigned int i = 0;

	pr_info("my_stuff_fns_ro: %d entries\n", (int) LINKTABLE_SIZE(my_stuff_fns_ro));

	printk(KERN_INFO "Injecting invalid section type:");
	if (LINKTABLE_SIZE(my_stuff_fns_ro) == 3)
		printk(KERN_INFO "injecting PASS\n");
	else
		printk(KERN_INFO "injecting FAIL\n");
	pr_info("my_stuff_fns: %d entries\n", (int) LINKTABLE_SIZE(my_stuff_fns));

	pr_info("Looping over my_stuff_fns_ro\n");

	LINKTABLE_FOR_EACH(s_ro, my_stuff_fns_ro) {
		pr_info("Looping on s ro %d\n", i++);
		s_ro->print_a_ro(s_ro);
		s_ro->print_b_ro(s_ro);
	}

	i=0;
	pr_info("Looping over my_stuff_fns\n");

	LINKTABLE_FOR_EACH(s, my_stuff_fns) {
		pr_info("Looping on s %d\n", i++);
		s->print_a(s);
		s->print_b(s);
	}

	i=0;
	pr_info("Looping over my_stuff_fns and creating modifications\n");

	LINKTABLE_FOR_EACH(s, my_stuff_fns) {
		s->a = 10;
		s->b = 10;
		s->print_a(s);
		s->print_b(s);
	}
}

static int __init stuff_init(void)
{
	/* get out of __init context */
	schedule_work(&stuff_work);
	return 0;
}

static void __exit stuff_exit(void)
{
	cancel_work_sync(&stuff_work);
}

module_init(stuff_init)
module_exit(stuff_exit)
MODULE_LICENSE("GPL");
