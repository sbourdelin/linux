#!/bin/bash

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
