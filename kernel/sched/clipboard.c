#include <mm/pmm.h>
#include <clipboard.h>
#include <pty.h>
#include <sched.h>
#include <string.h>
#include <text_utf8.h>
#include <tty.h>
#include <uapi/clipboard.h>
#include <accounts.h>
#include <credentials.h>
#include <video/window.h>

static spinlock_t lock;
static char text[OS_CLIPBOARD_LIMIT];
static uint32_t length;
static uint64_t generation = 1;

static bool permitted(struct task *t) {
    /* The native Electron compositor remains root and owns its window; its
       child runtime drops credentials and never owns that native window.
       Like account clients, the compositor must have actual window focus. */
    bool native_owner = t && t->context && !t->userns &&
        !t->browser_sandbox && !t->context->web_role && !credentials_uid(t);
    if ((accounts_window_client(t) || native_owner) && window_client_focused(t->tgid))
        return true;
    unsigned own = task_input_identity(t);
    for (unsigned i = 0; i < OS_FD_MAX; i++) {
        struct file *f = t->fds[i];
        /* A GUI child can inherit its parent's process group without a
           terminal capability. That alone must not authorize clipboard IO. */
        if (f && f->tty && tty_is_foreground(own))
            return true;
        if (f && f->pty && !f->pty_master && pty_job_foreground(f->pty) == own)
            return true;
    }
    return false;
}

long clipboard_call(struct task *t, unsigned op, uint64_t pointer, uint64_t size,
                    uint64_t expected) {
    if (!permitted(t))
        return -E_PERM;
    if (op == OS_CLIPBOARD_INFO) {
        if (size != sizeof(struct os_clipboard_info))
            return -E_INVAL;
        uint64_t irq = spin_lock_irqsave(&lock);
        struct os_clipboard_info info = {generation, length, OS_CLIPBOARD_LIMIT};
        spin_unlock_irqrestore(&lock, irq);
        return user_copy_out(&t->space, pointer, &info, sizeof(info)) ? 0 : -E_FAULT;
    }
    if (op == OS_CLIPBOARD_CLEAR) {
        if (pointer || size || expected)
            return -E_INVAL;
        uint64_t irq = spin_lock_irqsave(&lock);
        length = 0;
        memset(text, 0, sizeof(text));
        if (!++generation)
            generation = 1;
        spin_unlock_irqrestore(&lock, irq);
        return 0;
    }
    if (op != OS_CLIPBOARD_READ && op != OS_CLIPBOARD_WRITE &&
        op != OS_CLIPBOARD_WRITE_IF_CURRENT)
        return -E_INVAL;
    if (size > OS_CLIPBOARD_LIMIT)
        return -E_2BIG;
    if (op == OS_CLIPBOARD_WRITE && expected)
        return -E_INVAL;
    char *copy = kmalloc(OS_CLIPBOARD_LIMIT);
    if (!copy)
        return -E_NOMEM;
    long result;
    if (op == OS_CLIPBOARD_WRITE || op == OS_CLIPBOARD_WRITE_IF_CURRENT) {
        if (size && !user_copy_in(&t->space, copy, pointer, size))
            result = -E_FAULT;
        else if (!text_utf8_valid(copy, size))
            result = -E_INVAL;
        else {
            uint64_t irq = spin_lock_irqsave(&lock);
            if (op == OS_CLIPBOARD_WRITE_IF_CURRENT && (!expected || expected != generation)) {
                spin_unlock_irqrestore(&lock, irq);
                kfree(copy);
                return -E_AGAIN;
            }
            if (size)
                memcpy(text, copy, size);
            if (length > size)
                memset(text + size, 0, length - size);
            length = size;
            if (!++generation)
                generation = 1;
            spin_unlock_irqrestore(&lock, irq);
            result = size;
        }
    } else {
        uint64_t irq = spin_lock_irqsave(&lock);
        if (expected && expected != generation)
            result = -E_AGAIN;
        else if (size < length)
            result = -E_NOSPC;
        else {
            result = length;
            if (length)
                memcpy(copy, text, length);
        }
        spin_unlock_irqrestore(&lock, irq);
        if (result > 0 && !user_copy_out(&t->space, pointer, copy, result))
            result = -E_FAULT;
    }
    kfree(copy);
    return result;
}
