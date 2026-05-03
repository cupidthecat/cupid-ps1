#include "common/fastjmp.h"

#if defined(__x86_64__)

__asm__(
  "\t.global fastjmp_set\n"
  "\t.global fastjmp_jmp\n"
  "\t.text\n"
  "fastjmp_set:\n"
  "\tmovq 0(%rsp), %rax\n"
  "\tmovq %rsp, %rdx\n"          /* fixup stack pointer to skip the call */
  "\taddq $8, %rdx\n"
  "\tmovq %rax, 0(%rdi)\n"       /* rip */
  "\tmovq %rbx, 8(%rdi)\n"
  "\tmovq %rdx, 16(%rdi)\n"      /* rsp */
  "\tmovq %rbp, 24(%rdi)\n"
  "\tmovq %r12, 32(%rdi)\n"
  "\tmovq %r13, 40(%rdi)\n"
  "\tmovq %r14, 48(%rdi)\n"
  "\tmovq %r15, 56(%rdi)\n"
  "\txorl %eax, %eax\n"
  "\tret\n" 

  "fastjmp_jmp:\n"
  "\tmovl %esi, %eax\n" 
  "\tmovq 0(%rdi), %rdx\n"       /* rip */
  "\tmovq 8(%rdi), %rbx\n"
  "\tmovq 16(%rdi), %rsp\n"
  "\tmovq 24(%rdi), %rbp\n"
  "\tmovq 32(%rdi), %r12\n"
  "\tmovq 40(%rdi), %r13\n"
  "\tmovq 48(%rdi), %r14\n"
  "\tmovq 56(%rdi), %r15\n"
  "\tjmp *%rdx\n" 
);

#elif defined(__aarch64__)

__asm__(
  "\t.global fastjmp_set\n"
  "\t.global fastjmp_jmp\n"
  "\t.text\n"
  "\t.align 16\n"
  "fastjmp_set:\n"
  "\tmov x16, sp\n"
  "\tstp x16, x30, [x0]\n"
  "\tstp x19, x20, [x0, #16]\n"
  "\tstp x21, x22, [x0, #32]\n"
  "\tstp x23, x24, [x0, #48]\n"
  "\tstp x25, x26, [x0, #64]\n"
  "\tstp x27, x28, [x0, #80]\n"
  "\tstr x29, [x0, #96]\n"
  "\tstp d8,  d9,  [x0, #112]\n"
  "\tstp d10, d11, [x0, #128]\n"
  "\tstp d12, d13, [x0, #144]\n"
  "\tstp d14, d15, [x0, #160]\n"
  "\tmov w0, wzr\n"
  "\tbr x30\n" 

  "\t.align 16\n"
  "fastjmp_jmp:\n"
  "\tldp x16, x30, [x0]\n"
  "\tmov sp, x16\n"
  "\tldp x19, x20, [x0, #16]\n"
  "\tldp x21, x22, [x0, #32]\n"
  "\tldp x23, x24, [x0, #48]\n"
  "\tldp x25, x26, [x0, #64]\n"
  "\tldp x27, x28, [x0, #80]\n"
  "\tldr x29, [x0, #96]\n"
  "\tldp d8,  d9,  [x0, #112]\n"
  "\tldp d10, d11, [x0, #128]\n"
  "\tldp d12, d13, [x0, #144]\n"
  "\tldp d14, d15, [x0, #160]\n"
  "\tmov w0, w1\n"
  "\tbr x30\n" 
);

#else
#  error "fastjmp: unsupported architecture"
#endif
