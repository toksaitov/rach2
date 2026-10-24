/* Child exit notification and the parent's exec identity. */
#include "process.h"
#include "tgroup.h"

/* Advance on successful exec. At wrap, every existing child must retain a
 * different generation; later children snapshot the new value of one. */
void proc_exec_generation(process_t *parent)
{
    struct tgroup *tg = parent->tg;
    process_t *p;

    if (tg->exec_generation == (uint32_t)-1) {
        for (p = proc_list; p; p = p->next) {
            if (p->tg != NULL && p->pid == p->tgid && p->parent_pid == parent->tgid) {
                p->tg->parent_exec_generation = 0;
            }
        }
        tg->exec_generation = 1;
    } else {
        tg->exec_generation++;
    }
}

/* A new parent image gets ordinary SIGCHLD notification, even for a child
 * originally cloned with no signal. The wait classification changes too. */
int proc_child_signal(process_t *child, const process_t *parent)
{
    struct tgroup *tg = child->tg;

    if (tg->parent_exec_generation != parent->tg->exec_generation) {
        tg->no_child_signal = 0;
    }
    return !tg->no_child_signal;
}
