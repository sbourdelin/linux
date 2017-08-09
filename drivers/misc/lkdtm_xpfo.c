/*
 * This is for all the tests related to XPFO (eXclusive Page Frame Ownership).
 */

#include "lkdtm.h"

#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/xpfo.h>

void read_user_with_flags(unsigned long flags)
{
	unsigned long user_addr, user_data = 0xdeadbeef;
	phys_addr_t phys_addr;
	void *virt_addr;

	user_addr = vm_mmap(NULL, 0, PAGE_SIZE,
			    PROT_READ | PROT_WRITE | PROT_EXEC,
			    flags, 0);
	if (user_addr >= TASK_SIZE) {
		pr_warn("Failed to allocate user memory\n");
		return;
	}

	if (copy_to_user((void __user *)user_addr, &user_data,
			 sizeof(user_data))) {
		pr_warn("copy_to_user failed\n");
		goto free_user;
	}

	phys_addr = user_virt_to_phys(user_addr);
	if (!phys_addr) {
		pr_warn("Failed to get physical address of user memory\n");
		goto free_user;
	}

	virt_addr = phys_to_virt(phys_addr);
	if (phys_addr != virt_to_phys(virt_addr)) {
		pr_warn("Physical address of user memory seems incorrect\n");
		goto free_user;
	}

	pr_info("Attempting bad read from kernel address %p\n", virt_addr);
	if (*(unsigned long *)virt_addr == user_data)
		pr_info("Huh? Bad read succeeded?!\n");
	else
		pr_info("Huh? Bad read didn't fail but data is incorrect?!\n");

 free_user:
	vm_munmap(user_addr, PAGE_SIZE);
}

/* Read from userspace via the kernel's linear map. */
void lkdtm_XPFO_READ_USER(void)
{
	read_user_with_flags(MAP_PRIVATE | MAP_ANONYMOUS);
}

void lkdtm_XPFO_READ_USER_HUGE(void)
{
	read_user_with_flags(MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB);
}
