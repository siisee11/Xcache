cmd_/mnt/dg_nas/git/pmdfc/client/dram-backend/pmdfc_dram.ko := ld -r -m elf_x86_64  -z max-page-size=0x200000  --build-id  -T ./scripts/module-common.lds -o /mnt/dg_nas/git/pmdfc/client/dram-backend/pmdfc_dram.ko /mnt/dg_nas/git/pmdfc/client/dram-backend/pmdfc_dram.o /mnt/dg_nas/git/pmdfc/client/dram-backend/pmdfc_dram.mod.o;  true