/*
 * Copyright (C) 2017 Netronome Systems, Inc.
 *
 * This software is dual licensed under the GNU General License Version 2,
 * June 1991 as shown in the file COPYING in the top-level directory of this
 * source tree or the BSD 2-Clause License provided below.  You have the
 * option to license this software under the complete terms of either license.
 *
 * The BSD 2-Clause License:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      1. Redistributions of source code must retain the above
 *         copyright notice, this list of conditions and the following
 *         disclaimer.
 *
 *      2. Redistributions in binary form must reproduce the above
 *         copyright notice, this list of conditions and the following
 *         disclaimer in the documentation and/or other materials
 *         provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/slab.h>

#include "nfpcore/nfp_cpp.h"
#include "nfp_app.h"
#include "nfp_main.h"

/**
 * struct nfp_app - NFP application container
 * @pdev:	backpointer to PCI device
 * @pf:		backpointer to NFP PF structure
 * @cpp:	pointer to the CPP handle
 */
struct nfp_app {
	struct pci_dev *pdev;
	struct nfp_pf *pf;
	struct nfp_cpp *cpp;
};

struct nfp_cpp *nfp_app_cpp(struct nfp_app *app)
{
	return app->cpp;
}

struct nfp_pf *nfp_app_pf(struct nfp_app *app)
{
	return app->pf;
}

struct nfp_app *nfp_app_alloc(struct nfp_pf *pf)
{
	struct nfp_app *app;

	app = kzalloc(sizeof(*app), GFP_KERNEL);
	if (!app)
		return ERR_PTR(-ENOMEM);

	app->pf = pf;
	app->cpp = pf->cpp;
	app->pdev = pf->pdev;

	return app;
}

void nfp_app_free(struct nfp_app *app)
{
	kfree(app);
}
