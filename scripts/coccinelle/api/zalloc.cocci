/// Prefer zalloc functions instead of using allocate and memcpy.
///
// Confidence: High
// Copyright: (C) 2016 Amitoj Kaur Chawla

virtual patch
virtual context
virtual org
virtual report

@dma1 depends on patch && !context && !org && !report@
type T;
T *d;
statement S;
@@

        d =
-            dma_pool_alloc
+            dma_pool_zalloc
             (...);
        if (!d) S
-       memset(d, 0, sizeof(T));

@dma2 depends on patch && !context && !org && !report@
expression d;
statement S;
@@

        d =
-            dma_pool_alloc
+            dma_pool_zalloc
             (...);
        if (!d) S
-       memset(d, 0, sizeof(*d));
@vz1 depends on patch && !context && !org && !report@
type T;
T *d;
statement S;
@@

        d =
-            vmalloc
+            vzalloc
             (...);
        if (!d) S
-       memset(d, 0, sizeof(T));

@vz2 depends on patch && !context && !org && !report@
expression d;
statement S;
@@

        d =
-            vmalloc
+            vzalloc
             (...);
        if (!d) S
-       memset(d, 0, sizeof(*d));
@vzn1 depends on patch && !context && !org && !report@
type T;
T *d;
statement S;
@@

        d =
-            vmalloc_node
+            vzalloc_node
             (...);
        if (!d) S
-       memset(d, 0, sizeof(T));

@vzn2 depends on patch && !context && !org && !report@
expression d;
statement S;
@@

        d =
-            vmalloc_node
+            vzalloc_node
             (...);
        if (!d) S
-       memset(d, 0, sizeof(*d));
@pci1 depends on patch && !context && !org && !report@
type T;
T *d;
statement S;
@@

        d =
-            pci_alloc_consistent
+            pci_zalloc_consistent
             (...);
        if (!d) S
-       memset(d, 0, sizeof(T));

@pci2 depends on patch && !context && !org && !report@
expression d;
statement S;
@@

        d =
-            pci_alloc_consistent
+            pci_zalloc_consistent
             (...);
        if (!d) S
-       memset(d, 0, sizeof(*d));
@kmem1 depends on patch && !context && !org && !report@
type T;
T *d;
statement S;
@@

        d =
-            kmem_cache_alloc
+            kmem_cache_zalloc
             (...);
        if (!d) S
-       memset(d, 0, sizeof(T));

@kmem2 depends on patch && !context && !org && !report@
expression d;
statement S;
@@

        d =
-            kmem_cache_alloc
+            kmem_cache_zalloc
             (...);
        if (!d) S
-       memset(d, 0, sizeof(*d));
@dma3 depends on patch && !context && !org && !report@
type T;
T *d;
statement S;
@@

        d =
-            dma_alloc_coherent
+            dma_zalloc_coherent
             (...);
        if (!d) S
-       memset(d, 0, sizeof(T));

@dma4 depends on patch && !context && !org && !report@
expression d;
statement S;
@@

        d =
-            dma_alloc_coherent
+            dma_zalloc_coherent
             (...);
        if (!d) S
-       memset(d, 0, sizeof(*d));
@acpi1 depends on patch && !context && !org && !report@
type T;
T *d;
statement S;
@@

        d =
-            acpi_os_allocate
+            acpi_os_allocate_zeroed
             (...);
        if (!d) S
-       memset(d, 0, sizeof(T));

@acpi2 depends on patch && !context && !org && !report@
expression d;
statement S;
@@

        d =
-            acpi_os_allocate
+            acpi_os_allocate_zeroed
             (...);
        if (!d) S
-       memset(d, 0, sizeof(*d));

// ----------------------------------------------------------------------------

@dma1_context depends on !patch && (context || org || report)@
type T;
statement S;
T *d;
position j0;
@@

        d@j0 =
*             dma_pool_alloc
             (...);
        if (!d) S
*        memset(d, 0, sizeof(T));

@dma2_context depends on !patch && (context || org || report)@
statement S;
expression d;
position j0;
@@

        d@j0 =
*             dma_pool_alloc
             (...);
        if (!d) S
*        memset(d, 0, sizeof(*d));

@vz1_context depends on !patch && (context || org || report)@
type T;
statement S;
T *d;
position j0;
@@

        d@j0 =
*             vmalloc
             (...);
        if (!d) S
*        memset(d, 0, sizeof(T));

@vz2_context depends on !patch && (context || org || report)@
statement S;
expression d;
position j0;
@@

        d@j0 =
*             vmalloc
             (...);
        if (!d) S
*        memset(d, 0, sizeof(*d));

@vzn1_context depends on !patch && (context || org || report)@
type T;
statement S;
T *d;
position j0;
@@

        d@j0 =
*             vmalloc_node
             (...);
        if (!d) S
*        memset(d, 0, sizeof(T));

@vzn2_context depends on !patch && (context || org || report)@
statement S;
expression d;
position j0;
@@

        d@j0 =
*             vmalloc_node
             (...);
        if (!d) S
*        memset(d, 0, sizeof(*d));

@pci1_context depends on !patch && (context || org || report)@
type T;
statement S;
T *d;
position j0;
@@

        d@j0 =
*             pci_alloc_consistent
             (...);
        if (!d) S
*        memset(d, 0, sizeof(T));

@pci2_context depends on !patch && (context || org || report)@
statement S;
expression d;
position j0;
@@

        d@j0 =
*             pci_alloc_consistent
             (...);
        if (!d) S
*        memset(d, 0, sizeof(*d));

@kmem1_context depends on !patch && (context || org || report)@
type T;
statement S;
T *d;
position j0;
@@

        d@j0 =
*             kmem_cache_alloc
             (...);
        if (!d) S
*        memset(d, 0, sizeof(T));

@kmem2_context depends on !patch && (context || org || report)@
statement S;
expression d;
position j0;
@@

        d@j0 =
*             kmem_cache_alloc
             (...);
        if (!d) S
*        memset(d, 0, sizeof(*d));

@dma3_context depends on !patch && (context || org || report)@
type T;
statement S;
T *d;
position j0;
@@

        d@j0 =
*             dma_alloc_coherent
             (...);
        if (!d) S
*        memset(d, 0, sizeof(T));

@dma4_context depends on !patch && (context || org || report)@
statement S;
expression d;
position j0;
@@

        d@j0 =
*             dma_alloc_coherent
             (...);
        if (!d) S
*        memset(d, 0, sizeof(*d));

@acpi1_context depends on !patch && (context || org || report)@
type T;
statement S;
T *d;
position j0;
@@

        d@j0 =
*             acpi_os_allocate
             (...);
        if (!d) S
*        memset(d, 0, sizeof(T));

@acpi2_context depends on !patch && (context || org || report)@
statement S;
expression d;
position j0;
@@

        d@j0 =
*             acpi_os_allocate
             (...);
        if (!d) S
*        memset(d, 0, sizeof(*d));

// ----------------------------------------------------------------------------

@script:python dma1_org depends on org@
j0 << dma1_context.j0;
@@

msg = "Replace with dma_pool_zalloc."
coccilib.org.print_todo(j0[0], msg)

@script:python dma2_org depends on org@
j0 << dma2_context.j0;
@@

msg = "Replace with dma_pool_zalloc."
coccilib.org.print_todo(j0[0], msg)

@script:python vz1_org depends on org@
j0 << vz1_context.j0;
@@

msg = "Replace with vzalloc."
coccilib.org.print_todo(j0[0], msg)

@script:python vz2_org depends on org@
j0 << vz2_context.j0;
@@

msg = "Replace with vzalloc."
coccilib.org.print_todo(j0[0], msg)

@script:python vzn1_org depends on org@
j0 << vzn1_context.j0;
@@

msg = "Replace with vzalloc_node."
coccilib.org.print_todo(j0[0], msg)

@script:python vzn2_org depends on org@
j0 << vzn2_context.j0;
@@

msg = "Replace with vzalloc_node."
coccilib.org.print_todo(j0[0], msg)

@script:python pci1_org depends on org@
j0 << pci1_context.j0;
@@

msg = "Replace with pci_zalloc_consistent."
coccilib.org.print_todo(j0[0], msg)

@script:python pci2_org depends on org@
j0 << pci2_context.j0;
@@

msg = "Replace with pci_zalloc_consistent."
coccilib.org.print_todo(j0[0], msg)

@script:python kmem1_org depends on org@
j0 << kmem1_context.j0;
@@

msg = "Replace with kmem_cache_zalloc."
coccilib.org.print_todo(j0[0], msg)

@script:python kmem2_org depends on org@
j0 << kmem2_context.j0;
@@

msg = "Replace with kmem_cache_zalloc."
coccilib.org.print_todo(j0[0], msg)

@script:python dma3_org depends on org@
j0 << dma3_context.j0;
@@

msg = "Replace with dma_zalloc_coherent."
coccilib.org.print_todo(j0[0], msg)

@script:python dma4_org depends on org@
j0 << dma4_context.j0;
@@

msg = "Replace with dma_zalloc_coherent."
coccilib.org.print_todo(j0[0], msg)

@script:python acpi1_org depends on org@
j0 << acpi1_context.j0;
@@

msg = "Replace with acpi_os_allocate_zeroed."
coccilib.org.print_todo(j0[0], msg)

@script:python acpi2_org depends on org@
j0 << acpi2_context.j0;
@@

msg = "Replace with acpi_os_allocate_zeroed."
coccilib.org.print_todo(j0[0], msg)

// ----------------------------------------------------------------------------

@script:python dma1_report depends on report@
j0 << dma1_context.j0;
@@

msg = "Replace with dma_pool_zalloc."
coccilib.report.print_report(j0[0], msg)

@script:python dma2_report depends on report@
j0 << dma2_context.j0;
@@

msg = "Replace with dma_pool_zalloc."
coccilib.report.print_report(j0[0], msg)

@script:python vz1_report depends on report@
j0 << vz1_context.j0;
@@

msg = "Replace with vzalloc."
coccilib.report.print_report(j0[0], msg)

@script:python vz2_report depends on report@
j0 << vz2_context.j0;
@@

msg = "Replace with vzalloc."
coccilib.report.print_report(j0[0], msg)

@script:python vzn1_report depends on report@
j0 << vzn1_context.j0;
@@

msg = "Replace with vzalloc_node."
coccilib.report.print_report(j0[0], msg)

@script:python vzn2_report depends on report@
j0 << vzn2_context.j0;
@@

msg = "Replace with vzalloc_node."
coccilib.report.print_report(j0[0], msg)

@script:python pci1_report depends on report@
j0 << pci1_context.j0;
@@

msg = "Replace with pci_zalloc_consistent."
coccilib.report.print_report(j0[0], msg)

@script:python pci2_report depends on report@
j0 << pci2_context.j0;
@@

msg = "Replace with pci_zalloc_consistent."
coccilib.report.print_report(j0[0], msg)

@script:python kmem1_report depends on report@
j0 << kmem1_context.j0;
@@

msg = "Replace with kmem_cache_zalloc."
coccilib.report.print_report(j0[0], msg)

@script:python kmem2_report depends on report@
j0 << kmem2_context.j0;
@@

msg = "Replace with kmem_cache_zalloc."
coccilib.report.print_report(j0[0], msg)

@script:python dma3_report depends on report@
j0 << dma3_context.j0;
@@

msg = "Replace with dma_zalloc_coherent."
coccilib.report.print_report(j0[0], msg)

@script:python dma4_report depends on report@
j0 << dma4_context.j0;
@@

msg = "Replace with dma_zalloc_coherent."
coccilib.report.print_report(j0[0], msg)

@script:python acpi1_report depends on report@
j0 << acpi1_context.j0;
@@

msg = "Replace with acpi_os_allocate_zeroed."
coccilib.report.print_report(j0[0], msg)

@script:python acpi2_report depends on report@
j0 << acpi2_context.j0;
@@

msg = "Replace with acpi_os_allocate_zeroed."
coccilib.report.print_report(j0[0], msg)

