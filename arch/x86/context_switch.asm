; arch/x86/context_switch.asm
;
; Cooperative kernel-thread context switch.
;
; C prototype:
;
;   void x86_context_switch(
;       thread_context_t *old_context,
;       thread_context_t *new_context
;   );
;
; thread_context_t currently contains only:
;
;   uint32_t esp;
;
; The rest of the saved state lives on the stack.
;
; Stack layout expected for a newly created thread:
;
;   [esp +  0] saved EDI
;   [esp +  4] saved ESI
;   [esp +  8] saved EBX
;   [esp + 12] saved EBP
;   [esp + 16] return address
;
; After loading ESP, this routine pops EDI/ESI/EBX/EBP and RETs into the
; thread bootstrap function.

BITS 32

global x86_context_switch

x86_context_switch:
    ; Arguments before we push anything:
    ;   [esp + 4] = old_context
    ;   [esp + 8] = new_context
    mov eax, [esp + 4]
    mov edx, [esp + 8]

    ; Save callee-saved registers on the old thread's stack.
    push ebp
    push ebx
    push esi
    push edi

    ; Save old ESP into old_context->esp.
    mov [eax], esp

    ; Load new ESP from new_context->esp.
    mov esp, [edx]

    ; Restore callee-saved registers from the new thread's stack.
    pop edi
    pop esi
    pop ebx
    pop ebp

    ; Continue in the new thread.
    ret
