#include <drivers/audio.h>
#include <uapi/abi.h>
#include <drivers/pci/pci.h>

bool ac97_probe(void *);
bool hda_probe(void *);
long ac97_call(struct task *, unsigned, uint64_t, uint64_t);
long hda_call(struct task *, unsigned, uint64_t, uint64_t);
void hda_tick(void);
void aw88399_init(void);
static unsigned audio_driver;

bool audio_probe(void *header) {
    if (audio_driver)
        return false;
    pci_device_header_t *pci = header;
    if (pci->subclass == 1 && ac97_probe(header)) {
        audio_driver = 1;
        return true;
    }
    if (pci->subclass == 3 && hda_probe(header)) {
        audio_driver = 2;
        return true;
    }
    return false;
}

long audio_call(struct task *task, unsigned operation, uint64_t pointer, uint64_t frames) {
    if (audio_driver == 1)
        return ac97_call(task, operation, pointer, frames);
    if (audio_driver == 2)
        return hda_call(task, operation, pointer, frames);
    return -E_NODEV;
}

void audio_tick(void) {
    if (audio_driver == 2)
        hda_tick();
}

void audio_init(void) {
    if (audio_driver == 2)
        aw88399_init();
}
