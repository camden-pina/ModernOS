#include <power.h>
#include <tty.h>
#include <sched.h>
#include <usb_input.h>
#include <console.h>
#include <8250.h>
#include <io.h>
#include <input/mouse.h>
#include <video/display.h>

static spinlock_t input_lock;
static char editing[512], queue[4096];
static unsigned editing_size, head, tail, foreground;
static bool skip_lf, editing_overflow;
static unsigned canonical_escape;
static struct os_termios config = {.iflag = ICRNL,
                                   .cflag = 0xbf,
                                   .lflag = ISIG | ICANON | ECHO,
                                   .cc = {[VINTR] = 3, [VERASE] = 127, [VEOF] = 4, [VMIN] = 1}};

unsigned tty_job_foreground(void) {
    return __atomic_load_n(&foreground, __ATOMIC_ACQUIRE);
}

void tty_job_restore(unsigned from, unsigned to, const struct os_termios *mode) {
    uint64_t irq = spin_lock_irqsave(&input_lock);
    if (foreground == from) {
        if (mode && ((config.lflag ^ mode->lflag) & ICANON)) {
            editing_size = 0;
            tail = head;
            skip_lf = editing_overflow = false;
            canonical_escape = 0;
        }
        if (mode)
            config = *mode;
        __atomic_store_n(&foreground, to, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&input_lock, irq);
}

bool tty_is_foreground(unsigned pid) {
    return pid == __atomic_load_n(&foreground, __ATOMIC_ACQUIRE);
}

void tty_transfer_foreground(unsigned from, unsigned to) {
    uint64_t f = spin_lock_irqsave(&input_lock);
    if (__atomic_load_n(&foreground, __ATOMIC_ACQUIRE) == from) {
        __atomic_store_n(&foreground, to, __ATOMIC_RELEASE);
        /* Canonical editing belongs to the terminal, not the process. A shell
           background child may inherit stdin briefly before redirecting it;
           its fork/close/reap handoff must not discard text already typed for
           the live parent. Explicit flush/mode-reset operations still clear it. */
    }
    spin_unlock_irqrestore(&input_lock, f);
}

void tty_reset_config(unsigned pid) {
    uint64_t f = spin_lock_irqsave(&input_lock);
    if (foreground == pid) {
        config = (struct os_termios){.iflag = ICRNL,
                                     .cflag = 0xbf,
                                     .lflag = ISIG | ICANON | ECHO,
                                     .cc = {[VINTR] = 3, [VERASE] = 127, [VEOF] = 4, [VMIN] = 1}};
        editing_size = 0;
        tail = head;
        skip_lf = false;
        editing_overflow = false;
        canonical_escape = 0;
    }
    spin_unlock_irqrestore(&input_lock, f);
}

int tty_get_config(struct os_termios *out) {
    uint64_t f = spin_lock_irqsave(&input_lock);
    *out = config;
    spin_unlock_irqrestore(&input_lock, f);
    return 0;
}

int tty_set_config(const struct os_termios *in, bool flush) {
    if (!tty_is_foreground(task_input_identity(this_cpu()->current)))
        return this_cpu()->current->context->job_managed?task_background_terminal(this_cpu()->current,22):-E_PERM;
    /* Explicit supported subset; never silently enable unimplemented disciplines. */
    if (in->line || in->iflag & ~ICRNL || in->oflag || in->lflag & ~(ISIG | ICANON | ECHO | TOSTOP) ||
        in->cc[VMIN] > 1 || in->cc[VTIME])
        return -E_NOTSUP;
    uint64_t f = spin_lock_irqsave(&input_lock);
    if (flush) {
        editing_size = 0;
        tail = head;
        skip_lf = editing_overflow = false;
        canonical_escape = 0;
    } else if ((config.lflag & ICANON) && !(in->lflag & ICANON)) {
        unsigned free = (tail + sizeof(queue) - head - 1) % sizeof(queue);
        if (editing_overflow || editing_size > free) {
            spin_unlock_irqrestore(&input_lock, f);
            return editing_overflow ? -E_2BIG : -E_AGAIN;
        }
        for (unsigned i = 0; i < editing_size; i++) {
            queue[head] = editing[i];
            head = (head + 1) % sizeof(queue);
        }
        editing_size = 0;
        canonical_escape = 0;
        skip_lf = false;
    }
    config = *in;
    spin_unlock_irqrestore(&input_lock, f);
    return 0;
}

unsigned tty_available(void) {
    uint64_t f = spin_lock_irqsave(&input_lock);
    unsigned n = (head + sizeof(queue) - tail) % sizeof(queue);
    spin_unlock_irqrestore(&input_lock, f);
    return n;
}

void tty_input(char c) {
    display_key((uint8_t)c);
    unsigned pid = __atomic_load_n(&foreground, __ATOMIC_ACQUIRE);
    uint64_t f = spin_lock_irqsave(&input_lock);
    struct os_termios mode = config;
    spin_unlock_irqrestore(&input_lock, f);

    if ((mode.lflag & ISIG) && c && ((uint8_t)c==mode.cc[VINTR]||(uint8_t)c==mode.cc[VQUIT]||(uint8_t)c==mode.cc[VSUSP])) {
        unsigned signal_number=(uint8_t)c==mode.cc[VINTR]?2:(uint8_t)c==mode.cc[VQUIT]?3:20;
        if (pid > 1)
            task_signal_foreground(pid, signal_number);
        uint64_t f = spin_lock_irqsave(&input_lock);
        editing_size = 0;
        tail = head;
        editing_overflow = false;
        canonical_escape = 0;
        if (pid <= 1) {
            queue[head] = '\n';
            head = (head + 1) % sizeof(queue);
        }
        spin_unlock_irqrestore(&input_lock, f);
        if (mode.lflag & ECHO)
            console_write(signal_number==20?"^Z\n":signal_number==3?"^\\\n":"^C\n", 3);
        return;
    }

    f = spin_lock_irqsave(&input_lock);
    bool echo = false, erase = false;
    if (!(mode.lflag & ICANON)) {
        if (c == '\r' && (mode.iflag & ICRNL))
            c = '\n';
        if ((head + 1) % sizeof(queue) != tail) {
            queue[head] = c;
            head = (head + 1) % sizeof(queue);
            echo = true;
        }
        spin_unlock_irqrestore(&input_lock, f);
        if (echo && (mode.lflag & ECHO))
            console_write(&c, 1);
        return;
    }

    /* Navigation escapes are delivered in raw mode; they are not literal shell text. */
    if (canonical_escape) {
        if (c == '\n' || c == '\r')
            canonical_escape = 0;
        else {
            if (canonical_escape == 1)
                canonical_escape = (c == '[' || c == 'O') ? 2 : 0;
            else if (c >= '@' && c <= '~')
                canonical_escape = 0;
            spin_unlock_irqrestore(&input_lock, f);
            return;
        }
    }

    if (c == 27) {
        canonical_escape = 1;
        spin_unlock_irqrestore(&input_lock, f);
        return;
    }

    if (c == '\n' && skip_lf) {
        skip_lf = false;
        spin_unlock_irqrestore(&input_lock, f);
        return;
    }

    skip_lf = c == '\r';
    if (c == '\r')
        c = '\n';

    if (c == '\b' || (uint8_t)c == mode.cc[VERASE]) {
        if (editing_size) {
            editing_size--;
            erase = true;
        }
    } else if (c && (uint8_t)c == mode.cc[VEOF]) {
        /* EOF terminates buffered text, or represents EOF for one empty read. */
        unsigned free = (tail + sizeof(queue) - head - 1) % sizeof(queue);
        unsigned needed = editing_overflow ? 2 : editing_size + 1;
        if (free >= needed) {
            if (editing_overflow) {
                queue[head] = 1;
                head = (head + 1) % sizeof(queue);
            } else
                for (unsigned i = 0; i < editing_size; i++) {
                    queue[head] = editing[i];
                    head = (head + 1) % sizeof(queue);
                }
            queue[head] = editing_overflow ? '\n' : 0;
            head = (head + 1) % sizeof(queue);
            editing_size = 0;
            editing_overflow = false;
        }
    } else if (c == '\n') {
        unsigned free = (tail + sizeof(queue) - head - 1) % sizeof(queue);
        unsigned needed = editing_overflow ? 2 : editing_size + 1;
        if (free >= needed) {
            if (editing_overflow) {
                queue[head] = 1;
                head = (head + 1) % sizeof(queue);
            } else
                for (unsigned i = 0; i < editing_size; i++) {
                    queue[head] = editing[i];
                    head = (head + 1) % sizeof(queue);
                }
            queue[head] = '\n';
            head = (head + 1) % sizeof(queue);
            editing_size = 0;
            editing_overflow = false;
            echo = true;
        }
    } else if ((c >= 32 && c < 127) || c == '\t') {
        if (editing_size < sizeof(editing) - 1 && !editing_overflow) {
            editing[editing_size++] = c;
            echo = true;
        } else
            editing_overflow = true;
    }
    spin_unlock_irqrestore(&input_lock, f);

    if (erase && (mode.lflag & ECHO))
        console_write("\b \b", 3);
    if (echo && (mode.lflag & ECHO))
        console_write(&c, 1);
}

long tty_read(void *data, size_t size, bool nonblock) {
    if (!size)
        return 0;
    for (;;) {
        uint64_t f = spin_lock_irqsave(&input_lock);
        if (task_input_identity(this_cpu()->current) != foreground) {
            spin_unlock_irqrestore(&input_lock, f);
            return this_cpu()->current->context->job_managed?task_background_terminal(this_cpu()->current,21):-E_IO;
        }
        size_t done = 0;
        bool had_data = tail != head;
        while (tail != head && done < size) {
            char c = queue[tail];
            tail = (tail + 1) % sizeof(queue);
            if (c == 1 && (config.lflag & ICANON)) {
                while (tail != head) {
                    char next = queue[tail];
                    tail = (tail + 1) % sizeof(queue);
                    if (next == '\n')
                        break;
                }
                spin_unlock_irqrestore(&input_lock, f);
                return -E_2BIG;
            }
            if (!c && (config.lflag & ICANON))
                break;
            ((char *)data)[done++] = c;
            if (c == '\n' && (config.lflag & ICANON))
                break;
        }
        bool immediate = !(config.lflag & ICANON) && !config.cc[VMIN];
        spin_unlock_irqrestore(&input_lock, f);
        if (done || had_data)
            return done;
        if (nonblock)
            return -E_AGAIN;
        if (immediate)
            return 0;
        unsigned signal = task_pending_signal(this_cpu()->current);
        if (signal)
            task_exit(signal);
        if (signal_pending(this_cpu()->current))
            return -E_INTR;
        sched_sleep(1000);
    }
}

void input_task(void *arg) {
    (void)arg;
    for (;;) {
        power_transaction_begin();
        kernel_usb_input_poll();
        while (inb(COM1_PORT + 5) & 1)
            tty_input(inb(COM1_PORT));
        mouse_event_t event;
        while (mouse_read_event(&event))
            display_pointer(&event);
        power_transaction_end();
        sched_sleep(1000);
    }
}
