/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_IR_TYPECONTEXT_H
#define HERMES_IR_TYPECONTEXT_H

#include "hermes/Support/StringTable.h"

#include "llvh/ADT/ArrayRef.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace hermes {

/// Kinds of types in the IR type system.
enum class TypeKind : uint8_t {
  // --- Leaf kinds (no sub-type data) ---
  NoType, ///< Empty set, bottom.
  Undefined,
  Null,
  Boolean,
  Number, ///< Unrefined fp64.
  BigInt,
  String,
  Symbol,
  Empty, ///< TDZ.
  Uninit, ///< Declared, not initialized.
  Environment,
  PrivateName,
  FunctionCode,
  Object, ///< Unrefined JS object.
  Bits32, ///< Physical 32-bit integer, signless.

  // --- Number subtypes (still fp64, value constrained) ---
  Int32, ///< Integer in [-2^31, 2^31 - 1].
  Uint32, ///< Integer in [0, 2^32 - 1].
  UInt31, ///< Integer in [0, 2^31 - 1] (Int32 ∩ Uint32).

  // --- Refined object types (payload-bearing, not yet supported in Phase 1)
  _RefinedFirst,
  ClassInstance = _RefinedFirst, ///< Nominal class instance.
  Array, ///< Array with known element type.
  Tuple, ///< Fixed-length positional types.
  Function, ///< Callable with known signature.
  ExactObject, ///< Known exact field set.
  _RefinedLast = ExactObject,

  // --- Composite ---
  Union, ///< Union of two or more types.
};

/// An entry in the TypeContext type table.
struct TypeEntry {
  TypeKind kind;

  union {
    /// ClassInstance payload. Classes are nominal: each class declaration
    /// produces a unique type entry, not interned by structure.
    struct {
      /// Type ID of parent ClassInstance, or kNoTypeId if no parent.
      uint32_t superClassTypeId;
      /// Class name for runtime type descriptors and error messages.
      Identifier className;
      /// Offset into fieldArrays_.
      uint32_t fieldOffset;
      /// Number of fields.
      uint16_t fieldCount;
    } classInstance;

    /// Array payload.
    struct {
      uint32_t elemTypeId;
    } array;

    /// Tuple payload.
    struct {
      uint32_t elemOffset; ///< into typeArrays_
      uint16_t elemCount;
    } tuple;

    /// Function payload.
    struct {
      uint32_t returnTypeId;
      uint32_t thisTypeId;
      uint32_t restTypeId;
      uint32_t paramOffset; ///< into paramArrays_
      uint16_t paramCount;
      bool isAsync;
      bool isGenerator;
    } function;

    /// ExactObject payload.
    struct {
      uint32_t fieldOffset; ///< into fieldArrays_
      uint16_t fieldCount;
    } exactObject;

    /// Union payload.
    struct {
      uint32_t armOffset; ///< into typeArrays_
      uint16_t armCount; ///< >= 2
    } union_;
  };

  /// Construct a leaf type entry with no payload.
  static TypeEntry createLeaf(TypeKind k) {
    TypeEntry e{};
    e.kind = k;
    return e;
  }

  /// Construct a union type entry.
  static TypeEntry createUnion(uint32_t armOffset, uint16_t armCount) {
    assert(armCount >= 2 && "Union must have at least 2 arms");
    TypeEntry e{};
    e.kind = TypeKind::Union;
    e.union_.armOffset = armOffset;
    e.union_.armCount = armCount;
    return e;
  }
};

/// Side array element for function parameters.
struct FunctionParam {
  uint32_t typeId;
  bool isOptional;
};

/// Side array element for exact object fields.
struct ExactObjectField {
  Identifier name;
  uint32_t typeId;
};

/// Pre-allocated type IDs (assigned by TypeContext constructor).
/// These are constants, valid for any TypeContext instance.
/// The well-known ID assignment is frozen: IDs are never removed or reordered.
/// New well-known IDs are only appended before kFirstDynamicId.
enum : uint32_t {
  kNoTypeId,
  kEmptyId,
  kUninitId,
  kUndefinedId,
  kNullId,
  kBooleanId,
  kStringId,
  kNumberId,
  kBigIntId,
  kSymbolId,
  kEnvironmentId,
  kPrivateNameId,
  kFunctionCodeId,
  kObjectId,
  kBits32Id,
  kInt32Id,
  kUint32Id,
  /// Int32 ∩ Uint32: integer in [0, 2^31 - 1].
  kUInt31Id,
  /// Union of all JS-observable types.
  kAnyTypeId,
  /// Number | BigInt.
  kNumericId,
  /// any | Empty | Uninit.
  kAnyEmptyUninitId,
  /// Null | Undefined.
  kNullOrUndefId,

  /// IDs below this are reserved for well-known types.
  kFirstDynamicId = 32,
};

/// Owns the type table for a Module and provides type operations.
///
/// The constructor pre-allocates entries for all well-known types (primitives
/// and common unions). Well-known IDs have fixed values that are the same
/// across all TypeContext instances.
class TypeContext {
 public:
  TypeContext();

  /// Return the kind of the type entry at \p id.
  TypeKind getKind(uint32_t id) const {
    assert(id < entries_.size() && "Type ID out of range");
    return entries_[id].kind;
  }

  /// Return the arm IDs of a union type entry. Asserts that \p id is a Union.
  /// \warning The returned ArrayRef points into an internal vector. It is
  /// invalidated by any call that adds new type entries (e.g. addUnionEntry).
  llvh::ArrayRef<uint32_t> getUnionArms(uint32_t id) const {
    assert(id < entries_.size() && "Type ID out of range");
    const auto &entry = entries_[id];
    assert(entry.kind == TypeKind::Union && "Not a union type");
    assert(
        size_t(entry.union_.armOffset) + entry.union_.armCount <=
            typeArrays_.size() &&
        "Union arms out of bounds");
    return llvh::ArrayRef<uint32_t>(
        typeArrays_.data() + entry.union_.armOffset, entry.union_.armCount);
  }

 private:
  /// Type table. Index 0 = NoType. Pre-allocated entries for primitives.
  std::vector<TypeEntry> entries_;

  /// Side array storing union arm IDs (and in the future, tuple element IDs).
  /// Uses raw uint32_t since Type is still a bitmask at this point; changed
  /// to Type in P1-S8.
  std::vector<uint32_t> typeArrays_;

  /// Helper to append arms to typeArrays_ and create a union entry.
  /// \pre \p arms must not reference storage inside \c typeArrays_, because
  /// the append may reallocate the vector and invalidate the ArrayRef.
  uint32_t addUnionEntry(llvh::ArrayRef<uint32_t> arms);
};

} // namespace hermes

#endif // HERMES_IR_TYPECONTEXT_H
