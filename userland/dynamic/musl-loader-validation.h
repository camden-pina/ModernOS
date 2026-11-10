/* Coast's supported DSO metadata profile. Called after every LOAD has
 * passed the file-range, address-overflow and W^X checks in map_library. */
static int coast_cover(const Ehdr *eh, const Phdr *ph, size_t address,
                         size_t bytes, int file_backed, off_t *offset)
{
	for (size_t i=0; i<eh->e_phnum; i++) {
		const Phdr *p = ph+i;
		if (p->p_type != PT_LOAD || !(p->p_flags & PF_R) || address < p->p_vaddr)
			continue;
		size_t delta = address-p->p_vaddr;
		size_t limit = file_backed ? p->p_filesz : p->p_memsz;
		if (delta <= limit && bytes <= limit-delta) {
			if (offset) *offset = p->p_offset+delta;
			return 1;
		}
	}
	return 0;
}

static int coast_metadata_valid(int fd, const Ehdr *eh, const Phdr *ph)
{
	const Phdr *dynamic = 0;
	unsigned tls = 0, relro = 0;
	for (size_t i=0; i<eh->e_phnum; i++) {
		const Phdr *p = ph+i;
		off_t offset;
		if (p->p_type == PT_LOAD && p->p_memsz) {
			size_t first = p->p_vaddr & -PAGE_SIZE;
			size_t last = (p->p_vaddr+p->p_memsz+PAGE_SIZE-1) & -PAGE_SIZE;
			for (size_t j=0; j<i; j++) {
				const Phdr *q = ph+j;
				if (q->p_type == PT_LOAD && q->p_memsz &&
				    first < ((q->p_vaddr+q->p_memsz+PAGE_SIZE-1) & -PAGE_SIZE) &&
				    (q->p_vaddr & -PAGE_SIZE) < last) return 0;
			}
		}
		if (p->p_type == PT_DYNAMIC) {
			if (dynamic || !p->p_filesz || p->p_filesz > 65536
			    || p->p_filesz % sizeof(Elf64_Dyn) || p->p_filesz > p->p_memsz
			    || !coast_cover(eh, ph, p->p_vaddr, p->p_filesz, 1, &offset)
			    || offset != p->p_offset) return 0;
			dynamic = p;
		} else if (p->p_type == PT_TLS) {
			if (tls++ || p->p_filesz > p->p_memsz
			    || (p->p_align && (p->p_align & (p->p_align-1)))
			    || p->p_memsz > 128*1024*1024
			    || !coast_cover(eh, ph, p->p_vaddr, p->p_memsz, 0, 0)
			    || !coast_cover(eh, ph, p->p_vaddr, p->p_filesz, 1, &offset)
			    || offset != p->p_offset) return 0;
		} else if (p->p_type == PT_GNU_RELRO) {
			if (relro++ || !coast_cover(eh, ph, p->p_vaddr, p->p_memsz, 0, 0)) return 0;
		}
	}
	if (!dynamic) return 0;
	/* Bound the scan before the upstream decoder/relocator examines it. */
	for (size_t off=0; off<dynamic->p_filesz; off+=sizeof(Elf64_Dyn)) {
		Elf64_Dyn entry;
		if (pread(fd, &entry, sizeof entry, dynamic->p_offset+off) != sizeof entry) return 0;
		if (entry.d_tag == DT_NULL) return 1;
		if (entry.d_tag == DT_TEXTREL ||
		    (entry.d_tag == DT_FLAGS && (entry.d_un.d_val & DF_TEXTREL))) return 0;
	}
	return 0;
}
