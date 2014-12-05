//===--- ImportType.cpp - Import Clang Types ------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements support for importing Clang types as Swift types.
//
//===----------------------------------------------------------------------===//

#include "ImporterImpl.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Strings.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/Types.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/TypeVisitor.h"
#include "swift/ClangImporter/ClangModule.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"

using namespace swift;

/// Given that a type is the result of a special typedef import, was
/// it originally a CF pointer?
static bool isImportedCFPointer(clang::QualType clangType, Type type) {
  return (clangType->isPointerType() &&
          (type->is<ClassType>() || type->isClassExistentialType()));
}

namespace {
  /// Various types that we want to do something interesting to after
  /// importing them.
  enum class ImportHint {
    /// There is nothing special about the source type.
    None,

    /// The source type is 'void'.
    Void,

    /// The source type is 'BOOL'.
    BOOL,

    /// The source type is 'NSString'.
    NSString,

    /// The source type is 'NSArray'.
    NSArray,

    /// The source type is 'NSDictionary'.
    NSDictionary,

    /// The source type is 'NSUInteger'.
    NSUInteger,

    /// The source type is an Objective-C object pointer type.
    ObjCPointer,

    /// The source type is a CF object pointer type.
    CFPointer,

    /// The source type is a C++ reference type.
    Reference,

    /// The source type is a block pointer type.
    Block,
  };

  bool canImportAsOptional(ImportHint hint) {
    switch (hint) {
    case ImportHint::BOOL:
    case ImportHint::None:
    case ImportHint::NSUInteger:
    case ImportHint::Reference:
    case ImportHint::Void:
      return false;

    case ImportHint::Block:
    case ImportHint::CFPointer:
    case ImportHint::NSArray:
    case ImportHint::NSDictionary:
    case ImportHint::NSString:
    case ImportHint::ObjCPointer:
      return true;
    }
  }

  struct ImportResult {
    Type AbstractType;
    ImportHint Hint;

    /*implicit*/ ImportResult(Type type = Type(),
                              ImportHint hint = ImportHint::None)
      : AbstractType(type), Hint(hint) {}

    /*implicit*/ ImportResult(TypeBase *type = nullptr,
                              ImportHint hint = ImportHint::None)
      : AbstractType(type), Hint(hint) {}

    explicit operator bool() const { return (bool) AbstractType; }
  };

  /// Wrap a type in the Optional type appropriate to the import kind.
  static Type
  getOptionalType(Type payloadType,
                  ImportTypeKind kind,
                  OptionalTypeKind OptKind = OTK_ImplicitlyUnwrappedOptional) {
    // Import pointee types as true Optional.
    if (kind == ImportTypeKind::Pointee)
      return OptionalType::get(payloadType);

    switch (OptKind) {
      case OTK_ImplicitlyUnwrappedOptional:
        return ImplicitlyUnwrappedOptionalType::get(payloadType);
      case OTK_None:
        return payloadType;
      case OTK_Optional:
        return OptionalType::get(payloadType);
    }
  }

  class SwiftTypeConverter :
    public clang::TypeVisitor<SwiftTypeConverter, ImportResult>
  {
    ClangImporter::Implementation &Impl;
    bool IsUsedInSystemModule;
    
  public:
    SwiftTypeConverter(ClangImporter::Implementation &impl,
                       bool isUsedInSystemModule)
      : Impl(impl), IsUsedInSystemModule(isUsedInSystemModule) {}

    using TypeVisitor::Visit;
    ImportResult Visit(clang::QualType type) {
      return Visit(type.getTypePtr());
    }

#define DEPENDENT_TYPE(Class, Base)                               \
    ImportResult Visit##Class##Type(const clang::Class##Type *) { \
      llvm_unreachable("Dependent types cannot be converted");    \
    }
#define TYPE(Class, Base)
#include "clang/AST/TypeNodes.def"
    
    // Given a loaded type like CInt, look through the name alias sugar that the
    // stdlib uses to show the underlying type.  We want to import the signature
    // of the exit(3) libc function as "func exit(Int32)", not as
    // "func exit(CInt)".
    static Type unwrapCType(Type T) {
      if (auto *NAT = dyn_cast_or_null<NameAliasType>(T.getPointer()))
        return NAT->getSinglyDesugaredType();
      return T;
    }
    
    ImportResult VisitBuiltinType(const clang::BuiltinType *type) {
      switch (type->getKind()) {
      case clang::BuiltinType::Void:
        return { Type(), ImportHint::Void };

#define MAP_BUILTIN_TYPE(CLANG_BUILTIN_KIND, SWIFT_TYPE_NAME)             \
      case clang::BuiltinType::CLANG_BUILTIN_KIND:                        \
        return unwrapCType(Impl.getNamedSwiftType(Impl.getStdlibModule(), \
                                        #SWIFT_TYPE_NAME));
          
#include "swift/ClangImporter/BuiltinMappedTypes.def"

      // Types that cannot be mapped into Swift, and probably won't ever be.
      case clang::BuiltinType::Dependent:
      case clang::BuiltinType::ARCUnbridgedCast:
      case clang::BuiltinType::BoundMember:
      case clang::BuiltinType::BuiltinFn:
      case clang::BuiltinType::Overload:
      case clang::BuiltinType::PseudoObject:
      case clang::BuiltinType::UnknownAny:
        return Type();

      // FIXME: Types that can be mapped, but aren't yet.
      case clang::BuiltinType::Half:
      case clang::BuiltinType::LongDouble:
      case clang::BuiltinType::NullPtr:
        return Type();

      // Objective-C types that aren't mapped directly; rather, pointers to
      // these types will be mapped.
      case clang::BuiltinType::ObjCClass:
      case clang::BuiltinType::ObjCId:
      case clang::BuiltinType::ObjCSel:
        return Type();

      // OpenCL types that don't have Swift equivalents.
      case clang::BuiltinType::OCLImage1d:
      case clang::BuiltinType::OCLImage1dArray:
      case clang::BuiltinType::OCLImage1dBuffer:
      case clang::BuiltinType::OCLImage2d:
      case clang::BuiltinType::OCLImage2dArray:
      case clang::BuiltinType::OCLImage3d:
      case clang::BuiltinType::OCLEvent:
      case clang::BuiltinType::OCLSampler:
        return Type();
      }
    }

    ImportResult VisitComplexType(const clang::ComplexType *type) {
      // FIXME: Implement once Complex is in the library.
      return Type();
    }

    ImportResult VisitAtomicType(const clang::AtomicType *type) {
      // FIXME: handle pointers and fields of atomic type
      return Type();
    }

    ImportResult VisitMemberPointerType(const clang::MemberPointerType *type) {
      return Type();
    }
    
    ImportResult VisitPointerType(const clang::PointerType *type) {      
      auto pointeeQualType = type->getPointeeType();

      // Special case for NSZone*, which has its own Swift wrapper.
      if (const clang::RecordType *pointee =
            pointeeQualType->getAsStructureType()) {
        if (pointee && !pointee->getDecl()->isCompleteDefinition() &&
            pointee->getDecl()->getName() == "_NSZone") {
          Module *objCModule = Impl.getNamedModule(OBJC_MODULE_NAME);
          Type wrapperTy = Impl.getNamedSwiftType(objCModule, "NSZone");
          if (wrapperTy)
            return wrapperTy;
        }
      }
      
      // All other C pointers to concrete types map to
      // UnsafeMutablePointer<T> or COpaquePointer (FIXME:, except in
      // parameter position under the pre-
      // intrinsic-pointer-conversion regime.)

      // With pointer conversions enabled, map to the normal pointer types
      // without special hints.
      Type pointeeType;
      if (pointeeQualType->isVoidType())
        pointeeType = Impl.getNamedSwiftType(Impl.getStdlibModule(), "Void");
      else
        pointeeType = Impl.importType(pointeeQualType,
                                      ImportTypeKind::Pointee,
                                      IsUsedInSystemModule);

      // If the pointed-to type is unrepresentable in Swift, import as
      // COpaquePointer.
      if (!pointeeType)
        return getOpaquePointerType();
      
      if (pointeeQualType->isFunctionType()) {
        // FIXME: Function pointer types can be mapped to Swift function types
        // once we have the notion of a "thin" function that does not capture
        // anything.
        return Impl.getNamedSwiftTypeSpecialization(Impl.getStdlibModule(),
                                                    "CFunctionPointer",
                                                    pointeeType);
      }

      auto quals = pointeeQualType.getQualifiers();
      
      if (quals.hasConst())
        return {Impl.getNamedSwiftTypeSpecialization(Impl.getStdlibModule(),
                                                     "UnsafePointer",
                                                     pointeeType),
                ImportHint::None};
      // Mutable pointers with __autoreleasing or __unsafe_unretained
      // ownership map to AutoreleasingUnsafeMutablePointer<T>.
      else if (quals.getObjCLifetime() == clang::Qualifiers::OCL_Autoreleasing
            || quals.getObjCLifetime() == clang::Qualifiers::OCL_ExplicitNone)
        return {
          Impl.getNamedSwiftTypeSpecialization(
            Impl.getStdlibModule(), "AutoreleasingUnsafeMutablePointer",
            pointeeType),
            ImportHint::None};
      // All other mutable pointers map to UnsafeMutablePointer.
      return {Impl.getNamedSwiftTypeSpecialization(Impl.getStdlibModule(),
                                                   "UnsafeMutablePointer",
                                                   pointeeType),
              ImportHint::None};
    }

    Type getOpaquePointerType() {
      return Impl.getNamedSwiftType(Impl.getStdlibModule(), "COpaquePointer");
    }

    ImportResult VisitBlockPointerType(const clang::BlockPointerType *type) {
      // Block pointer types are mapped to function types.
      Type pointeeType = Impl.importType(type->getPointeeType(),
                                         ImportTypeKind::Abstract,
                                         IsUsedInSystemModule);
      if (!pointeeType)
        return Type();
      FunctionType *fTy = pointeeType->castTo<FunctionType>();
      
      auto rep = FunctionType::Representation::Block;
      auto funcTy = FunctionType::get(fTy->getInput(), fTy->getResult(),
                                   fTy->getExtInfo().withRepresentation(rep));
      return { funcTy, ImportHint::Block };
    }

    ImportResult VisitReferenceType(const clang::ReferenceType *type) {
      return { nullptr, ImportHint::Reference };
    }

    ImportResult VisitMemberPointer(const clang::MemberPointerType *type) {
      // FIXME: Member function pointers can be mapped to curried functions,
      // but only when we can express the notion of a function that does
      // not capture anything from its enclosing context.
      return Type();
    }

    ImportResult VisitArrayType(const clang::ArrayType *type) {
      // FIXME: Array types will need to be mapped differently depending on
      // context.
      return Type();
    }
    
    ImportResult VisitConstantArrayType(const clang::ConstantArrayType *type) {
      // FIXME: In a function argument context, arrays should import as
      // pointers.
      
      // FIXME: Map to a real fixed-size Swift array type when we have those.
      // Importing as a tuple at least fills the right amount of space, and
      // we can cheese static-offset "indexing" using .$n operations.
      
      Type elementType = Impl.importType(type->getElementType(),
                                         ImportTypeKind::Pointee,
                                         IsUsedInSystemModule);
      if (!elementType)
        return Type();
      
      TupleTypeElt elt(elementType);
      SmallVector<TupleTypeElt, 8> elts;
      for (size_t i = 0, size = type->getSize().getZExtValue(); i < size; ++i)
        elts.push_back(elt);
      
      return TupleType::get(elts, elementType->getASTContext());
    }

    ImportResult VisitVectorType(const clang::VectorType *type) {
      // FIXME: We could map these.
      return Type();
    }

    ImportResult VisitExtVectorType(const clang::ExtVectorType *type) {
      // FIXME: We could map these.
      return Type();
    }

    ImportResult VisitFunctionProtoType(const clang::FunctionProtoType *type) {
      // C-style variadic functions cannot be called from Swift.
      if (type->isVariadic())
        return Type();

      // Import the result type.  We currently provide no mechanism
      // for this to be audited.
      auto resultTy = Impl.importType(type->getReturnType(),
                                      ImportTypeKind::Result,
                                      IsUsedInSystemModule);
      if (!resultTy)
        return Type();

      SmallVector<TupleTypeElt, 4> params;
      for (auto param = type->param_type_begin(),
             paramEnd = type->param_type_end();
           param != paramEnd; ++param) {
        auto swiftParamTy = Impl.importType(*param, ImportTypeKind::Parameter,
                                            IsUsedInSystemModule);
        if (!swiftParamTy)
          return Type();

        // FIXME: If we were walking TypeLocs, we could actually get parameter
        // names. The probably doesn't matter outside of a FuncDecl, which
        // we'll have to special-case, but it's an interesting bit of data loss.
        params.push_back(swiftParamTy);
      }

      // Form the parameter tuple.
      auto paramsTy = TupleType::get(params, Impl.SwiftContext);

      // Form the function type.
      return FunctionType::get(paramsTy, resultTy);
    }

    ImportResult
    VisitFunctionNoProtoType(const clang::FunctionNoProtoType *type) {
      // Import functions without prototypes as functions with no parameters.
      auto resultTy = Impl.importType(type->getReturnType(),
                                      ImportTypeKind::Result,
                                      IsUsedInSystemModule);
      if (!resultTy)
        return Type();

      return FunctionType::get(TupleType::getEmpty(Impl.SwiftContext),resultTy);
    }

    ImportResult VisitParenType(const clang::ParenType *type) {
      auto inner = Visit(type->getInnerType());
      if (!inner)
        return Type();

      return { ParenType::get(Impl.SwiftContext, inner.AbstractType),
               inner.Hint };
    }

    ImportResult VisitTypedefType(const clang::TypedefType *type) {
      // Import the underlying declaration.
      auto decl = dyn_cast_or_null<TypeDecl>(Impl.importDecl(type->getDecl()));

      // If that fails, fall back on importing the underlying type.
      if (!decl) return Visit(type->desugar());

      Type mappedType = decl->getDeclaredType();
      ImportHint hint = ImportHint::None;

      // For certain special typedefs, we don't want to use the imported type.
      if (auto specialKind = Impl.getSpecialTypedefKind(type->getDecl())) {
        switch (specialKind.getValue()) {
        case MappedTypeNameKind::DoNothing:
        case MappedTypeNameKind::DefineAndUse:
          break;
        case MappedTypeNameKind::DefineOnly:
          mappedType = cast<TypeAliasDecl>(decl)->getUnderlyingType();
          break;
        }

        if (type->getDecl()->getName() == "BOOL") {
          hint = ImportHint::BOOL;
        } else if (type->getDecl()->getName() == "NSUInteger") {
          hint = ImportHint::NSUInteger;
        } else if (isImportedCFPointer(type->desugar(), mappedType)) {
          hint = ImportHint::CFPointer;
        } else if (mappedType->isAnyExistentialType()) { // id, Class
          hint = ImportHint::ObjCPointer;
        }
        // Any other interesting mapped types should be hinted here.

      // Otherwise, recurse on the underlying type in order to compute
      // the hint correctly.
      } else {
        auto underlyingResult = Visit(type->desugar());
        assert(underlyingResult.AbstractType->isEqual(mappedType) &&
               "typedef without special typedef kind was mapped "
               "differently from its underlying type?");
        hint = underlyingResult.Hint;
      }

      return { mappedType, hint };
    }

#define SUGAR_TYPE(KIND)                                            \
    ImportResult Visit##KIND##Type(const clang::KIND##Type *type) { \
      return Visit(type->desugar());                                \
    }
    SUGAR_TYPE(TypeOfExpr)
    SUGAR_TYPE(TypeOf)
    SUGAR_TYPE(Decltype)
    SUGAR_TYPE(UnaryTransform)
    SUGAR_TYPE(Elaborated)
    SUGAR_TYPE(SubstTemplateTypeParm)
    SUGAR_TYPE(TemplateSpecialization)
    SUGAR_TYPE(Auto)
    SUGAR_TYPE(Adjusted)
    SUGAR_TYPE(PackExpansion)

    ImportResult VisitAttributedType(const clang::AttributedType *type) {
      // If the type has a nullability specifier, translate that into an
      // optional type.
      if (auto nullability = type->getImmediateNullability()) {
        clang::ASTContext &clangCtx = Impl.getClangASTContext();

        // Strip off additional levels of nullability so we don't get
        // extra nested optionals.
        clang::QualType underlyingType(type, 0);
        do {
          if (auto attributed
                = dyn_cast<clang::AttributedType>(underlyingType.getTypePtr()))
            underlyingType = attributed->getModifiedType();
          else
            underlyingType = underlyingType.getDesugaredType(clangCtx);
        } while (underlyingType->getNullability(clangCtx));
          underlyingType = clangCtx.getCanonicalType(underlyingType);

        ImportResult underlying = Visit(underlyingType);
        if (canImportAsOptional(underlying.Hint)) {
          return getOptionalType(underlying.AbstractType,
                                 ImportTypeKind::Abstract,
                                 Impl.translateNullability(*nullability));
        }

        return underlying;
      }

      return Visit(type->desugar());
    }

    ImportResult VisitDecayedType(const clang::DecayedType *type) {
      clang::ASTContext &clangCtx = Impl.getClangASTContext();
      if (clangCtx.hasSameType(type->getOriginalType(),
                               clangCtx.getBuiltinVaListType()))
        return Impl.getNamedSwiftType(Impl.getStdlibModule(), "CVaListPointer");
      return Visit(type->desugar());
    }

    ImportResult VisitRecordType(const clang::RecordType *type) {
      auto decl = dyn_cast_or_null<TypeDecl>(Impl.importDecl(type->getDecl()));
      if (!decl)
        return nullptr;

      return decl->getDeclaredType();
    }

    ImportResult VisitEnumType(const clang::EnumType *type) {
      auto clangDecl = type->getDecl();
      switch (Impl.classifyEnum(clangDecl)) {
      case ClangImporter::Implementation::EnumKind::Constants: {
        auto clangDef = clangDecl->getDefinition();
        // Map anonymous enums with no fixed underlying type to Int /if/
        // they fit in an Int32. If not, this mapping isn't guaranteed to be
        // consistent for all platforms we care about.
        if (!clangDef->isFixed() &&
            clangDef->getNumPositiveBits() < 32 &&
            clangDef->getNumNegativeBits() <= 32)
          return Impl.getNamedSwiftType(Impl.getStdlibModule(), "Int");

        // Import the underlying integer type.
        return Visit(clangDecl->getIntegerType());
      }
      case ClangImporter::Implementation::EnumKind::Enum:
      case ClangImporter::Implementation::EnumKind::Unknown:
      case ClangImporter::Implementation::EnumKind::Options: {
        auto decl = dyn_cast_or_null<TypeDecl>(Impl.importDecl(clangDecl));
        if (!decl)
          return nullptr;

        return decl->getDeclaredType();
      }
      }
    }

    ImportResult VisitObjCObjectType(const clang::ObjCObjectType *type) {
      // If this is id<P> , turn this into a protocol type.
      // FIXME: What about Class<P>?
      if (type->isObjCQualifiedId()) {
        SmallVector<Type, 4> protocols;
        for (auto cp = type->qual_begin(), cpEnd = type->qual_end();
             cp != cpEnd; ++cp) {
          auto proto = cast_or_null<ProtocolDecl>(Impl.importDecl(*cp));
          if (!proto)
            return Type();

          protocols.push_back(proto->getDeclaredType());
        }

        return { ProtocolCompositionType::get(Impl.SwiftContext, protocols),
                 ImportHint::ObjCPointer };
      }

      // FIXME: Swift cannot express qualified object pointer types, e.g.,
      // NSObject<Proto>, so we drop the <Proto> part.
      return Visit(type->getBaseType());
    }

    ImportResult VisitObjCInterfaceType(const clang::ObjCInterfaceType *type) {
      auto imported = cast_or_null<ClassDecl>(Impl.importDecl(type->getDecl()));
      if (!imported)
        return nullptr;

      Type importedType = imported->getDeclaredType();

      if (imported->hasName() &&
          imported->getName().str() == "NSString") {
        return { importedType, ImportHint::NSString };
      }

      if (imported->hasName() && imported->getName().str() == "NSArray") {
        return { importedType, ImportHint::NSArray };
      }

      if (imported->hasName() && imported->getName().str() == "NSDictionary") {
        return { importedType, ImportHint::NSDictionary };
      }

      return { importedType, ImportHint::ObjCPointer };
    }

    ImportResult
    VisitObjCObjectPointerType(const clang::ObjCObjectPointerType *type) {
      // If this object pointer refers to an Objective-C class (possibly
      // qualified),
      if (auto interface = type->getInterfaceType()) {
        // FIXME: Swift cannot express qualified object pointer types, e.g.,
        // NSObject<Proto>, so we drop the <Proto> part.
        return VisitObjCInterfaceType(interface);
      }

      // If this is id<P>, turn this into a protocol type.
      // FIXME: What about Class<P>?
      if (type->isObjCQualifiedIdType()) {
        SmallVector<Type, 4> protocols;
        for (auto cp = type->qual_begin(), cpEnd = type->qual_end();
             cp != cpEnd; ++cp) {
          auto proto = cast_or_null<ProtocolDecl>(Impl.importDecl(*cp));
          if (!proto)
            return Type();

          protocols.push_back(proto->getDeclaredType());
        }

        return { ProtocolCompositionType::get(Impl.SwiftContext, protocols),
                 ImportHint::ObjCPointer };
      }
      
      // Beyond here, we're using AnyObject.
      auto proto = Impl.SwiftContext.getProtocol(
                     KnownProtocolKind::AnyObject);
      if (!proto)
        return Type();

      // id maps to AnyObject.
      if (type->isObjCIdType()) {
        return { proto->getDeclaredType(), ImportHint::ObjCPointer };
      }

      // Class maps to AnyObject.Type.
      assert(type->isObjCClassType() || type->isObjCQualifiedClassType());
      return { ExistentialMetatypeType::get(proto->getDeclaredType()),
               ImportHint::ObjCPointer };
    }
  };
}

/// True if we're converting a function parameter, property type, or
/// function result type, and can thus safely apply representation
/// conversions for bridged types.
static bool canBridgeTypes(ImportTypeKind importKind) {
  switch (importKind) {
  case ImportTypeKind::Abstract:
  case ImportTypeKind::Value:
  case ImportTypeKind::Variable:
  case ImportTypeKind::AuditedVariable:
  case ImportTypeKind::Pointee:
  case ImportTypeKind::Enum:
    return false;
  case ImportTypeKind::Result:
  case ImportTypeKind::AuditedResult:
  case ImportTypeKind::Parameter:
  case ImportTypeKind::Property:
  case ImportTypeKind::PropertyAccessor:
    return true;
  }
}

/// True if the type has known CoreFoundation reference counting semantics.
static bool isCFAudited(ImportTypeKind importKind) {
  switch (importKind) {
  case ImportTypeKind::Abstract:
  case ImportTypeKind::Value:
  case ImportTypeKind::Variable:
  case ImportTypeKind::Result:
  case ImportTypeKind::Pointee:
  case ImportTypeKind::Enum:
    return false;
  case ImportTypeKind::AuditedVariable:
  case ImportTypeKind::AuditedResult:
  case ImportTypeKind::Parameter:
  case ImportTypeKind::Property:
  case ImportTypeKind::PropertyAccessor:
    return true;
  }
}

/// Turn T into Unmanaged<T>.
static Type getUnmanagedType(ClangImporter::Implementation &impl,
                             Type payloadType) {
  Module *stdlib = impl.getStdlibModule();
  if (!stdlib) return payloadType;
  Type unmanagedType = impl.getNamedSwiftType(stdlib, "Unmanaged");
  if (!unmanagedType) return payloadType;
  auto unboundTy = unmanagedType->getAs<UnboundGenericType>();
  if (!unboundTy || unboundTy->getDecl()->getGenericParams()->size() != 1)
    return payloadType;

  Type unmanagedClassType =
    BoundGenericType::get(unboundTy->getDecl(), /*parent*/ Type(),
                          payloadType);
  return unmanagedClassType;
}

static Type adjustTypeForConcreteImport(ClangImporter::Implementation &impl,
                                        clang::QualType clangType,
                                        Type importedType,
                                        ImportTypeKind importKind,
                                        ImportHint hint,
                                        bool isUsedInSystemModule,
                                        OptionalTypeKind optKind) {
  if (importKind == ImportTypeKind::Abstract) {
    return importedType;
  }

  // 'void' can only be imported as a function result type.
  if (hint == ImportHint::Void &&
      (importKind == ImportTypeKind::AuditedResult ||
       importKind == ImportTypeKind::Result ||
       importKind == ImportTypeKind::PropertyAccessor)) {
    return impl.getNamedSwiftType(impl.getStdlibModule(), "Void");
  }

  // Reference types are only permitted as function parameter types.
  if (hint == ImportHint::Reference &&
      importKind == ImportTypeKind::Parameter) {
    auto refType = clangType->castAs<clang::ReferenceType>();
    // Import the underlying type.
    auto objectType = impl.importType(refType->getPointeeType(),
                                      ImportTypeKind::Pointee,
                                      isUsedInSystemModule);
    if (!objectType)
      return nullptr;

    return InOutType::get(objectType);
  }

  // For anything else, if we completely failed to import the type
  // abstractly, give up now.
  if (!importedType)
    return Type();

  // Special case AutoreleasingUnsafeMutablePointer<NSError?> parameters.
  auto maybeImportNSErrorPointer = [&]() -> Type {
    if (importKind != ImportTypeKind::Parameter)
      return Type();

    PointerTypeKind PTK;
    auto elementType = importedType->getAnyPointerElementType(PTK);
    if (!elementType || PTK != PTK_AutoreleasingUnsafeMutablePointer)
      return Type();

    auto elementObj = elementType->getAnyOptionalObjectType();
    if (!elementObj)
      return Type();

    auto elementClass = elementObj->getClassOrBoundGenericClass();
    if (!elementClass)
      return Type();

    // FIXME: Avoid string comparison by caching this identifier.
    if (elementClass->getName().str() != "NSError")
      return Type();

    Module *foundationModule = impl.tryLoadFoundationModule();
    if (!foundationModule ||
        foundationModule->Name != elementClass->getModuleContext()->Name)
      return Type();

    return impl.getNamedSwiftType(foundationModule, "NSErrorPointer");
  };
  if (Type result = maybeImportNSErrorPointer())
    return result;

  // Turn block pointer types back into normal function types in any
  // context where bridging is possible, unless the block has a typedef.
  if (hint == ImportHint::Block && canBridgeTypes(importKind) &&
      !isa<NameAliasType>(importedType.getPointer())) {
    auto fTy = importedType->castTo<FunctionType>();
    FunctionType::ExtInfo einfo =
      fTy->getExtInfo().withRepresentation(FunctionType::Representation::Thick);
    importedType = FunctionType::get(fTy->getInput(), fTy->getResult(), einfo);
  }

  // Turn BOOL into Bool in contexts that can bridge types.
  if (hint == ImportHint::BOOL && canBridgeTypes(importKind)) {
    return impl.getNamedSwiftType(impl.getStdlibModule(), "Bool");
  }

  // When NSUInteger is used as an enum's underlying type or if it does not come
  // from a system module, make sure it stays unsigned.
  if (hint == ImportHint::NSUInteger)
     if (importKind == ImportTypeKind::Enum || !isUsedInSystemModule) {
    return impl.getNamedSwiftType(impl.getStdlibModule(), "UInt");
  }

  // Wrap CF pointers up as unmanaged types, unless this is an audited
  // context.
  if (hint == ImportHint::CFPointer && !isCFAudited(importKind)) {
    importedType = getUnmanagedType(impl, importedType);
  }

  // When NSString* is the type of a function parameter or a function
  // result type, map it to String.
  // FIXME: It's not really safe to do this when Foundation is missing.
  // We do it anyway for ImportForwardDeclarations mode so that generated
  // interfaces are correct, but trying to use the resulting declarations
  // may result in compiler crashes further down the line.
  if (hint == ImportHint::NSString && canBridgeTypes(importKind) &&
      (impl.tryLoadFoundationModule() || impl.ImportForwardDeclarations)) {
    importedType = impl.getNamedSwiftType(impl.getStdlibModule(), "String");
  }


  // When NSArray* is the type of a function parameter or a function
  // result type, map it to [AnyObject].
  if (hint == ImportHint::NSArray && canBridgeTypes(importKind) &&
      impl.tryLoadFoundationModule()) {
    Type anyObject = impl.getNamedSwiftType(impl.getStdlibModule(), 
                                            "AnyObject");
    importedType = ArraySliceType::get(anyObject);
  }

  // When NSDictionary* is the type of a function parameter or a function
  // result type, map it to [NSObject : AnyObject].
  if (hint == ImportHint::NSDictionary && canBridgeTypes(importKind) &&
      impl.tryLoadFoundationModule()) {
    importedType = DictionaryType::get(
                     impl.getNSObjectType(),
                     impl.getNamedSwiftType(impl.getStdlibModule(),
                                            "AnyObject"));
  }

  // Wrap class, class protocol, function, and metatype types in an
  // optional type.
  if (canImportAsOptional(hint) &&
      !clangType->getNullability(impl.getClangASTContext())) {
    importedType = getOptionalType(importedType, importKind, optKind);
  }

  return importedType;
}

Type ClangImporter::Implementation::importType(clang::QualType type,
                                               ImportTypeKind importKind,
                                               bool isUsedInSystemModule,
                                               OptionalTypeKind optionality) {
  if (type.isNull())
    return Type();

  // The "built-in" Objective-C types id, Class, and SEL can actually be (and
  // are) defined within the library. Clang tracks the redefinition types
  // separately, so it can provide fallbacks in certain cases. For Swift, we
  // map the redefinition types back to the equivalent of the built-in types.
  // This bans some trickery that the redefinition types enable, but is a more
  // sane model overall.
  auto &clangContext = getClangASTContext();
  if (clangContext.getLangOpts().ObjC1) {
    if (clangContext.hasSameUnqualifiedType(
          type, clangContext.getObjCIdRedefinitionType()) &&
        !clangContext.hasSameUnqualifiedType(
           clangContext.getObjCIdType(),
           clangContext.getObjCIdRedefinitionType()))
      type = clangContext.getObjCIdType();
    else if (clangContext.hasSameUnqualifiedType(
                type, clangContext.getObjCClassRedefinitionType()) &&
             !clangContext.hasSameUnqualifiedType(
                clangContext.getObjCClassType(),
                clangContext.getObjCClassRedefinitionType()))
      type = clangContext.getObjCClassType();
    else if (clangContext.hasSameUnqualifiedType(
               type, clangContext.getObjCSelRedefinitionType()) &&
             !clangContext.hasSameUnqualifiedType(
                clangContext.getObjCSelType(),
                clangContext.getObjCSelRedefinitionType()))
      type = clangContext.getObjCSelType();
  }
  
  // Perform abstract conversion, ignoring how the type is actually used.
  SwiftTypeConverter converter(*this, isUsedInSystemModule);
  auto importResult = converter.Visit(type);

  // Now fix up the type based on we're concretely using it.
  return adjustTypeForConcreteImport(*this, type, importResult.AbstractType,
                                     importKind, importResult.Hint,
                                     isUsedInSystemModule,
                                     optionality);
}

Type ClangImporter::Implementation::importPropertyType(
       const clang::ObjCPropertyDecl *decl,
       bool isFromSystemModule) {
  OptionalTypeKind optionality = OTK_ImplicitlyUnwrappedOptional;
  if (auto info = getKnownObjCProperty(decl)) {
    if (auto nullability = info->getNullability())
      optionality = translateNullability(*nullability);
  }
  return importType(decl->getType(), ImportTypeKind::Property,
                    isFromSystemModule, optionality);
}

/// Get a bit vector indicating which arguments are non-null for a
/// given function or method.
static llvm::SmallBitVector getNonNullArgs(
                              const clang::Decl *decl,
                              ArrayRef<const clang::ParmVarDecl *> params) {
  llvm::SmallBitVector result;
  if (!decl)
    return result;

  for (const auto *nonnull : decl->specific_attrs<clang::NonNullAttr>()) {
    if (!nonnull->args_size()) {
      // Easy case: all pointer arguments are non-null.
      if (result.empty())
        result.resize(params.size(), true);
      else
        result.set(0, params.size());

      return result;
    }

    // Mark each of the listed parameters as non-null.
    if (result.empty())
      result.resize(params.size(), false);

    for (unsigned idx : nonnull->args()) {
      if (idx < result.size())
        result.set(idx);
    }
  }

  return result;
}

Type ClangImporter::Implementation::importFunctionType(
       const clang::FunctionDecl *clangDecl,
       clang::QualType resultType,
       ArrayRef<const clang::ParmVarDecl *> params,
       bool isVariadic, bool isNoReturn,
       bool isFromSystemModule,
       SmallVectorImpl<Pattern*> &bodyPatterns) {

  // Cannot import variadic types.
  if (isVariadic)
    return Type();

  // CF function results can be managed if they are audited or
  // the ownership convention is explicitly declared.
  bool isAuditedResult =
    (clangDecl &&
     (clangDecl->hasAttr<clang::CFAuditedTransferAttr>() ||
      clangDecl->hasAttr<clang::CFReturnsRetainedAttr>() ||
      clangDecl->hasAttr<clang::CFReturnsNotRetainedAttr>()));

  // Check if we know more about the type from our whitelists.
  Optional<api_notes::GlobalFunctionInfo> knownFn;
  if (auto knownFnTmp = getKnownGlobalFunction(clangDecl))
    if (knownFnTmp->NullabilityAudited)
      knownFn = knownFnTmp;

  OptionalTypeKind OptionalityOfReturn;
  if (clangDecl->hasAttr<clang::ReturnsNonNullAttr>()) {
    OptionalityOfReturn = OTK_None;
  } else if (knownFn) {
    OptionalityOfReturn = translateNullability(knownFn->getReturnTypeInfo());
  } else {
    OptionalityOfReturn = OTK_ImplicitlyUnwrappedOptional;
  }

  // Import the result type.
  auto swiftResultTy = importType(resultType,
                                  (isAuditedResult
                                    ? ImportTypeKind::AuditedResult
                                    : ImportTypeKind::Result),
                                  isFromSystemModule,
                                  OptionalityOfReturn);
  if (!swiftResultTy)
    return Type();

  // Import the parameters.
  SmallVector<TupleTypeElt, 4> swiftArgParams;
  SmallVector<TupleTypeElt, 4> swiftBodyParams;
  SmallVector<TuplePatternElt, 4> argPatternElts;
  SmallVector<TuplePatternElt, 4> bodyPatternElts;
  unsigned index = 0;
  llvm::SmallBitVector nonNullArgs = getNonNullArgs(clangDecl, params);
  for (auto param : params) {
    auto paramTy = param->getType();
    if (paramTy->isVoidType()) {
      ++index;
      continue;
    }

    // Check nullability of the parameter.
    OptionalTypeKind OptionalityOfParam;
    if (!nonNullArgs.empty() && nonNullArgs[index]) {
      OptionalityOfParam = OTK_None;
    } else if (param->hasAttr<clang::NonNullAttr>()) {
      OptionalityOfParam = OTK_None;
    } else if (knownFn) {
      OptionalityOfParam = translateNullability(
                             knownFn->getParamTypeInfo(index));
    } else {
      OptionalityOfParam = OTK_ImplicitlyUnwrappedOptional;
    }

    // Import the parameter type into Swift.
    Type swiftParamTy = importType(paramTy, ImportTypeKind::Parameter,
                                   isFromSystemModule, OptionalityOfParam);
    if (!swiftParamTy)
      return Type();

    // Figure out the name for this parameter.
    Identifier bodyName = importName(param->getDeclName());

    // Note: C functions never have argument names.
    Identifier name;

    // Compute the pattern to put into the body.
    Pattern *bodyPattern;
    // It doesn't actually matter which DeclContext we use, so just use the
    // imported header unit.
    auto bodyVar
      = createDeclWithClangNode<ParamDecl>(param,
                                     /*IsLet*/ true,
                                     SourceLoc(), name,
                                     importSourceLoc(param->getLocation()),
                                     bodyName, swiftParamTy, 
                                     ImportedHeaderUnit);
    bodyPattern = new (SwiftContext) NamedPattern(bodyVar);
    bodyPattern->setType(swiftParamTy);
    bodyPattern
      = new (SwiftContext) TypedPattern(bodyPattern,
                                        TypeLoc::withoutLoc(swiftParamTy));
    bodyPattern->setType(swiftParamTy);
    bodyPatternElts.push_back(TuplePatternElt(bodyPattern));
    
    // Add the tuple elements for the function types.
    swiftArgParams.push_back(TupleTypeElt(swiftParamTy, name));
    swiftBodyParams.push_back(TupleTypeElt(swiftParamTy, bodyName));
    ++index;
  }

  // Form the parameter tuples.
  auto bodyParamsTy = TupleType::get(swiftBodyParams, SwiftContext);

  // Form the body patterns.
  bodyPatterns.push_back(TuplePattern::create(SwiftContext, SourceLoc(),
                                              bodyPatternElts, SourceLoc()));
  bodyPatterns.back()->setType(bodyParamsTy);  
  
  FunctionType::ExtInfo extInfo;
  extInfo = extInfo.withIsNoReturn(isNoReturn);
  
  // Form the function type.
  auto argTy = TupleType::get(swiftArgParams, SwiftContext);
  return FunctionType::get(argTy, swiftResultTy, extInfo);
}

static bool isObjCMethodResultAudited(const clang::Decl *decl) {
  if (!decl)
    return false;
  return (decl->hasAttr<clang::CFReturnsRetainedAttr>() ||
          decl->hasAttr<clang::CFReturnsNotRetainedAttr>() ||
          decl->hasAttr<clang::ObjCReturnsInnerPointerAttr>());
}

Type ClangImporter::Implementation::importMethodType(
       const clang::Decl *clangDecl,
       clang::QualType resultType,
       ArrayRef<const clang::ParmVarDecl *> params,
       bool isVariadic, bool isNoReturn,
       bool isFromSystemModule,
       SmallVectorImpl<Pattern*> &bodyPatterns,
       DeclName methodName,
       SpecialMethodKind kind) {

  // Cannot import variadic types.
  if (isVariadic)
    return Type();

  // Clang doesn't provide pragmas for auditing the CF behavior of
  // ObjC methods, but it does have attributes for declaring
  // return-type management:
  //   - cf_returns_retained and cf_returns_not_retained are obvious
  //   - objc_returns_inner_pointer is sometimes used on methods
  //     returning CF types as a workaround for ARC not managing CF
  //     objects
  ImportTypeKind resultKind;
  if (kind == SpecialMethodKind::PropertyAccessor)
    resultKind = ImportTypeKind::PropertyAccessor;
  else if (isObjCMethodResultAudited(clangDecl))
    resultKind = ImportTypeKind::AuditedResult;
  else
    resultKind = ImportTypeKind::Result;

  // Check if we know more about the type from our whitelists.
  Optional<api_notes::ObjCMethodInfo> knownMethod;
  if (auto MD = dyn_cast<clang::ObjCMethodDecl>(clangDecl)) {
    if (auto knownMethodTmp = getKnownObjCMethod(MD)) {
      if (knownMethodTmp->NullabilityAudited)
        knownMethod = knownMethodTmp;
    }
  }

  OptionalTypeKind OptionalityOfReturn;
  if (clangDecl->hasAttr<clang::ReturnsNonNullAttr>()) {
    OptionalityOfReturn = OTK_None;
  } else if (knownMethod) {
    OptionalityOfReturn = translateNullability(
                            knownMethod->getReturnTypeInfo());
  } else {
    OptionalityOfReturn = OTK_ImplicitlyUnwrappedOptional;
  }

  // Import the result type.
  auto swiftResultTy = importType(resultType, resultKind,
                                  isFromSystemModule, OptionalityOfReturn);
  if (!swiftResultTy)
    return Type();

  // Import the parameters.
  SmallVector<TupleTypeElt, 4> swiftArgParams;
  SmallVector<TupleTypeElt, 4> swiftBodyParams;
  SmallVector<TuplePatternElt, 4> bodyPatternElts;
  auto argNames = methodName.getArgumentNames();
  llvm::SmallBitVector nonNullArgs = getNonNullArgs(clangDecl, params);
  unsigned index = 0;
  for (auto param : params) {
    auto paramTy = param->getType();
    if (paramTy->isVoidType()) {
      ++index;
      continue;
    }

    // Import the parameter type into Swift.
    OptionalTypeKind optionalityOfParam = OTK_ImplicitlyUnwrappedOptional;
    if (!nonNullArgs.empty() && nonNullArgs[index]) {
      optionalityOfParam = OTK_None;
    } else if (param->hasAttr<clang::NonNullAttr>()) {
      optionalityOfParam = OTK_None;
    } else if (knownMethod) {
      optionalityOfParam =
        translateNullability(knownMethod->getParamTypeInfo(index));
    }

    Type swiftParamTy;
    if (kind == SpecialMethodKind::NSDictionarySubscriptGetter &&
        paramTy->isObjCIdType()) {
      swiftParamTy = getOptionalType(getNSCopyingType(),
                                     ImportTypeKind::Parameter,
                                     optionalityOfParam);
    } else if (kind == SpecialMethodKind::PropertyAccessor) {
      swiftParamTy = importType(paramTy,
                                ImportTypeKind::PropertyAccessor,
                                isFromSystemModule,
                                optionalityOfParam);
    } else {
      swiftParamTy = importType(paramTy,
                                ImportTypeKind::Parameter,
                                isFromSystemModule,
                                optionalityOfParam);
    }
    if (!swiftParamTy)
      return Type();

    // Figure out the name for this parameter.
    Identifier bodyName = importName(param->getDeclName());

    // Figure out the name for this argument, which comes from the method name.
    Identifier name;
    if (index < argNames.size()) {
      name = argNames[index];
    }

    // Compute the pattern to put into the body.
    Pattern *bodyPattern;
    // It doesn't actually matter which DeclContext we use, so just use the
    // imported header unit.
    auto bodyVar
      = createDeclWithClangNode<ParamDecl>(param,
                                     /*IsLet*/ true, SourceLoc(), name,
                                     importSourceLoc(param->getLocation()),
                                     bodyName, swiftParamTy, 
                                     ImportedHeaderUnit);
    bodyPattern = new (SwiftContext) NamedPattern(bodyVar);
    bodyPattern->setType(swiftParamTy);
    bodyPattern
      = new (SwiftContext) TypedPattern(bodyPattern,
                                        TypeLoc::withoutLoc(swiftParamTy));
    bodyPattern->setType(swiftParamTy);
    bodyPatternElts.push_back(TuplePatternElt(bodyPattern));

    // Add the tuple elements for the function types.
    swiftArgParams.push_back(TupleTypeElt(swiftParamTy, name));
    swiftBodyParams.push_back(TupleTypeElt(swiftParamTy, bodyName));
    ++index;
  }

  // If we have a constructor with no parameters and a name with an
  // argument name, synthesize a Void parameter with that name.
  if (kind == SpecialMethodKind::Constructor && params.empty() && 
      argNames.size() == 1) {
    // It doesn't actually matter which DeclContext we use, so just use the
    // imported header unit.
    auto argName = argNames[0];
    auto type = TupleType::getEmpty(SwiftContext);
    auto var = new (SwiftContext) ParamDecl(/*IsLet*/ true,
                                            SourceLoc(), argName,
                                            SourceLoc(), argName, type,
                                            ImportedHeaderUnit);
    Pattern *pattern = new (SwiftContext) NamedPattern(var);
    pattern->setType(type);
    pattern = new (SwiftContext) TypedPattern(pattern,
                                              TypeLoc::withoutLoc(type));
    pattern->setType(type);
    
    bodyPatternElts.push_back(TuplePatternElt(pattern));
    swiftArgParams.push_back(TupleTypeElt(type, argName));
    swiftBodyParams.push_back(TupleTypeElt(type, argName));
  }

  // Form the parameter tuple.
  auto bodyParamsTy = TupleType::get(swiftBodyParams, SwiftContext);
  
  // Form the body patterns.
  bodyPatterns.push_back(TuplePattern::create(SwiftContext, SourceLoc(),
                                              bodyPatternElts, SourceLoc()));
  bodyPatterns.back()->setType(bodyParamsTy);
  
  FunctionType::ExtInfo extInfo;
  extInfo = extInfo.withIsNoReturn(isNoReturn);
  
  // Form the function type.
  auto argTy = TupleType::get(swiftArgParams, SwiftContext);
  return FunctionType::get(argTy, swiftResultTy, extInfo);
}

Module *ClangImporter::Implementation::getStdlibModule() {
  return SwiftContext.getStdlibModule(true);
}

Module *ClangImporter::Implementation::getNamedModule(StringRef name) {
  return SwiftContext.getLoadedModule(SwiftContext.getIdentifier(name));
}

Module *ClangImporter::Implementation::tryLoadFoundationModule() {
  if (!checkedFoundationModule.hasValue()) {
    Identifier name = SwiftContext.getIdentifier(FOUNDATION_MODULE_NAME);

    // If we're synthesizing forward declarations, we don't want to pull in
    // Foundation too eagerly.
    if (ImportForwardDeclarations)
      checkedFoundationModule = SwiftContext.getLoadedModule(name);
    else
      checkedFoundationModule = SwiftContext.getModule({ {name, SourceLoc()} });
  }

  return checkedFoundationModule.getValue();
}

Type ClangImporter::Implementation::getNamedSwiftType(Module *module,
                                                      StringRef name) {
  if (!module)
    return Type();

  // Look for the type.
  SmallVector<ValueDecl *, 2> results;
  module->lookupValue({ }, SwiftContext.getIdentifier(name),
                      NLKind::UnqualifiedLookup, results);
  if (results.size() == 1) {
    if (auto type = dyn_cast<TypeDecl>(results.front())) {
      if (typeResolver)
        typeResolver->resolveDeclSignature(type);
      return type->getDeclaredType();
    }
  }

  return Type();
}

Type
ClangImporter::Implementation::
getNamedSwiftTypeSpecialization(Module *module, StringRef name,
                                ArrayRef<Type> args) {
  if (!module)
    return Type();

  // Look for the type.
  SmallVector<ValueDecl *, 2> results;
  module->lookupValue({ }, SwiftContext.getIdentifier(name),
                      NLKind::UnqualifiedLookup, results);
  if (results.size() == 1) {
    if (auto nominalDecl = dyn_cast<NominalTypeDecl>(results.front())) {
      if (typeResolver)
        typeResolver->resolveDeclSignature(nominalDecl);
      if (auto params = nominalDecl->getGenericParams()) {
        if (params->size() == args.size()) {
          // When we form the bound generic type, make sure we get the
          // substitutions.
          auto *BGT = BoundGenericType::get(nominalDecl, Type(), args);
          return BGT;
        }
      }
    }
  }

  return Type();
}

Decl *ClangImporter::Implementation::importDeclByName(StringRef name) {
  auto &sema = Instance->getSema();

  // Map the name. If we can't represent the Swift name in Clang, bail out now.
  auto clangName = &getClangASTContext().Idents.get(name);

  // Perform name lookup into the global scope.
  // FIXME: Map source locations over.
  clang::LookupResult lookupResult(sema, clangName, clang::SourceLocation(),
                                   clang::Sema::LookupOrdinaryName);
  if (!sema.LookupName(lookupResult, /*Scope=*/0)) {
    return nullptr;
  }

  for (auto decl : lookupResult) {
    if (auto swiftDecl = importDecl(decl->getUnderlyingDecl())) {
      return swiftDecl;
    }
  }

  return nullptr;
}

Type ClangImporter::Implementation::getNSObjectType() {
  if (NSObjectTy)
    return NSObjectTy;

  if (auto decl = dyn_cast_or_null<ClassDecl>(importDeclByName("NSObject"))) {
    NSObjectTy = decl->getDeclaredType();
    return NSObjectTy;
  }

  return Type();
}

Type ClangImporter::Implementation::getNSCopyingType() {
  auto &sema = Instance->getSema();
  auto clangName = &getClangASTContext().Idents.get("NSCopying");
  assert(clangName);

  // Perform name lookup into the global scope.
  clang::LookupResult lookupResult(sema, clangName, clang::SourceLocation(),
                                   clang::Sema::LookupObjCProtocolName);
  if (!sema.LookupName(lookupResult, /*Scope=*/0))
    return Type();

  for (auto decl : lookupResult) {
    if (auto swiftDecl = importDecl(decl->getUnderlyingDecl())) {
      if (auto protoDecl = dyn_cast<ProtocolDecl>(swiftDecl)) {
        return protoDecl->getDeclaredType();
      }
    }
  }

  return Type();
}

Type ClangImporter::Implementation::getCFStringRefType() {
  if (auto decl = dyn_cast_or_null<TypeDecl>(importDeclByName("CFStringRef")))
    return decl->getDeclaredType();
  return Type();
}

