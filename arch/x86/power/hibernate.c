int reallocate_restore_code(void)
{
	relocated_restore_code = (void *)get_safe_page(GFP_ATOMIC);
	if (!relocated_restore_code)
		return -ENOMEM;
	memcpy(relocated_restore_code, &core_restore_code,
	       &restore_registers - &core_restore_code);
	return 0;
}

struct restore_data_record {
	unsigned long jump_address;
	unsigned long jump_address_phys;
	unsigned long cr3;
	unsigned long magic;
};

/**
 *	arch_hibernation_header_save - populate the architecture specific part
 *		of a hibernation image header
 *	@addr: address to save the data at
 */
int arch_hibernation_header_save(void *addr, unsigned int max_size)
{
	struct restore_data_record *rdr = addr;

	if (max_size < sizeof(struct restore_data_record))
		return -EOVERFLOW;
	rdr->jump_address = (unsigned long)&restore_registers;
	rdr->jump_address_phys = __pa_symbol(&restore_registers);
	rdr->cr3 = restore_cr3;
	rdr->magic = RESTORE_MAGIC;
	return 0;
}

/**
 *	arch_hibernation_header_restore - read the architecture specific data
 *		from the hibernation image header
 *	@addr: address to read the data from
 */
int arch_hibernation_header_restore(void *addr)
{
	struct restore_data_record *rdr = addr;

	restore_jump_address = rdr->jump_address;
	jump_address_phys = rdr->jump_address_phys;
	restore_cr3 = rdr->cr3;
	return (rdr->magic == RESTORE_MAGIC) ? 0 : -EINVAL;
}
