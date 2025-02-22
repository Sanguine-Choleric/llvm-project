; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc < %s -mtriple=x86_64-unknown-unknown -mattr=+sse4.2 | FileCheck %s

; widen a v3i1 to v4i1 to do a vector load/store. We would previously
; reconstruct the said v3i1 from the first element of the vector by filling all
; the lanes of the vector with that first element, which was obviously wrong.
; This was done in the type-legalizing of the DAG, when legalizing the load.

; Function Attrs: argmemonly nounwind readonly
declare <3 x i32> @llvm.masked.load.v3i32.p1v3i32(<3 x i32> addrspace(1)*, i32, <3 x i1>, <3 x i32>)

; Function Attrs: argmemonly nounwind
declare void @llvm.masked.store.v3i32.p1v3i32(<3 x i32>, <3 x i32> addrspace(1)*, i32, <3 x i1>)

define  <3 x i32> @masked_load_v3(i32 addrspace(1)*, <3 x i1>) {
; CHECK-LABEL: masked_load_v3:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    andb $1, %sil
; CHECK-NEXT:    andb $1, %dl
; CHECK-NEXT:    addb %dl, %dl
; CHECK-NEXT:    orb %sil, %dl
; CHECK-NEXT:    andb $1, %cl
; CHECK-NEXT:    shlb $2, %cl
; CHECK-NEXT:    orb %dl, %cl
; CHECK-NEXT:    testb $1, %cl
; CHECK-NEXT:    # implicit-def: $xmm0
; CHECK-NEXT:    jne .LBB0_1
; CHECK-NEXT:  # %bb.2: # %else
; CHECK-NEXT:    testb $2, %cl
; CHECK-NEXT:    jne .LBB0_3
; CHECK-NEXT:  .LBB0_4: # %else2
; CHECK-NEXT:    testb $4, %cl
; CHECK-NEXT:    jne .LBB0_5
; CHECK-NEXT:  .LBB0_6: # %else5
; CHECK-NEXT:    retq
; CHECK-NEXT:  .LBB0_1: # %cond.load
; CHECK-NEXT:    movd {{.*#+}} xmm0 = mem[0],zero,zero,zero
; CHECK-NEXT:    testb $2, %cl
; CHECK-NEXT:    je .LBB0_4
; CHECK-NEXT:  .LBB0_3: # %cond.load1
; CHECK-NEXT:    pinsrd $1, 4(%rdi), %xmm0
; CHECK-NEXT:    testb $4, %cl
; CHECK-NEXT:    je .LBB0_6
; CHECK-NEXT:  .LBB0_5: # %cond.load4
; CHECK-NEXT:    pinsrd $2, 8(%rdi), %xmm0
; CHECK-NEXT:    retq
entry:
  %2 = bitcast i32 addrspace(1)* %0 to <3 x i32> addrspace(1)*
  %3 = call <3 x i32> @llvm.masked.load.v3i32.p1v3i32(<3 x i32> addrspace(1)* %2, i32 4, <3 x i1> %1, <3 x i32> undef)
  ret <3 x i32> %3
}

define void @masked_store4_v3(<3 x i32>, i32 addrspace(1)*, <3 x i1>) {
; CHECK-LABEL: masked_store4_v3:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    andb $1, %sil
; CHECK-NEXT:    andb $1, %dl
; CHECK-NEXT:    addb %dl, %dl
; CHECK-NEXT:    orb %sil, %dl
; CHECK-NEXT:    andb $1, %cl
; CHECK-NEXT:    shlb $2, %cl
; CHECK-NEXT:    orb %dl, %cl
; CHECK-NEXT:    testb $1, %cl
; CHECK-NEXT:    jne .LBB1_1
; CHECK-NEXT:  # %bb.2: # %else
; CHECK-NEXT:    testb $2, %cl
; CHECK-NEXT:    jne .LBB1_3
; CHECK-NEXT:  .LBB1_4: # %else2
; CHECK-NEXT:    testb $4, %cl
; CHECK-NEXT:    jne .LBB1_5
; CHECK-NEXT:  .LBB1_6: # %else4
; CHECK-NEXT:    retq
; CHECK-NEXT:  .LBB1_1: # %cond.store
; CHECK-NEXT:    movss %xmm0, (%rdi)
; CHECK-NEXT:    testb $2, %cl
; CHECK-NEXT:    je .LBB1_4
; CHECK-NEXT:  .LBB1_3: # %cond.store1
; CHECK-NEXT:    extractps $1, %xmm0, 4(%rdi)
; CHECK-NEXT:    testb $4, %cl
; CHECK-NEXT:    je .LBB1_6
; CHECK-NEXT:  .LBB1_5: # %cond.store3
; CHECK-NEXT:    extractps $2, %xmm0, 8(%rdi)
; CHECK-NEXT:    retq
entry:
  %3 = bitcast i32 addrspace(1)* %1 to <3 x i32> addrspace(1)*
  call void @llvm.masked.store.v3i32.p1v3i32(<3 x i32> %0, <3 x i32> addrspace(1)* %3, i32 4, <3 x i1> %2)
  ret void
}

define void @local_load_v3i1(i32 addrspace(1)* %out, i32 addrspace(1)* %in, <3 x i1>* %predicate_ptr) nounwind {
; CHECK-LABEL: local_load_v3i1:
; CHECK:       # %bb.0:
; CHECK-NEXT:    pushq %rbp
; CHECK-NEXT:    pushq %r15
; CHECK-NEXT:    pushq %r14
; CHECK-NEXT:    pushq %rbx
; CHECK-NEXT:    pushq %rax
; CHECK-NEXT:    movq %rdi, %r14
; CHECK-NEXT:    movzbl (%rdx), %eax
; CHECK-NEXT:    movl %eax, %ecx
; CHECK-NEXT:    shrb %cl
; CHECK-NEXT:    andb $1, %cl
; CHECK-NEXT:    movl %eax, %edx
; CHECK-NEXT:    shrb $2, %dl
; CHECK-NEXT:    andb $1, %al
; CHECK-NEXT:    movzbl %al, %ebp
; CHECK-NEXT:    movzbl %dl, %r15d
; CHECK-NEXT:    movzbl %cl, %ebx
; CHECK-NEXT:    movq %rsi, %rdi
; CHECK-NEXT:    movl %ebp, %esi
; CHECK-NEXT:    movl %ebx, %edx
; CHECK-NEXT:    movl %r15d, %ecx
; CHECK-NEXT:    callq masked_load_v3@PLT
; CHECK-NEXT:    movq %r14, %rdi
; CHECK-NEXT:    movl %ebp, %esi
; CHECK-NEXT:    movl %ebx, %edx
; CHECK-NEXT:    movl %r15d, %ecx
; CHECK-NEXT:    callq masked_store4_v3@PLT
; CHECK-NEXT:    addq $8, %rsp
; CHECK-NEXT:    popq %rbx
; CHECK-NEXT:    popq %r14
; CHECK-NEXT:    popq %r15
; CHECK-NEXT:    popq %rbp
; CHECK-NEXT:    retq
  %predicate = load <3 x i1>, <3 x i1>* %predicate_ptr
  %load1 = call <3 x i32> @masked_load_v3(i32 addrspace(1)* %in, <3 x i1> %predicate)
  call void @masked_store4_v3(<3 x i32> %load1, i32 addrspace(1)* %out, <3 x i1> %predicate)
  ret void
}
