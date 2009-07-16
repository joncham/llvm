//==-- SystemZISelDAGToDAG.cpp - A dag to dag inst selector for SystemZ ---===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines an instruction selector for the SystemZ target.
//
//===----------------------------------------------------------------------===//

#include "SystemZ.h"
#include "SystemZISelLowering.h"
#include "SystemZTargetMachine.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/Intrinsics.h"
#include "llvm/CallingConv.h"
#include "llvm/Constants.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
using namespace llvm;

namespace {
  /// SystemZRRIAddressMode - This corresponds to rriaddr, but uses SDValue's
  /// instead of register numbers for the leaves of the matched tree.
  struct SystemZRRIAddressMode {
    enum {
      RegBase,
      FrameIndexBase
    } BaseType;

    struct {            // This is really a union, discriminated by BaseType!
      SDValue Reg;
      int FrameIndex;
    } Base;

    SDValue IndexReg;
    int64_t Disp;

    SystemZRRIAddressMode()
      : BaseType(RegBase), IndexReg(), Disp(0) {
    }

    void dump() {
      cerr << "SystemZRRIAddressMode " << this << '\n';
      if (BaseType == RegBase) {
        cerr << "Base.Reg ";
        if (Base.Reg.getNode() != 0) Base.Reg.getNode()->dump();
        else cerr << "nul";
        cerr << '\n';
      } else {
        cerr << " Base.FrameIndex " << Base.FrameIndex << '\n';
      }
      cerr << "IndexReg ";
      if (IndexReg.getNode() != 0) IndexReg.getNode()->dump();
      else cerr << "nul";
      cerr << " Disp " << Disp << '\n';
    }
  };
}

/// SystemZDAGToDAGISel - SystemZ specific code to select SystemZ machine
/// instructions for SelectionDAG operations.
///
namespace {
  class SystemZDAGToDAGISel : public SelectionDAGISel {
    SystemZTargetLowering &Lowering;
    const SystemZSubtarget &Subtarget;

    void getAddressOperands(const SystemZRRIAddressMode &AM,
                            SDValue &Base, SDValue &Disp,
                            SDValue &Index);

  public:
    SystemZDAGToDAGISel(SystemZTargetMachine &TM, CodeGenOpt::Level OptLevel)
      : SelectionDAGISel(TM, OptLevel),
        Lowering(*TM.getTargetLowering()),
        Subtarget(*TM.getSubtargetImpl()) { }

    virtual void InstructionSelect();

    virtual const char *getPassName() const {
      return "SystemZ DAG->DAG Pattern Instruction Selection";
    }

    /// getI16Imm - Return a target constant with the specified value, of type
    /// i16.
    inline SDValue getI16Imm(uint64_t Imm) {
      return CurDAG->getTargetConstant(Imm, MVT::i16);
    }

    /// getI32Imm - Return a target constant with the specified value, of type
    /// i32.
    inline SDValue getI32Imm(uint64_t Imm) {
      return CurDAG->getTargetConstant(Imm, MVT::i32);
    }

    // Include the pieces autogenerated from the target description.
    #include "SystemZGenDAGISel.inc"

  private:
    bool SelectAddrRI32(const SDValue& Op, SDValue& Addr,
                        SDValue &Base, SDValue &Disp);
    bool SelectAddrRI(const SDValue& Op, SDValue& Addr,
                      SDValue &Base, SDValue &Disp);
    bool SelectAddrRRI12(SDValue Op, SDValue Addr,
                         SDValue &Base, SDValue &Disp, SDValue &Index);
    bool SelectAddrRRI20(SDValue Op, SDValue Addr,
                         SDValue &Base, SDValue &Disp, SDValue &Index);
    bool SelectLAAddr(SDValue Op, SDValue Addr,
                      SDValue &Base, SDValue &Disp, SDValue &Index);

    SDNode *Select(SDValue Op);
    bool MatchAddress(SDValue N, SystemZRRIAddressMode &AM,
                      bool is12Bit, unsigned Depth = 0);
    bool MatchAddressBase(SDValue N, SystemZRRIAddressMode &AM);

  #ifndef NDEBUG
    unsigned Indent;
  #endif
  };
}  // end anonymous namespace

/// createSystemZISelDag - This pass converts a legalized DAG into a
/// SystemZ-specific DAG, ready for instruction scheduling.
///
FunctionPass *llvm::createSystemZISelDag(SystemZTargetMachine &TM,
                                        CodeGenOpt::Level OptLevel) {
  return new SystemZDAGToDAGISel(TM, OptLevel);
}

/// isImmSExt20 - This method tests to see if the node is either a 32-bit
/// or 64-bit immediate, and if the value can be accurately represented as a
/// sign extension from a 20-bit value. If so, this returns true and the
/// immediate.
static bool isImmSExt20(int64_t Val, int64_t &Imm) {
  if (Val >= -524288 && Val <= 524287) {
    Imm = Val;
    return true;
  }
  return false;
}

static bool isImmSExt20(SDNode *N, int64_t &Imm) {
  if (N->getOpcode() != ISD::Constant)
    return false;

  return isImmSExt20(cast<ConstantSDNode>(N)->getSExtValue(), Imm);
}

static bool isImmSExt20(SDValue Op, int64_t &Imm) {
  return isImmSExt20(Op.getNode(), Imm);
}

/// isImmZExt12 - This method tests to see if the node is either a 32-bit
/// or 64-bit immediate, and if the value can be accurately represented as a
/// zero extension from a 12-bit value. If so, this returns true and the
/// immediate.
static bool isImmZExt12(int64_t Val, int64_t &Imm) {
  if (Val >= 0 && Val <= 0xFFF) {
    Imm = Val;
    return true;
  }
  return false;
}

static bool isImmZExt12(SDNode *N, int64_t &Imm) {
  if (N->getOpcode() != ISD::Constant)
    return false;

  return isImmZExt12(cast<ConstantSDNode>(N)->getSExtValue(), Imm);
}

static bool isImmZExt12(SDValue Op, int64_t &Imm) {
  return isImmZExt12(Op.getNode(), Imm);
}

/// Returns true if the address can be represented by a base register plus
/// an unsigned 12-bit displacement [r+imm].
bool SystemZDAGToDAGISel::SelectAddrRI32(const SDValue& Op, SDValue& Addr,
                                         SDValue &Base, SDValue &Disp) {
  // FIXME dl should come from parent load or store, not from address
  DebugLoc dl = Addr.getDebugLoc();
  MVT VT = Addr.getValueType();

  if (Addr.getOpcode() == ISD::ADD) {
    int64_t Imm = 0;
    if (isImmZExt12(Addr.getOperand(1), Imm)) {
      Disp = CurDAG->getTargetConstant(Imm, MVT::i64);
      if (FrameIndexSDNode *FI =
          dyn_cast<FrameIndexSDNode>(Addr.getOperand(0))) {
        Base = CurDAG->getTargetFrameIndex(FI->getIndex(), VT);
      } else {
        Base = Addr.getOperand(0);
      }
      return true; // [r+i]
    }
  } else if (Addr.getOpcode() == ISD::OR) {
    int64_t Imm = 0;
    if (isImmZExt12(Addr.getOperand(1), Imm)) {
      // If this is an or of disjoint bitfields, we can codegen this as an add
      // (for better address arithmetic) if the LHS and RHS of the OR are
      // provably disjoint.
      APInt LHSKnownZero, LHSKnownOne;
      CurDAG->ComputeMaskedBits(Addr.getOperand(0),
                                APInt::getAllOnesValue(Addr.getOperand(0)
                                                       .getValueSizeInBits()),
                                LHSKnownZero, LHSKnownOne);

      if ((LHSKnownZero.getZExtValue()|~(uint64_t)Imm) == ~0ULL) {
        // If all of the bits are known zero on the LHS or RHS, the add won't
        // carry.
        Base = Addr.getOperand(0);
        Disp = CurDAG->getTargetConstant(Imm, MVT::i64);
        return true;
      }
    }
  } else if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(Addr)) {
    // Loading from a constant address.

    // If this address fits entirely in a 12-bit zext immediate field, codegen
    // this as "d(r0)"
    int64_t Imm;
    if (isImmZExt12(CN, Imm)) {
      Disp = CurDAG->getTargetConstant(Imm, MVT::i64);
      Base = CurDAG->getRegister(0, VT);
      return true;
    }
  }

  Disp = CurDAG->getTargetConstant(0, MVT::i64);
  if (FrameIndexSDNode *FI = dyn_cast<FrameIndexSDNode>(Addr))
    Base = CurDAG->getTargetFrameIndex(FI->getIndex(), VT);
  else
    Base = Addr;
  return true;      // [r+0]
}

/// Returns true if the address can be represented by a base register plus
/// a signed 20-bit displacement [r+imm].
bool SystemZDAGToDAGISel::SelectAddrRI(const SDValue& Op, SDValue& Addr,
                                       SDValue &Base, SDValue &Disp) {
  // FIXME dl should come from parent load or store, not from address
  DebugLoc dl = Addr.getDebugLoc();
  MVT VT = Addr.getValueType();

  if (Addr.getOpcode() == ISD::ADD) {
    int64_t Imm = 0;
    if (isImmSExt20(Addr.getOperand(1), Imm)) {
      Disp = CurDAG->getTargetConstant(Imm, MVT::i64);
      if (FrameIndexSDNode *FI =
          dyn_cast<FrameIndexSDNode>(Addr.getOperand(0))) {
        Base = CurDAG->getTargetFrameIndex(FI->getIndex(), VT);
      } else {
        Base = Addr.getOperand(0);
      }
      return true; // [r+i]
    }
  } else if (Addr.getOpcode() == ISD::OR) {
    int64_t Imm = 0;
    if (isImmSExt20(Addr.getOperand(1), Imm)) {
      // If this is an or of disjoint bitfields, we can codegen this as an add
      // (for better address arithmetic) if the LHS and RHS of the OR are
      // provably disjoint.
      APInt LHSKnownZero, LHSKnownOne;
      CurDAG->ComputeMaskedBits(Addr.getOperand(0),
                                APInt::getAllOnesValue(Addr.getOperand(0)
                                                       .getValueSizeInBits()),
                                LHSKnownZero, LHSKnownOne);

      if ((LHSKnownZero.getZExtValue()|~(uint64_t)Imm) == ~0ULL) {
        // If all of the bits are known zero on the LHS or RHS, the add won't
        // carry.
        Base = Addr.getOperand(0);
        Disp = CurDAG->getTargetConstant(Imm, MVT::i64);
        return true;
      }
    }
  } else if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(Addr)) {
    // Loading from a constant address.

    // If this address fits entirely in a 20-bit sext immediate field, codegen
    // this as "d(r0)"
    int64_t Imm;
    if (isImmSExt20(CN, Imm)) {
      Disp = CurDAG->getTargetConstant(Imm, MVT::i64);
      Base = CurDAG->getRegister(0, VT);
      return true;
    }
  }

  Disp = CurDAG->getTargetConstant(0, MVT::i64);
  if (FrameIndexSDNode *FI = dyn_cast<FrameIndexSDNode>(Addr))
    Base = CurDAG->getTargetFrameIndex(FI->getIndex(), VT);
  else
    Base = Addr;
  return true;      // [r+0]
}

/// MatchAddress - Add the specified node to the specified addressing mode,
/// returning true if it cannot be done.  This just pattern matches for the
/// addressing mode.
bool SystemZDAGToDAGISel::MatchAddress(SDValue N, SystemZRRIAddressMode &AM,
                                       bool is12Bit, unsigned Depth) {
  DebugLoc dl = N.getDebugLoc();
  DOUT << "MatchAddress: "; DEBUG(AM.dump());
  // Limit recursion.
  if (Depth > 5)
    return MatchAddressBase(N, AM);

  // FIXME: We can perform better here. If we have something like
  // (shift (add A, imm), N), we can try to reassociate stuff and fold shift of
  // imm into addressing mode.
  switch (N.getOpcode()) {
  default: break;
  case ISD::Constant: {
    int64_t Val = cast<ConstantSDNode>(N)->getSExtValue();
    int64_t Imm;
    bool Match = (is12Bit ?
                  isImmZExt12(AM.Disp + Val, Imm) :
                  isImmSExt20(AM.Disp + Val, Imm));
    if (Match) {
      AM.Disp = Imm;
      return false;
    }
    break;
  }

  case ISD::FrameIndex:
    if (AM.BaseType == SystemZRRIAddressMode::RegBase &&
        AM.Base.Reg.getNode() == 0) {
      AM.BaseType = SystemZRRIAddressMode::FrameIndexBase;
      AM.Base.FrameIndex = cast<FrameIndexSDNode>(N)->getIndex();
      return false;
    }
    break;

  case ISD::SUB: {
    // Given A-B, if A can be completely folded into the address and
    // the index field with the index field unused, use -B as the index.
    // This is a win if a has multiple parts that can be folded into
    // the address. Also, this saves a mov if the base register has
    // other uses, since it avoids a two-address sub instruction, however
    // it costs an additional mov if the index register has other uses.

    // Test if the LHS of the sub can be folded.
    SystemZRRIAddressMode Backup = AM;
    if (MatchAddress(N.getNode()->getOperand(0), AM, is12Bit, Depth+1)) {
      AM = Backup;
      break;
    }
    // Test if the index field is free for use.
    if (AM.IndexReg.getNode()) {
      AM = Backup;
      break;
    }

    // If the base is a register with multiple uses, this transformation may
    // save a mov. Otherwise it's probably better not to do it.
    if (AM.BaseType == SystemZRRIAddressMode::RegBase &&
        (!AM.Base.Reg.getNode() || AM.Base.Reg.getNode()->hasOneUse())) {
      AM = Backup;
      break;
    }

    // Ok, the transformation is legal and appears profitable. Go for it.
    SDValue RHS = N.getNode()->getOperand(1);
    SDValue Zero = CurDAG->getConstant(0, N.getValueType());
    SDValue Neg = CurDAG->getNode(ISD::SUB, dl, N.getValueType(), Zero, RHS);
    AM.IndexReg = Neg;

    // Insert the new nodes into the topological ordering.
    if (Zero.getNode()->getNodeId() == -1 ||
        Zero.getNode()->getNodeId() > N.getNode()->getNodeId()) {
      CurDAG->RepositionNode(N.getNode(), Zero.getNode());
      Zero.getNode()->setNodeId(N.getNode()->getNodeId());
    }
    if (Neg.getNode()->getNodeId() == -1 ||
        Neg.getNode()->getNodeId() > N.getNode()->getNodeId()) {
      CurDAG->RepositionNode(N.getNode(), Neg.getNode());
      Neg.getNode()->setNodeId(N.getNode()->getNodeId());
    }
    return false;
  }

  case ISD::ADD: {
    SystemZRRIAddressMode Backup = AM;
    if (!MatchAddress(N.getNode()->getOperand(0), AM, is12Bit, Depth+1) &&
        !MatchAddress(N.getNode()->getOperand(1), AM, is12Bit, Depth+1))
      return false;
    AM = Backup;
    if (!MatchAddress(N.getNode()->getOperand(1), AM, is12Bit, Depth+1) &&
        !MatchAddress(N.getNode()->getOperand(0), AM, is12Bit, Depth+1))
      return false;
    AM = Backup;

    // If we couldn't fold both operands into the address at the same time,
    // see if we can just put each operand into a register and fold at least
    // the add.
    if (AM.BaseType == SystemZRRIAddressMode::RegBase &&
        !AM.Base.Reg.getNode() && !AM.IndexReg.getNode()) {
      AM.Base.Reg = N.getNode()->getOperand(0);
      AM.IndexReg = N.getNode()->getOperand(1);
      return false;
    }
    break;
  }

  case ISD::OR:
    // Handle "X | C" as "X + C" iff X is known to have C bits clear.
    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
      SystemZRRIAddressMode Backup = AM;
      int64_t Offset = CN->getSExtValue();
      int64_t Imm;
      bool MatchOffset = (is12Bit ?
                          isImmZExt12(AM.Disp + Offset, Imm) :
                          isImmSExt20(AM.Disp + Offset, Imm));
      // The resultant disp must fit in 12 or 20-bits.
      if (MatchOffset &&
          // LHS should be an addr mode.
          !MatchAddress(N.getOperand(0), AM, is12Bit, Depth+1) &&
          // Check to see if the LHS & C is zero.
          CurDAG->MaskedValueIsZero(N.getOperand(0), CN->getAPIntValue())) {
        AM.Disp = Imm;
        return false;
      }
      AM = Backup;
    }
    break;
  }

  return MatchAddressBase(N, AM);
}

/// MatchAddressBase - Helper for MatchAddress. Add the specified node to the
/// specified addressing mode without any further recursion.
bool SystemZDAGToDAGISel::MatchAddressBase(SDValue N,
                                           SystemZRRIAddressMode &AM) {
  // Is the base register already occupied?
  if (AM.BaseType != SystemZRRIAddressMode::RegBase || AM.Base.Reg.getNode()) {
    // If so, check to see if the scale index register is set.
    if (AM.IndexReg.getNode() == 0) {
      AM.IndexReg = N;
      return false;
    }

    // Otherwise, we cannot select it.
    return true;
  }

  // Default, generate it as a register.
  AM.BaseType = SystemZRRIAddressMode::RegBase;
  AM.Base.Reg = N;
  return false;
}

void SystemZDAGToDAGISel::getAddressOperands(const SystemZRRIAddressMode &AM,
                                             SDValue &Base, SDValue &Disp,
                                             SDValue &Index) {
  if (AM.BaseType == SystemZRRIAddressMode::RegBase)
    Base = AM.Base.Reg;
  else
    Base = CurDAG->getTargetFrameIndex(AM.Base.FrameIndex, TLI.getPointerTy());
  Index = AM.IndexReg;
  Disp = CurDAG->getTargetConstant(AM.Disp, MVT::i64);
}

/// Returns true if the address can be represented by a base register plus
/// index register plus an unsigned 12-bit displacement [base + idx + imm].
bool SystemZDAGToDAGISel::SelectAddrRRI12(SDValue Op, SDValue Addr,
                                SDValue &Base, SDValue &Disp, SDValue &Index) {
  SystemZRRIAddressMode AM20, AM12;
  bool Done = false;

  if (!Addr.hasOneUse()) {
    unsigned Opcode = Addr.getOpcode();
    if (Opcode != ISD::Constant && Opcode != ISD::FrameIndex) {
      // If we are able to fold N into addressing mode, then we'll allow it even
      // if N has multiple uses. In general, addressing computation is used as
      // addresses by all of its uses. But watch out for CopyToReg uses, that
      // means the address computation is liveout. It will be computed by a LA
      // so we want to avoid computing the address twice.
      for (SDNode::use_iterator UI = Addr.getNode()->use_begin(),
             UE = Addr.getNode()->use_end(); UI != UE; ++UI) {
        if (UI->getOpcode() == ISD::CopyToReg) {
          MatchAddressBase(Addr, AM12);
          Done = true;
          break;
        }
      }
    }
  }
  if (!Done && MatchAddress(Addr, AM12, /* is12Bit */ true))
    return false;

  // Check, whether we can match stuff using 20-bit displacements
  if (!Done && !MatchAddress(Addr, AM20, /* is12Bit */ false))
    if (AM12.Disp == 0 && AM20.Disp != 0)
      return false;

  DOUT << "MatchAddress (final): "; DEBUG(AM12.dump());

  MVT VT = Addr.getValueType();
  if (AM12.BaseType == SystemZRRIAddressMode::RegBase) {
    if (!AM12.Base.Reg.getNode())
      AM12.Base.Reg = CurDAG->getRegister(0, VT);
  }

  if (!AM12.IndexReg.getNode())
    AM12.IndexReg = CurDAG->getRegister(0, VT);

  getAddressOperands(AM12, Base, Disp, Index);

  return true;
}

/// Returns true if the address can be represented by a base register plus
/// index register plus a signed 20-bit displacement [base + idx + imm].
bool SystemZDAGToDAGISel::SelectAddrRRI20(SDValue Op, SDValue Addr,
                                SDValue &Base, SDValue &Disp, SDValue &Index) {
  SystemZRRIAddressMode AM;
  bool Done = false;

  if (!Addr.hasOneUse()) {
    unsigned Opcode = Addr.getOpcode();
    if (Opcode != ISD::Constant && Opcode != ISD::FrameIndex) {
      // If we are able to fold N into addressing mode, then we'll allow it even
      // if N has multiple uses. In general, addressing computation is used as
      // addresses by all of its uses. But watch out for CopyToReg uses, that
      // means the address computation is liveout. It will be computed by a LA
      // so we want to avoid computing the address twice.
      for (SDNode::use_iterator UI = Addr.getNode()->use_begin(),
             UE = Addr.getNode()->use_end(); UI != UE; ++UI) {
        if (UI->getOpcode() == ISD::CopyToReg) {
          MatchAddressBase(Addr, AM);
          Done = true;
          break;
        }
      }
    }
  }
  if (!Done && MatchAddress(Addr, AM, /* is12Bit */ false))
    return false;

  DOUT << "MatchAddress (final): "; DEBUG(AM.dump());

  MVT VT = Addr.getValueType();
  if (AM.BaseType == SystemZRRIAddressMode::RegBase) {
    if (!AM.Base.Reg.getNode())
      AM.Base.Reg = CurDAG->getRegister(0, VT);
  }

  if (!AM.IndexReg.getNode())
    AM.IndexReg = CurDAG->getRegister(0, VT);

  getAddressOperands(AM, Base, Disp, Index);

  return true;
}

/// SelectLAAddr - it calls SelectAddr and determines if the maximal addressing
/// mode it matches can be cost effectively emitted as an LA/LAY instruction.
bool SystemZDAGToDAGISel::SelectLAAddr(SDValue Op, SDValue Addr,
                                  SDValue &Base, SDValue &Disp, SDValue &Index) {
  SystemZRRIAddressMode AM;

  if (MatchAddress(Addr, AM, false))
    return false;

  MVT VT = Addr.getValueType();
  unsigned Complexity = 0;
  if (AM.BaseType == SystemZRRIAddressMode::RegBase)
    if (AM.Base.Reg.getNode())
      Complexity = 1;
    else
      AM.Base.Reg = CurDAG->getRegister(0, VT);
  else if (AM.BaseType == SystemZRRIAddressMode::FrameIndexBase)
    Complexity = 4;

  if (AM.IndexReg.getNode())
    Complexity += 1;
  else
    AM.IndexReg = CurDAG->getRegister(0, VT);

  if (AM.Disp && (AM.Base.Reg.getNode() || AM.IndexReg.getNode()))
    Complexity += 1;

  if (Complexity > 2) {
    getAddressOperands(AM, Base, Disp, Index);
    return true;
  }

  return false;
}

/// InstructionSelect - This callback is invoked by
/// SelectionDAGISel when it has created a SelectionDAG for us to codegen.
void SystemZDAGToDAGISel::InstructionSelect() {
  DEBUG(BB->dump());

  // Codegen the basic block.
#ifndef NDEBUG
  DOUT << "===== Instruction selection begins:\n";
  Indent = 0;
#endif
  SelectRoot(*CurDAG);
#ifndef NDEBUG
  DOUT << "===== Instruction selection ends:\n";
#endif

  CurDAG->RemoveDeadNodes();
}

SDNode *SystemZDAGToDAGISel::Select(SDValue Op) {
  SDNode *Node = Op.getNode();
  DebugLoc dl = Op.getDebugLoc();

  // Dump information about the Node being selected
  #ifndef NDEBUG
  DOUT << std::string(Indent, ' ') << "Selecting: ";
  DEBUG(Node->dump(CurDAG));
  DOUT << "\n";
  Indent += 2;
  #endif

  // If we have a custom node, we already have selected!
  if (Node->isMachineOpcode()) {
    #ifndef NDEBUG
    DOUT << std::string(Indent-2, ' ') << "== ";
    DEBUG(Node->dump(CurDAG));
    DOUT << "\n";
    Indent -= 2;
    #endif
    return NULL;
  }

  // Select the default instruction
  SDNode *ResNode = SelectCode(Op);

  #ifndef NDEBUG
  DOUT << std::string(Indent-2, ' ') << "=> ";
  if (ResNode == NULL || ResNode == Op.getNode())
    DEBUG(Op.getNode()->dump(CurDAG));
  else
    DEBUG(ResNode->dump(CurDAG));
  DOUT << "\n";
  Indent -= 2;
  #endif

  return ResNode;
}
