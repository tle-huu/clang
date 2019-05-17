//===--- APValue.h - Union class for APFloat/APSInt/Complex -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines the APValue class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_AST_APVALUE_H
#define LLVM_CLANG_AST_APVALUE_H

#include "clang/Basic/FixedPoint.h"
#include "clang/Basic/LLVM.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/PointerUnion.h"

namespace clang {
  class AddrLabelExpr;
  class ASTContext;
  class CharUnits;
  class CXXRecordDecl;
  class Decl;
  class DiagnosticBuilder;
  class Expr;
  class FieldDecl;
  struct PrintingPolicy;
  class Type;
  class ValueDecl;

/// Symbolic representation of typeid(T) for some type T.
class TypeInfoLValue {
  const Type *T;

public:
  TypeInfoLValue() : T() {}
  explicit TypeInfoLValue(const Type *T);

  const Type *getType() const { return T; }
  explicit operator bool() const { return T; }

  void *getOpaqueValue() { return const_cast<Type*>(T); }
  static TypeInfoLValue getFromOpaqueValue(void *Value) {
    TypeInfoLValue V;
    V.T = reinterpret_cast<const Type*>(Value);
    return V;
  }

  void print(llvm::raw_ostream &Out, const PrintingPolicy &Policy) const;
};
}

namespace llvm {
template<> struct PointerLikeTypeTraits<clang::TypeInfoLValue> {
  static void *getAsVoidPointer(clang::TypeInfoLValue V) {
    return V.getOpaqueValue();
  }
  static clang::TypeInfoLValue getFromVoidPointer(void *P) {
    return clang::TypeInfoLValue::getFromOpaqueValue(P);
  }
  // Validated by static_assert in APValue.cpp; hardcoded to avoid needing
  // to include Type.h.
  static constexpr int NumLowBitsAvailable = 3;
};
}

namespace clang {
/// APValue - This class implements a discriminated union of [uninitialized]
/// [APSInt] [APFloat], [Complex APSInt] [Complex APFloat], [Expr + Offset],
/// [Vector: N * APValue], [Array: N * APValue]
class APValue {
  typedef llvm::APSInt APSInt;
  typedef llvm::APFloat APFloat;
public:
  enum ValueKind {
    Uninitialized,
    Int,
    Float,
    FixedPoint,
    ComplexInt,
    ComplexFloat,
    LValue,
    Vector,
    Array,
    Struct,
    Union,
    MemberPointer,
    AddrLabelDiff
  };

  class LValueBase {
  public:
    typedef llvm::PointerUnion<const ValueDecl *, const Expr *, TypeInfoLValue>
        PtrTy;

    LValueBase() : Local{} {}

    template <class T>
    LValueBase(T P, unsigned I = 0, unsigned V = 0) : Ptr(P), Local{I, V} {
      assert(!is<TypeInfoLValue>() &&
             "don't use this constructor to form a type_info lvalue");
    }

    static LValueBase getTypeInfo(TypeInfoLValue LV, QualType TypeInfo);

    template <class T>
    bool is() const { return Ptr.is<T>(); }

    template <class T>
    T get() const { return Ptr.get<T>(); }

    template <class T>
    T dyn_cast() const { return Ptr.dyn_cast<T>(); }

    void *getOpaqueValue() const;

    bool isNull() const;

    explicit operator bool() const;

    PtrTy getPointer() const { return Ptr; }

    unsigned getCallIndex() const;
    unsigned getVersion() const;
    QualType getTypeInfoType() const;

    friend bool operator==(const LValueBase &LHS, const LValueBase &RHS);
    friend bool operator!=(const LValueBase &LHS, const LValueBase &RHS) {
      return !(LHS == RHS);
    }
    friend llvm::hash_code hash_value(const LValueBase &Base);

  private:
    PtrTy Ptr;
    struct LocalState {
      unsigned CallIndex, Version;
    };
    union {
      LocalState Local;
      /// The type std::type_info, if this is a TypeInfoLValue.
      void *TypeInfoType;
    };
  };

  /// A FieldDecl or CXXRecordDecl, along with a flag indicating whether we
  /// mean a virtual or non-virtual base class subobject.
  typedef llvm::PointerIntPair<const Decl *, 1, bool> BaseOrMemberType;

  /// A non-discriminated union of a base, field, or array index.
  class LValuePathEntry {
    static_assert(sizeof(uintptr_t) <= sizeof(uint64_t),
                  "pointer doesn't fit in 64 bits?");
    uint64_t Value;

  public:
    LValuePathEntry() : Value() {}
    LValuePathEntry(BaseOrMemberType BaseOrMember)
        : Value{reinterpret_cast<uintptr_t>(BaseOrMember.getOpaqueValue())} {}
    static LValuePathEntry ArrayIndex(uint64_t Index) {
      LValuePathEntry Result;
      Result.Value = Index;
      return Result;
    }

    BaseOrMemberType getAsBaseOrMember() const {
      return BaseOrMemberType::getFromOpaqueValue(
          reinterpret_cast<void *>(Value));
    }
    uint64_t getAsArrayIndex() const { return Value; }

    friend bool operator==(LValuePathEntry A, LValuePathEntry B) {
      return A.Value == B.Value;
    }
    friend bool operator!=(LValuePathEntry A, LValuePathEntry B) {
      return A.Value != B.Value;
    }
    friend llvm::hash_code hash_value(LValuePathEntry A) {
      return llvm::hash_value(A.Value);
    }
  };
  struct NoLValuePath {};
  struct UninitArray {};
  struct UninitStruct {};
private:
  ValueKind Kind;

  struct ComplexAPSInt {
    APSInt Real, Imag;
    ComplexAPSInt() : Real(1), Imag(1) {}
  };
  struct ComplexAPFloat {
    APFloat Real, Imag;
    ComplexAPFloat() : Real(0.0), Imag(0.0) {}
  };
  struct LV;
  struct Vec {
    APValue *Elts;
    unsigned NumElts;
    Vec() : Elts(nullptr), NumElts(0) {}
    ~Vec() { delete[] Elts; }
  };
  struct Arr {
    APValue *Elts;
    unsigned NumElts, ArrSize;
    Arr(unsigned NumElts, unsigned ArrSize);
    ~Arr();
  };
  struct StructData {
    APValue *Elts;
    unsigned NumBases;
    unsigned NumFields;
    StructData(unsigned NumBases, unsigned NumFields);
    ~StructData();
  };
  struct UnionData {
    const FieldDecl *Field;
    APValue *Value;
    UnionData();
    ~UnionData();
  };
  struct AddrLabelDiffData {
    const AddrLabelExpr* LHSExpr;
    const AddrLabelExpr* RHSExpr;
  };
  struct MemberPointerData;

  // We ensure elsewhere that Data is big enough for LV and MemberPointerData.
  typedef llvm::AlignedCharArrayUnion<void *, APSInt, APFloat, ComplexAPSInt,
                                      ComplexAPFloat, Vec, Arr, StructData,
                                      UnionData, AddrLabelDiffData> DataType;
  static const size_t DataSize = sizeof(DataType);

  DataType Data;

public:
  APValue() : Kind(Uninitialized) {}
  explicit APValue(APSInt I) : Kind(Uninitialized) {
    MakeInt(); setInt(std::move(I));
  }
  explicit APValue(APFloat F) : Kind(Uninitialized) {
    MakeFloat(); setFloat(std::move(F));
  }
  explicit APValue(APFixedPoint FX) : Kind(Uninitialized) {
    MakeFixedPoint(std::move(FX));
  }
  explicit APValue(const APValue *E, unsigned N) : Kind(Uninitialized) {
    MakeVector(); setVector(E, N);
  }
  APValue(APSInt R, APSInt I) : Kind(Uninitialized) {
    MakeComplexInt(); setComplexInt(std::move(R), std::move(I));
  }
  APValue(APFloat R, APFloat I) : Kind(Uninitialized) {
    MakeComplexFloat(); setComplexFloat(std::move(R), std::move(I));
  }
  APValue(const APValue &RHS);
  APValue(APValue &&RHS) : Kind(Uninitialized) { swap(RHS); }
  APValue(LValueBase B, const CharUnits &O, NoLValuePath N,
          bool IsNullPtr = false)
      : Kind(Uninitialized) {
    MakeLValue(); setLValue(B, O, N, IsNullPtr);
  }
  APValue(LValueBase B, const CharUnits &O, ArrayRef<LValuePathEntry> Path,
          bool OnePastTheEnd, bool IsNullPtr = false)
      : Kind(Uninitialized) {
    MakeLValue(); setLValue(B, O, Path, OnePastTheEnd, IsNullPtr);
  }
  APValue(UninitArray, unsigned InitElts, unsigned Size) : Kind(Uninitialized) {
    MakeArray(InitElts, Size);
  }
  APValue(UninitStruct, unsigned B, unsigned M) : Kind(Uninitialized) {
    MakeStruct(B, M);
  }
  explicit APValue(const FieldDecl *D, const APValue &V = APValue())
      : Kind(Uninitialized) {
    MakeUnion(); setUnion(D, V);
  }
  APValue(const ValueDecl *Member, bool IsDerivedMember,
          ArrayRef<const CXXRecordDecl*> Path) : Kind(Uninitialized) {
    MakeMemberPointer(Member, IsDerivedMember, Path);
  }
  APValue(const AddrLabelExpr* LHSExpr, const AddrLabelExpr* RHSExpr)
      : Kind(Uninitialized) {
    MakeAddrLabelDiff(); setAddrLabelDiff(LHSExpr, RHSExpr);
  }

  ~APValue() {
    MakeUninit();
  }

  /// Returns whether the object performed allocations.
  ///
  /// If APValues are constructed via placement new, \c needsCleanup()
  /// indicates whether the destructor must be called in order to correctly
  /// free all allocated memory.
  bool needsCleanup() const;

  /// Swaps the contents of this and the given APValue.
  void swap(APValue &RHS);

  ValueKind getKind() const { return Kind; }
  bool isUninit() const { return Kind == Uninitialized; }
  bool isInt() const { return Kind == Int; }
  bool isFloat() const { return Kind == Float; }
  bool isFixedPoint() const { return Kind == FixedPoint; }
  bool isComplexInt() const { return Kind == ComplexInt; }
  bool isComplexFloat() const { return Kind == ComplexFloat; }
  bool isLValue() const { return Kind == LValue; }
  bool isVector() const { return Kind == Vector; }
  bool isArray() const { return Kind == Array; }
  bool isStruct() const { return Kind == Struct; }
  bool isUnion() const { return Kind == Union; }
  bool isMemberPointer() const { return Kind == MemberPointer; }
  bool isAddrLabelDiff() const { return Kind == AddrLabelDiff; }

  void dump() const;
  void dump(raw_ostream &OS) const;

  void printPretty(raw_ostream &OS, ASTContext &Ctx, QualType Ty) const;
  std::string getAsString(ASTContext &Ctx, QualType Ty) const;

  APSInt &getInt() {
    assert(isInt() && "Invalid accessor");
    return *(APSInt*)(char*)Data.buffer;
  }
  const APSInt &getInt() const {
    return const_cast<APValue*>(this)->getInt();
  }

  /// Try to convert this value to an integral constant. This works if it's an
  /// integer, null pointer, or offset from a null pointer. Returns true on
  /// success.
  bool toIntegralConstant(APSInt &Result, QualType SrcTy,
                          const ASTContext &Ctx) const;

  APFloat &getFloat() {
    assert(isFloat() && "Invalid accessor");
    return *(APFloat*)(char*)Data.buffer;
  }
  const APFloat &getFloat() const {
    return const_cast<APValue*>(this)->getFloat();
  }

  APFixedPoint &getFixedPoint() {
    assert(isFixedPoint() && "Invalid accessor");
    return *(APFixedPoint *)(char *)Data.buffer;
  }
  const APFixedPoint &getFixedPoint() const {
    return const_cast<APValue *>(this)->getFixedPoint();
  }

  APSInt &getComplexIntReal() {
    assert(isComplexInt() && "Invalid accessor");
    return ((ComplexAPSInt*)(char*)Data.buffer)->Real;
  }
  const APSInt &getComplexIntReal() const {
    return const_cast<APValue*>(this)->getComplexIntReal();
  }

  APSInt &getComplexIntImag() {
    assert(isComplexInt() && "Invalid accessor");
    return ((ComplexAPSInt*)(char*)Data.buffer)->Imag;
  }
  const APSInt &getComplexIntImag() const {
    return const_cast<APValue*>(this)->getComplexIntImag();
  }

  APFloat &getComplexFloatReal() {
    assert(isComplexFloat() && "Invalid accessor");
    return ((ComplexAPFloat*)(char*)Data.buffer)->Real;
  }
  const APFloat &getComplexFloatReal() const {
    return const_cast<APValue*>(this)->getComplexFloatReal();
  }

  APFloat &getComplexFloatImag() {
    assert(isComplexFloat() && "Invalid accessor");
    return ((ComplexAPFloat*)(char*)Data.buffer)->Imag;
  }
  const APFloat &getComplexFloatImag() const {
    return const_cast<APValue*>(this)->getComplexFloatImag();
  }

  const LValueBase getLValueBase() const;
  CharUnits &getLValueOffset();
  const CharUnits &getLValueOffset() const {
    return const_cast<APValue*>(this)->getLValueOffset();
  }
  bool isLValueOnePastTheEnd() const;
  bool hasLValuePath() const;
  ArrayRef<LValuePathEntry> getLValuePath() const;
  unsigned getLValueCallIndex() const;
  unsigned getLValueVersion() const;
  bool isNullPointer() const;

  APValue &getVectorElt(unsigned I) {
    assert(isVector() && "Invalid accessor");
    assert(I < getVectorLength() && "Index out of range");
    return ((Vec*)(char*)Data.buffer)->Elts[I];
  }
  const APValue &getVectorElt(unsigned I) const {
    return const_cast<APValue*>(this)->getVectorElt(I);
  }
  unsigned getVectorLength() const {
    assert(isVector() && "Invalid accessor");
    return ((const Vec*)(const void *)Data.buffer)->NumElts;
  }

  APValue &getArrayInitializedElt(unsigned I) {
    assert(isArray() && "Invalid accessor");
    assert(I < getArrayInitializedElts() && "Index out of range");
    return ((Arr*)(char*)Data.buffer)->Elts[I];
  }
  const APValue &getArrayInitializedElt(unsigned I) const {
    return const_cast<APValue*>(this)->getArrayInitializedElt(I);
  }
  bool hasArrayFiller() const {
    return getArrayInitializedElts() != getArraySize();
  }
  APValue &getArrayFiller() {
    assert(isArray() && "Invalid accessor");
    assert(hasArrayFiller() && "No array filler");
    return ((Arr*)(char*)Data.buffer)->Elts[getArrayInitializedElts()];
  }
  const APValue &getArrayFiller() const {
    return const_cast<APValue*>(this)->getArrayFiller();
  }
  unsigned getArrayInitializedElts() const {
    assert(isArray() && "Invalid accessor");
    return ((const Arr*)(const void *)Data.buffer)->NumElts;
  }
  unsigned getArraySize() const {
    assert(isArray() && "Invalid accessor");
    return ((const Arr*)(const void *)Data.buffer)->ArrSize;
  }

  unsigned getStructNumBases() const {
    assert(isStruct() && "Invalid accessor");
    return ((const StructData*)(const char*)Data.buffer)->NumBases;
  }
  unsigned getStructNumFields() const {
    assert(isStruct() && "Invalid accessor");
    return ((const StructData*)(const char*)Data.buffer)->NumFields;
  }
  APValue &getStructBase(unsigned i) {
    assert(isStruct() && "Invalid accessor");
    return ((StructData*)(char*)Data.buffer)->Elts[i];
  }
  APValue &getStructField(unsigned i) {
    assert(isStruct() && "Invalid accessor");
    return ((StructData*)(char*)Data.buffer)->Elts[getStructNumBases() + i];
  }
  const APValue &getStructBase(unsigned i) const {
    return const_cast<APValue*>(this)->getStructBase(i);
  }
  const APValue &getStructField(unsigned i) const {
    return const_cast<APValue*>(this)->getStructField(i);
  }

  const FieldDecl *getUnionField() const {
    assert(isUnion() && "Invalid accessor");
    return ((const UnionData*)(const char*)Data.buffer)->Field;
  }
  APValue &getUnionValue() {
    assert(isUnion() && "Invalid accessor");
    return *((UnionData*)(char*)Data.buffer)->Value;
  }
  const APValue &getUnionValue() const {
    return const_cast<APValue*>(this)->getUnionValue();
  }

  const ValueDecl *getMemberPointerDecl() const;
  bool isMemberPointerToDerivedMember() const;
  ArrayRef<const CXXRecordDecl*> getMemberPointerPath() const;

  const AddrLabelExpr* getAddrLabelDiffLHS() const {
    assert(isAddrLabelDiff() && "Invalid accessor");
    return ((const AddrLabelDiffData*)(const char*)Data.buffer)->LHSExpr;
  }
  const AddrLabelExpr* getAddrLabelDiffRHS() const {
    assert(isAddrLabelDiff() && "Invalid accessor");
    return ((const AddrLabelDiffData*)(const char*)Data.buffer)->RHSExpr;
  }

  void setInt(APSInt I) {
    assert(isInt() && "Invalid accessor");
    *(APSInt *)(char *)Data.buffer = std::move(I);
  }
  void setFloat(APFloat F) {
    assert(isFloat() && "Invalid accessor");
    *(APFloat *)(char *)Data.buffer = std::move(F);
  }
  void setFixedPoint(APFixedPoint FX) {
    assert(isFixedPoint() && "Invalid accessor");
    *(APFixedPoint *)(char *)Data.buffer = std::move(FX);
  }
  void setVector(const APValue *E, unsigned N) {
    assert(isVector() && "Invalid accessor");
    ((Vec*)(char*)Data.buffer)->Elts = new APValue[N];
    ((Vec*)(char*)Data.buffer)->NumElts = N;
    for (unsigned i = 0; i != N; ++i)
      ((Vec*)(char*)Data.buffer)->Elts[i] = E[i];
  }
  void setComplexInt(APSInt R, APSInt I) {
    assert(R.getBitWidth() == I.getBitWidth() &&
           "Invalid complex int (type mismatch).");
    assert(isComplexInt() && "Invalid accessor");
    ((ComplexAPSInt *)(char *)Data.buffer)->Real = std::move(R);
    ((ComplexAPSInt *)(char *)Data.buffer)->Imag = std::move(I);
  }
  void setComplexFloat(APFloat R, APFloat I) {
    assert(&R.getSemantics() == &I.getSemantics() &&
           "Invalid complex float (type mismatch).");
    assert(isComplexFloat() && "Invalid accessor");
    ((ComplexAPFloat *)(char *)Data.buffer)->Real = std::move(R);
    ((ComplexAPFloat *)(char *)Data.buffer)->Imag = std::move(I);
  }
  void setLValue(LValueBase B, const CharUnits &O, NoLValuePath,
                 bool IsNullPtr);
  void setLValue(LValueBase B, const CharUnits &O,
                 ArrayRef<LValuePathEntry> Path, bool OnePastTheEnd,
                 bool IsNullPtr);
  void setUnion(const FieldDecl *Field, const APValue &Value) {
    assert(isUnion() && "Invalid accessor");
    ((UnionData*)(char*)Data.buffer)->Field = Field;
    *((UnionData*)(char*)Data.buffer)->Value = Value;
  }
  void setAddrLabelDiff(const AddrLabelExpr* LHSExpr,
                        const AddrLabelExpr* RHSExpr) {
    ((AddrLabelDiffData*)(char*)Data.buffer)->LHSExpr = LHSExpr;
    ((AddrLabelDiffData*)(char*)Data.buffer)->RHSExpr = RHSExpr;
  }

  /// Assign by swapping from a copy of the RHS.
  APValue &operator=(APValue RHS) {
    swap(RHS);
    return *this;
  }

private:
  void DestroyDataAndMakeUninit();
  void MakeUninit() {
    if (Kind != Uninitialized)
      DestroyDataAndMakeUninit();
  }
  void MakeInt() {
    assert(isUninit() && "Bad state change");
    new ((void*)Data.buffer) APSInt(1);
    Kind = Int;
  }
  void MakeFloat() {
    assert(isUninit() && "Bad state change");
    new ((void*)(char*)Data.buffer) APFloat(0.0);
    Kind = Float;
  }
  void MakeFixedPoint(APFixedPoint &&FX) {
    assert(isUninit() && "Bad state change");
    new ((void *)(char *)Data.buffer) APFixedPoint(std::move(FX));
    Kind = FixedPoint;
  }
  void MakeVector() {
    assert(isUninit() && "Bad state change");
    new ((void*)(char*)Data.buffer) Vec();
    Kind = Vector;
  }
  void MakeComplexInt() {
    assert(isUninit() && "Bad state change");
    new ((void*)(char*)Data.buffer) ComplexAPSInt();
    Kind = ComplexInt;
  }
  void MakeComplexFloat() {
    assert(isUninit() && "Bad state change");
    new ((void*)(char*)Data.buffer) ComplexAPFloat();
    Kind = ComplexFloat;
  }
  void MakeLValue();
  void MakeArray(unsigned InitElts, unsigned Size);
  void MakeStruct(unsigned B, unsigned M) {
    assert(isUninit() && "Bad state change");
    new ((void*)(char*)Data.buffer) StructData(B, M);
    Kind = Struct;
  }
  void MakeUnion() {
    assert(isUninit() && "Bad state change");
    new ((void*)(char*)Data.buffer) UnionData();
    Kind = Union;
  }
  void MakeMemberPointer(const ValueDecl *Member, bool IsDerivedMember,
                         ArrayRef<const CXXRecordDecl*> Path);
  void MakeAddrLabelDiff() {
    assert(isUninit() && "Bad state change");
    new ((void*)(char*)Data.buffer) AddrLabelDiffData();
    Kind = AddrLabelDiff;
  }
};

} // end namespace clang.

namespace llvm {
template<> struct DenseMapInfo<clang::APValue::LValueBase> {
  static clang::APValue::LValueBase getEmptyKey();
  static clang::APValue::LValueBase getTombstoneKey();
  static unsigned getHashValue(const clang::APValue::LValueBase &Base);
  static bool isEqual(const clang::APValue::LValueBase &LHS,
                      const clang::APValue::LValueBase &RHS);
};
}

#endif
