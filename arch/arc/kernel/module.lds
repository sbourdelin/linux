SECTIONS
{
	/DISCARD/ : { *(.debug_aranges) }
	/DISCARD/ : { *(.debug_pubnames) }
	/DISCARD/ : { *(.debug_info) }
	/DISCARD/ : { *(.debug_abbrev) }
	/DISCARD/ : { *(.debug_line) }
	/DISCARD/ : { *(.debug_str) }
	/DISCARD/ : { *(.debug_loc) }
	/DISCARD/ : { *(.debug_macinfo) }
	/DISCARD/ : { *(.debug_ranges) }
}
