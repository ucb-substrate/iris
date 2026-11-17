// Wraps newlib-nano's exit() (linked with -Wl,--wrap=exit), for every
// htif_nano program.
//
// In riscv-tools 1.0.9 (GCC 16.1), newlib-nano's __call_exitprocs, which
// exit() runs, reads args->_fntypes without checking args for NULL. Under
// _REENT_SMALL, args is the atexit block's _on_exit_args_ptr, which stays NULL
// unless something registers with on_exit or __cxa_atexit -- and the startup
// code only registers __libc_fini_array with plain atexit. So returning from
// main reads 0x200, inside the debug module's range, which the cores deny
// outside debug mode: a load access fault (mcause 5), and the program exits
// with code 5. riscv-tools 1.0.6 (GCC 13.2) checks for NULL there.
//
// Pointing each block's _on_exit_args_ptr at zeroed storage before newlib's
// exit() runs gives that read a valid target. _fntypes 0 marks every handler
// as plain atexit, which is what they are, so exit() behaves as it should:
// it runs the atexit handlers and .fini_array and flushes stdio. Fixing it up
// at exit rather than at startup also covers blocks registered from main.
#include <stddef.h>
#include <sys/reent.h>

// The layout __call_exitprocs was compiled against; a different toolchain
// whose headers disagree fails here rather than faulting at run time.
_Static_assert(offsetof(struct _atexit, _on_exit_args_ptr) == 272,
               "newlib _atexit layout differs from riscv-tools 1.0.9");
_Static_assert(offsetof(struct _on_exit_args, _fntypes) == 512,
               "newlib _on_exit_args layout differs from riscv-tools 1.0.9");

extern struct _atexit *__atexit;  // newlib's list of atexit blocks

void __real_exit(int code) __attribute__((noreturn));

void __wrap_exit(int code) {
  // Shared by every block: it is only read, and all-zero is right for each.
  static struct _on_exit_args args;
  for (struct _atexit *p = __atexit; p != NULL; p = p->_next)
    if (p->_on_exit_args_ptr == NULL)
      p->_on_exit_args_ptr = &args;
  __real_exit(code);
}
