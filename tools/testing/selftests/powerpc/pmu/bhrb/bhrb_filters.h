/*
 * Copyright (C) 2015 Anshuman Khandual, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */

/* Assembly routines */
extern void fetch_all_branches(unsigned int *);
extern void fetch_all_calls(unsigned int *);
extern void fetch_all_rets(unsigned int *);
extern void fetch_all_conds(unsigned int *);
extern void fetch_all_inds(unsigned int *);
extern void start_loop(void);
