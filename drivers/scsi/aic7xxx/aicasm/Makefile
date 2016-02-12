PROG=	$(obj)/aicasm

all: $(PROG)

.SUFFIXES= .l .y .c .h

CSRCS=	$(srctree)/$(src)/aicasm.c $(srctree)/$(src)/aicasm_symbol.c
YSRCS=	$(src)/aicasm_gram.y $(src)/aicasm_macro_gram.y
LSRCS=	$(src)/aicasm_scan.l $(src)/aicasm_macro_scan.l

GENHDRS=	$(obj)/aicdb.h $(patsubst $(src)/%.y,$(obj)/%.h,$(YSRCS))
GENSRCS=	$(patsubst $(src)/%.y,$(obj)/%.c,$(YSRCS)) \
		$(patsubst $(src)/%.l,$(obj)/%.c,$(LSRCS))

SRCS=	$(CSRCS) $(GENSRCS)
LIBS=	-ldb
clean-files:= ${GENSRCS} ${GENHDRS} $(YSRCS:.y=.output) $(PROG)
# Override default kernel CFLAGS.  This is a userland app.
AICASM_CFLAGS:= -I/usr/include -I. -I$(srctree)/$(src)
LEX= flex
YACC= bison
YFLAGS= -d

NOMAN=	noman

ifneq ($(HOSTCC),)
AICASM_CC= $(HOSTCC)
else
AICASM_CC= $(CC)
endif

ifdef DEBUG
CFLAGS+= -DDEBUG -g
YFLAGS+= -t -v
LFLAGS= -d
endif

$(PROG): $(GENHDRS) $(SRCS)
	echo "SRCS=$(SRCS)"
	$(AICASM_CC) $(AICASM_CFLAGS) $(SRCS) -o $@ $(LIBS)

$(obj)/aicdb.h:
	@if [ -e "/usr/include/db4/db_185.h" ]; then		\
		echo "#include <db4/db_185.h>" > $(obj)/aicdb.h;	\
	 elif [ -e "/usr/include/db3/db_185.h" ]; then		\
		echo "#include <db3/db_185.h>" > $(obj)/aicdb.h;	\
	 elif [ -e "/usr/include/db2/db_185.h" ]; then		\
		echo "#include <db2/db_185.h>" > $(obj)/aicdb.h;	\
	 elif [ -e "/usr/include/db1/db_185.h" ]; then		\
		echo "#include <db1/db_185.h>" > $(obj)/aicdb.h;	\
	 elif [ -e "/usr/include/db/db_185.h" ]; then		\
		echo "#include <db/db_185.h>" > $(obj)/aicdb.h;	\
	 elif [ -e "/usr/include/db_185.h" ]; then		\
		echo "#include <db_185.h>" > $(obj)/aicdb.h;		\
	 else							\
		echo "*** Install db development libraries";	\
	 fi

clean:
	rm -f $(clean-files)

# Create a dependency chain in generated files
# to avoid concurrent invocations of the single
# rule that builds them all.
$(obj)/aicasm_gram.c $(obj)/aicasm_gram.h: $(src)/aicasm_gram.y
	$(YACC) $(YFLAGS) -b $(@:.h=) $<
	mv $(@:.h=).tab.c $(@:.h=.c)
	mv $(@:.h=).tab.h $(@:.y=.h)

# Create a dependency chain in generated files
# to avoid concurrent invocations of the single
# rule that builds them all.
$(obj)/aicasm_macro_gram.c $(obj)/aicasm_macro_gram.h: $(src)/aicasm_macro_gram.y
	$(YACC) $(YFLAGS) -b $(@:.h=) -p mm $<
	mv $(@:.h=).tab.c $(@:.h=.c)
	mv $(@:.h=).tab.h $(@:.h=.h)

$(obj)/aicasm_scan.c: $(src)/aicasm_scan.l
	$(LEX) $(LFLAGS) -o$@ $<

$(obj)/aicasm_macro_scan.c: $(src)/aicasm_macro_scan.l
	$(LEX) $(LFLAGS) -Pmm -o$@ $<
