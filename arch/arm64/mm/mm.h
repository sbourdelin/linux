extern void __init bootmem_init(void);

void fixup_init(void);

void split_pud(pud_t *old_pud, pmd_t *pmd);
void split_pmd(pmd_t *pmd, pte_t *pte);
