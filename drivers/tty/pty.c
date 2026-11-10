#include <mm/pmm.h>
#include <pty.h>
#include <sched.h>
#include <string.h>
#include <credentials.h>
#include <userns.h>
#include <pidns.h>

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
    unsigned master, slave;
    bool discard_line;
    unsigned input_errors;
    unsigned foreground, creator, eof, editing_size;
    uint8_t editing[512];
    struct user_budget *budget;
    unsigned private_id, session;
    bool locked;
};

#include "private_pty.inc"

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
    p->creator = task_input_identity(this_cpu()->current);
    p->size = (struct os_winsize){24, 80, 0, 0};
    p->config = (struct os_termios){.iflag = ICRNL,
                                    .oflag = OPOST | ONLCR,
                                    .cflag = 0xbf,
                                    .lflag = ISIG | ICANON | ECHO,
                                    .cc = {[VINTR] = 3, [VQUIT]=28, [VSUSP]=26, [VERASE] = 127, [VEOF] = 4, [VMIN] = 1}};
    return p;
}

unsigned pty_job_creator(struct pty *p) {
    return p->creator; /* Immutable for the lifetime of the pinned PTY. */
}
unsigned pty_job_foreground(struct pty *p) {
    uint64_t irq = spin_lock_irqsave(&p->lock);
    unsigned group = p->foreground;
    spin_unlock_irqrestore(&p->lock, irq);
    return group;
}
void pty_job_config(struct pty *p, struct os_termios *out) {
    uint64_t irq = spin_lock_irqsave(&p->lock);
    *out = p->config;
    spin_unlock_irqrestore(&p->lock, irq);
}
void pty_job_restore(struct pty *p, unsigned from, unsigned to, const struct os_termios *mode) {
    uint64_t irq = spin_lock_irqsave(&p->lock);
    if (p->foreground == from) {
        if (mode && ((p->config.lflag ^ mode->lflag) & ICANON)) {
            p->input = (struct pty_ring){0};
            p->editing_size = p->eof = p->input_errors = 0;
            p->discard_line = false;
        }
        if (mode)
            p->config = *mode;
        p->foreground = to;
    }
    spin_unlock_irqrestore(&p->lock, irq);
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
        p->master--;
    } else {
        p->slave--;
    }
    /* The creator controls the master. Closing its own recovered endpoint
       must not hang up the unrelated terminal containing that controller. */
    unsigned pid = f->pty_master && !p->master && p->foreground != p->creator ? p->foreground : 0;
    bool unused = !p->master && !p->slave;
    spin_unlock_irqrestore(&p->lock, irq);
    f->pty = NULL;
    if (pid > 1) {
        task_signal_foreground(pid, 1);
    }
    if (unused) {
        private_pty_release(p);
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
    if (!f->pty_master && this_cpu()->current->context->job_managed &&
        !task_terminal_access(this_cpu()->current, f))
        return task_background_terminal(this_cpu()->current,21);
    for (;;) {
        int e = interrupted();
        if (e) {
            return e;
        }
        uint64_t irq = spin_lock_irqsave(&p->lock);
        if (!f->pty_master && this_cpu()->current->context->job_managed &&
            p->foreground != task_input_identity(this_cpu()->current)) {
            spin_unlock_irqrestore(&p->lock, irq);
            return task_background_terminal(this_cpu()->current,21);
        }
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
        if(!f->pty_master&&this_cpu()->current->context->job_managed) {
            uint64_t flags=spin_lock_irqsave(&p->lock);
            bool denied=(p->config.lflag&TOSTOP)&&p->foreground!=task_input_identity(this_cpu()->current);
            spin_unlock_irqrestore(&p->lock,flags);
            if(denied)return done?(long)done:task_background_terminal(this_cpu()->current,22);
        }
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
        unsigned sig = 0,signal_number=2;

        if (!f->pty_master) {
            bool cr = c == '\n' && (p->config.oflag & OPOST) && (p->config.oflag & ONLCR);
            if (PTY_BUFFER - p->output.count >= (cr ? 2u : 1u)) {
                if (cr) {
                    push(&p->output, '\r');
                }
                push(&p->output, c);
                accepted = true;
            }
        } else if ((p->config.lflag & ISIG) && c &&
                   (c==p->config.cc[VINTR]||c==p->config.cc[VQUIT]||c==p->config.cc[VSUSP])) {
            signal_number=c==p->config.cc[VINTR]?2:c==p->config.cc[VQUIT]?3:20;
            sig = p->foreground;
            if (!(p->config.lflag & NOFLSH)) {
                p->input = (struct pty_ring){0};
                p->editing_size = p->input_errors = 0;
                p->discard_line = false;
            }

            if (p->config.lflag & ECHO) {
                echo(p, '^');
                echo(p, signal_number==20?'Z':signal_number==3?'\\':'C');
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
            task_signal_foreground(sig, signal_number);
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
    if(p->budget) {
        long result=private_pty_control(t,f,request,arg);
        if(result!=1)return result;
    }
    if (request == TIOCSPGRP) {
        unsigned group;
        if (!user_copy_in(&t->space, &group, arg, sizeof(group)))
            return -E_FAULT;
        return task_terminal_set(t, f, group);
    }
    if (!f->pty_master && t->context->job_managed &&
        (request == TCSETS || request == TCSETSW || request == TCSETSF ||
         request == TCFLSH || request == FIONREAD) &&
        !task_terminal_access(t, f))
        return request==FIONREAD?-E_PERM:task_background_terminal(t,22);
    if (request == OS_PTY_GET_FOREGROUND || request == TIOCGPGRP) {
        uint64_t irq = spin_lock_irqsave(&p->lock);
        unsigned pid = p->foreground;
        spin_unlock_irqrestore(&p->lock, irq);
        return user_copy_out(&t->space, arg, &pid, 4) ? 0 : -E_FAULT;
    }
    if (request == OS_PTY_GET_CWD) {
        struct os_pty_cwd request;
        if (!f->pty_master)
            return -E_PERM;
        if (!user_copy_in(&t->space, &request, arg, sizeof(request)))
            return -E_FAULT;
        if (request.reserved)
            return -E_INVAL;
        if (pty_job_foreground(p) != request.pid)
            return -E_AGAIN;
        int e = task_terminal_cwd(f, request.pid, request.path);
        if (e)
            return e;
        if (pty_job_foreground(p) != request.pid)
            return -E_AGAIN;
        return user_copy_out(&t->space, arg, &request, sizeof(request)) ? 0 : -E_FAULT;
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
            c.lflag & ~(ISIG | ICANON | ECHO | NOFLSH | TOSTOP) || c.cc[VMIN] > 1 || c.cc[VTIME]) {
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
            task_signal_foreground(pid, 28);
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
