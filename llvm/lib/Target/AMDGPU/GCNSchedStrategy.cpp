//===-- GCNSchedStrategy.cpp - GCN Scheduler Strategy ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This contains a MachineSchedStrategy implementation for maximizing wave
/// occupancy on GCN hardware.
///
/// This pass will apply multiple scheduling stages to the same function.
/// Regions are first recorded in GCNScheduleDAGMILive::schedule. The actual
/// entry point for the scheduling of those regions is
/// GCNScheduleDAGMILive::runSchedStages.

/// Generally, the reason for having multiple scheduling stages is to account
/// for the kernel-wide effect of register usage on occupancy.  Usually, only a
/// few scheduling regions will have register pressure high enough to limit
/// occupancy for the kernel, so constraints can be relaxed to improve ILP in
/// other regions.
///
//===----------------------------------------------------------------------===//

#include "GCNSchedStrategy.h"
#include "AMDGPUIGroupLP.h"
#include "SIMachineFunctionInfo.h"
#include "llvm/CodeGen/RegisterClassInfo.h"

#define DEBUG_TYPE "machine-scheduler"

using namespace llvm;

static cl::opt<bool>
    DisableUnclusterHighRP("amdgpu-disable-unclustred-high-rp-reschedule",
                           cl::Hidden,
                           cl::desc("Disable unclustred high register pressure "
                                    "reduction scheduling stage."),
                           cl::init(false));

GCNMaxOccupancySchedStrategy::GCNMaxOccupancySchedStrategy(
    const MachineSchedContext *C)
    : GenericScheduler(C), TargetOccupancy(0), MF(nullptr),
      HasHighPressure(false) {
        SIMachineFunctionInfo *MFI;
        MFI =
          const_cast<SIMachineFunctionInfo *>(C->MF->getInfo<SIMachineFunctionInfo>());
        MFI->setInitialOccupancy();
        #ifdef DEBUG_RESET_OCCUPANCY
          printf("Occ before AMD: %d\n", MFI->getOccupancy());
        #endif
      }

void GCNMaxOccupancySchedStrategy::initialize(ScheduleDAGMI *DAG) {
  GenericScheduler::initialize(DAG);

  MF = &DAG->MF;

  const GCNSubtarget &ST = MF->getSubtarget<GCNSubtarget>();

  SGPRExcessLimit =
      Context->RegClassInfo->getNumAllocatableRegs(&AMDGPU::SGPR_32RegClass);
  VGPRExcessLimit =
      Context->RegClassInfo->getNumAllocatableRegs(&AMDGPU::VGPR_32RegClass);

  SIMachineFunctionInfo &MFI = *MF->getInfo<SIMachineFunctionInfo>();
  // Set the initial TargetOccupnacy to the maximum occupancy that we can
  // achieve for this function. This effectively sets a lower bound on the
  // 'Critical' register limits in the scheduler.
  TargetOccupancy = MFI.getOccupancy();
  SGPRCriticalLimit =
      std::min(ST.getMaxNumSGPRs(TargetOccupancy, true), SGPRExcessLimit);
  VGPRCriticalLimit =
      std::min(ST.getMaxNumVGPRs(TargetOccupancy), VGPRExcessLimit);

  // Subtract error margin from register limits and avoid overflow.
  SGPRCriticalLimit =
      std::min(SGPRCriticalLimit - ErrorMargin, SGPRCriticalLimit);
  VGPRCriticalLimit =
      std::min(VGPRCriticalLimit - ErrorMargin, VGPRCriticalLimit);
  SGPRExcessLimit = std::min(SGPRExcessLimit - ErrorMargin, SGPRExcessLimit);
  VGPRExcessLimit = std::min(VGPRExcessLimit - ErrorMargin, VGPRExcessLimit);
}

void GCNMaxOccupancySchedStrategy::initCandidate(SchedCandidate &Cand, SUnit *SU,
                                     bool AtTop, const RegPressureTracker &RPTracker,
                                     const SIRegisterInfo *SRI,
                                     unsigned SGPRPressure,
                                     unsigned VGPRPressure) {

  Cand.SU = SU;
  Cand.AtTop = AtTop;

  // getDownwardPressure() and getUpwardPressure() make temporary changes to
  // the tracker, so we need to pass those function a non-const copy.
  RegPressureTracker &TempTracker = const_cast<RegPressureTracker&>(RPTracker);

  Pressure.clear();
  MaxPressure.clear();

  if (AtTop)
    TempTracker.getDownwardPressure(SU->getInstr(), Pressure, MaxPressure);
  else {
    // FIXME: I think for bottom up scheduling, the register pressure is cached
    // and can be retrieved by DAG->getPressureDif(SU).
    TempTracker.getUpwardPressure(SU->getInstr(), Pressure, MaxPressure);
  }

  unsigned NewSGPRPressure = Pressure[AMDGPU::RegisterPressureSets::SReg_32];
  unsigned NewVGPRPressure = Pressure[AMDGPU::RegisterPressureSets::VGPR_32];

  // If two instructions increase the pressure of different register sets
  // by the same amount, the generic scheduler will prefer to schedule the
  // instruction that increases the set with the least amount of registers,
  // which in our case would be SGPRs.  This is rarely what we want, so
  // when we report excess/critical register pressure, we do it either
  // only for VGPRs or only for SGPRs.

  // FIXME: Better heuristics to determine whether to prefer SGPRs or VGPRs.
  const unsigned MaxVGPRPressureInc = 16;
  bool ShouldTrackVGPRs = VGPRPressure + MaxVGPRPressureInc >= VGPRExcessLimit;
  bool ShouldTrackSGPRs = !ShouldTrackVGPRs && SGPRPressure >= SGPRExcessLimit;


  // FIXME: We have to enter REG-EXCESS before we reach the actual threshold
  // to increase the likelihood we don't go over the limits.  We should improve
  // the analysis to look through dependencies to find the path with the least
  // register pressure.

  // We only need to update the RPDelta for instructions that increase register
  // pressure. Instructions that decrease or keep reg pressure the same will be
  // marked as RegExcess in tryCandidate() when they are compared with
  // instructions that increase the register pressure.
  if (ShouldTrackVGPRs && NewVGPRPressure >= VGPRExcessLimit) {
    HasHighPressure = true;
    Cand.RPDelta.Excess = PressureChange(AMDGPU::RegisterPressureSets::VGPR_32);
    Cand.RPDelta.Excess.setUnitInc(NewVGPRPressure - VGPRExcessLimit);
  }

  if (ShouldTrackSGPRs && NewSGPRPressure >= SGPRExcessLimit) {
    HasHighPressure = true;
    Cand.RPDelta.Excess = PressureChange(AMDGPU::RegisterPressureSets::SReg_32);
    Cand.RPDelta.Excess.setUnitInc(NewSGPRPressure - SGPRExcessLimit);
  }

  // Register pressure is considered 'CRITICAL' if it is approaching a value
  // that would reduce the wave occupancy for the execution unit.  When
  // register pressure is 'CRITICAL', increasing SGPR and VGPR pressure both
  // has the same cost, so we don't need to prefer one over the other.

  int SGPRDelta = NewSGPRPressure - SGPRCriticalLimit;
  int VGPRDelta = NewVGPRPressure - VGPRCriticalLimit;

  if (SGPRDelta >= 0 || VGPRDelta >= 0) {
    HasHighPressure = true;
    if (SGPRDelta > VGPRDelta) {
      Cand.RPDelta.CriticalMax =
        PressureChange(AMDGPU::RegisterPressureSets::SReg_32);
      Cand.RPDelta.CriticalMax.setUnitInc(SGPRDelta);
    } else {
      Cand.RPDelta.CriticalMax =
        PressureChange(AMDGPU::RegisterPressureSets::VGPR_32);
      Cand.RPDelta.CriticalMax.setUnitInc(VGPRDelta);
    }
  }
}

// This function is mostly cut and pasted from
// GenericScheduler::pickNodeFromQueue()
void GCNMaxOccupancySchedStrategy::pickNodeFromQueue(SchedBoundary &Zone,
                                         const CandPolicy &ZonePolicy,
                                         const RegPressureTracker &RPTracker,
                                         SchedCandidate &Cand) {
  const SIRegisterInfo *SRI = static_cast<const SIRegisterInfo*>(TRI);
  ArrayRef<unsigned> Pressure = RPTracker.getRegSetPressureAtPos();
  unsigned SGPRPressure = Pressure[AMDGPU::RegisterPressureSets::SReg_32];
  unsigned VGPRPressure = Pressure[AMDGPU::RegisterPressureSets::VGPR_32];
  ReadyQueue &Q = Zone.Available;
  for (SUnit *SU : Q) {

    SchedCandidate TryCand(ZonePolicy);
    initCandidate(TryCand, SU, Zone.isTop(), RPTracker, SRI,
                  SGPRPressure, VGPRPressure);
    // Pass SchedBoundary only when comparing nodes from the same boundary.
    SchedBoundary *ZoneArg = Cand.AtTop == TryCand.AtTop ? &Zone : nullptr;
    GenericScheduler::tryCandidate(Cand, TryCand, ZoneArg);
    if (TryCand.Reason != NoCand) {
      // Initialize resource delta if needed in case future heuristics query it.
      if (TryCand.ResDelta == SchedResourceDelta())
        TryCand.initResourceDelta(Zone.DAG, SchedModel);
      Cand.setBest(TryCand);
      LLVM_DEBUG(traceCandidate(Cand));
    }
  }
}

// This function is mostly cut and pasted from
// GenericScheduler::pickNodeBidirectional()
SUnit *GCNMaxOccupancySchedStrategy::pickNodeBidirectional(bool &IsTopNode) {
  // Schedule as far as possible in the direction of no choice. This is most
  // efficient, but also provides the best heuristics for CriticalPSets.
  if (SUnit *SU = Bot.pickOnlyChoice()) {
    IsTopNode = false;
    return SU;
  }
  if (SUnit *SU = Top.pickOnlyChoice()) {
    IsTopNode = true;
    return SU;
  }
  // Set the bottom-up policy based on the state of the current bottom zone and
  // the instructions outside the zone, including the top zone.
  CandPolicy BotPolicy;
  setPolicy(BotPolicy, /*IsPostRA=*/false, Bot, &Top);
  // Set the top-down policy based on the state of the current top zone and
  // the instructions outside the zone, including the bottom zone.
  CandPolicy TopPolicy;
  setPolicy(TopPolicy, /*IsPostRA=*/false, Top, &Bot);

  // See if BotCand is still valid (because we previously scheduled from Top).
  LLVM_DEBUG(dbgs() << "Picking from Bot:\n");
  if (!BotCand.isValid() || BotCand.SU->isScheduled ||
      BotCand.Policy != BotPolicy) {
    BotCand.reset(CandPolicy());
    pickNodeFromQueue(Bot, BotPolicy, DAG->getBotRPTracker(), BotCand);
    assert(BotCand.Reason != NoCand && "failed to find the first candidate");
  } else {
    LLVM_DEBUG(traceCandidate(BotCand));
#ifndef NDEBUG
    if (VerifyScheduling) {
      SchedCandidate TCand;
      TCand.reset(CandPolicy());
      pickNodeFromQueue(Bot, BotPolicy, DAG->getBotRPTracker(), TCand);
      assert(TCand.SU == BotCand.SU &&
             "Last pick result should correspond to re-picking right now");
    }
#endif
  }

  // Check if the top Q has a better candidate.
  LLVM_DEBUG(dbgs() << "Picking from Top:\n");
  if (!TopCand.isValid() || TopCand.SU->isScheduled ||
      TopCand.Policy != TopPolicy) {
    TopCand.reset(CandPolicy());
    pickNodeFromQueue(Top, TopPolicy, DAG->getTopRPTracker(), TopCand);
    assert(TopCand.Reason != NoCand && "failed to find the first candidate");
  } else {
    LLVM_DEBUG(traceCandidate(TopCand));
#ifndef NDEBUG
    if (VerifyScheduling) {
      SchedCandidate TCand;
      TCand.reset(CandPolicy());
      pickNodeFromQueue(Top, TopPolicy, DAG->getTopRPTracker(), TCand);
      assert(TCand.SU == TopCand.SU &&
           "Last pick result should correspond to re-picking right now");
    }
#endif
  }

  // Pick best from BotCand and TopCand.
  LLVM_DEBUG(dbgs() << "Top Cand: "; traceCandidate(TopCand);
             dbgs() << "Bot Cand: "; traceCandidate(BotCand););
  SchedCandidate Cand = BotCand;
  TopCand.Reason = NoCand;
  GenericScheduler::tryCandidate(Cand, TopCand, nullptr);
  if (TopCand.Reason != NoCand) {
    Cand.setBest(TopCand);
  }
  LLVM_DEBUG(dbgs() << "Picking: "; traceCandidate(Cand););

  IsTopNode = Cand.AtTop;
  return Cand.SU;
}

// This function is mostly cut and pasted from
// GenericScheduler::pickNode()
SUnit *GCNMaxOccupancySchedStrategy::pickNode(bool &IsTopNode) {
  if (DAG->top() == DAG->bottom()) {
    assert(Top.Available.empty() && Top.Pending.empty() &&
           Bot.Available.empty() && Bot.Pending.empty() && "ReadyQ garbage");
    return nullptr;
  }
  SUnit *SU;
  do {
    if (RegionPolicy.OnlyTopDown) {
      SU = Top.pickOnlyChoice();
      if (!SU) {
        CandPolicy NoPolicy;
        TopCand.reset(NoPolicy);
        pickNodeFromQueue(Top, NoPolicy, DAG->getTopRPTracker(), TopCand);
        assert(TopCand.Reason != NoCand && "failed to find a candidate");
        SU = TopCand.SU;
      }
      IsTopNode = true;
    } else if (RegionPolicy.OnlyBottomUp) {
      SU = Bot.pickOnlyChoice();
      if (!SU) {
        CandPolicy NoPolicy;
        BotCand.reset(NoPolicy);
        pickNodeFromQueue(Bot, NoPolicy, DAG->getBotRPTracker(), BotCand);
        assert(BotCand.Reason != NoCand && "failed to find a candidate");
        SU = BotCand.SU;
      }
      IsTopNode = false;
    } else {
      SU = pickNodeBidirectional(IsTopNode);
    }
  } while (SU->isScheduled);

  if (SU->isTopReady())
    Top.removeReady(SU);
  if (SU->isBottomReady())
    Bot.removeReady(SU);

  LLVM_DEBUG(dbgs() << "Scheduling SU(" << SU->NodeNum << ") "
                    << *SU->getInstr());
  return SU;
}

GCNScheduleDAGMILive::GCNScheduleDAGMILive(
    MachineSchedContext *C, std::unique_ptr<MachineSchedStrategy> S)
    : ScheduleDAGMILive(C, std::move(S)), ST(MF.getSubtarget<GCNSubtarget>()),
      MFI(*MF.getInfo<SIMachineFunctionInfo>()),
      StartingOccupancy(MFI.getOccupancy()), MinOccupancy(StartingOccupancy) {

  LLVM_DEBUG(dbgs() << "Starting occupancy is " << StartingOccupancy << ".\n");
}

void GCNScheduleDAGMILive::schedule() {
  // Collect all scheduling regions. The actual scheduling is performed in
  // GCNScheduleDAGMILive::finalizeSchedule.
  Regions.push_back(std::make_pair(RegionBegin, RegionEnd));
}
GCNRegPressure
GCNScheduleDAGMILive::getRealRegPressure(unsigned RegionIdx) const {
  GCNDownwardRPTracker RPTracker(*LIS);
  RPTracker.advance(begin(), end(), &LiveIns[RegionIdx]);
  return RPTracker.moveMaxPressure();
}

void GCNScheduleDAGMILive::computeBlockPressure(unsigned RegionIdx,
                                                const MachineBasicBlock *MBB) {
  GCNDownwardRPTracker RPTracker(*LIS);

  // If the block has the only successor then live-ins of that successor are
  // live-outs of the current block. We can reuse calculated live set if the
  // successor will be sent to scheduling past current block.
  const MachineBasicBlock *OnlySucc = nullptr;
  if (MBB->succ_size() == 1 && !(*MBB->succ_begin())->empty()) {
    SlotIndexes *Ind = LIS->getSlotIndexes();
    if (Ind->getMBBStartIdx(MBB) < Ind->getMBBStartIdx(*MBB->succ_begin()))
      OnlySucc = *MBB->succ_begin();
  }

  // Scheduler sends regions from the end of the block upwards.
  size_t CurRegion = RegionIdx;
  for (size_t E = Regions.size(); CurRegion != E; ++CurRegion)
    if (Regions[CurRegion].first->getParent() != MBB)
      break;
  --CurRegion;

  auto I = MBB->begin();
  auto LiveInIt = MBBLiveIns.find(MBB);
  auto &Rgn = Regions[CurRegion];
  auto *NonDbgMI = &*skipDebugInstructionsForward(Rgn.first, Rgn.second);
  if (LiveInIt != MBBLiveIns.end()) {
    auto LiveIn = std::move(LiveInIt->second);
    RPTracker.reset(*MBB->begin(), &LiveIn);
    MBBLiveIns.erase(LiveInIt);
  } else {
    I = Rgn.first;
    auto LRS = BBLiveInMap.lookup(NonDbgMI);
#ifdef EXPENSIVE_CHECKS
    assert(isEqual(getLiveRegsBefore(*NonDbgMI, *LIS), LRS));
#endif
    RPTracker.reset(*I, &LRS);
  }

  for (;;) {
    I = RPTracker.getNext();

    if (Regions[CurRegion].first == I || NonDbgMI == I) {
      LiveIns[CurRegion] = RPTracker.getLiveRegs();
      RPTracker.clearMaxPressure();
    }

    if (Regions[CurRegion].second == I) {
      Pressure[CurRegion] = RPTracker.moveMaxPressure();
      if (CurRegion-- == RegionIdx)
        break;
    }
    RPTracker.advanceToNext();
    RPTracker.advanceBeforeNext();
  }

  if (OnlySucc) {
    if (I != MBB->end()) {
      RPTracker.advanceToNext();
      RPTracker.advance(MBB->end());
    }
    RPTracker.reset(*OnlySucc->begin(), &RPTracker.getLiveRegs());
    RPTracker.advanceBeforeNext();
    MBBLiveIns[OnlySucc] = RPTracker.moveLiveRegs();
  }
}

DenseMap<MachineInstr *, GCNRPTracker::LiveRegSet>
GCNScheduleDAGMILive::getBBLiveInMap() const {
  assert(!Regions.empty());
  std::vector<MachineInstr *> BBStarters;
  BBStarters.reserve(Regions.size());
  auto I = Regions.rbegin(), E = Regions.rend();
  auto *BB = I->first->getParent();
  do {
    auto *MI = &*skipDebugInstructionsForward(I->first, I->second);
    BBStarters.push_back(MI);
    do {
      ++I;
    } while (I != E && I->first->getParent() == BB);
  } while (I != E);
  return getLiveRegMap(BBStarters, false /*After*/, *LIS);
}

void GCNScheduleDAGMILive::finalizeSchedule() {
  // Start actual scheduling here. This function is called by the base
  // MachineScheduler after all regions have been recorded by
  // GCNScheduleDAGMILive::schedule().
  LiveIns.resize(Regions.size());
  Pressure.resize(Regions.size());
  RescheduleRegions.resize(Regions.size());
  RegionsWithHighRP.resize(Regions.size());
  RegionsWithExcessRP.resize(Regions.size());
  RegionsWithMinOcc.resize(Regions.size());
  RegionsWithIGLPInstrs.resize(Regions.size());
  RescheduleRegions.set();
  RegionsWithHighRP.reset();
  RegionsWithExcessRP.reset();
  RegionsWithMinOcc.reset();
  RegionsWithIGLPInstrs.reset();

  runSchedStages();
}

void GCNScheduleDAGMILive::runSchedStages() {
  LLVM_DEBUG(dbgs() << "All regions recorded, starting actual scheduling.\n");
  InitialScheduleStage S0(GCNSchedStageID::InitialSchedule, *this);
  UnclusteredHighRPStage S1(GCNSchedStageID::UnclusteredHighRPReschedule,
                            *this);
  ClusteredLowOccStage S2(GCNSchedStageID::ClusteredLowOccupancyReschedule,
                          *this);
  PreRARematStage S3(GCNSchedStageID::PreRARematerialize, *this);
  GCNSchedStage *SchedStages[] = {&S0, &S1, &S2, &S3};

  if (!Regions.empty())
    BBLiveInMap = getBBLiveInMap();

  for (auto *Stage : SchedStages) {
    if (!Stage->initGCNSchedStage())
      continue;

    for (auto Region : Regions) {
      RegionBegin = Region.first;
      RegionEnd = Region.second;
      // Setup for scheduling the region and check whether it should be skipped.
      if (!Stage->initGCNRegion()) {
        Stage->advanceRegion();
        exitRegion();
        continue;
      }

      ScheduleDAGMILive::schedule();
      Stage->finalizeGCNRegion();
    }

    Stage->finalizeGCNSchedStage();
  }
}

#ifndef NDEBUG
raw_ostream &llvm::operator<<(raw_ostream &OS, const GCNSchedStageID &StageID) {
  switch (StageID) {
  case GCNSchedStageID::InitialSchedule:
    OS << "Initial Schedule";
    break;
  case GCNSchedStageID::UnclusteredHighRPReschedule:
    OS << "Unclustered High Register Pressure Reschedule";
    break;
  case GCNSchedStageID::ClusteredLowOccupancyReschedule:
    OS << "Clustered Low Occupancy Reschedule";
    break;
  case GCNSchedStageID::PreRARematerialize:
    OS << "Pre-RA Rematerialize";
    break;
  }
  return OS;
}
#endif

GCNSchedStage::GCNSchedStage(GCNSchedStageID StageID, GCNScheduleDAGMILive &DAG)
    : DAG(DAG), S(static_cast<GCNMaxOccupancySchedStrategy &>(*DAG.SchedImpl)),
      MF(DAG.MF), MFI(DAG.MFI), ST(DAG.ST), StageID(StageID) {}

bool GCNSchedStage::initGCNSchedStage() {
  if (!DAG.LIS)
    return false;

  LLVM_DEBUG(dbgs() << "Starting scheduling stage: " << StageID << "\n");
  return true;
}

bool UnclusteredHighRPStage::initGCNSchedStage() {
  if (DisableUnclusterHighRP)
    return false;

  if (!GCNSchedStage::initGCNSchedStage())
    return false;

  if (DAG.RegionsWithHighRP.none() && DAG.RegionsWithExcessRP.none())
    return false;

  SavedMutations.swap(DAG.Mutations);
  DAG.addMutation(createIGroupLPDAGMutation());
  InitialOccupancy = DAG.MinOccupancy;
  // Aggressivly try to reduce register pressure in the unclustered high RP
  // stage. Temporarily increase occupancy target in the region.
  S.ErrorMargin = S.HighRPErrorMargin;
  if (MFI.getMaxWavesPerEU() > DAG.MinOccupancy)
    MFI.increaseOccupancy(MF, ++DAG.MinOccupancy);

  LLVM_DEBUG(
      dbgs()
      << "Retrying function scheduling without clustering. "
         "Aggressivly try to reduce register pressure to achieve occupancy "
      << DAG.MinOccupancy << ".\n");

  return true;
}

bool ClusteredLowOccStage::initGCNSchedStage() {
  if (!GCNSchedStage::initGCNSchedStage())
    return false;

  // Don't bother trying to improve ILP in lower RP regions if occupancy has not
  // been dropped. All regions will have already been scheduled with the ideal
  // occupancy targets.
  if (DAG.StartingOccupancy <= DAG.MinOccupancy)
    return false;

  LLVM_DEBUG(
      dbgs() << "Retrying function scheduling with lowest recorded occupancy "
             << DAG.MinOccupancy << ".\n");
  return true;
}

bool PreRARematStage::initGCNSchedStage() {
  if (!GCNSchedStage::initGCNSchedStage())
    return false;

  if (DAG.RegionsWithMinOcc.none() || DAG.Regions.size() == 1)
    return false;

  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  // Check maximum occupancy
  if (ST.computeOccupancy(MF.getFunction(), MFI.getLDSSize()) ==
      DAG.MinOccupancy)
    return false;

  // FIXME: This pass will invalidate cached MBBLiveIns for regions
  // inbetween the defs and region we sinked the def to. Cached pressure
  // for regions where a def is sinked from will also be invalidated. Will
  // need to be fixed if there is another pass after this pass.

  collectRematerializableInstructions();
  if (RematerializableInsts.empty() || !sinkTriviallyRematInsts(ST, TII))
    return false;

  LLVM_DEBUG(
      dbgs() << "Retrying function scheduling with improved occupancy of "
             << DAG.MinOccupancy << " from rematerializing\n");
  return true;
}

void GCNSchedStage::finalizeGCNSchedStage() {
  DAG.finishBlock();
  LLVM_DEBUG(dbgs() << "Ending scheduling stage: " << StageID << "\n");
}

void UnclusteredHighRPStage::finalizeGCNSchedStage() {
  SavedMutations.swap(DAG.Mutations);
  S.ErrorMargin = S.DefaultErrorMargin;
  if (DAG.MinOccupancy > InitialOccupancy) {
    for (unsigned IDX = 0; IDX < DAG.Pressure.size(); ++IDX)
      DAG.RegionsWithMinOcc[IDX] =
          DAG.Pressure[IDX].getOccupancy(DAG.ST) == DAG.MinOccupancy;

    LLVM_DEBUG(dbgs() << StageID
                      << " stage successfully increased occupancy to "
                      << DAG.MinOccupancy << '\n');
  }

  GCNSchedStage::finalizeGCNSchedStage();

}

bool GCNSchedStage::initGCNRegion() {
  // Check whether this new region is also a new block.
  if (DAG.RegionBegin->getParent() != CurrentMBB)
    setupNewBlock();

  unsigned NumRegionInstrs = std::distance(DAG.begin(), DAG.end());
  DAG.enterRegion(CurrentMBB, DAG.begin(), DAG.end(), NumRegionInstrs);

  // Skip empty scheduling regions (0 or 1 schedulable instructions).
  if (DAG.begin() == DAG.end() || DAG.begin() == std::prev(DAG.end()))
    return false;
  
  LLVM_DEBUG(dbgs() << "********** MI Scheduling **********\n");
  LLVM_DEBUG(dbgs() << MF.getName() << ":" << printMBBReference(*CurrentMBB)
                    << " " << CurrentMBB->getName()
                    << "\n  From: " << *DAG.begin() << "    To: ";
             if (DAG.RegionEnd != CurrentMBB->end()) dbgs() << *DAG.RegionEnd;
             else dbgs() << "End";
             dbgs() << " RegionInstrs: " << NumRegionInstrs << '\n');
  // Save original instruction order before scheduling for possible revert.
  Unsched.clear();
  Unsched.reserve(DAG.NumRegionInstrs);
  if (StageID == GCNSchedStageID::InitialSchedule) {
    for (auto &I : DAG) {
      Unsched.push_back(&I);
      if (I.getOpcode() == AMDGPU::SCHED_GROUP_BARRIER ||
          I.getOpcode() == AMDGPU::IGLP_OPT)
        DAG.RegionsWithIGLPInstrs[RegionIdx] = true;
    }
  } else {
    for (auto &I : DAG)
      Unsched.push_back(&I);
  }
  PressureBefore = DAG.Pressure[RegionIdx];
  LLVM_DEBUG(
      dbgs() << "Pressure before scheduling:\nRegion live-ins:";
      GCNRPTracker::printLiveRegs(dbgs(), DAG.LiveIns[RegionIdx], DAG.MRI);
      dbgs() << "Region live-in pressure:  ";
      llvm::getRegPressure(DAG.MRI, DAG.LiveIns[RegionIdx]).print(dbgs());
      dbgs() << "Region register pressure: "; PressureBefore.print(dbgs()));
  S.HasHighPressure = false;
    if (DAG.RegionsWithIGLPInstrs[RegionIdx] &&
      StageID != GCNSchedStageID::UnclusteredHighRPReschedule) {
    SavedMutations.clear();
    SavedMutations.swap(DAG.Mutations);
    DAG.addMutation(createIGroupLPDAGMutation());
  }
  return true;
}

bool UnclusteredHighRPStage::initGCNRegion() {
  // Only reschedule regions with the minimum occupancy or regions that may have
  // spilling (excess register pressure).
  if ((!DAG.RegionsWithMinOcc[RegionIdx] ||
       DAG.MinOccupancy <= InitialOccupancy) &&
      !DAG.RegionsWithExcessRP[RegionIdx])
    return false;

  return GCNSchedStage::initGCNRegion();
}

bool ClusteredLowOccStage::initGCNRegion() {
  // We may need to reschedule this region if it wasn't rescheduled in the last
  // stage, or if we found it was testing critical register pressure limits in
  // the unclustered reschedule stage. The later is because we may not have been
  // able to raise the min occupancy in the previous stage so the region may be
  // overly constrained even if it was already rescheduled.
  if (!DAG.RegionsWithHighRP[RegionIdx])
    return false;

  return GCNSchedStage::initGCNRegion();
}

bool PreRARematStage::initGCNRegion() {
  if (!DAG.RescheduleRegions[RegionIdx])
    return false;

  return GCNSchedStage::initGCNRegion();
}

void GCNSchedStage::setupNewBlock() {
  if (CurrentMBB)
    DAG.finishBlock();

  CurrentMBB = DAG.RegionBegin->getParent();
  DAG.startBlock(CurrentMBB);
  // Get real RP for the region if it hasn't be calculated before. After the
  // initial schedule stage real RP will be collected after scheduling.
  if (StageID == GCNSchedStageID::InitialSchedule)
    DAG.computeBlockPressure(RegionIdx, CurrentMBB);
}

void GCNSchedStage::finalizeGCNRegion() {
  DAG.Regions[RegionIdx] = std::make_pair(DAG.RegionBegin, DAG.RegionEnd);
  DAG.RescheduleRegions[RegionIdx] = false;
  if (S.HasHighPressure)
    DAG.RegionsWithHighRP[RegionIdx] = true;

  // Revert scheduling if we have dropped occupancy or there is some other
  // reason that the original schedule is better.
  checkScheduling();

  if (DAG.RegionsWithIGLPInstrs[RegionIdx] &&
      StageID != GCNSchedStageID::UnclusteredHighRPReschedule)
    SavedMutations.swap(DAG.Mutations);

  DAG.exitRegion();
  RegionIdx++;
}

void GCNSchedStage::checkScheduling() {
  // Check the results of scheduling.
  PressureAfter = DAG.getRealRegPressure(RegionIdx);
  LLVM_DEBUG(dbgs() << "Pressure after scheduling: ";
             PressureAfter.print(dbgs()));

  if (PressureAfter.getSGPRNum() <= S.SGPRCriticalLimit &&
      PressureAfter.getVGPRNum(ST.hasGFX90AInsts()) <= S.VGPRCriticalLimit) {
    DAG.Pressure[RegionIdx] = PressureAfter;
    DAG.RegionsWithMinOcc[RegionIdx] =
        PressureAfter.getOccupancy(ST) == DAG.MinOccupancy;

    // Early out if we have achieve the occupancy target.
    LLVM_DEBUG(dbgs() << "Pressure in desired limits, done.\n");
    return;
  }

  unsigned WavesAfter =
      std::min(S.getTargetOccupancy(), PressureAfter.getOccupancy(ST));
  unsigned WavesBefore =
      std::min(S.getTargetOccupancy(), PressureBefore.getOccupancy(ST));
  LLVM_DEBUG(dbgs() << "Occupancy before scheduling: " << WavesBefore
                    << ", after " << WavesAfter << ".\n");

  // We may not be able to keep the current target occupancy because of the just
  // scheduled region. We might still be able to revert scheduling if the
  // occupancy before was higher, or if the current schedule has register
  // pressure higher than the excess limits which could lead to more spilling.
  unsigned NewOccupancy = std::max(WavesAfter, WavesBefore);

  // Allow memory bound functions to drop to 4 waves if not limited by an
  // attribute.
  if (WavesAfter < WavesBefore && WavesAfter < DAG.MinOccupancy &&
      WavesAfter >= MFI.getMinAllowedOccupancy()) {
    LLVM_DEBUG(dbgs() << "Function is memory bound, allow occupancy drop up to "
                      << MFI.getMinAllowedOccupancy() << " waves\n");
    NewOccupancy = WavesAfter;
  }

  if (NewOccupancy < DAG.MinOccupancy) {
    DAG.MinOccupancy = NewOccupancy;
    MFI.limitOccupancy(DAG.MinOccupancy);
    DAG.RegionsWithMinOcc.reset();
    LLVM_DEBUG(dbgs() << "Occupancy lowered for the function to "
                      << DAG.MinOccupancy << ".\n");
  }

  unsigned MaxVGPRs = ST.getMaxNumVGPRs(MF);
  unsigned MaxSGPRs = ST.getMaxNumSGPRs(MF);
  if (PressureAfter.getVGPRNum(false) > MaxVGPRs ||
      PressureAfter.getAGPRNum() > MaxVGPRs ||
      PressureAfter.getSGPRNum() > MaxSGPRs) {
    DAG.RescheduleRegions[RegionIdx] = true;
    DAG.RegionsWithHighRP[RegionIdx] = true;
    DAG.RegionsWithExcessRP[RegionIdx] = true;
  }

  // Revert if this region's schedule would cause a drop in occupancy or
  // spilling.
  if (shouldRevertScheduling(WavesAfter)) {
    revertScheduling();
  } else {
    DAG.Pressure[RegionIdx] = PressureAfter;
    DAG.RegionsWithMinOcc[RegionIdx] =
        PressureAfter.getOccupancy(ST) == DAG.MinOccupancy;
  }
}

bool GCNSchedStage::shouldRevertScheduling(unsigned WavesAfter) {
  if (WavesAfter < DAG.MinOccupancy)
    return true;

  return false;
}

bool InitialScheduleStage::shouldRevertScheduling(unsigned WavesAfter) {
  if (GCNSchedStage::shouldRevertScheduling(WavesAfter))
    return true;

  if (mayCauseSpilling(WavesAfter))
    return true;

  return false;
}

bool UnclusteredHighRPStage::shouldRevertScheduling(unsigned WavesAfter) {
  // If RP is not reduced in the unclustred reschedule stage, revert to the
  // old schedule.
  if ((WavesAfter <= PressureBefore.getOccupancy(ST) &&
       mayCauseSpilling(WavesAfter)) ||
      GCNSchedStage::shouldRevertScheduling(WavesAfter)) {
    LLVM_DEBUG(dbgs() << "Unclustered reschedule did not help.\n");
    return true;
  }

  return false;
}

bool ClusteredLowOccStage::shouldRevertScheduling(unsigned WavesAfter) {
  if (GCNSchedStage::shouldRevertScheduling(WavesAfter))
    return true;

  if (mayCauseSpilling(WavesAfter))
    return true;

  return false;
}

bool PreRARematStage::shouldRevertScheduling(unsigned WavesAfter) {
  if (GCNSchedStage::shouldRevertScheduling(WavesAfter))
    return true;

  if (mayCauseSpilling(WavesAfter))
    return true;

  return false;
}

bool GCNSchedStage::mayCauseSpilling(unsigned WavesAfter) {
  if (WavesAfter <= MFI.getMinWavesPerEU() &&
      !PressureAfter.less(ST, PressureBefore) &&
      DAG.RegionsWithExcessRP[RegionIdx]) {
    LLVM_DEBUG(dbgs() << "New pressure will result in more spilling.\n");
    return true;
  }

  return false;
}

void GCNSchedStage::revertScheduling() {
  DAG.RegionsWithMinOcc[RegionIdx] =
      PressureBefore.getOccupancy(ST) == DAG.MinOccupancy;
  LLVM_DEBUG(dbgs() << "Attempting to revert scheduling.\n");
  DAG.RescheduleRegions[RegionIdx] =
      (nextStage(StageID)) != GCNSchedStageID::UnclusteredHighRPReschedule;
  DAG.RegionEnd = DAG.RegionBegin;
  int SkippedDebugInstr = 0;
  for (MachineInstr *MI : Unsched) {
    if (MI->isDebugInstr()) {
      ++SkippedDebugInstr;
      continue;
    }

    if (MI->getIterator() != DAG.RegionEnd) {
      DAG.BB->remove(MI);
      DAG.BB->insert(DAG.RegionEnd, MI);
      if (!MI->isDebugInstr())
        DAG.LIS->handleMove(*MI, true);
    }

    // Reset read-undef flags and update them later.
    for (auto &Op : MI->operands())
      if (Op.isReg() && Op.isDef())
        Op.setIsUndef(false);
    RegisterOperands RegOpers;
    RegOpers.collect(*MI, *DAG.TRI, DAG.MRI, DAG.ShouldTrackLaneMasks, false);
    if (!MI->isDebugInstr()) {
      if (DAG.ShouldTrackLaneMasks) {
        // Adjust liveness and add missing dead+read-undef flags.
        SlotIndex SlotIdx = DAG.LIS->getInstructionIndex(*MI).getRegSlot();
        RegOpers.adjustLaneLiveness(*DAG.LIS, DAG.MRI, SlotIdx, MI);
      } else {
        // Adjust for missing dead-def flags.
        RegOpers.detectDeadDefs(*MI, *DAG.LIS);
      }
    }
    DAG.RegionEnd = MI->getIterator();
    ++DAG.RegionEnd;
    LLVM_DEBUG(dbgs() << "Scheduling " << *MI);
  }

  // After reverting schedule, debug instrs will now be at the end of the block
  // and RegionEnd will point to the first debug instr. Increment RegionEnd
  // pass debug instrs to the actual end of the scheduling region.
  while (SkippedDebugInstr-- > 0)
    ++DAG.RegionEnd;

  // If Unsched.front() instruction is a debug instruction, this will actually
  // shrink the region since we moved all debug instructions to the end of the
  // block. Find the first instruction that is not a debug instruction.
  DAG.RegionBegin = Unsched.front()->getIterator();
  if (DAG.RegionBegin->isDebugInstr()) {
    for (MachineInstr *MI : Unsched) {
      if (MI->isDebugInstr())
        continue;
      DAG.RegionBegin = MI->getIterator();
      break;
    }
  }

  // Then move the debug instructions back into their correct place and set
  // RegionBegin and RegionEnd if needed.
  DAG.placeDebugValues();

  DAG.Regions[RegionIdx] = std::make_pair(DAG.RegionBegin, DAG.RegionEnd);
}

void PreRARematStage::collectRematerializableInstructions() {
  const SIRegisterInfo *SRI = static_cast<const SIRegisterInfo *>(DAG.TRI);
  for (unsigned I = 0, E = DAG.MRI.getNumVirtRegs(); I != E; ++I) {
    Register Reg = Register::index2VirtReg(I);
    if (!DAG.LIS->hasInterval(Reg))
      continue;

    // TODO: Handle AGPR and SGPR rematerialization
    if (!SRI->isVGPRClass(DAG.MRI.getRegClass(Reg)) ||
        !DAG.MRI.hasOneDef(Reg) || !DAG.MRI.hasOneNonDBGUse(Reg))
      continue;

    MachineOperand *Op = DAG.MRI.getOneDef(Reg);
    MachineInstr *Def = Op->getParent();
    if (Op->getSubReg() != 0 || !isTriviallyReMaterializable(*Def))
      continue;

    MachineInstr *UseI = &*DAG.MRI.use_instr_nodbg_begin(Reg);
    if (Def->getParent() == UseI->getParent())
      continue;

    // We are only collecting defs that are defined in another block and are
    // live-through or used inside regions at MinOccupancy. This means that the
    // register must be in the live-in set for the region.
    bool AddedToRematList = false;
    for (unsigned I = 0, E = DAG.Regions.size(); I != E; ++I) {
      auto It = DAG.LiveIns[I].find(Reg);
      if (It != DAG.LiveIns[I].end() && !It->second.none()) {
        if (DAG.RegionsWithMinOcc[I]) {
          RematerializableInsts[I][Def] = UseI;
          AddedToRematList = true;
        }

        // Collect regions with rematerializable reg as live-in to avoid
        // searching later when updating RP.
        RematDefToLiveInRegions[Def].push_back(I);
      }
    }
    if (!AddedToRematList)
      RematDefToLiveInRegions.erase(Def);
  }
}

bool PreRARematStage::sinkTriviallyRematInsts(const GCNSubtarget &ST,
                                              const TargetInstrInfo *TII) {
  // Temporary copies of cached variables we will be modifying and replacing if
  // sinking succeeds.
  SmallVector<
      std::pair<MachineBasicBlock::iterator, MachineBasicBlock::iterator>, 32>
      NewRegions;
  DenseMap<unsigned, GCNRPTracker::LiveRegSet> NewLiveIns;
  DenseMap<unsigned, GCNRegPressure> NewPressure;
  BitVector NewRescheduleRegions;
  LiveIntervals *LIS = DAG.LIS;

  NewRegions.resize(DAG.Regions.size());
  NewRescheduleRegions.resize(DAG.Regions.size());

  // Collect only regions that has a rematerializable def as a live-in.
  SmallSet<unsigned, 16> ImpactedRegions;
  for (const auto &It : RematDefToLiveInRegions)
    ImpactedRegions.insert(It.second.begin(), It.second.end());

  // Make copies of register pressure and live-ins cache that will be updated
  // as we rematerialize.
  for (auto Idx : ImpactedRegions) {
    NewPressure[Idx] = DAG.Pressure[Idx];
    NewLiveIns[Idx] = DAG.LiveIns[Idx];
  }
  NewRegions = DAG.Regions;
  NewRescheduleRegions.reset();

  DenseMap<MachineInstr *, MachineInstr *> InsertedMIToOldDef;
  bool Improved = false;
  for (auto I : ImpactedRegions) {
    if (!DAG.RegionsWithMinOcc[I])
      continue;

    Improved = false;
    int VGPRUsage = NewPressure[I].getVGPRNum(ST.hasGFX90AInsts());
    int SGPRUsage = NewPressure[I].getSGPRNum();

    // TODO: Handle occupancy drop due to AGPR and SGPR.
    // Check if cause of occupancy drop is due to VGPR usage and not SGPR.
    if (ST.getOccupancyWithNumSGPRs(SGPRUsage) == DAG.MinOccupancy)
      break;

    // The occupancy of this region could have been improved by a previous
    // iteration's sinking of defs.
    if (NewPressure[I].getOccupancy(ST) > DAG.MinOccupancy) {
      NewRescheduleRegions[I] = true;
      Improved = true;
      continue;
    }

    // First check if we have enough trivially rematerializable instructions to
    // improve occupancy. Optimistically assume all instructions we are able to
    // sink decreased RP.
    int TotalSinkableRegs = 0;
    for (const auto &It : RematerializableInsts[I]) {
      MachineInstr *Def = It.first;
      Register DefReg = Def->getOperand(0).getReg();
      TotalSinkableRegs +=
          SIRegisterInfo::getNumCoveredRegs(NewLiveIns[I][DefReg]);
    }
    int VGPRsAfterSink = VGPRUsage - TotalSinkableRegs;
    unsigned OptimisticOccupancy = ST.getOccupancyWithNumVGPRs(VGPRsAfterSink);
    // If in the most optimistic scenario, we cannot improve occupancy, then do
    // not attempt to sink any instructions.
    if (OptimisticOccupancy <= DAG.MinOccupancy)
      break;

    unsigned ImproveOccupancy = 0;
    SmallVector<MachineInstr *, 4> SinkedDefs;
    for (auto &It : RematerializableInsts[I]) {
      MachineInstr *Def = It.first;
      MachineBasicBlock::iterator InsertPos =
          MachineBasicBlock::iterator(It.second);
      Register Reg = Def->getOperand(0).getReg();
      // Rematerialize MI to its use block. Since we are only rematerializing
      // instructions that do not have any virtual reg uses, we do not need to
      // call LiveRangeEdit::allUsesAvailableAt() and
      // LiveRangeEdit::canRematerializeAt().
      TII->reMaterialize(*InsertPos->getParent(), InsertPos, Reg,
                         Def->getOperand(0).getSubReg(), *Def, *DAG.TRI);
      MachineInstr *NewMI = &*(--InsertPos);
      LIS->InsertMachineInstrInMaps(*NewMI);
      LIS->removeInterval(Reg);
      LIS->createAndComputeVirtRegInterval(Reg);
      InsertedMIToOldDef[NewMI] = Def;

      // Update region boundaries in scheduling region we sinked from since we
      // may sink an instruction that was at the beginning or end of its region
      DAG.updateRegionBoundaries(NewRegions, Def, /*NewMI =*/nullptr,
                                 /*Removing =*/true);

      // Update region boundaries in region we sinked to.
      DAG.updateRegionBoundaries(NewRegions, InsertPos, NewMI);

      LaneBitmask PrevMask = NewLiveIns[I][Reg];
      // FIXME: Also update cached pressure for where the def was sinked from.
      // Update RP for all regions that has this reg as a live-in and remove
      // the reg from all regions as a live-in.
      for (auto Idx : RematDefToLiveInRegions[Def]) {
        NewLiveIns[Idx].erase(Reg);
        if (InsertPos->getParent() != DAG.Regions[Idx].first->getParent()) {
          // Def is live-through and not used in this block.
          NewPressure[Idx].inc(Reg, PrevMask, LaneBitmask::getNone(), DAG.MRI);
        } else {
          // Def is used and rematerialized into this block.
          GCNDownwardRPTracker RPT(*LIS);
          auto *NonDbgMI = &*skipDebugInstructionsForward(
              NewRegions[Idx].first, NewRegions[Idx].second);
          RPT.reset(*NonDbgMI, &NewLiveIns[Idx]);
          RPT.advance(NewRegions[Idx].second);
          NewPressure[Idx] = RPT.moveMaxPressure();
        }
      }

      SinkedDefs.push_back(Def);
      ImproveOccupancy = NewPressure[I].getOccupancy(ST);
      if (ImproveOccupancy > DAG.MinOccupancy)
        break;
    }

    // Remove defs we just sinked from all regions' list of sinkable defs
    for (auto &Def : SinkedDefs)
      for (auto TrackedIdx : RematDefToLiveInRegions[Def])
        RematerializableInsts[TrackedIdx].erase(Def);

    if (ImproveOccupancy <= DAG.MinOccupancy)
      break;

    NewRescheduleRegions[I] = true;
    Improved = true;
  }

  if (!Improved) {
    // Occupancy was not improved for all regions that were at MinOccupancy.
    // Undo sinking and remove newly rematerialized instructions.
    for (auto &Entry : InsertedMIToOldDef) {
      MachineInstr *MI = Entry.first;
      MachineInstr *OldMI = Entry.second;
      Register Reg = MI->getOperand(0).getReg();
      LIS->RemoveMachineInstrFromMaps(*MI);
      MI->eraseFromParent();
      OldMI->clearRegisterDeads(Reg);
      LIS->removeInterval(Reg);
      LIS->createAndComputeVirtRegInterval(Reg);
    }
    return false;
  }

  // Occupancy was improved for all regions.
  for (auto &Entry : InsertedMIToOldDef) {
    MachineInstr *MI = Entry.first;
    MachineInstr *OldMI = Entry.second;

    // Remove OldMI from BBLiveInMap since we are sinking it from its MBB.
    DAG.BBLiveInMap.erase(OldMI);

    // Remove OldMI and update LIS
    Register Reg = MI->getOperand(0).getReg();
    LIS->RemoveMachineInstrFromMaps(*OldMI);
    OldMI->eraseFromParent();
    LIS->removeInterval(Reg);
    LIS->createAndComputeVirtRegInterval(Reg);
  }

  // Update live-ins, register pressure, and regions caches.
  for (auto Idx : ImpactedRegions) {
    DAG.LiveIns[Idx] = NewLiveIns[Idx];
    DAG.Pressure[Idx] = NewPressure[Idx];
    DAG.MBBLiveIns.erase(DAG.Regions[Idx].first->getParent());
  }
  DAG.Regions = NewRegions;
  DAG.RescheduleRegions = NewRescheduleRegions;

  SIMachineFunctionInfo &MFI = *MF.getInfo<SIMachineFunctionInfo>();
  MFI.increaseOccupancy(MF, ++DAG.MinOccupancy);

  return true;
}

// Copied from MachineLICM
bool PreRARematStage::isTriviallyReMaterializable(const MachineInstr &MI) {
  if (!DAG.TII->isTriviallyReMaterializable(MI))
    return false;

  for (const MachineOperand &MO : MI.operands())
    if (MO.isReg() && MO.isUse() && MO.getReg().isVirtual())
      return false;

  return true;
}

// When removing, we will have to check both beginning and ending of the region.
// When inserting, we will only have to check if we are inserting NewMI in front
// of a scheduling region and do not need to check the ending since we will only
// ever be inserting before an already existing MI.
void GCNScheduleDAGMILive::updateRegionBoundaries(
    SmallVectorImpl<std::pair<MachineBasicBlock::iterator,
                              MachineBasicBlock::iterator>> &RegionBoundaries,
    MachineBasicBlock::iterator MI, MachineInstr *NewMI, bool Removing) {
  unsigned I = 0, E = RegionBoundaries.size();
  // Search for first region of the block where MI is located
  while (I != E && MI->getParent() != RegionBoundaries[I].first->getParent())
    ++I;

  for (; I != E; ++I) {
    if (MI->getParent() != RegionBoundaries[I].first->getParent())
      return;

    if (Removing && MI == RegionBoundaries[I].first &&
        MI == RegionBoundaries[I].second) {
      // MI is in a region with size 1, after removing, the region will be
      // size 0, set RegionBegin and RegionEnd to pass end of block iterator.
      RegionBoundaries[I] =
          std::make_pair(MI->getParent()->end(), MI->getParent()->end());
      return;
    }
    if (MI == RegionBoundaries[I].first) {
      if (Removing)
        RegionBoundaries[I] =
            std::make_pair(std::next(MI), RegionBoundaries[I].second);
      else
        // Inserted NewMI in front of region, set new RegionBegin to NewMI
        RegionBoundaries[I] = std::make_pair(MachineBasicBlock::iterator(NewMI),
                                             RegionBoundaries[I].second);
      return;
    }
    if (Removing && MI == RegionBoundaries[I].second) {
      RegionBoundaries[I] =
          std::make_pair(RegionBoundaries[I].first, std::prev(MI));
      return;
    }
  }
}

static bool hasIGLPInstrs(ScheduleDAGInstrs *DAG) {
  return std::any_of(
      DAG->begin(), DAG->end(), [](MachineBasicBlock::iterator MI) {
        unsigned Opc = MI->getOpcode();
        return Opc == AMDGPU::SCHED_GROUP_BARRIER || Opc == AMDGPU::IGLP_OPT;
      });
}

GCNPostScheduleDAGMILive::GCNPostScheduleDAGMILive(
    MachineSchedContext *C, std::unique_ptr<MachineSchedStrategy> S,
    bool RemoveKillFlags)
    : ScheduleDAGMI(C, std::move(S), RemoveKillFlags) {}

void GCNPostScheduleDAGMILive::schedule() {
  HasIGLPInstrs = hasIGLPInstrs(this);
  if (HasIGLPInstrs) {
    SavedMutations.clear();
    SavedMutations.swap(Mutations);
    addMutation(createIGroupLPDAGMutation());
  }

  ScheduleDAGMI::schedule();
}

void GCNPostScheduleDAGMILive::finalizeSchedule() {
  if (HasIGLPInstrs)
    SavedMutations.swap(Mutations);

  ScheduleDAGMI::finalizeSchedule();
}
