cmd_/nfs/pmdfc/client/pmdfc_client.o := ld -m elf_x86_64  -z max-page-size=0x200000    -r -o /nfs/pmdfc/client/pmdfc_client.o /nfs/pmdfc/client/pmdfc.o /nfs/pmdfc/client/bloom_filter.o
