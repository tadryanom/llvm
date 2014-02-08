//===-- TileISelDAGToDAG.cpp - A Dag to Dag Inst Selector for Tile --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines an instruction selector for the Tile target.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "tile-isel"
#include "Tile.h"
#include "TileMachineFunction.h"
#include "TileRegisterInfo.h"
#include "TileSubtarget.h"
#include "TileTargetMachine.h"
#include "MCTargetDesc/TileBaseInfo.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CFG.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/CodeGen/SelectionDAGNodes.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

namespace {

class TileDAGToDAGISel : public SelectionDAGISel {

  // Keep a reference to TileTargetMachine.
  TileTargetMachine &TM;

  // Keep a pointer to the TileSubtarget around so that we can make
  // the right decision when generating code for different targets.
  const TileSubtarget &Subtarget;

public:
  explicit TileDAGToDAGISel(TileTargetMachine &tm)
      : SelectionDAGISel(tm), TM(tm),
        Subtarget(tm.getSubtarget<TileSubtarget>()) {}

  // Pass Name.
  virtual const char *getPassName() const {
    return "Tile DAG->DAG Pattern Instruction Selection";
  }

  virtual bool runOnMachineFunction(MachineFunction &MF);

private:
// Include the pieces autogenerated from the target description.
#include "TileGenDAGISel.inc"

  // Return a reference to the TargetMachine,
  // casted to the target-specific type.
  const TileTargetMachine &getTargetMachine() {
    return static_cast<const TileTargetMachine &>(TM);
  }

  // Return a reference to the TargetInstrInfo,
  // casted to the target-specific type.
  const TileInstrInfo *getInstrInfo() {
    return getTargetMachine().getInstrInfo();
  }

  // Output the instructions required to put the GOT address into a register.
  SDNode *getGlobalBaseReg();

  // Output the instructions required to put the current pc into a register.
  SDNode *getLinkReg();

  // Select instructions not customized.
  SDNode *Select(SDNode *N);

  // Select Float Operations.
  SDNode *SelectSingleFloatBinOp(SDNode *N);
  SDNode *SelectDoubleFloatBinOp(SDNode *N);
  // INT/UNSIGNED to F32/F64 has hardware support.
  // All other type conversion should use softfloat.
  SDNode *SelectIntToFloatConv(SDNode *N);
  // Select MULHS/MULHU for i32 and i64
  SDNode *SelectMULHIPart32(SDNode *N);
  SDNode *SelectMULHIPart64(SDNode *N);

  // Complex Pattern.
  bool SelectFI(SDValue N, SDValue &R1);

  // Return a target constant with the specified value.
  inline SDValue getImm(const SDNode *Node, unsigned Imm) {
    return CurDAG->getTargetConstant(Imm, Node->getValueType(0));
  }

  void ProcessFunctionAfterISel(MachineFunction &MF);
  bool ReplaceUsesWithZeroReg(MachineRegisterInfo *MRI, const MachineInstr &);
  void GenPICHeader(MachineFunction &MF);

  virtual bool SelectInlineAsmMemoryOperand(
      const SDValue &Op, char ConstraintCode, std::vector<SDValue> &OutOps);
};

}

// Insert PIC header to initialize gp.
void TileDAGToDAGISel::GenPICHeader(MachineFunction &MF) {
  TileFunctionInfo *TileFI = MF.getInfo<TileFunctionInfo>();

  if (!TileFI->globalBaseRegSet() && !TileFI->linkRegSet())
    return;

  MachineBasicBlock &MBB = MF.front();
  MachineBasicBlock::iterator I = MBB.begin();
  const TargetInstrInfo &TII = *MF.getTarget().getInstrInfo();
  DebugLoc DL = I != MBB.end() ? I->getDebugLoc() : DebugLoc();
  unsigned GP = TileFI->getGlobalBaseReg();
  unsigned LP = TileFI->getLinkReg();

  BuildMI(MBB, I, DL, TII.get(Tile::LNK), LP)
      .addExternalSymbol("HOLDER", TileII::MO_NO_FLAG_PIC);

  if (!TileFI->globalBaseRegSet())
    return;

  BuildMI(MBB, I, DL, TII.get(Tile::MOVELI), GP)
      .addExternalSymbol("_GLOBAL_OFFSET_TABLE_", TileII::MO_HW1_LAST_PIC);
  BuildMI(MBB, I, DL, TII.get(Tile::SHL16INSLI), GP).addReg(GP)
      .addExternalSymbol("_GLOBAL_OFFSET_TABLE_", TileII::MO_HW0_PIC);
  BuildMI(MBB, I, DL, TII.get(Tile::ADD), GP).addReg(GP).addReg(LP);
  return;
}

bool TileDAGToDAGISel::ReplaceUsesWithZeroReg(MachineRegisterInfo *MRI,
                                              const MachineInstr &MI) {
  unsigned DstReg = 0, ZeroReg = 0;

  // Check if MI is "addi/addli/addxi/addxli $dst, $zero, 0".
  if ((MI.getOpcode() == Tile::ADDI || MI.getOpcode() == Tile::ADDLI) &&
      (MI.getOperand(1).getReg() == Tile::ZERO) &&
      (MI.getOperand(2).getImm() == 0)) {
    DstReg = MI.getOperand(0).getReg();
    ZeroReg = Tile::ZERO;
  } else if ((MI.getOpcode() == Tile::ADDXI || MI.getOpcode() ==
              Tile::ADDXLI) && (MI.getOperand(1).getReg() == Tile::ZERO_32) &&
             (MI.getOperand(2).getImm() == 0)) {
    DstReg = MI.getOperand(0).getReg();
    ZeroReg = Tile::ZERO_32;
  }

  if (!DstReg)
    return false;

  // Replace uses with ZeroReg.
  for (MachineRegisterInfo::use_iterator U = MRI->use_begin(DstReg),
                                         E = MRI->use_end();
       U != E; ++U) {
    MachineOperand &MO = U.getOperand();
    MachineInstr *MI = MO.getParent();

    // Do not replace if it is a phi's operand or is tied to def operand.
    if (MI->isPHI() || MI->isRegTiedToDefOperand(U.getOperandNo()))
      continue;

    MO.setReg(ZeroReg);
  }

  return true;
}

void TileDAGToDAGISel::ProcessFunctionAfterISel(MachineFunction &MF) {
  GenPICHeader(MF);

  MachineRegisterInfo *MRI = &MF.getRegInfo();

  for (MachineFunction::iterator MFI = MF.begin(), MFE = MF.end(); MFI != MFE;
       ++MFI)
    for (MachineBasicBlock::iterator I = MFI->begin(); I != MFI->end(); ++I)
      ReplaceUsesWithZeroReg(MRI, *I);
}

bool TileDAGToDAGISel::runOnMachineFunction(MachineFunction &MF) {
  bool Ret = SelectionDAGISel::runOnMachineFunction(MF);

  ProcessFunctionAfterISel(MF);

  return Ret;
}

SDNode *TileDAGToDAGISel::getGlobalBaseReg() {
  unsigned GlobalBaseReg = MF->getInfo<TileFunctionInfo>()->getGlobalBaseReg();
  return CurDAG->getRegister(GlobalBaseReg, TLI.getPointerTy()).getNode();
}

SDNode *TileDAGToDAGISel::getLinkReg() {
  unsigned LinkReg = MF->getInfo<TileFunctionInfo>()->getLinkReg();
  return CurDAG->getRegister(LinkReg, TLI.getPointerTy()).getNode();
}

bool TileDAGToDAGISel::SelectFI(SDValue N, SDValue &R1) {
  if (FrameIndexSDNode *FIN = dyn_cast<FrameIndexSDNode>(N)) {
    R1 = CurDAG->getTargetFrameIndex(FIN->getIndex(), MVT::i64);
    return true;
  }
  return false;
}

SDNode *TileDAGToDAGISel::SelectIntToFloatConv(SDNode *Node) {
  unsigned Opcode = Node->getOpcode();
  DebugLoc dl = Node->getDebugLoc();
  EVT NodeTy = Node->getValueType(0);
  SDVTList VTs = CurDAG->getVTList(MVT::i64);
  SDNode *SrcExtend, *Exp, *Neg, *Sign, *Abs, *Tmp1, *Tmp2, *Tmp3;
  SDValue Src32 = Node->getOperand(0);
  SDValue ZeroV = CurDAG->getRegister(Tile::ZERO, MVT::i64);
  SrcExtend = CurDAG->getMachineNode(Tile::ADD_EXTEND, dl, MVT::i64, Src32);
  SDValue Src64 = SDValue(SrcExtend, 0);
  if (NodeTy == MVT::f64) {
    Exp = CurDAG->getMachineNode(Tile::MOVELI, dl, MVT::i64,
                                 CurDAG->getTargetConstant(539, MVT::i64));
    Exp = CurDAG->getMachineNode(Tile::SHLI, dl, MVT::i64, SDValue(Exp, 0),
                                 CurDAG->getTargetConstant(8, MVT::i64));
  } else
    Exp = CurDAG->getMachineNode(Tile::MOVELI, dl, MVT::i64,
                                 CurDAG->getTargetConstant(158, MVT::i64));

  if (Opcode == ISD::UINT_TO_FP) {
    if (NodeTy == MVT::f64) {
      Tmp1 = CurDAG->getMachineNode(Tile::ADD, dl, MVT::i64, ZeroV, ZeroV);
      SDValue Ops[] = { Src64, CurDAG->getTargetConstant(4, MVT::i64),
                        CurDAG->getTargetConstant(35, MVT::i64),
                        SDValue(Tmp1, 0) };
      Tmp2 = CurDAG->getMachineNode(Tile::BFINS, dl, VTs, Ops,
                                    array_lengthof(Ops));

      SDValue Tmp2V = SDValue(Tmp2, 0);

      Tmp3 = CurDAG->getMachineNode(Tile::FDOUBLE_PACK1, dl, MVT::f64, Tmp2V,
                                    SDValue(Exp, 0));
      return CurDAG->getMachineNode(Tile::FDOUBLE_PACK2, dl, MVT::f64, Tmp2V,
                                    ZeroV, SDValue(Tmp3, 0));
    }

    SDValue Ops[] = { Src64, CurDAG->getTargetConstant(32, MVT::i64),
                      CurDAG->getTargetConstant(63, MVT::i64),
                      SDValue(Exp, 0) };
    Tmp1 =
        CurDAG->getMachineNode(Tile::BFINS, dl, VTs, Ops, array_lengthof(Ops));
    Tmp2 = CurDAG->getMachineNode(Tile::FSINGLE_PACK164, dl, MVT::f64,
                                  SDValue(Tmp1, 0));
    return CurDAG->getMachineNode(Tile::FSINGLE_PACK232_64, dl, MVT::f32,
                                  SDValue(Tmp1, 0), SDValue(Tmp2, 0));
  }

  assert(Opcode == ISD::SINT_TO_FP && "ISD::SINT_TO_FP expected here!\n");

  Neg = CurDAG->getMachineNode(Tile::SUB, dl, MVT::i64, ZeroV, Src64);

  Sign = CurDAG->getMachineNode(Tile::CMPLTSI, dl, MVT::i64, Src64,
                                CurDAG->getTargetConstant(0, MVT::i64));
  SDValue SignV = SDValue(Sign, 0);
  Abs = CurDAG->getMachineNode(Tile::CMOVNEZ, dl, MVT::i64, SignV,
                               SDValue(Neg, 0), Src64);
  if (NodeTy == MVT::f64) {
    Tmp1 = CurDAG->getMachineNode(Tile::SHLI, dl, MVT::i64, SDValue(Abs, 0),
                                  CurDAG->getTargetConstant(4, MVT::i64));
    SDValue Tmp1V = SDValue(Tmp1, 0);
    SDValue Ops[] = { SignV, CurDAG->getTargetConstant(20, MVT::i64),
                      CurDAG->getTargetConstant(20, MVT::i64),
                      SDValue(Exp, 0) };
    Tmp2 =
        CurDAG->getMachineNode(Tile::BFINS, dl, VTs, Ops, array_lengthof(Ops));
    Tmp3 = CurDAG->getMachineNode(Tile::FDOUBLE_PACK1, dl, MVT::f64, Tmp1V,
                                  SDValue(Tmp2, 0));
    return CurDAG->getMachineNode(Tile::FDOUBLE_PACK2, dl, MVT::f64, Tmp1V,
                                  ZeroV, SDValue(Tmp3, 0));
  }

  SDValue Ops[] = { SignV, CurDAG->getTargetConstant(10, MVT::i64),
                    CurDAG->getTargetConstant(10, MVT::i64), SDValue(Exp, 0) };
  Tmp1 = CurDAG->getMachineNode(Tile::BFINS, dl, VTs, Ops, array_lengthof(Ops));
  Ops[0] = SDValue(Abs, 0);
  Ops[1] = CurDAG->getTargetConstant(32, MVT::i64);
  Ops[2] = CurDAG->getTargetConstant(63, MVT::i64);
  Ops[3] = SDValue(Tmp1, 0);
  Tmp2 = CurDAG->getMachineNode(Tile::BFINS, dl, VTs, Ops, array_lengthof(Ops));
  Tmp3 = CurDAG->getMachineNode(Tile::FSINGLE_PACK164, dl, MVT::f64,
                                SDValue(Tmp2, 0));
  return CurDAG->getMachineNode(Tile::FSINGLE_PACK232_64, dl, MVT::f32,
                                SDValue(Tmp2, 0), SDValue(Tmp3, 0));
}

SDNode *TileDAGToDAGISel::SelectMULHIPart32(SDNode *N) {
  unsigned Opcode = N->getOpcode();
  DebugLoc dl = N->getDebugLoc();
  SDValue SrcA = N->getOperand(0);
  SDValue SrcB = N->getOperand(1);

  SDNode *Tmp = CurDAG->getMachineNode(
      Opcode == ISD::MULHU ? Tile::MUL_LU_LU32 : Tile::MUL_LS_LS32, dl,
      MVT::i64, SrcA, SrcB);

  return CurDAG->getMachineNode(
      Opcode == ISD::MULHU ? Tile::SHRUI32_64 : Tile::SHRSI32_64, dl, MVT::i32,
      SDValue(Tmp, 0), CurDAG->getTargetConstant(32, MVT::i64));
}

SDNode *TileDAGToDAGISel::SelectMULHIPart64(SDNode *N) {
  unsigned Opcode = N->getOpcode();
  DebugLoc dl = N->getDebugLoc();
  SDValue SrcA = N->getOperand(0);
  SDValue SrcB = N->getOperand(1);
  bool Signed = Opcode == ISD::MULHS;
  SDNode *Tmp0, *Tmp1, *Tmp2, *Tmp3, *Tmp4, *Tmp5, *Tmp6, *Tmp7, *Tmp8, *Tmp9,
      *Tmp10, *Tmp11, *Tmp12, *Tmp13, *ResultLo;
  SDValue Const32 = CurDAG->getTargetConstant(32, MVT::i64);

  Tmp2 = CurDAG->getMachineNode(Tile::MUL_LU_LU, dl, MVT::i64, SrcA, SrcB);
  if (Signed) {
    Tmp0 = CurDAG->getMachineNode(Tile::MUL_HS_LU, dl, MVT::i64, SrcA, SrcB);
    Tmp1 = CurDAG->getMachineNode(Tile::MUL_HS_LU, dl, MVT::i64, SrcB, SrcA);
    Tmp3 = CurDAG->getMachineNode(Tile::MUL_HS_HS, dl, MVT::i64, SrcA, SrcB);
  } else {
    Tmp0 = CurDAG->getMachineNode(Tile::MUL_HU_LU, dl, MVT::i64, SrcA, SrcB);
    Tmp1 = CurDAG->getMachineNode(Tile::MUL_HU_LU, dl, MVT::i64, SrcB, SrcA);
    Tmp3 = CurDAG->getMachineNode(Tile::MUL_HU_HU, dl, MVT::i64, SrcA, SrcB);
  }

  Tmp4 = CurDAG->getMachineNode(Tile::SHLI, dl, MVT::i64, SDValue(Tmp0, 0),
                                Const32);
  Tmp5 = CurDAG->getMachineNode(Tile::SHLI, dl, MVT::i64, SDValue(Tmp1, 0),
                                Const32);
  Tmp6 = CurDAG->getMachineNode(Tile::ADD, dl, MVT::i64, SDValue(Tmp4, 0),
                                SDValue(Tmp5, 0));
  ResultLo = CurDAG->getMachineNode(Tile::ADD, dl, MVT::i64, SDValue(Tmp2, 0),
                                    SDValue(Tmp6, 0));
  Tmp7 = CurDAG->getMachineNode(Tile::CMPLTU, dl, MVT::i64, SDValue(Tmp6, 0),
                                SDValue(Tmp4, 0));
  Tmp8 = CurDAG->getMachineNode(Tile::CMPLTU, dl, MVT::i64,
                                SDValue(ResultLo, 0), SDValue(Tmp2, 0));

  if (Signed) {
    Tmp9 = CurDAG->getMachineNode(Tile::SHRSI, dl, MVT::i64, SDValue(Tmp0, 0),
                                  Const32);
    Tmp10 = CurDAG->getMachineNode(Tile::SHRSI, dl, MVT::i64, SDValue(Tmp1, 0),
                                   Const32);
  } else {
    Tmp9 = CurDAG->getMachineNode(Tile::SHRUI, dl, MVT::i64, SDValue(Tmp0, 0),
                                  Const32);
    Tmp10 = CurDAG->getMachineNode(Tile::SHRUI, dl, MVT::i64, SDValue(Tmp1, 0),
                                   Const32);
  }

  Tmp11 = CurDAG->getMachineNode(Tile::ADD, dl, MVT::i64, SDValue(Tmp3, 0),
                                 SDValue(Tmp7, 0));
  Tmp12 = CurDAG->getMachineNode(Tile::ADD, dl, MVT::i64, SDValue(Tmp8, 0),
                                 SDValue(Tmp9, 0));
  Tmp13 = CurDAG->getMachineNode(Tile::ADD, dl, MVT::i64, SDValue(Tmp11, 0),
                                 SDValue(Tmp12, 0));
  return CurDAG->getMachineNode(Tile::ADD, dl, MVT::i64, SDValue(Tmp13, 0),
                                SDValue(Tmp10, 0));
}

SDNode *TileDAGToDAGISel::SelectDoubleFloatBinOp(SDNode *Node) {

  unsigned Opcode = Node->getOpcode();
  DebugLoc dl = Node->getDebugLoc();
  unsigned FlagOpcode = Tile::FDOUBLE_ADD_FLAGS;
  SDNode *Min, *Max, *Mid, *Flag, *Result;
  SDValue SrcA = Node->getOperand(0);
  SDValue SrcB = Node->getOperand(1);

  if (Opcode == ISD::FSUB)
    FlagOpcode = Tile::FDOUBLE_SUB_FLAGS;

  Min = CurDAG->getMachineNode(Tile::FDOUBLE_UNPACK_MIN, dl, MVT::f64, SrcA,
                               SrcB);
  Max = CurDAG->getMachineNode(Tile::FDOUBLE_UNPACK_MAX, dl, MVT::f64, SrcA,
                               SrcB);
  Flag = CurDAG->getMachineNode(FlagOpcode, dl, MVT::f64, SrcA, SrcB);
  SDValue FlagV = SDValue(Flag, 0);

  Mid = CurDAG->getMachineNode(Tile::FDOUBLE_ADDSUB, dl, MVT::f64,
                               SDValue(Min, 0), FlagV, SDValue(Max, 0));
  SDValue MidV = SDValue(Mid, 0);

  Result =
      CurDAG->getMachineNode(Tile::FDOUBLE_PACK1, dl, MVT::f64, MidV, FlagV);
  return CurDAG->getMachineNode(Tile::FDOUBLE_PACK2, dl, MVT::f64, MidV,
                                CurDAG->getRegister(Tile::ZERO, MVT::f64),
                                SDValue(Result, 0));
}

SDNode *TileDAGToDAGISel::SelectSingleFloatBinOp(SDNode *Node) {

  unsigned Opcode = Node->getOpcode();
  DebugLoc dl = Node->getDebugLoc();
  unsigned Part1Opcode, Part2Opcode = Tile::FSINGLE_ADDSUB2;
  SDNode *Tmp0, *Tmp1, *Flag;
  SDValue SrcA = Node->getOperand(0);
  SDValue SrcB = Node->getOperand(1);

  switch (Opcode) {
  case ISD::FADD:
    Part1Opcode = Tile::FSINGLE_ADD1;
    break;
  case ISD::FSUB:
    Part1Opcode = Tile::FSINGLE_SUB1;
    break;
  case ISD::FMUL:
    Part1Opcode = Tile::FSINGLE_MUL1;
    Part2Opcode = Tile::FSINGLE_MUL2;
    break;
  default:
    llvm_unreachable("SelectSingleFloatBinOp: unexpected Opcode.\n");
  }
  Tmp0 = CurDAG->getMachineNode(Part1Opcode, dl, MVT::f32, SrcA, SrcB);
  if (Opcode == ISD::FMUL)
    Tmp1 = CurDAG->getMachineNode(Part2Opcode, dl, MVT::f32, SDValue(Tmp0, 0),
                                  SrcB);
  else

    Tmp1 = CurDAG->getMachineNode(Part2Opcode, dl, MVT::f32, SrcA, SrcB,
                                  SDValue(Tmp0, 0));

  SDValue Tmp1V = SDValue(Tmp1, 0);
  Flag = CurDAG->getMachineNode(Tile::FSINGLE_PACK1, dl, MVT::f32, Tmp1V);
  return CurDAG->getMachineNode(Tile::FSINGLE_PACK2, dl, MVT::f32, Tmp1V,
                                SDValue(Flag, 0));
}

SDNode *TileDAGToDAGISel::Select(SDNode *Node) {
  unsigned Opcode = Node->getOpcode();
  DebugLoc dl = Node->getDebugLoc();

  // Dump information about the Node being selected.
  DEBUG(errs() << "Selecting: "; Node->dump(CurDAG); errs() << "\n");

  // If we have a custom node, we already have selected.
  if (Node->isMachineOpcode()) {
    DEBUG(errs() << "== "; Node->dump(CurDAG); errs() << "\n");
    return NULL;
  }

  // Instruction Selection not handled by the auto-generated
  // tablegen selection should be handled here.
  EVT NodeTy = Node->getValueType(0);

  switch (Opcode) {
  default:
    break;

  case ISD::INTRINSIC_VOID: {
    unsigned NetReg = 0xFFFFFFFF;
    switch (cast<ConstantSDNode>(Node->getOperand(1))->getZExtValue()) {
    default:
      llvm_unreachable("INTRINSIC_VOID should not reach here.\n");
    case Intrinsic::tilegx_netbarrier:
      ReplaceUses(SDValue(Node, 0), Node->getOperand(0));
      return NULL;
    case Intrinsic::tilegx_usend:
      NetReg = Tile::UDN0;
      break;
    case Intrinsic::tilegx_isend:
      NetReg = Tile::IDN0;
      break;
    }

    SDNode *ResNode = CurDAG->getMachineNode(
        Tile::NET, dl, MVT::i64, MVT::Other, CurDAG->getRegister(NetReg, MVT::i64),
        Node->getOperand(2), Node->getOperand(0));
    ReplaceUses(SDValue(Node, 0), SDValue(ResNode, 1));
    return NULL;
  }

  case ISD::INTRINSIC_W_CHAIN: {
    unsigned NetReg = 0xFFFFFFFF;
    switch (cast<ConstantSDNode>(Node->getOperand(1))->getZExtValue()) {
    default:
      break;
   case Intrinsic::tilegx_udn0r:
      NetReg = Tile::UDN0;
      break;
    case Intrinsic::tilegx_udn1r:
      NetReg = Tile::UDN1;
      break;
    case Intrinsic::tilegx_udn2r:
      NetReg = Tile::UDN2;
      break;
    case Intrinsic::tilegx_udn3r:
      NetReg = Tile::UDN3;
      break;
    case Intrinsic::tilegx_idn0r:
      NetReg = Tile::IDN0;
      break;
    case Intrinsic::tilegx_idn1r:
      NetReg = Tile::IDN1;
      break;
    }

    if (NetReg == 0xFFFFFFFF)
      break;

    SDNode *ResNode = CurDAG->getMachineNode(
        Tile::NET, dl, MVT::i64, MVT::Other, CurDAG->getRegister(Tile::ZERO, MVT::i64),
        CurDAG->getRegister(NetReg, MVT::i64), Node->getOperand(0));
    ReplaceUses(SDValue(Node, 0), SDValue(ResNode, 0));
    ReplaceUses(SDValue(Node, 1), SDValue(ResNode, 1));
    return NULL;
  }

  // Carry-bit add/sub.
  case ISD::SUBE:
  case ISD::ADDE: {
    SDValue InFlag = Node->getOperand(2), CmpLHS;
    unsigned Opc = InFlag.getOpcode();
    (void) Opc;
    assert(((Opc == ISD::ADDC || Opc == ISD::ADDE) ||
            (Opc == ISD::SUBC || Opc == ISD::SUBE)) &&
           "(ADD|SUB)E flag operand must come from (ADD|SUB)C/E insn");

    unsigned MOp;
    if (Opcode == ISD::ADDE) {
      CmpLHS = InFlag.getValue(0);
      MOp = NodeTy == MVT::i64 ? Tile::ADD : Tile::ADDX;
    } else {
      CmpLHS = InFlag.getOperand(0);
      MOp = NodeTy == MVT::i64 ? Tile::SUB : Tile::SUBX;
    }

    SDValue Ops[] = { CmpLHS, InFlag.getOperand(1) };

    SDValue LHS = Node->getOperand(0);
    SDValue RHS = Node->getOperand(1);

    EVT VT = LHS.getValueType();
    SDNode *Carry = CurDAG->getMachineNode(
        NodeTy == MVT::i64 ? Tile::CMPLTU : Tile::CMPLTU32, dl, VT, Ops, 2);
    SDNode *AddCarry =
        CurDAG->getMachineNode(NodeTy == MVT::i64 ? Tile::ADD : Tile::ADDX, dl,
                               VT, SDValue(Carry, 0), RHS);

    return CurDAG->SelectNodeTo(Node, MOp, VT, MVT::Glue, LHS,
                                SDValue(AddCarry, 0));
  }

  // 64bit mul.
  case ISD::MUL: {

    if (NodeTy == MVT::i32)
      break;

    SDValue SrcA = Node->getOperand(0);
    SDValue SrcB = Node->getOperand(1);
    SDNode *Tmp =
        CurDAG->getMachineNode(Tile::MUL_HU_LU, dl, MVT::i64, SrcA, SrcB);

    Tmp = CurDAG->getMachineNode(Tile::MULA_HU_LU, dl, MVT::i64,
                                 SDValue(Tmp, 0), SrcB, SrcA);

    Tmp = CurDAG->getMachineNode(Tile::SHLI, dl, MVT::i64, SDValue(Tmp, 0),
                                 CurDAG->getTargetConstant(32, MVT::i64));

    return CurDAG->getMachineNode(Tile::MULA_LU_LU, dl, MVT::i64,
                                  SDValue(Tmp, 0), SrcB, SrcA);
  }

  case ISD::MULHU:
  case ISD::MULHS:

    if (NodeTy == MVT::i32)
      return SelectMULHIPart32(Node);

    return SelectMULHIPart64(Node);

  case ISD::SINT_TO_FP:
  case ISD::UINT_TO_FP:
    assert(Node->getOperand(0).getValueType() == MVT::i32 &&
           "SINT_TO_FP: i64 to float should be expanded into libcall.");
    return SelectIntToFloatConv(Node);
  case ISD::FADD:
  case ISD::FSUB:

    if (NodeTy == MVT::f32)
      return SelectSingleFloatBinOp(Node);

    assert(NodeTy == MVT::f64 &&
           "FADD: no support for other types except f32/f64");
    return SelectDoubleFloatBinOp(Node);

  case ISD::FMUL: {

    // for float multiply, TILE-Gx need to use
    // instruction sequences to finish the work.
    //
    // these sequences are copied from TILE-Gx gcc md files.

    if (NodeTy == MVT::f32)
      return SelectSingleFloatBinOp(Node);

    assert(NodeTy == MVT::f64 &&
           "FMUL: no support for other types except f32/f64");

    SDValue SrcA = Node->getOperand(0);
    SDValue SrcB = Node->getOperand(1);
    SDValue Const32 = CurDAG->getTargetConstant(32, MVT::i64);
    SDNode *UnpackedA =
        CurDAG->getMachineNode(Tile::FDOUBLE_UNPACK_MAX, dl, MVT::f64, SrcA,
                               CurDAG->getRegister(Tile::ZERO, MVT::f64));
    SDNode *UnpackedB =
        CurDAG->getMachineNode(Tile::FDOUBLE_UNPACK_MAX, dl, MVT::f64, SrcB,
                               CurDAG->getRegister(Tile::ZERO, MVT::f64));
    SDValue UnpackedAV = SDValue(UnpackedA, 0);
    SDValue UnpackedBV = SDValue(UnpackedB, 0);
    SDNode *Flag = CurDAG->getMachineNode(Tile::FDOUBLE_MUL_FLAGS, dl, MVT::f64,
                                          SrcA, SrcB);
    SDNode *LowA = CurDAG->getMachineNode(Tile::MUL_LU_LU, dl, MVT::f64,
                                          UnpackedAV, UnpackedBV);
    SDNode *MidA = CurDAG->getMachineNode(Tile::MUL_HU_LU, dl, MVT::f64,
                                          UnpackedAV, UnpackedBV);
    SDNode *MidB =
        CurDAG->getMachineNode(Tile::MULA_HU_LU, dl, MVT::f64, SDValue(MidA, 0),
                               UnpackedBV, UnpackedAV);
    SDValue MidBV = SDValue(MidB, 0);
    SDNode *HighA = CurDAG->getMachineNode(Tile::MUL_HU_HU, dl, MVT::f64,
                                           UnpackedAV, UnpackedBV);
    SDNode *MidC =
        CurDAG->getMachineNode(Tile::SHLI, dl, MVT::i64, MidBV, Const32);
    SDValue MidCV = SDValue(MidC, 0);
    SDNode *MidD =
        CurDAG->getMachineNode(Tile::SHRUI, dl, MVT::i64, MidBV, Const32);
    SDNode *HighB = CurDAG->getMachineNode(Tile::ADD, dl, MVT::i64,
                                           SDValue(HighA, 0), SDValue(MidD, 0));
    SDNode *LowB = CurDAG->getMachineNode(Tile::ADD, dl, MVT::i64,
                                          SDValue(LowA, 0), MidCV);
    SDValue LowBV = SDValue(LowB, 0);

    SDNode *LowCarry =
        CurDAG->getMachineNode(Tile::CMPLTU, dl, MVT::i64, LowBV, MidCV);

    SDNode *HighC = CurDAG->getMachineNode(
        Tile::ADD, dl, MVT::i64, SDValue(HighB, 0), SDValue(LowCarry, 0));
    SDValue HighCV = SDValue(HighC, 0);

    SDNode *Result = CurDAG->getMachineNode(Tile::FDOUBLE_PACK1, dl, MVT::f64,
                                            HighCV, SDValue(Flag, 0));

    return CurDAG->getMachineNode(Tile::FDOUBLE_PACK2, dl, MVT::f64, HighCV,
                                  LowBV, SDValue(Result, 0));
  }

  // Get target GOT address.
  case ISD::GLOBAL_OFFSET_TABLE:
    return getGlobalBaseReg();
  }

  // Select the default instruction.
  SDNode *ResNode = SelectCode(Node);

  DEBUG(errs() << "=> ");
  if (ResNode == NULL || ResNode == Node)
    DEBUG(Node->dump(CurDAG));
  else
    DEBUG(ResNode->dump(CurDAG));
  DEBUG(errs() << "\n");
  return ResNode;
}

bool TileDAGToDAGISel::SelectInlineAsmMemoryOperand(
    const SDValue &Op, char ConstraintCode, std::vector<SDValue> &OutOps) {
  assert(ConstraintCode == 'm' && "unexpected asm memory constraint");
  OutOps.push_back(Op);
  return false;
}

// This pass converts a legalized DAG into a Tile-specific DAG,
// ready for instruction scheduling.
FunctionPass *llvm::createTileISelDag(TileTargetMachine &TM) {
  return new TileDAGToDAGISel(TM);
}