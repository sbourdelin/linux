# mips: system_call_table_generator

- This architecture does have more than one ABI.
  syscall.tbl contains the information like 
    - system call number
    - name 
    - entry_64
    - entry_32
    - compat
    - comments

- The scripts syscallhdr.sh will generate uapi header- 
  arch/powerpc/include/uapi/asm/unistd.h. 

- The script syscalltbl.sh will generate syscalls.h 
  which will be included by syscall_64/32/n32/o32.S file
