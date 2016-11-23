/* To be included by arch/s390/tools/gen_facilities.c */
	{
		.name = "FACILITIES_KVM",
		.bits = (int[]){
			0,  /* N3 instructions */
			1,  /* z/Arch mode installed */
			2,  /* z/Arch mode active */
			3,  /* DAT-enhancement */
			4,  /* idte segment table */
			5,  /* idte region table */
			6,  /* ASN-and-LX reuse */
			7,  /* stfle */
			8,  /* enhanced-DAT 1 */
			9,  /* sense-running-status */
			10, /* conditional sske */
			13, /* ipte-range */
			14, /* nonquiescing key-setting */
			73, /* transactional execution */
			75, /* access-exception-fetch/store indication */
			76, /* msa extension 3 */
			77, /* msa extension 4 */
			78, /* enhanced-DAT 2 */
			-1  /* END */
		}
	},

