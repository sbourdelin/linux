#!/bin/bash

# This script should update all files in a git tree that use deprecated
# v2 SPDX license identifiers to use v3 styles with -only and -or-later.

# The following styles are intended to be converted

# GPL-1.0+	->	GPL-1.0-or-later
# GPL-2.0	->	GPL-2.0-only
# GPL-2.0+	->	GPL-2.0-or-later
# LGPL-2.0	->	LGPL-2.0-only
# LGPL-2.0+	->	LGPL-2.0-or-later
# LGPL-2.1	->	LGPL-2.1-only
# LGPL-2.1+	->	LGPL-2.1-or-later

# GPL variants without \+ that should use -only

spdx_find='(SPDX-License-Identifier:\s[\s\(]*.*\bL?GPL-[12].[01])(\s|\)|$)'
spdx_replace='\1-only\2'
git grep -P --name-only "$spdx_find" -- './*' ':(exclude)LICENSES/' | \
    xargs -r perl -p -i -e "s/$spdx_find/$spdx_replace/"

# GPL variants with \+ that should use -or-later

spdx_find='(SPDX-License-Identifier:\s[\s\(]*.*\bL?GPL-[12].[01])\+(\s|\)|$)'
spdx_replace='\1-or-later\2'
git grep -P --name-only "$spdx_find" -- './*' ':(exclude)LICENSES/' | \
    xargs -r perl -p -i -e "s/$spdx_find/$spdx_replace/"
