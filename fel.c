/*
 * Copyright (C) 2012  Henrik Nordstrom <henrik@henriknordstrom.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#include "common.h"
#include "fel_usblib.h"
#include "portable_endian.h"
#include "progress.h"

static bool verbose = false; /* If set, makes the 'fel' tool more talkative */
static uint32_t uboot_entry = 0; /* entry point (address) of U-Boot */
static uint32_t uboot_size  = 0; /* size of U-Boot binary */


/* Constants taken from ${U-BOOT}/include/image.h */
#define IH_MAGIC	0x27051956	/* Image Magic Number	*/
#define IH_ARCH_ARM		2	/* ARM			*/
#define IH_TYPE_INVALID		0	/* Invalid Image	*/
#define IH_TYPE_FIRMWARE	5	/* Firmware Image	*/
#define IH_TYPE_SCRIPT		6	/* Script file		*/
#define IH_NMLEN		32	/* Image Name Length	*/

/* Additional error codes, newly introduced for get_image_type() */
#define IH_TYPE_ARCH_MISMATCH	-1

#define HEADER_NAME_OFFSET	32	/* offset of name field	*/
#define HEADER_SIZE		(HEADER_NAME_OFFSET + IH_NMLEN)

/*
 * Utility function to determine the image type from a mkimage-compatible
 * header at given buffer (address).
 *
 * For invalid headers (insufficient size or 'magic' mismatch) the function
 * will return IH_TYPE_INVALID. Negative return values might indicate
 * special error conditions, e.g. IH_TYPE_ARCH_MISMATCH signals that the
 * image doesn't match the expected (ARM) architecture.
 * Otherwise the function will return the "ih_type" field for valid headers.
 */
int get_image_type(const uint8_t *buf, size_t len)
{
	uint32_t *buf32 = (uint32_t *)buf;

	if (len <= HEADER_SIZE) /* insufficient length/size */
		return IH_TYPE_INVALID;
	if (be32toh(buf32[0]) != IH_MAGIC) /* signature mismatch */
		return IH_TYPE_INVALID;
	/* For sunxi, we always expect ARM architecture here */
	if (buf[29] != IH_ARCH_ARM)
		return IH_TYPE_ARCH_MISMATCH;

	/* assume a valid header, and return ih_type */
	return buf[30];
}

void aw_fel_print_version(felusb_handle *usb)
{
	struct aw_fel_version buf;
	aw_fel_get_version(usb, &buf);

	const char *soc_name="unknown";
	switch (buf.soc_id) {
	case 0x1623: soc_name="A10"; break;
	case 0x1625: soc_name="A13"; break;
	case 0x1633: soc_name="A31"; break;
	case 0x1651: soc_name="A20"; break;
	case 0x1650: soc_name="A23"; break;
	case 0x1689: soc_name="A64"; break;
	case 0x1639: soc_name="A80"; break;
	case 0x1667: soc_name="A33"; break;
	case 0x1673: soc_name="A83T"; break;
	case 0x1680: soc_name="H3"; break;
	}

	printf("%.8s soc=%08x(%s) %08x ver=%04x %02x %02x scratchpad=%08x %08x %08x\n",
		buf.signature, buf.soc_id, soc_name, buf.unknown_0a,
		buf.protocol, buf.unknown_12, buf.unknown_13,
		buf.scratchpad, buf.pad[0], buf.pad[1]);
}

/*
 * This wrapper for the FEL write functionality safeguards against overwriting
 * an already loaded U-Boot binary.
 * The return value represents elapsed time in seconds (needed for execution).
 */
double aw_write_buffer(felusb_handle *usb, void *buf, uint32_t offset,
		       size_t len, bool progress)
{
	/* safeguard against overwriting an already loaded U-Boot binary */
	if (uboot_size > 0 && offset <= uboot_entry + uboot_size
			   && offset + len >= uboot_entry) {
		fprintf(stderr, "ERROR: Attempt to overwrite U-Boot! "
			"Request 0x%08X-0x%08X overlaps 0x%08X-0x%08X.\n",
			offset, (uint32_t)(offset + len),
			uboot_entry, uboot_entry + uboot_size);
		exit(1);
	}
	double start = gettime();
	aw_fel_write_buffer(usb, buf, offset, len, progress);
	return gettime() - start;
}

void hexdump(void *data, uint32_t offset, size_t size)
{
	size_t j;
	unsigned char *buf = data;
	for (j = 0; j < size; j+=16) {
		size_t i;
		printf("%08lx: ",(long int)offset + j);
		for (i = 0; i < 16; i++) {
			if (j + i < size)
				printf("%02x ", buf[j+i]);
			else
				printf("__ ");
		}
		putchar(' ');
		for (i = 0; i < 16; i++) {
			if (j + i >= size)
				putchar('.');
			else
				putchar(isprint(buf[j+i]) ? buf[j+i] : '.');
		}
		putchar('\n');
	}
}

unsigned int file_size(const char *filename)
{
	struct stat st;
	if (stat(filename, &st) != 0) {
		fprintf(stderr, "stat() error on file \"%s\": %s\n", filename,
			strerror(errno));
		exit(1);
	}
	if (!S_ISREG(st.st_mode)) {
		fprintf(stderr, "error: \"%s\" is not a regular file\n", filename);
		exit(1);
	}
	return st.st_size;
}

int save_file(const char *name, void *data, size_t size)
{
	FILE *out = fopen(name, "wb");
	int rc;
	if (!out) {
		perror("Failed to open output file");
		exit(1);
	}
	rc = fwrite(data, size, 1, out);
	fclose(out);
	return rc;
}

void *load_file(const char *name, size_t *size)
{
	size_t bufsize = 8192;
	size_t offset = 0;
	char *buf = malloc(bufsize);
	FILE *in;
	if (strcmp(name, "-") == 0)
		in = stdin;
	else
		in = fopen(name, "rb");
	if (!in) {
		perror("Failed to open input file");
		exit(1);
	}
	
	while (true) {
		ssize_t len = bufsize - offset;
		ssize_t n = fread(buf+offset, 1, len, in);
		offset += n;
		if (n < len)
			break;
		bufsize <<= 1;
		buf = realloc(buf, bufsize);
	}
	if (size) 
		*size = offset;
	if (in != stdin)
		fclose(in);
	return buf;
}

void aw_fel_hexdump(felusb_handle *usb, uint32_t offset, size_t size)
{
	unsigned char buf[size];
	aw_fel_read(usb, offset, buf, size);
	hexdump(buf, offset, size);
}

void aw_fel_dump(felusb_handle *usb, uint32_t offset, size_t size)
{
	unsigned char buf[size];
	aw_fel_read(usb, offset, buf, size);
	fwrite(buf, size, 1, stdout);
}
void aw_fel_fill(felusb_handle *usb, uint32_t offset, size_t size, unsigned char value)
{
	unsigned char buf[size];
	memset(buf, value, size);
	aw_write_buffer(usb, buf, offset, size, false);
}

/*
 * The 'sram_swap_buffers' structure is used to describe information about
 * two buffers in SRAM, the content of which needs to be exchanged before
 * calling the U-Boot SPL code and then exchanged again before returning
 * control back to the FEL code from the BROM.
 */

typedef struct {
	uint32_t buf1; /* BROM buffer */
	uint32_t buf2; /* backup storage location */
	uint32_t size; /* buffer size */
} sram_swap_buffers;

/*
 * Each SoC variant may have its own list of memory buffers to be exchanged
 * and the information about the placement of the thunk code, which handles
 * the transition of execution from the BROM FEL code to the U-Boot SPL and
 * back.
 *
 * Note: the entries in the 'swap_buffers' tables need to be sorted by 'buf1'
 * addresses. And the 'buf1' addresses are the BROM data buffers, while 'buf2'
 * addresses are the intended backup locations.
 *
 * Also for performance reasons, we optionally want to have MMU enabled with
 * optimal section attributes configured (the code from the BROM should use
 * I-cache, writing data to the DRAM area should use write combining). The
 * reason is that the BROM FEL protocol implementation moves data using the
 * CPU somewhere on the performance critical path when transferring data over
 * USB. The older SoC variants (A10/A13/A20/A31/A23) already have MMU enabled
 * and we only need to adjust section attributes. The BROM in newer SoC variants
 * (A33/A83T/H3) doesn't enable MMU anymore, so we need to find some 16K of
 * spare space in SRAM to place the translation table there and specify it as
 * the 'mmu_tt_addr' field in the 'soc_sram_info' structure. The 'mmu_tt_addr'
 * address must be 16K aligned.
 */
typedef struct {
	uint32_t           soc_id;       /* ID of the SoC */
	uint32_t           spl_addr;     /* SPL load address */
	uint32_t           scratch_addr; /* A safe place to upload & run code */
	uint32_t           thunk_addr;   /* Address of the thunk code */
	uint32_t           thunk_size;   /* Maximal size of the thunk code */
	bool               needs_l2en;   /* Set the L2EN bit */
	uint32_t           mmu_tt_addr;  /* MMU translation table address */
	uint32_t           sid_addr;     /* base address for SID_KEY[0-3] registers */
	sram_swap_buffers *swap_buffers;
} soc_sram_info;

/*
 * The FEL code from BROM in A10/A13/A20 sets up two stacks for itself. One
 * at 0x2000 (and growing down) for the IRQ handler. And another one at 0x7000
 * (and also growing down) for the regular code. In order to use the whole
 * 32 KiB in the A1/A2 sections of SRAM, we need to temporarily move these
 * stacks elsewhere. And the addresses 0x7D00-0x7FFF contain something
 * importantant too (overwriting them kills FEL). On A10/A13/A20 we can use
 * the SRAM sections A3/A4 (0x8000-0xBFFF) for this purpose.
 */
sram_swap_buffers a10_a13_a20_sram_swap_buffers[] = {
	/* 0x1C00-0x1FFF (IRQ stack) */
	{ .buf1 = 0x01C00, .buf2 = 0xA400, .size = 0x0400 },
	/* 0x5C00-0x6FFF (Stack) */
	{ .buf1 = 0x05C00, .buf2 = 0xA800, .size = 0x1400 },
	/* 0x7C00-0x7FFF (Something important) */
	{ .buf1 = 0x07C00, .buf2 = 0xBC00, .size = 0x0400 },
	{ .size = 0 }  /* End of the table */
};

/*
 * A31 is very similar to A10/A13/A20, except that it has no SRAM at 0x8000.
 * So we use the SRAM section B at 0x20000-0x2FFFF instead. In the FEL mode,
 * the MMU translation table is allocated by the BROM at 0x20000. But we can
 * also safely use it as the backup storage because the MMU is temporarily
 * disabled during the time of the SPL execution.
 */
sram_swap_buffers a31_sram_swap_buffers[] = {
	{ .buf1 = 0x01800, .buf2 = 0x20000, .size = 0x800 },
	{ .buf1 = 0x05C00, .buf2 = 0x20800, .size = 0x8000 - 0x5C00 },
	{ .size = 0 }  /* End of the table */
};

/*
 * A64 has 32KiB of SRAM A at 0x10000 and a large SRAM C at 0x18000. SRAM A
 * and SRAM C reside in the address space back-to-back without any gaps, thus
 * representing a singe large contiguous area. Everything is the same as on
 * A10/A13/A20, but just shifted by 0x10000.
 */
sram_swap_buffers a64_sram_swap_buffers[] = {
	/* 0x11C00-0x11FFF (IRQ stack) */
	{ .buf1 = 0x11C00, .buf2 = 0x1A400, .size = 0x0400 },
	/* 0x15C00-0x16FFF (Stack) */
	{ .buf1 = 0x15C00, .buf2 = 0x1A800, .size = 0x1400 },
	/* 0x17C00-0x17FFF (Something important) */
	{ .buf1 = 0x17C00, .buf2 = 0x1BC00, .size = 0x0400 },
	{ .size = 0 }  /* End of the table */
};

/*
 * Use the SRAM section at 0x44000 as the backup storage. This is the memory,
 * which is normally shared with the OpenRISC core (should we do an extra check
 * to ensure that this core is powered off and can't interfere?).
 */
sram_swap_buffers ar100_abusing_sram_swap_buffers[] = {
	{ .buf1 = 0x01800, .buf2 = 0x44000, .size = 0x800 },
	{ .buf1 = 0x05C00, .buf2 = 0x44800, .size = 0x8000 - 0x5C00 },
	{ .size = 0 }  /* End of the table */
};

/*
 * A80 has 40KiB SRAM A1 at 0x10000 where the SPL has to be loaded to. The
 * secure SRAM B at 0x20000 is used as backup area for FEL stacks and data.
 */
sram_swap_buffers a80_sram_swap_buffers[] = {
	{ .buf1 = 0x11800, .buf2 = 0x20000, .size = 0x800 },
	{ .buf1 = 0x15400, .buf2 = 0x20800, .size = 0x18000 - 0x15400 },
	{ .size = 0 }  /* End of the table */
};

soc_sram_info soc_sram_info_table[] = {
	{
		.soc_id       = 0x1623, /* Allwinner A10 */
		.scratch_addr = 0x1000,
		.thunk_addr   = 0xA200, .thunk_size = 0x200,
		.swap_buffers = a10_a13_a20_sram_swap_buffers,
		.needs_l2en   = true,
		.sid_addr     = 0x01C23800,
	},
	{
		.soc_id       = 0x1625, /* Allwinner A13 */
		.scratch_addr = 0x1000,
		.thunk_addr   = 0xA200, .thunk_size = 0x200,
		.swap_buffers = a10_a13_a20_sram_swap_buffers,
		.needs_l2en   = true,
		.sid_addr     = 0x01C23800,
	},
	{
		.soc_id       = 0x1651, /* Allwinner A20 */
		.scratch_addr = 0x1000,
		.thunk_addr   = 0xA200, .thunk_size = 0x200,
		.swap_buffers = a10_a13_a20_sram_swap_buffers,
		.sid_addr     = 0x01C23800,
	},
	{
		.soc_id       = 0x1650, /* Allwinner A23 */
		.scratch_addr = 0x1000,
		.thunk_addr   = 0x46E00, .thunk_size = 0x200,
		.swap_buffers = ar100_abusing_sram_swap_buffers,
		.sid_addr     = 0x01C23800,
	},
	{
		.soc_id       = 0x1633, /* Allwinner A31 */
		.scratch_addr = 0x1000,
		.thunk_addr   = 0x22E00, .thunk_size = 0x200,
		.swap_buffers = a31_sram_swap_buffers,
	},
	{
		.soc_id       = 0x1667, /* Allwinner A33 */
		.scratch_addr = 0x1000,
		.thunk_addr   = 0x46E00, .thunk_size = 0x200,
		.swap_buffers = ar100_abusing_sram_swap_buffers,
		.sid_addr     = 0x01C23800,
	},
	{
		.soc_id       = 0x1689, /* Allwinner A64 */
		.spl_addr     = 0x10000,
		.scratch_addr = 0x11000,
		.thunk_addr   = 0x1A200, .thunk_size = 0x200,
		.swap_buffers = a64_sram_swap_buffers,
		.sid_addr     = 0x01C14200,
	},
	{
		.soc_id       = 0x1673, /* Allwinner A83T */
		.scratch_addr = 0x1000,
		.thunk_addr   = 0x46E00, .thunk_size = 0x200,
		.swap_buffers = ar100_abusing_sram_swap_buffers,
		.sid_addr     = 0x01C14200,
	},
	{
		.soc_id       = 0x1680, /* Allwinner H3 */
		.scratch_addr = 0x1000,
		.mmu_tt_addr  = 0x8000,
		.thunk_addr   = 0xA200, .thunk_size = 0x200,
		.swap_buffers = a10_a13_a20_sram_swap_buffers,
		.sid_addr     = 0x01C14200,
	},
	{
		.soc_id       = 0x1639, /* Allwinner A80 */
		.spl_addr     = 0x10000,
		.scratch_addr = 0x11000,
		.thunk_addr   = 0x23400, .thunk_size = 0x200,
		.swap_buffers = a80_sram_swap_buffers,
	},
	{ .swap_buffers = NULL } /* End of the table */
};

/*
 * This generic record assumes BROM with similar properties to A10/A13/A20/A31,
 * but no extra SRAM sections beyond 0x8000. It also assumes that the IRQ
 * handler stack usage never exceeds 0x400 bytes.
 *
 * The users may or may not hope that the 0x7000-0x8000 area is also unused
 * by the BROM and re-purpose it for the SPL stack.
 *
 * The size limit for the ".text + .data" sections is ~21 KiB.
 */
sram_swap_buffers generic_sram_swap_buffers[] = {
	{ .buf1 = 0x01C00, .buf2 = 0x5800, .size = 0x400 },
	{ .size = 0 }  /* End of the table */
};

soc_sram_info generic_sram_info = {
	.scratch_addr = 0x1000,
	.thunk_addr   = 0x5680, .thunk_size = 0x180,
	.swap_buffers = generic_sram_swap_buffers,
};

soc_sram_info *aw_fel_get_sram_info(felusb_handle *usb)
{
	/* persistent sram_info, retrieves result pointer once and caches it */
	static soc_sram_info *result = NULL;
	if (result == NULL) {
		int i;

		struct aw_fel_version buf;
		aw_fel_get_version(usb, &buf);

		for (i = 0; soc_sram_info_table[i].swap_buffers; i++)
			if (soc_sram_info_table[i].soc_id == buf.soc_id) {
				result = &soc_sram_info_table[i];
				break;
			}

		if (!result) {
			printf("Warning: no 'soc_sram_info' data for your SoC (id=%04X)\n",
			       buf.soc_id);
			result = &generic_sram_info;
		}
	}
	return result;
}

static uint32_t fel_to_spl_thunk[] = {
	#include "fel-to-spl-thunk.h"
};

#define	DRAM_BASE		0x40000000
#define	DRAM_SIZE		0x80000000

uint32_t aw_read_arm_cp_reg(felusb_handle *usb, soc_sram_info *sram_info,
			    uint32_t coproc, uint32_t opc1, uint32_t crn,
			    uint32_t crm, uint32_t opc2)
{
	uint32_t val = 0;
	uint32_t opcode = 0xEE000000 | (1 << 20) | (1 << 4) |
			  ((opc1 & 7) << 21)    |
			  ((crn & 15) << 16)    |
			  ((coproc & 15) << 8)  |
			  ((opc2 & 7) << 5)     |
			  (crm & 15);
	uint32_t arm_code[] = {
		htole32(opcode),     /* mrc  coproc, opc1, r0, crn, crm, opc2 */
		htole32(0xe58f0000), /* str  r0, [pc]                         */
		htole32(0xe12fff1e), /* bx   lr                               */
	};
	aw_fel_write(usb, arm_code, sram_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, sram_info->scratch_addr);
	aw_fel_read(usb, sram_info->scratch_addr + 12, &val, sizeof(val));
	return le32toh(val);
}

void aw_write_arm_cp_reg(felusb_handle *usb, soc_sram_info *sram_info,
			 uint32_t coproc, uint32_t opc1, uint32_t crn,
			 uint32_t crm, uint32_t opc2, uint32_t val)
{
	uint32_t opcode = 0xEE000000 | (0 << 20) | (1 << 4) |
			  ((opc1 & 7) << 21)                |
			  ((crn & 15) << 16)                |
			  ((coproc & 15) << 8)              |
			  ((opc2 & 7) << 5)                 |
			  (crm & 15);
	uint32_t arm_code[] = {
		htole32(0xe59f000c), /* ldr  r0, [pc, #12]                    */
		htole32(opcode),     /* mcr  coproc, opc1, r0, crn, crm, opc2 */
		htole32(0xf57ff04f), /* dsb  sy                               */
		htole32(0xf57ff06f), /* isb  sy                               */
		htole32(0xe12fff1e), /* bx   lr                               */
		htole32(val)
	};
	aw_fel_write(usb, arm_code, sram_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, sram_info->scratch_addr);
}

/* multiple "readl" from sequential addresses to a destination buffer */
void aw_fel_readl_n(felusb_handle *usb, uint32_t addr,
		    uint32_t *dst, size_t count)
{
	soc_sram_info *sram_info = aw_fel_get_sram_info(usb);
	uint32_t val;
	uint32_t arm_code[] = {
		htole32(0xe59f0010), /* ldr        r0, [pc, #16]            */
		htole32(0xe5901000), /* ldr        r1, [r0]                 */
		htole32(0xe58f100c), /* str        r1, [pc, #12]            */
		htole32(0xe2800004), /* add        r0, r0, #4               */
		htole32(0xe58f0000), /* str        r0, [pc]                 */
		htole32(0xe12fff1e), /* bx         lr                       */
		htole32(addr),
		/* value goes here */
	};
	/* scratch buffer setup: transfers ARM code and also sets the addr */
	aw_fel_write(usb, arm_code, sram_info->scratch_addr, sizeof(arm_code));
	while (count-- > 0) {
		/*
		 * Since the scratch code auto-increments addr, we can simply
		 * execute it repeatedly for sequential "readl"s; retrieving
		 * one uint32_t each time.
		 */
		aw_fel_execute(usb, sram_info->scratch_addr);
		aw_fel_read(usb, sram_info->scratch_addr + 28, &val, sizeof(val));
		*dst++ = le32toh(val);
	}
}

/* "readl" of a single value */
uint32_t aw_fel_readl(felusb_handle *usb, uint32_t addr)
{
	uint32_t val;
	aw_fel_readl_n(usb, addr, &val, 1);
	return val;
}

/* multiple "writel" from a source buffer to sequential addresses */
void aw_fel_writel_n(felusb_handle *usb, uint32_t addr,
		     uint32_t *src, size_t count)
{
	if (count == 0) return; /* on zero count, do not access *src at all */

	soc_sram_info *sram_info = aw_fel_get_sram_info(usb);
	uint32_t arm_code[] = {
		htole32(0xe59f0010), /* ldr        r0, [pc, #16]            */
		htole32(0xe59f1010), /* ldr        r1, [pc, #16]            */
		htole32(0xe5801000), /* str        r1, [r0]                 */
		htole32(0xe2800004), /* add        r0, r0, #4               */
		htole32(0xe58f0000), /* str        r0, [pc]                 */
		htole32(0xe12fff1e), /* bx         lr                       */
		htole32(addr),
		htole32(*src++)
	};
	/* scratch buffer setup: transfers ARM code, addr and first value */
	aw_fel_write(usb, arm_code, sram_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, sram_info->scratch_addr); /* stores first value */
	while (--count > 0) {
		/*
		 * Subsequent transfers only need to set up the next value
		 * to store (since the scratch code auto-increments addr).
		 */
		aw_fel_write(usb, src++, sram_info->scratch_addr + 28, sizeof(uint32_t));
		aw_fel_execute(usb, sram_info->scratch_addr);
	}
}

/* "writel" of a single value */
void aw_fel_writel(felusb_handle *usb, uint32_t addr, uint32_t val)
{
	aw_fel_writel_n(usb, addr, &val, 1);
}

void aw_fel_print_sid(felusb_handle *usb)
{
	soc_sram_info *soc_info = aw_fel_get_sram_info(usb);
	if (soc_info->sid_addr) {
		pr_info("SID key (e-fuses) at 0x%08X\n", soc_info->sid_addr);

		uint32_t key[4];
		aw_fel_readl_n(usb, soc_info->sid_addr, key, 4);

		unsigned int i;
		/* output SID in "xxxxxxxx:xxxxxxxx:xxxxxxxx:xxxxxxxx" format */
		for (i = 0; i <= 3; i++)
			printf("%08x%c", key[i], i < 3 ? ':' : '\n');
	} else {
		printf("SID registers for your SoC (id=%04X) are unknown or inaccessible.\n",
			soc_info->soc_id);
	}
}

void aw_enable_l2_cache(felusb_handle *usb, soc_sram_info *sram_info)
{
	uint32_t arm_code[] = {
		htole32(0xee112f30), /* mrc        15, 0, r2, cr1, cr0, {1}  */
		htole32(0xe3822002), /* orr        r2, r2, #2                */
		htole32(0xee012f30), /* mcr        15, 0, r2, cr1, cr0, {1}  */
		htole32(0xe12fff1e), /* bx         lr                        */
	};

	aw_fel_write(usb, arm_code, sram_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, sram_info->scratch_addr);
}

void aw_get_stackinfo(felusb_handle *usb, soc_sram_info *sram_info,
                      uint32_t *sp_irq, uint32_t *sp)
{
	uint32_t results[2] = { 0 };
#if 0
	/* Does not work on Cortex-A8 (needs Virtualization Extensions) */
	uint32_t arm_code[] = {
		htole32(0xe1010300), /* mrs        r0, SP_irq                */
		htole32(0xe58f0004), /* str        r0, [pc, #4]              */
		htole32(0xe58fd004), /* str        sp, [pc, #4]              */
		htole32(0xe12fff1e), /* bx         lr                        */
	};

	aw_fel_write(usb, arm_code, sram_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, sram_info->scratch_addr);
	aw_fel_read(usb, sram_info->scratch_addr + 0x10, results, 8);
#else
	/* Works everywhere */
	uint32_t arm_code[] = {
		htole32(0xe10f0000), /* mrs        r0, CPSR                  */
		htole32(0xe3c0101f), /* bic        r1, r0, #31               */
		htole32(0xe3811012), /* orr        r1, r1, #18               */
		htole32(0xe121f001), /* msr        CPSR_c, r1                */
		htole32(0xe1a0100d), /* mov        r1, sp                    */
		htole32(0xe121f000), /* msr        CPSR_c, r0                */
		htole32(0xe58f1004), /* str        r1, [pc, #4]              */
		htole32(0xe58fd004), /* str        sp, [pc, #4]              */
		htole32(0xe12fff1e), /* bx         lr                        */
	};

	aw_fel_write(usb, arm_code, sram_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, sram_info->scratch_addr);
	aw_fel_read(usb, sram_info->scratch_addr + 0x24, results, 8);
#endif
	*sp_irq = le32toh(results[0]);
	*sp     = le32toh(results[1]);
}

uint32_t aw_get_ttbr0(felusb_handle *usb, soc_sram_info *sram_info)
{
	return aw_read_arm_cp_reg(usb, sram_info, 15, 0, 2, 0, 0);
}

uint32_t aw_get_ttbcr(felusb_handle *usb, soc_sram_info *sram_info)
{
	return aw_read_arm_cp_reg(usb, sram_info, 15, 0, 2, 0, 2);
}

uint32_t aw_get_dacr(felusb_handle *usb, soc_sram_info *sram_info)
{
	return aw_read_arm_cp_reg(usb, sram_info, 15, 0, 3, 0, 0);
}

uint32_t aw_get_sctlr(felusb_handle *usb, soc_sram_info *sram_info)
{
	return aw_read_arm_cp_reg(usb, sram_info, 15, 0, 1, 0, 0);
}

void aw_set_ttbr0(felusb_handle *usb, soc_sram_info *sram_info,
		  uint32_t ttbr0)
{
	return aw_write_arm_cp_reg(usb, sram_info, 15, 0, 2, 0, 0, ttbr0);
}

void aw_set_ttbcr(felusb_handle *usb, soc_sram_info *sram_info,
		  uint32_t ttbcr)
{
	return aw_write_arm_cp_reg(usb, sram_info, 15, 0, 2, 0, 2, ttbcr);
}

void aw_set_dacr(felusb_handle *usb, soc_sram_info *sram_info,
		 uint32_t dacr)
{
	aw_write_arm_cp_reg(usb, sram_info, 15, 0, 3, 0, 0, dacr);
}

void aw_set_sctlr(felusb_handle *usb, soc_sram_info *sram_info,
		  uint32_t sctlr)
{
	aw_write_arm_cp_reg(usb, sram_info, 15, 0, 1, 0, 0, sctlr);
}

/*
 * Reconstruct the same MMU translation table as used by the A20 BROM.
 * We are basically reverting the changes, introduced in newer SoC
 * variants. This works fine for the SoC variants with the memory
 * layout similar to A20 (the SRAM is in the first megabyte of the
 * address space and the BROM is in the last megabyte of the address
 * space).
 */
uint32_t *aw_generate_mmu_translation_table(void)
{
	uint32_t *tt = malloc(4096 * sizeof(uint32_t));
	uint32_t i;

	/*
	 * Direct mapping using 1MB sections with TEXCB=00000 (Strongly
	 * ordered) for all memory except the first and the last sections,
	 * which have TEXCB=00100 (Normal). Domain bits are set to 1111
	 * and AP bits are set to 11, but this is mostly irrelevant.
	 */
	for (i = 0; i < 4096; i++)
		tt[i] = 0x00000DE2 | (i << 20);
	tt[0x000] |= 0x1000;
	tt[0xFFF] |= 0x1000;

	return tt;
}

uint32_t *aw_backup_and_disable_mmu(felusb_handle *usb,
                                    soc_sram_info *sram_info)
{
	uint32_t *tt = NULL;
	uint32_t sctlr, ttbr0, ttbcr, dacr;
	uint32_t i;

	uint32_t arm_code[] = {
		/* Disable I-cache, MMU and branch prediction */
		htole32(0xee110f10), /* mrc        15, 0, r0, cr1, cr0, {0}  */
		htole32(0xe3c00001), /* bic        r0, r0, #1                */
		htole32(0xe3c00a01), /* bic        r0, r0, #4096             */
		htole32(0xe3c00b02), /* bic        r0, r0, #2048             */
		htole32(0xee010f10), /* mcr        15, 0, r0, cr1, cr0, {0}  */
		/* Return back to FEL */
		htole32(0xe12fff1e), /* bx         lr                        */
	};

	/*
	 * Below are some checks for the register values, which are known
	 * to be initialized in this particular way by the existing BROM
	 * implementations. We don't strictly need them to exactly match,
	 * but still have these safety guards in place in order to detect
	 * and review any potential configuration changes in future SoC
	 * variants (if one of these checks fails, then it is not a serious
	 * problem but more likely just an indication that one of these
	 * checks needs to be relaxed).
	 */

	/* Basically, ignore M/Z/I/V/UNK bits and expect no TEX remap */
	sctlr = aw_get_sctlr(usb, sram_info);
	if ((sctlr & ~((0x7 << 11) | (1 << 6) | 1)) != 0x00C50038) {
		fprintf(stderr, "Unexpected SCTLR (%08X)\n", sctlr);
		exit(1);
	}

	if (!(sctlr & 1)) {
		pr_info("MMU is not enabled by BROM\n");
		return NULL;
	}

	dacr = aw_get_dacr(usb, sram_info);
	if (dacr != 0x55555555) {
		fprintf(stderr, "Unexpected DACR (%08X)\n", dacr);
		exit(1);
	}

	ttbcr = aw_get_ttbcr(usb, sram_info);
	if (ttbcr != 0x00000000) {
		fprintf(stderr, "Unexpected TTBCR (%08X)\n", ttbcr);
		exit(1);
	}

	ttbr0 = aw_get_ttbr0(usb, sram_info);
	if (ttbr0 & 0x3FFF) {
		fprintf(stderr, "Unexpected TTBR0 (%08X)\n", ttbr0);
		exit(1);
	}

	tt = malloc(16 * 1024);
	pr_info("Reading the MMU translation table from 0x%08X\n", ttbr0);
	aw_fel_read(usb, ttbr0, tt, 16 * 1024);
	for (i = 0; i < 4096; i++)
		tt[i] = le32toh(tt[i]);

	/* Basic sanity checks to be sure that this is a valid table */
	for (i = 0; i < 4096; i++) {
		if (((tt[i] >> 1) & 1) != 1 || ((tt[i] >> 18) & 1) != 0) {
			fprintf(stderr, "MMU: not a section descriptor\n");
			exit(1);
		}
		if ((tt[i] >> 20) != i) {
			fprintf(stderr, "MMU: not a direct mapping\n");
			exit(1);
		}
	}

	pr_info("Disabling I-cache, MMU and branch prediction...");
	aw_fel_write(usb, arm_code, sram_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, sram_info->scratch_addr);
	pr_info(" done.\n");

	return tt;
}

void aw_restore_and_enable_mmu(felusb_handle *usb,
                               soc_sram_info *sram_info,
                               uint32_t *tt)
{
	uint32_t i;
	uint32_t ttbr0 = aw_get_ttbr0(usb, sram_info);

	uint32_t arm_code[] = {
		/* Invalidate I-cache, TLB and BTB */
		htole32(0xe3a00000), /* mov        r0, #0                    */
		htole32(0xee080f17), /* mcr        15, 0, r0, cr8, cr7, {0}  */
		htole32(0xee070f15), /* mcr        15, 0, r0, cr7, cr5, {0}  */
		htole32(0xee070fd5), /* mcr        15, 0, r0, cr7, cr5, {6}  */
		htole32(0xf57ff04f), /* dsb        sy                        */
		htole32(0xf57ff06f), /* isb        sy                        */
		/* Enable I-cache, MMU and branch prediction */
		htole32(0xee110f10), /* mrc        15, 0, r0, cr1, cr0, {0}  */
		htole32(0xe3800001), /* orr        r0, r0, #1                */
		htole32(0xe3800a01), /* orr        r0, r0, #4096             */
		htole32(0xe3800b02), /* orr        r0, r0, #2048             */
		htole32(0xee010f10), /* mcr        15, 0, r0, cr1, cr0, {0}  */
		/* Return back to FEL */
		htole32(0xe12fff1e), /* bx         lr                        */
	};

	pr_info("Setting write-combine mapping for DRAM.\n");
	for (i = (DRAM_BASE >> 20); i < ((DRAM_BASE + DRAM_SIZE) >> 20); i++) {
		/* Clear TEXCB bits */
		tt[i] &= ~((7 << 12) | (1 << 3) | (1 << 2));
		/* Set TEXCB to 00100 (Normal uncached mapping) */
		tt[i] |= (1 << 12);
	}

	pr_info("Setting cached mapping for BROM.\n");
	/* Clear TEXCB bits first */
	tt[0xFFF] &= ~((7 << 12) | (1 << 3) | (1 << 2));
	/* Set TEXCB to 00111 (Normal write-back cached mapping) */
	tt[0xFFF] |= (1 << 12) | /* TEX */
		     (1 << 3)  | /* C */
		     (1 << 2);   /* B */

	pr_info("Writing back the MMU translation table.\n");
	for (i = 0; i < 4096; i++)
		tt[i] = htole32(tt[i]);
	aw_fel_write(usb, tt, ttbr0, 16 * 1024);

	pr_info("Enabling I-cache, MMU and branch prediction...");
	aw_fel_write(usb, arm_code, sram_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, sram_info->scratch_addr);
	pr_info(" done.\n");

	free(tt);
}

/*
 * Maximum size of SPL, at the same time this is the start offset
 * of the main U-Boot image within u-boot-sunxi-with-spl.bin
 */
#define SPL_LEN_LIMIT 0x8000

void aw_fel_write_and_execute_spl(felusb_handle *usb, uint8_t *buf, size_t len)
{
	soc_sram_info *sram_info = aw_fel_get_sram_info(usb);
	sram_swap_buffers *swap_buffers;
	char header_signature[9] = { 0 };
	size_t i, thunk_size;
	uint32_t *thunk_buf;
	uint32_t sp, sp_irq;
	uint32_t spl_checksum, spl_len, spl_len_limit = SPL_LEN_LIMIT;
	uint32_t *buf32 = (uint32_t *)buf;
	uint32_t cur_addr = sram_info->spl_addr;
	uint32_t *tt = NULL;

	if (!sram_info || !sram_info->swap_buffers) {
		fprintf(stderr, "SPL: Unsupported SoC type\n");
		exit(1);
	}

	if (len < 32 || memcmp(buf + 4, "eGON.BT0", 8) != 0) {
		fprintf(stderr, "SPL: eGON header is not found\n");
		exit(1);
	}

	spl_checksum = 2 * le32toh(buf32[3]) - 0x5F0A6C39;
	spl_len = le32toh(buf32[4]);

	if (spl_len > len || (spl_len % 4) != 0) {
		fprintf(stderr, "SPL: bad length in the eGON header\n");
		exit(1);
	}

	len = spl_len;
	for (i = 0; i < len / 4; i++)
		spl_checksum -= le32toh(buf32[i]);

	if (spl_checksum != 0) {
		fprintf(stderr, "SPL: checksum check failed\n");
		exit(1);
	}

	if (sram_info->needs_l2en) {
		pr_info("Enabling the L2 cache\n");
		aw_enable_l2_cache(usb, sram_info);
	}

	aw_get_stackinfo(usb, sram_info, &sp_irq, &sp);
	pr_info("Stack pointers: sp_irq=0x%08X, sp=0x%08X\n", sp_irq, sp);

	tt = aw_backup_and_disable_mmu(usb, sram_info);
	if (!tt && sram_info->mmu_tt_addr) {
		if (sram_info->mmu_tt_addr & 0x3FFF) {
			fprintf(stderr, "SPL: 'mmu_tt_addr' must be 16K aligned\n");
			exit(1);
		}
		pr_info("Generating the new MMU translation table at 0x%08X\n",
		        sram_info->mmu_tt_addr);
		/*
		 * These settings are used by the BROM in A10/A13/A20 and
		 * we replicate them here when enabling the MMU. The DACR
		 * value 0x55555555 means that accesses are checked against
		 * the permission bits in the translation tables for all
		 * domains. The TTBCR value 0x00000000 means that the short
		 * descriptor translation table format is used, TTBR0 is used
		 * for all the possible virtual addresses (N=0) and that the
		 * translation table must be aligned at a 16K boundary.
		 */
		aw_set_dacr(usb, sram_info, 0x55555555);
		aw_set_ttbcr(usb, sram_info, 0x00000000);
		aw_set_ttbr0(usb, sram_info, sram_info->mmu_tt_addr);
		tt = aw_generate_mmu_translation_table();
	}

	swap_buffers = sram_info->swap_buffers;
	for (i = 0; swap_buffers[i].size; i++) {
		if ((swap_buffers[i].buf2 >= sram_info->spl_addr) &&
		    (swap_buffers[i].buf2 < sram_info->spl_addr + spl_len_limit))
			spl_len_limit = swap_buffers[i].buf2 - sram_info->spl_addr;
		if (len > 0 && cur_addr < swap_buffers[i].buf1) {
			uint32_t tmp = swap_buffers[i].buf1 - cur_addr;
			if (tmp > len)
				tmp = len;
			aw_fel_write(usb, buf, cur_addr, tmp);
			cur_addr += tmp;
			buf += tmp;
			len -= tmp;
		}
		if (len > 0 && cur_addr == swap_buffers[i].buf1) {
			uint32_t tmp = swap_buffers[i].size;
			if (tmp > len)
				tmp = len;
			aw_fel_write(usb, buf, swap_buffers[i].buf2, tmp);
			cur_addr += tmp;
			buf += tmp;
			len -= tmp;
		}
	}

	/* Clarify the SPL size limitations, and bail out if they are not met */
	if (sram_info->thunk_addr < spl_len_limit)
		spl_len_limit = sram_info->thunk_addr;

	if (spl_len > spl_len_limit) {
		fprintf(stderr, "SPL: too large (need %d, have %d)\n",
			(int)spl_len, (int)spl_len_limit);
		exit(1);
	}

	/* Write the remaining part of the SPL */
	if (len > 0)
		aw_fel_write(usb, buf, cur_addr, len);

	thunk_size = sizeof(fel_to_spl_thunk) + sizeof(sram_info->spl_addr) +
		     (i + 1) * sizeof(*swap_buffers);

	if (thunk_size > sram_info->thunk_size) {
		fprintf(stderr, "SPL: bad thunk size (need %d, have %d)\n",
			(int)sizeof(fel_to_spl_thunk), sram_info->thunk_size);
		exit(1);
	}

	thunk_buf = malloc(thunk_size);
	memcpy(thunk_buf, fel_to_spl_thunk, sizeof(fel_to_spl_thunk));
	memcpy(thunk_buf + sizeof(fel_to_spl_thunk) / sizeof(uint32_t),
	       &sram_info->spl_addr, sizeof(sram_info->spl_addr));
	memcpy(thunk_buf + sizeof(fel_to_spl_thunk) / sizeof(uint32_t) + 1,
	       swap_buffers, (i + 1) * sizeof(*swap_buffers));

	for (i = 0; i < thunk_size / sizeof(uint32_t); i++)
		thunk_buf[i] = htole32(thunk_buf[i]);

	pr_info("=> Executing the SPL...");
	aw_fel_write(usb, thunk_buf, sram_info->thunk_addr, thunk_size);
	aw_fel_execute(usb, sram_info->thunk_addr);
	pr_info(" done.\n");

	free(thunk_buf);

	/* TODO: Try to find and fix the bug, which needs this workaround */
	usleep(250000);

	/* Read back the result and check if everything was fine */
	aw_fel_read(usb, sram_info->spl_addr + 4, header_signature, 8);
	if (strcmp(header_signature, "eGON.FEL") != 0) {
		fprintf(stderr, "SPL: failure code '%s'\n",
			header_signature);
		exit(1);
	}

	/* re-enable the MMU if it was enabled by BROM */
	if (tt != NULL)
		aw_restore_and_enable_mmu(usb, sram_info, tt);
}

/*
 * This function tests a given buffer address and length for a valid U-Boot
 * image. Upon success, the image data gets transferred to the default memory
 * address stored within the image header; and the function preserves the
 * U-Boot entry point (offset) and size values.
 */
void aw_fel_write_uboot_image(felusb_handle *usb, uint8_t *buf, size_t len)
{
	if (len <= HEADER_SIZE)
		return; /* Insufficient size (no actual data), just bail out */

	uint32_t *buf32 = (uint32_t *)buf;

	/* Check for a valid mkimage header */
	int image_type = get_image_type(buf, len);
	if (image_type <= IH_TYPE_INVALID) {
		switch (image_type) {
		case IH_TYPE_INVALID:
			fprintf(stderr, "Invalid U-Boot image: bad size or signature\n");
			break;
		case IH_TYPE_ARCH_MISMATCH:
			fprintf(stderr, "Invalid U-Boot image: wrong architecture\n");
			break;
		default:
			fprintf(stderr, "Invalid U-Boot image: error code %d\n",
				image_type);
		}
		exit(1);
	}
	if (image_type != IH_TYPE_FIRMWARE) {
		fprintf(stderr, "U-Boot image type mismatch: "
			"expected IH_TYPE_FIRMWARE, got %02X\n", image_type);
		exit(1);
	}
	uint32_t data_size = be32toh(buf32[3]); /* Image Data Size */
	uint32_t load_addr = be32toh(buf32[4]); /* Data Load Address */
	if (data_size != len - HEADER_SIZE) {
		fprintf(stderr, "U-Boot image data size mismatch: "
			"expected %zu, got %u\n", len - HEADER_SIZE, data_size);
		exit(1);
	}
	/* TODO: Verify image data integrity using the checksum field ih_dcrc,
	 * available from be32toh(buf32[6])
	 *
	 * However, this requires CRC routines that mimic their U-Boot
	 * counterparts, namely image_check_dcrc() in ${U-BOOT}/common/image.c
	 * and crc_wd() in ${U-BOOT}/lib/crc32.c
	 *
	 * It should be investigated if existing CRC routines in sunxi-tools
	 * could be factored out and reused for this purpose - e.g. calc_crc32()
	 * from nand-part-main.c
	 */

	/* If we get here, we're "good to go" (i.e. actually write the data) */
	pr_info("Writing image \"%.*s\", %u bytes @ 0x%08X.\n",
		IH_NMLEN, buf + HEADER_NAME_OFFSET, data_size, load_addr);

	aw_write_buffer(usb, buf + HEADER_SIZE, load_addr, data_size, false);

	/* keep track of U-Boot memory region in global vars */
	uboot_entry = load_addr;
	uboot_size = data_size;
}

/*
 * This function handles the common part of both "spl" and "uboot" commands.
 */
void aw_fel_process_spl_and_uboot(felusb_handle *usb, const char *filename)
{
	/* load file into memory buffer */
	size_t size;
	uint8_t *buf = load_file(filename, &size);
	/* write and execute the SPL from the buffer */
	aw_fel_write_and_execute_spl(usb, buf, size);
	/* check for optional main U-Boot binary (and transfer it, if applicable) */
	if (size > SPL_LEN_LIMIT)
		aw_fel_write_uboot_image(usb, buf + SPL_LEN_LIMIT, size - SPL_LEN_LIMIT);
	free(buf);
}

/*
 * Test the SPL header for our "sunxi" variant. We want to make sure that
 * we can safely use specific header fields to pass information to U-Boot.
 * In case of a missing signature (e.g. Allwinner boot0) or header version
 * mismatch, this function will return "false". If all seems fine,
 * the result is "true".
 */
#define SPL_SIGNATURE			"SPL" /* marks "sunxi" header */
#define SPL_MIN_VERSION			1 /* minimum required version */
#define SPL_MAX_VERSION			1 /* maximum supported version */
bool have_sunxi_spl(felusb_handle *usb, uint32_t spl_addr)
{
	uint8_t spl_signature[4];

	aw_fel_read(usb, spl_addr + 0x14,
		&spl_signature, sizeof(spl_signature));

	if (memcmp(spl_signature, SPL_SIGNATURE, 3) != 0)
		return false; /* signature mismatch, no "sunxi" SPL */

	if (spl_signature[3] < SPL_MIN_VERSION) {
		fprintf(stderr, "sunxi SPL version mismatch: "
			"found 0x%02X < required minimum 0x%02X\n",
			spl_signature[3], SPL_MIN_VERSION);
		fprintf(stderr, "You need to update your U-Boot (mksunxiboot) to a more recent version.\n");
		return false;
	}
	if (spl_signature[3] > SPL_MAX_VERSION) {
		fprintf(stderr, "sunxi SPL version mismatch: "
			"found 0x%02X > maximum supported 0x%02X\n",
			spl_signature[3], SPL_MAX_VERSION);
		fprintf(stderr, "You need a more recent version of this (sunxi-tools) fel utility.\n");
		return false;
	}
	return true; /* sunxi SPL and suitable version */
}

/*
 * Pass information to U-Boot via specialized fields in the SPL header
 * (see "boot_file_head" in ${U-BOOT}/arch/arm/include/asm/arch-sunxi/spl.h),
 * providing the boot script address (DRAM location of boot.scr).
 */
void pass_fel_information(felusb_handle *usb, uint32_t script_address)
{
	soc_sram_info *sram_info = aw_fel_get_sram_info(usb);

	/* write something _only_ if we have a suitable SPL header */
	if (have_sunxi_spl(usb, sram_info->spl_addr)) {
		pr_info("Passing boot info via sunxi SPL: script address = 0x%08X\n",
			script_address);
		aw_fel_write(usb, &script_address,
			sram_info->spl_addr + 0x18, sizeof(script_address));
	}
}

/* private helper function, gets used for "write*" and "multi*" transfers */
static unsigned int file_upload(felusb_handle *handle, size_t count,
				size_t argc, char **argv, progress_cb_t progress)
{
	if (argc < count * 2) {
		fprintf(stderr, "error: too few arguments for uploading %zu files\n",
			count);
		exit(1);
	}

	/* get all file sizes, keeping track of total bytes */
	size_t size = 0;
	unsigned int i;
	for (i = 0; i < count; i++)
		size += file_size(argv[i * 2 + 1]);

	progress_start(progress, size); /* set total size and progress callback */

	/* now transfer each file in turn */
	for (i = 0; i < count; i++) {
		void *buf = load_file(argv[i * 2 + 1], &size);
		if (size > 0) {
			uint32_t offset = strtoul(argv[i * 2], NULL, 0);
			aw_write_buffer(handle, buf, offset, size, true);

			/* If we transferred a script, try to inform U-Boot about its address. */
			if (get_image_type(buf, size) == IH_TYPE_SCRIPT)
				pass_fel_information(handle, offset);
		}
		free(buf);
	}

	return i; /* return number of files that were processed */
}

int main(int argc, char **argv)
{
	bool uboot_autostart = false; /* flag for "uboot" command = U-Boot autostart */
	bool pflag_active = false; /* -p switch, causing "write" to output progress */
	felusb_handle *handle;
	int busnum = -1, devnum = -1;

	if (argc <= 1) {
		printf("Usage: %s [options] command arguments... [command...]\n"
			"	-v, --verbose			Verbose logging\n"
			"	-p, --progress			\"write\" transfers show a progress bar\n"
			"	-d, --dev bus:devnum		Use specific USB bus and device number\n"
			"\n"
			"	spl file			Load and execute U-Boot SPL\n"
			"		If file additionally contains a main U-Boot binary\n"
			"		(u-boot-sunxi-with-spl.bin), this command also transfers that\n"
			"		to memory (default address from image), but won't execute it.\n"
			"\n"
			"	uboot file-with-spl		like \"spl\", but actually starts U-Boot\n"
			"		U-Boot execution will take place when the fel utility exits.\n"
			"		This allows combining \"uboot\" with further \"write\" commands\n"
			"		(to transfer other files needed for the boot).\n"
			"\n"
			"	hex[dump] address length	Dumps memory region in hex\n"
			"	dump address length		Binary memory dump\n"
			"	exe[cute] address		Call function address\n"
			"	readl address			Read 32-bit value from device memory\n"
			"	writel address value		Write 32-bit value to device memory\n"
			"	read address length file	Write memory contents into file\n"
			"	write address file		Store file contents into memory\n"
			"	write-with-progress addr file	\"write\" with progress bar\n"
			"	write-with-gauge addr file	Output progress for \"dialog --gauge\"\n"
			"	write-with-xgauge addr file	Extended gauge output (updates prompt)\n"
			"	multi[write] # addr file ...	\"write-with-progress\" multiple files,\n"
			"					sharing a common progress status\n"
			"	multi[write]-with-gauge ...	like their \"write-with-*\" counterpart,\n"
			"	multi[write]-with-xgauge ...	  but following the 'multi' syntax:\n"
			"					  <#> addr file [addr file [...]]\n"
			"	echo-gauge \"some text\"		Update prompt/caption for gauge output\n"
			"	ver[sion]			Show BROM version\n"
			"	sid				Retrieve and output 128-bit SID key\n"
			"	clear address length		Clear memory\n"
			"	fill address length value	Fill memory\n"
			, argv[0]
		);
		exit(0);
	}

	/* process all "prefix"-type arguments first */
	while (argc > 1) {
		if (strcmp(argv[1], "--verbose") == 0 || strcmp(argv[1], "-v") == 0)
			verbose = true;
		else if (strcmp(argv[1], "--progress") == 0 || strcmp(argv[1], "-p") == 0)
			pflag_active = true;
		else if (strncmp(argv[1], "--dev", 5) == 0 || strncmp(argv[1], "-d", 2) == 0) {
			char *dev_arg = argv[1];
			dev_arg += strspn(dev_arg, "-dev="); /* skip option chars, ignore '=' */
			if (*dev_arg == 0 && argc > 2) { /* at end of argument, use the next one instead */
				dev_arg = argv[2];
				argc -= 1;
				argv += 1;
			}
			if (sscanf(dev_arg, "%d:%d", &busnum, &devnum) != 2
			    || busnum <= 0 || devnum <= 0) {
				fprintf(stderr, "ERROR: Expected 'bus:devnum', got '%s'.\n", dev_arg);
				exit(1);
			}
			pr_info("Selecting USB Bus %03d Device %03d\n", busnum, devnum);
		} else
			break; /* no valid (prefix) option detected, exit loop */
		argc -= 1;
		argv += 1;
	}

	felusb_init();
	handle = open_fel_device(busnum, devnum, AW_USB_VENDOR_ID, AW_USB_PRODUCT_ID);

	while (argc > 1 ) {
		int skip = 1;

		if (strncmp(argv[1], "hex", 3) == 0 && argc > 3) {
			aw_fel_hexdump(handle, strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
			skip = 3;
		} else if (strncmp(argv[1], "dump", 4) == 0 && argc > 3) {
			aw_fel_dump(handle, strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
			skip = 3;
		} else if (strcmp(argv[1], "readl") == 0 && argc > 2) {
			printf("0x%08x\n", aw_fel_readl(handle, strtoul(argv[2], NULL, 0)));
			skip = 2;
		} else if (strcmp(argv[1], "writel") == 0 && argc > 3) {
			aw_fel_writel(handle, strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
			skip = 3;
		} else if (strncmp(argv[1], "exe", 3) == 0 && argc > 2) {
			aw_fel_execute(handle, strtoul(argv[2], NULL, 0));
			skip=3;
		} else if (strncmp(argv[1], "ver", 3) == 0) {
			aw_fel_print_version(handle);
		} else if (strcmp(argv[1], "sid") == 0) {
			aw_fel_print_sid(handle);
		} else if (strcmp(argv[1], "write") == 0 && argc > 3) {
			skip += 2 * file_upload(handle, 1, argc - 2, argv + 2,
					pflag_active ? progress_bar : NULL);
		} else if (strcmp(argv[1], "write-with-progress") == 0 && argc > 3) {
			skip += 2 * file_upload(handle, 1, argc - 2, argv + 2,
						progress_bar);
		} else if (strcmp(argv[1], "write-with-gauge") == 0 && argc > 3) {
			skip += 2 * file_upload(handle, 1, argc - 2, argv + 2,
						progress_gauge);
		} else if (strcmp(argv[1], "write-with-xgauge") == 0 && argc > 3) {
			skip += 2 * file_upload(handle, 1, argc - 2, argv + 2,
						progress_gauge_xxx);
		} else if ((strcmp(argv[1], "multiwrite") == 0 ||
			    strcmp(argv[1], "multi") == 0) && argc > 4) {
			size_t count = strtoul(argv[2], NULL, 0); /* file count */
			skip = 2 + 2 * file_upload(handle, count, argc - 3,
						   argv + 3, progress_bar);
		} else if ((strcmp(argv[1], "multiwrite-with-gauge") == 0 ||
			    strcmp(argv[1], "multi-with-gauge") == 0) && argc > 4) {
			size_t count = strtoul(argv[2], NULL, 0); /* file count */
			skip = 2 + 2 * file_upload(handle, count, argc - 3,
						   argv + 3, progress_gauge);
		} else if ((strcmp(argv[1], "multiwrite-with-xgauge") == 0 ||
			    strcmp(argv[1], "multi-with-xgauge") == 0) && argc > 4) {
			size_t count = strtoul(argv[2], NULL, 0); /* file count */
			skip = 2 + 2 * file_upload(handle, count, argc - 3,
						   argv + 3, progress_gauge_xxx);
		} else if ((strcmp(argv[1], "echo-gauge") == 0) && argc > 2) {
			skip = 2;
			printf("XXX\n0\n%s\nXXX\n", argv[2]);
			fflush(stdout);
		} else if (strcmp(argv[1], "read") == 0 && argc > 4) {
			size_t size = strtoul(argv[3], NULL, 0);
			void *buf = malloc(size);
			aw_fel_read(handle, strtoul(argv[2], NULL, 0), buf, size);
			save_file(argv[4], buf, size);
			free(buf);
			skip=4;
		} else if (strcmp(argv[1], "clear") == 0 && argc > 2) {
			aw_fel_fill(handle, strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0), 0);
			skip=3;
		} else if (strcmp(argv[1], "fill") == 0 && argc > 3) {
			aw_fel_fill(handle, strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0), (unsigned char)strtoul(argv[4], NULL, 0));
			skip=4;
		} else if (strcmp(argv[1], "spl") == 0 && argc > 2) {
			aw_fel_process_spl_and_uboot(handle, argv[2]);
			skip=2;
		} else if (strcmp(argv[1], "uboot") == 0 && argc > 2) {
			aw_fel_process_spl_and_uboot(handle, argv[2]);
			uboot_autostart = (uboot_entry > 0 && uboot_size > 0);
			if (!uboot_autostart)
				printf("Warning: \"uboot\" command failed to detect image! Can't execute U-Boot.\n");
			skip=2;
		} else {
			fprintf(stderr,"Invalid command %s\n", argv[1]);
			exit(1);
		}
		argc-=skip;
		argv+=skip;
	}

	/* auto-start U-Boot if requested (by the "uboot" command) */
	if (uboot_autostart) {
		pr_info("Starting U-Boot (0x%08X).\n", uboot_entry);
		aw_fel_execute(handle, uboot_entry);
	}

	felusb_done(handle);

	return 0;
}
