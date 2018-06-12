#include <linux/efi.h>
#include <asm/efi.h>
#include "misc.h"

#define ACPI_NAME_SIZE                  4
#define ACPI_OEM_ID_SIZE                6
#define ACPI_OEM_TABLE_ID_SIZE          8
#define ACPI_SRAT_MEM_HOT_PLUGGABLE (1<<1)        /* 01: Memory region is hot pluggable */

#define ACPI_RSDP_SCAN_STEP             16
#define ACPI_RSDP_CHECKSUM_LENGTH       20
#define ACPI_RSDP_XCHECKSUM_LENGTH      36

#define ACPI_CAST_PTR(t, p)	((t *) (void *) (p))
#define ACPI_SIG_RSDP	"RSD PTR "      /* Root System Description Pointer */
#define ACPI_VALIDATE_RSDP_SIG(a)	(!strncmp(ACPI_CAST_PTR(char, (a)), ACPI_SIG_RSDP, 8))

#define ACPI_EBDA_PTR_LOCATION          0x0000040E      /* Physical Address */
#define ACPI_HI_RSDP_WINDOW_BASE        0x000E0000      /* Physical Address */
#define ACPI_HI_RSDP_WINDOW_SIZE        0x00020000
#define ACPI_EBDA_WINDOW_SIZE		1024
#define ACPI_MAX_TABLES			128
#define ACPI_PTR_DIFF(a, b)	((acpi_size)(ACPI_CAST_PTR(u8, (a)) - ACPI_CAST_PTR(u8, (b))))
#define ACPI_XSDT_ENTRY_SIZE	(sizeof(u64))
#define ACPI_RSDT_ENTRY_SIZE	(sizeof(u32))

#define ACPI_ADD_PTR(t, a, b)	(ACPI_CAST_PTR(t, (ACPI_CAST_PTR(u8, (a)) + (acpi_size)(b))))

#ifdef ACPI_32BIT_PHYSICAL_ADDRESS

/*
 * OSPMs can define this to shrink the size of the structures for 32-bit
 * none PAE environment. ASL compiler may always define this to generate
 * 32-bit OSPM compliant tables.
 */
typedef u32 acpi_physical_address;

#else                           /* ACPI_32BIT_PHYSICAL_ADDRESS */

/*
 * It is reported that, after some calculations, the physical addresses can
 * wrap over the 32-bit boundary on 32-bit PAE environment.
 * https://bugzilla.kernel.org/show_bug.cgi?id=87971
 */
typedef u64 acpi_physical_address;

#endif                          /* ACPI_32BIT_PHYSICAL_ADDRESS */

typedef u64 acpi_size;

struct acpi_table_rsdp {
	char signature[8];	/* ACPI signature, contains "RSD PTR " */
	u8 checksum;		/* ACPI 1.0 checksum */
	char oem_id[ACPI_OEM_ID_SIZE];	/* OEM identification */
	u8 revision;		/* Must be (0) for ACPI 1.0 or (2) for ACPI 2.0+ */
	u32 rsdt_physical_address;	/* 32-bit physical address of the RSDT */
	u32 length;		/* Table length in bytes, including header (ACPI 2.0+) */
	u64 xsdt_physical_address;	/* 64-bit physical address of the XSDT (ACPI 2.0+) */
	u8 extended_checksum;	/* Checksum of entire table (ACPI 2.0+) */
	u8 reserved[3];		/* Reserved, must be zero */
};

struct acpi_table_header *get_acpi_srat_table(void);
/*******************************************************************************
 *
 * Master ACPI Table Header. This common header is used by all ACPI tables
 * except the RSDP and FACS.
 *
 ******************************************************************************/

struct acpi_table_header {
	char signature[ACPI_NAME_SIZE];	/* ASCII table signature */
	u32 length;		/* Length of table in bytes, including this header */
	u8 revision;		/* ACPI Specification minor version number */
	u8 checksum;		/* To make sum of entire table == 0 */
	char oem_id[ACPI_OEM_ID_SIZE];	/* ASCII OEM identification */
	char oem_table_id[ACPI_OEM_TABLE_ID_SIZE];	/* ASCII OEM table identification */
	u32 oem_revision;	/* OEM revision number */
	char asl_compiler_id[ACPI_NAME_SIZE];	/* ASCII ASL compiler vendor ID */
	u32 asl_compiler_revision;	/* ASL compiler version */
};

/*******************************************************************************
 *
 * SRAT - System Resource Affinity Table
 *        Version 3
 *
 ******************************************************************************/

struct acpi_table_srat {
	struct acpi_table_header header;	/* Common ACPI table header */
	u32 table_revision;	/* Must be value '1' */
	u64 reserved;		/* Reserved, must be zero */
};

/* Generic subtable header (used in MADT, SRAT, etc.) */

struct acpi_subtable_header {
	u8 type;
	u8 length;
};

struct acpi_srat_mem_affinity {
	struct acpi_subtable_header header;
	u32 proximity_domain;
	u16 reserved;		/* Reserved, must be zero */
	u64 base_address;
	u64 length;
	u32 reserved1;
	u32 flags;
	u64 reserved2;		/* Reserved, must be zero */
};

/*
 * Internal table-related structures
 */
union acpi_name_union {
	u32 integer;
	char ascii[4];
};

typedef u8 acpi_owner_id;

/* Internal ACPI Table Descriptor. One per ACPI table. */

struct acpi_table_desc {
	acpi_physical_address address;
	struct acpi_table_header *pointer;
	u32 length;             /* Length fixed at 32 bits (fixed in table header) */
	union acpi_name_union signature;
	acpi_owner_id owner_id;
	u8 flags;
	u16 validation_count;
};

#ifdef ACPI_BIG_ENDIAN
#define ACPI_MOVE_64_TO_64(d, s) \
{((u8 *)(void *)(d))[0] = ((u8 *)(void *)(s))[7]; \
((u8 *)(void *)(d))[1] = ((u8 *)(void *)(s))[6]; \
((u8 *)(void *)(d))[2] = ((u8 *)(void *)(s))[5]; \
((u8 *)(void *)(d))[3] = ((u8 *)(void *)(s))[4]; \
((u8 *)(void *)(d))[4] = ((u8 *)(void *)(s))[3]; \
((u8 *)(void *)(d))[5] = ((u8 *)(void *)(s))[2]; \
((u8 *)(void *)(d))[6] = ((u8 *)(void *)(s))[1]; \
((u8 *)(void *)(d))[7] = ((u8 *)(void *)(s))[0]; }
#else
#ifndef ACPI_MISALIGNMENT_NOT_SUPPORTED
#define ACPI_MOVE_64_TO_64(d, s) \
{*(u64 *)(void *)(d) = *(u64 *)(void *)(s); }
#else
#define ACPI_MOVE_64_TO_64(d, s) \
{((u8 *)(void *)(d))[0] = ((u8 *)(void *)(s))[0]; \
((u8 *)(void *)(d))[1] = ((u8 *)(void *)(s))[1]; \
((u8 *)(void *)(d))[2] = ((u8 *)(void *)(s))[2]; \
((u8 *)(void *)(d))[3] = ((u8 *)(void *)(s))[3]; \
((u8 *)(void *)(d))[4] = ((u8 *)(void *)(s))[4]; \
((u8 *)(void *)(d))[5] = ((u8 *)(void *)(s))[5]; \
((u8 *)(void *)(d))[6] = ((u8 *)(void *)(s))[6]; \
((u8 *)(void *)(d))[7] = ((u8 *)(void *)(s))[7]; }
#endif
#endif
#ifdef ACPI_BIG_ENDIAN
#define ACPI_MOVE_16_TO_32(d, s) \
{(*(u32 *)(void *)(d)) = 0; \
((u8 *)(void *)(d))[2] = ((u8 *)(void *)(s))[1]; \
((u8 *)(void *)(d))[3] = ((u8 *)(void *)(s))[0]; }
#else
#ifndef ACPI_MISALIGNMENT_NOT_SUPPORTED
#define ACPI_MOVE_16_TO_32(d, s) \
{*(u32 *)(void *)(d) = *(u16 *)(void *)(s); }
#else
#define ACPI_MOVE_16_TO_32(d, s) \
{(*(u32 *)(void *)(d)) = 0; ACPI_MOVE_16_TO_16(d, s); }
#endif
#endif
