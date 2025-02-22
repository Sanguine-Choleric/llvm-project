; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc -mtriple=amdgcn-amd-amdhsa -mcpu=gfx900 < %s | FileCheck %s

; We have an indirect call with a known set of callees, which are
; known to not need any special inputs. The ABI still needs to use the
; register

; FIXME: Passing real values for workitem ID, and 0s that can be undef

define amdgpu_kernel void @indirect_call_known_no_special_inputs() {
; CHECK-LABEL: indirect_call_known_no_special_inputs:
; CHECK:       ; %bb.0: ; %bb
; CHECK-NEXT:    s_mov_b64 s[40:41], s[4:5]
; CHECK-NEXT:    s_mov_b64 s[4:5], 0
; CHECK-NEXT:    s_load_dword s4, s[4:5], 0x0
; CHECK-NEXT:    s_add_u32 flat_scratch_lo, s12, s17
; CHECK-NEXT:    s_addc_u32 flat_scratch_hi, s13, 0
; CHECK-NEXT:    s_add_u32 s0, s0, s17
; CHECK-NEXT:    s_addc_u32 s1, s1, 0
; CHECK-NEXT:    s_waitcnt lgkmcnt(0)
; CHECK-NEXT:    s_bitcmp1_b32 s4, 0
; CHECK-NEXT:    s_cselect_b64 vcc, -1, 0
; CHECK-NEXT:    s_getpc_b64 s[4:5]
; CHECK-NEXT:    s_add_u32 s4, s4, wobble@gotpcrel32@lo+4
; CHECK-NEXT:    s_addc_u32 s5, s5, wobble@gotpcrel32@hi+12
; CHECK-NEXT:    s_mov_b64 s[38:39], s[6:7]
; CHECK-NEXT:    s_getpc_b64 s[6:7]
; CHECK-NEXT:    s_add_u32 s6, s6, snork@gotpcrel32@lo+4
; CHECK-NEXT:    s_addc_u32 s7, s7, snork@gotpcrel32@hi+12
; CHECK-NEXT:    s_mov_b64 s[34:35], s[10:11]
; CHECK-NEXT:    s_mov_b64 s[36:37], s[8:9]
; CHECK-NEXT:    s_load_dwordx2 s[8:9], s[6:7], 0x0
; CHECK-NEXT:    s_load_dwordx2 s[10:11], s[4:5], 0x0
; CHECK-NEXT:    v_lshlrev_b32_e32 v2, 20, v2
; CHECK-NEXT:    v_lshlrev_b32_e32 v1, 10, v1
; CHECK-NEXT:    s_mov_b32 s33, s16
; CHECK-NEXT:    s_waitcnt lgkmcnt(0)
; CHECK-NEXT:    v_mov_b32_e32 v3, s9
; CHECK-NEXT:    v_mov_b32_e32 v4, s11
; CHECK-NEXT:    v_cndmask_b32_e32 v4, v3, v4, vcc
; CHECK-NEXT:    v_mov_b32_e32 v3, s8
; CHECK-NEXT:    v_mov_b32_e32 v5, s10
; CHECK-NEXT:    s_mov_b32 s42, s15
; CHECK-NEXT:    s_mov_b32 s43, s14
; CHECK-NEXT:    v_cndmask_b32_e32 v3, v3, v5, vcc
; CHECK-NEXT:    v_or3_b32 v31, v0, v1, v2
; CHECK-NEXT:    v_mov_b32_e32 v1, 0
; CHECK-NEXT:    s_mov_b32 s32, 0
; CHECK-NEXT:    s_mov_b64 s[4:5], exec
; CHECK-NEXT:  .LBB0_1: ; =>This Inner Loop Header: Depth=1
; CHECK-NEXT:    v_readfirstlane_b32 s16, v3
; CHECK-NEXT:    v_readfirstlane_b32 s17, v4
; CHECK-NEXT:    v_cmp_eq_u64_e32 vcc, s[16:17], v[3:4]
; CHECK-NEXT:    s_and_saveexec_b64 s[44:45], vcc
; CHECK-NEXT:    s_mov_b64 s[4:5], s[40:41]
; CHECK-NEXT:    s_mov_b64 s[6:7], s[38:39]
; CHECK-NEXT:    s_mov_b64 s[8:9], s[36:37]
; CHECK-NEXT:    s_mov_b64 s[10:11], s[34:35]
; CHECK-NEXT:    s_mov_b32 s12, s43
; CHECK-NEXT:    s_mov_b32 s13, s42
; CHECK-NEXT:    s_mov_b32 s14, s33
; CHECK-NEXT:    v_mov_b32_e32 v4, v1
; CHECK-NEXT:    s_swappc_b64 s[30:31], s[16:17]
; CHECK-NEXT:    ; implicit-def: $vgpr3_vgpr4
; CHECK-NEXT:    ; implicit-def: $vgpr31
; CHECK-NEXT:    ; implicit-def: $vgpr1
; CHECK-NEXT:    s_xor_b64 exec, exec, s[44:45]
; CHECK-NEXT:    s_cbranch_execnz .LBB0_1
; CHECK-NEXT:  ; %bb.2:
; CHECK-NEXT:    s_endpgm

bb:
  %cond = load i1, i1 addrspace(4)* null
  %tmp = select i1 %cond, void (i8*, i32, i8*)* bitcast (void ()* @wobble to void (i8*, i32, i8*)*), void (i8*, i32, i8*)* bitcast (void ()* @snork to void (i8*, i32, i8*)*)
  call void %tmp(i8* undef, i32 undef, i8* undef)
  ret void
}

define void @wobble() {
; CHECK-LABEL: wobble:
; CHECK:       ; %bb.0: ; %bb
; CHECK-NEXT:    s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
; CHECK-NEXT:    s_setpc_b64 s[30:31]
bb:
  ret void
}

define void @snork() {
; CHECK-LABEL: snork:
; CHECK:       ; %bb.0: ; %bb
; CHECK-NEXT:    s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
; CHECK-NEXT:    s_setpc_b64 s[30:31]
bb:
  ret void
}
