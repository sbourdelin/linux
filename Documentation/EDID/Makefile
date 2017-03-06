
# use "make O=dir" to locate all output files in "dir"
O ?= .

SOURCES	:= $(wildcard [0-9]*x[0-9]*.S)

BIN	:= $(patsubst %.S, $(O)/%.bin, $(SOURCES))

IHEX	:= $(patsubst %.S, $(O)/%.bin.ihex, $(SOURCES))

CODE	:= $(patsubst %.S, $(O)/%.c, $(SOURCES))

all:	$(BIN) $(IHEX) $(CODE)

clean:
	@rm -f $(O)/*.o $(O)/*.bin.ihex $(O)/*.bin $(O)/*.c

$(O)/%.o:	%.S
	@cc -c $^ -o $@

$(O)/%.bin:	$(O)/%.o
	@objcopy -Obinary $^ $@

$(O)/%.bin.ihex:	$(O)/%.o
	@objcopy -Oihex $^ $@
	@dos2unix $@ 2>/dev/null

$(O)/%.c:	$(O)/%.bin
	@echo "{" >$@; hexdump -f hex $^ >>$@; echo "};" >>$@
