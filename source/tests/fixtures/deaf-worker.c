/* deaf-worker -- a worker that exits without reading its request.
 *
 * This is not contrived: the M4 worker writes straight into the jail and never
 * touches stdin, and any worker that dies early (bad args, cannot enter the
 * jail, OOM) behaves the same way. The parent is still writing a request that
 * can be over a megabyte when the pipe closes.
 *
 * On a developer machine the parent survives this; in the booted image the same
 * code produced rc=141 -- SIGPIPE -- and killed PID 1. The difference is timing:
 * whoever wins the race decides whether the write lands before the exit.
 */
#include <unistd.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    return 3;              /* gone before reading a single byte */
}
