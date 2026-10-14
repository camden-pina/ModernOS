#include <mm/pmm.h>
#include <pty.h>
#include <sched.h>
#include <string.h>

#define PTY_BUFFER 16384

struct pty_ring {
    uint8_t bytes[PTY_BUFFER];
    unsigned head, tail, count;
};

struct pty {
    spinlock_t lock;
    struct pty_ring input, output;
    struct os_termios config;
    struct os_winsize size;
    bool master, slave, discard_line;
    unsigned input_errors;
    unsigned foreground, eof, editing_size;
    uint8_t editing[512];
};

static void push(struct pty_ring *r, uint8_t c) {
    r->bytes[r->head] = c;
    r->head = (r->head + 1) % PTY_BUFFER;
    r->count++;
}

static uint8_t pop(struct pty_ring *r) {
    uint8_t c = r->bytes[r->tail];
    r->tail = (r->tail + 1) % PTY_BUFFER;
    r->count--;
    return c;
}

static void echo(struct pty *p, uint8_t c) {
    if (p->output.count < PTY_BUFFER) {
        push(&p->output, c);
    }
}

struct pty *pty_allocate(void) {
    struct pty *p = kmallocz(sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->master = p->slave = true;
    p->size = (struct os_winsize){24, 80, 0, 0};
    p->config = (struct os_termios){.iflag = ICRNL,
                                    .oflag = OPOST | ONLCR,
                                    .cflag = 0xbf,
                                    .lflag = ISIG | ICANON | ECHO,
                                    .cc = {[VINTR] = 3, [VERASE] = 127, [VEOF] = 4, [VMIN] = 1}};
    return p;
}

void pty_foreground(struct pty *p, unsigned from, unsigned to) {
    uint64_t irq = spin_lock_irqsave(&p->lock);
    if (!p->foreground || p->foreground == from) {
        p->foreground = to;
    }
    spin_unlock_irqrestore(&p->lock, irq);
}

void pty_close(struct file *f) {
    struct pty *p = f->pty;
    uint64_t irq = spin_lock_irqsave(&p->lock);
    if (f->pty_master) {
        p->master = false;
    } else {
        p->slave = false;
    }
    unsigned pid = f->pty_master ? p->foreground : 0;
    bool unused = !p->master && !p->slave;
    spin_unlock_irqrestore(&p->lock, irq);
    f->pty = NULL;
    if (pid > 1) {
        task_kill(pid, 1);
    }
    if (unused) {
        kfree(p);
    }
}

static int interrupted(void) {
    struct task *t = this_cpu()->current;
    unsigned killed = task_pending_signal(t);
    if (killed) {
        task_exit(killed);
    }
    return signal_pending(t) ? -E_INTR : 0;
}

long pty_read(struct file *f, void *data, size_t size) {
    if (!size) {
        return 0;
    }
    struct pty *p = f->pty;
    for (;;) {
        int e = interrupted();
        if (e) {
            return e;
        }
        uint64_t irq = spin_lock_irqsave(&p->lock);
        struct pty_ring *r = f->pty_master ? &p->output : &p->input;
        size_t done = 0;
        while (done < size && r->count) {
            uint8_t c = pop(r);
            ((uint8_t *)data)[done++] = c;
            if (!f->pty_master && (p->config.lflag & ICANON) && c == '\n') {
                break;
            }
        }
        bool peer = f->pty_master ? p->slave : p->master;
        bool immediate = !f->pty_master && !(p->config.lflag & ICANON) && !p->config.cc[VMIN];
        bool eof = !f->pty_master && !done && p->eof;
        if (eof) {
            p->eof--;
        }
        bool overflow = !f->pty_master && !done && p->input_errors;
        if (overflow) {
            p->input_errors--;
        }
        spin_unlock_irqrestore(&p->lock, irq);
        if (overflow) {
            return -E_2BIG;
        }
        if (done || !peer || eof || immediate) {
            return done;
        }
        if (f->flags & O_NONBLOCK) {
            return -E_AGAIN;
        }
        sched_sleep(1000);
    }
}

long pty_write(struct file *f, const void *data, size_t size) {
    struct pty *p = f->pty;
    size_t done = 0;

    while (done < size) {
        int e = interrupted();
        if (e) {
            return done ? (long)done : e;
        }

        uint64_t irq = spin_lock_irqsave(&p->lock);
        bool peer = f->pty_master ? p->slave : p->master;
        if (!peer) {
            spin_unlock_irqrestore(&p->lock, irq);
            return done ? (long)done : -E_IO;
        }

        uint8_t c = ((const uint8_t *)data)[done];
        bool accepted = false;
        unsigned sig = 0;

        if (!f->pty_master) {
            bool cr = c == '\n' && (p->config.oflag & OPOST) && (p->config.oflag & ONLCR);
            if (PTY_BUFFER - p->output.count >= (cr ? 2u : 1u)) {
                if (cr) {
                    push(&p->output, '\r');
                }
                push(&p->output, c);
                accepted = true;
            }
        } else if ((p->config.lflag & ISIG) && c && c == p->config.cc[VINTR]) {
            sig = p->foreground;
            if (!(p->config.lflag & NOFLSH)) {
                p->input = (struct pty_ring){0};
                p->editing_size = p->input_errors = 0;
                p->discard_line = false;
            }

            if (p->config.lflag & ECHO) {
                echo(p, '^');
                echo(p, 'C');
                echo(p, '\r');
                echo(p, '\n');
            }
            accepted = true;
        } else {
            if (c == '\r' && (p->config.iflag & ICRNL)) {
                c = '\n';
            }

            if (p->config.lflag & ICANON) {
                if (p->discard_line) {
                    if (c == '\n' || (c && c == p->config.cc[VEOF])) {
                        p->discard_line = false;
                        if (p->input_errors < PTY_BUFFER) {
                            p->input_errors++;
                        }
                    }
                    accepted = true;
                } else if (c == p->config.cc[VERASE] || c == 8) {
                    if (p->editing_size) {
                        p->editing_size--;
                        if (p->config.lflag & ECHO) {
                            echo(p, 8);
                            echo(p, ' ');
                            echo(p, 8);
                        }
                    }
                    accepted = true;
                } else if (c == '\n' || (c && c == p->config.cc[VEOF])) {
                    unsigned needed = p->editing_size + (c == '\n');
                    if (PTY_BUFFER - p->input.count >= needed) {
                        for (unsigned i = 0; i < p->editing_size; i++) {
                            push(&p->input, p->editing[i]);
                        }
                        if (c == '\n') {
                            push(&p->input, '\n');
                            if (p->config.lflag & ECHO) {
                                echo(p, '\r');
                                echo(p, '\n');
                            }
                        } else if (!p->editing_size) {
                            p->eof++;
                        }
                        p->editing_size = 0;
                        accepted = true;
                    }
                } else if (p->editing_size < sizeof(p->editing) - 1) {
                    p->editing[p->editing_size++] = c;
                    accepted = true;
                    if (p->config.lflag & ECHO) {
                        echo(p, c);
                    }
                } else {
                    /* Discard the entire overlong line. Never block before its
                       terminator or let a shell execute a truncated command. */
                    p->editing_size = 0;
                    p->discard_line = true;
                    accepted = true;
                }
            } else if (p->input.count < PTY_BUFFER) {
                push(&p->input, c);
                accepted = true;
                if (p->config.lflag & ECHO) {
                    echo(p, c);
                }
            }
        }

        spin_unlock_irqrestore(&p->lock, irq);
        if (sig > 1) {
            task_kill(sig, 2);
        }

        if (accepted) {
            done++;
            continue;
        }

        if (done) {
            return done;
        }
        if (f->flags & O_NONBLOCK) {
            return -E_AGAIN;
        }

        sched_sleep(1000);
    }
    return done;
}

unsigned pty_poll(struct file *f, unsigned events) {
    struct pty *p = f->pty;
    uint64_t irq = spin_lock_irqsave(&p->lock);
    struct pty_ring *read = f->pty_master ? &p->output : &p->input,
                    *write = f->pty_master ? &p->input : &p->output;
    bool peer = f->pty_master ? p->slave : p->master;
    unsigned result = 0;
    if (read->count || !peer || (!f->pty_master && p->eof)) {
        result |= events & POLLIN;
    }
    if (!f->pty_master && p->input_errors) {
        result |= (events & POLLIN) | POLLERR;
    }
    if (write->count < PTY_BUFFER && peer) {
        result |= events & POLLOUT;
    }
    if (!peer) {
        result |= POLLHUP;
    }
    spin_unlock_irqrestore(&p->lock, irq);
    return result;
}

long pty_ioctl(struct file *f, unsigned request, uint64_t arg) {
    struct pty *p = f->pty;
    struct task *t = this_cpu()->current;
    if (request == OS_PTY_GET_FOREGROUND) {
        uint64_t irq = spin_lock_irqsave(&p->lock);
        unsigned pid = p->foreground;
        spin_unlock_irqrestore(&p->lock, irq);
        return user_copy_out(&t->space, arg, &pid, 4) ? 0 : -E_FAULT;
    }
    if (request == TCGETS) {
        uint64_t irq = spin_lock_irqsave(&p->lock);
        struct os_termios c = p->config;
        spin_unlock_irqrestore(&p->lock, irq);
        return user_copy_out(&t->space, arg, &c, sizeof(c)) ? 0 : -E_FAULT;
    }
    if (request == TCSETS || request == TCSETSW || request == TCSETSF) {
        struct os_termios c;
        if (!user_copy_in(&t->space, &c, arg, sizeof(c))) {
            return -E_FAULT;
        }
        if (c.line || c.iflag & ~ICRNL || c.oflag & ~(OPOST | ONLCR) ||
            c.lflag & ~(ISIG | ICANON | ECHO | NOFLSH) || c.cc[VMIN] > 1 || c.cc[VTIME]) {
            return -E_NOTSUP;
        }
        uint64_t irq = spin_lock_irqsave(&p->lock);
        if (request == TCSETSF) {
            p->input = (struct pty_ring){0};
            p->editing_size = p->eof = p->input_errors = 0;
            p->discard_line = false;
        }
        p->config = c;
        spin_unlock_irqrestore(&p->lock, irq);
        return 0;
    }
    if (request == TIOCGWINSZ) {
        uint64_t irq = spin_lock_irqsave(&p->lock);
        struct os_winsize size = p->size;
        spin_unlock_irqrestore(&p->lock, irq);
        return user_copy_out(&t->space, arg, &size, sizeof(size)) ? 0 : -E_FAULT;
    }
    if (request == TIOCSWINSZ) {
        struct os_winsize size;
        if (!user_copy_in(&t->space, &size, arg, sizeof(size))) {
            return -E_FAULT;
        }
        if (!size.rows || size.rows > 100 || !size.cols || size.cols > 240) {
            return -E_INVAL;
        }
        uint64_t irq = spin_lock_irqsave(&p->lock);
        bool changed = memcmp(&p->size, &size, sizeof(size));
        p->size = size;
        unsigned pid = p->foreground;
        spin_unlock_irqrestore(&p->lock, irq);
        if (changed && pid > 1) {
            task_kill(pid, 28);
        }
        return 0;
    }
    if (request == FIONREAD) {
        uint64_t irq = spin_lock_irqsave(&p->lock);
        int count = f->pty_master ? p->output.count : p->input.count;
        spin_unlock_irqrestore(&p->lock, irq);
        return user_copy_out(&t->space, arg, &count, sizeof(count)) ? 0 : -E_FAULT;
    }
    if (request == TCFLSH) {
        if (arg > 2) {
            return -E_INVAL;
        }
        uint64_t irq = spin_lock_irqsave(&p->lock);
        if (arg != 1) {
            p->input = (struct pty_ring){0};
            p->editing_size = p->eof = p->input_errors = 0;
            p->discard_line = false;
        }
        if (arg != 0) {
            p->output = (struct pty_ring){0};
        }
        spin_unlock_irqrestore(&p->lock, irq);
        return 0;
    }
    return -E_NOTTY;
}
