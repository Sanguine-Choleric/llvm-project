; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc -mtriple=amdgcn-amd-amdhsa -mcpu=gfx900 -amdgpu-spill-cfi-saved-regs < %s | FileCheck %s
; RUN: llc -mtriple=amdgcn-amd-amdhsa -mcpu=gfx908 -amdgpu-spill-cfi-saved-regs < %s | FileCheck %s

; Function Attrs: noinline optnone
define fastcc void @tail_callee() #2 {
; CHECK-LABEL: tail_callee:
; CHECK:       ; %bb.0:
; CHECK-NEXT:    s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
; CHECK-NEXT:    s_or_saveexec_b64 s[4:5], -1
; CHECK-NEXT:    buffer_store_dword v0, off, s[0:3], s32 ; 4-byte Folded Spill
; CHECK-NEXT:    s_mov_b64 exec, s[4:5]
; CHECK-NEXT:    v_writelane_b32 v0, exec_lo, 0
; CHECK-NEXT:    v_writelane_b32 v0, exec_hi, 1
; CHECK-NEXT:    s_or_saveexec_b64 s[4:5], -1
; CHECK-NEXT:    buffer_load_dword v0, off, s[0:3], s32 ; 4-byte Folded Reload
; CHECK-NEXT:    s_mov_b64 exec, s[4:5]
; CHECK-NEXT:    s_waitcnt vmcnt(0)
; CHECK-NEXT:    s_setpc_b64 s[30:31]
  ret void
}

; Function Attrs: noinline
define fastcc void @callee_no_fp() #0 {
; CHECK-LABEL: callee_no_fp:
; CHECK:       ; %bb.0: ; %entry
; CHECK-NEXT:    s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
; CHECK-NEXT:    s_or_saveexec_b64 s[16:17], -1
; CHECK-NEXT:    buffer_store_dword v1, off, s[0:3], s32 ; 4-byte Folded Spill
; CHECK-NEXT:    s_mov_b64 exec, s[16:17]
; CHECK-NEXT:    v_writelane_b32 v1, exec_lo, 2
; CHECK-NEXT:    v_writelane_b32 v1, exec_hi, 3
; CHECK-NEXT:    v_writelane_b32 v1, s33, 4
; CHECK-NEXT:    s_mov_b32 s33, s32
; CHECK-NEXT:    v_writelane_b32 v1, s30, 0
; CHECK-NEXT:    s_addk_i32 s32, 0x400
; CHECK-NEXT:    v_writelane_b32 v1, s31, 1
; CHECK-NEXT:    s_getpc_b64 s[16:17]
; CHECK-NEXT:    s_add_u32 s16, s16, tail_callee@gotpcrel32@lo+4
; CHECK-NEXT:    s_addc_u32 s17, s17, tail_callee@gotpcrel32@hi+12
; CHECK-NEXT:    s_load_dwordx2 s[16:17], s[16:17], 0x0
; CHECK-NEXT:    s_waitcnt lgkmcnt(0)
; CHECK-NEXT:    s_swappc_b64 s[30:31], s[16:17]
entry:
  tail call fastcc void @tail_callee() #3
  unreachable
}

define protected amdgpu_kernel void @kernel() #1 {
; CHECK-LABEL: kernel:
; CHECK:       ; %bb.0: ; %entry
; CHECK-NEXT:    s_add_u32 flat_scratch_lo, s12, s17
; CHECK-NEXT:    s_addc_u32 flat_scratch_hi, s13, 0
; CHECK-NEXT:    s_add_u32 s0, s0, s17
; CHECK-NEXT:    s_addc_u32 s1, s1, 0
; CHECK-NEXT:    s_mov_b32 s32, 0
; CHECK-NEXT:    s_cbranch_scc0 .LBB2_2
; CHECK-NEXT:  ; %bb.1: ; %end
; CHECK-NEXT:    s_endpgm
; CHECK-NEXT:  .LBB2_2: ; %body
; CHECK-NEXT:    s_getpc_b64 s[12:13]
; CHECK-NEXT:    s_add_u32 s12, s12, callee_no_fp@gotpcrel32@lo+4
; CHECK-NEXT:    s_addc_u32 s13, s13, callee_no_fp@gotpcrel32@hi+12
; CHECK-NEXT:    s_load_dwordx2 s[18:19], s[12:13], 0x0
; CHECK-NEXT:    v_lshlrev_b32_e32 v2, 20, v2
; CHECK-NEXT:    v_lshlrev_b32_e32 v1, 10, v1
; CHECK-NEXT:    v_or3_b32 v31, v0, v1, v2
; CHECK-NEXT:    s_mov_b32 s12, s14
; CHECK-NEXT:    s_mov_b32 s13, s15
; CHECK-NEXT:    s_mov_b32 s14, s16
; CHECK-NEXT:    s_waitcnt lgkmcnt(0)
; CHECK-NEXT:    s_swappc_b64 s[30:31], s[18:19]
entry:
  br i1 undef, label %end, label %body

body:                                 ; preds = %entry
  tail call fastcc void @callee_no_fp() #3
  unreachable

end:                                  ; preds = %entry
  ret void
}

; When we have calls, spilling a CSR VGPR for CFI saves should force FP usage
; Function Attrs: noinline
define dso_local fastcc void @func_needs_fp() unnamed_addr #0 {
; CHECK-LABEL: func_needs_fp:
; CHECK:       func_needs_fp$local:
; CHECK-NEXT:  ; %bb.0: ; %entry
; CHECK-NEXT:    s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
; CHECK-NEXT:    s_or_saveexec_b64 s[16:17], -1
; CHECK-NEXT:    buffer_store_dword v40, off, s[0:3], s32 ; 4-byte Folded Spill
; CHECK-NEXT:    s_mov_b64 exec, s[16:17]
; CHECK-NEXT:    v_writelane_b32 v40, exec_lo, 2
; CHECK-NEXT:    v_writelane_b32 v40, exec_hi, 3
; CHECK-NEXT:    v_writelane_b32 v40, s33, 4
; CHECK-NEXT:    s_mov_b32 s33, s32
; CHECK-NEXT:    v_writelane_b32 v40, s30, 0
; CHECK-NEXT:    s_addk_i32 s32, 0x400
; CHECK-NEXT:    v_writelane_b32 v40, s31, 1
; CHECK-NEXT:    s_getpc_b64 s[16:17]
; CHECK-NEXT:    s_add_u32 s16, s16, tail_callee_fp@rel32@lo+4
; CHECK-NEXT:    s_addc_u32 s17, s17, tail_callee_fp@rel32@hi+12
; CHECK-NEXT:    s_swappc_b64 s[30:31], s[16:17]
entry:
  tail call fastcc void @tail_callee_fp() #3
  unreachable
}

; Function Attrs: noinline optnone
declare dso_local fastcc void @tail_callee_fp() unnamed_addr #2

attributes #0 = { noinline }
attributes #1 = { "use-soft-float"="false" }
attributes #2 = { noinline optnone }
attributes #3 = { convergent nounwind }

