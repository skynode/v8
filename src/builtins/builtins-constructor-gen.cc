// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/builtins/builtins-constructor-gen.h"

#include "src/ast/ast.h"
#include "src/builtins/builtins-constructor.h"
#include "src/builtins/builtins-utils-gen.h"
#include "src/builtins/builtins.h"
#include "src/code-factory.h"
#include "src/code-stub-assembler.h"
#include "src/counters.h"
#include "src/interface-descriptors.h"
#include "src/objects-inl.h"

namespace v8 {
namespace internal {

typedef compiler::Node Node;

Node* ConstructorBuiltinsAssembler::EmitFastNewClosure(Node* shared_info,
                                                       Node* feedback_vector,
                                                       Node* slot,
                                                       Node* context) {
  Isolate* isolate = this->isolate();
  Factory* factory = isolate->factory();
  IncrementCounter(isolate->counters()->fast_new_closure_total(), 1);

  // Create a new closure from the given function info in new space
  Node* result = Allocate(JSFunction::kSize);

  // Calculate the index of the map we should install on the function based on
  // the FunctionKind and LanguageMode of the function.
  // Note: Must be kept in sync with Context::FunctionMapIndex
  Node* compiler_hints =
      LoadObjectField(shared_info, SharedFunctionInfo::kCompilerHintsOffset,
                      MachineType::Uint32());
  Node* is_strict = Word32And(
      compiler_hints, Int32Constant(1 << SharedFunctionInfo::kStrictModeBit));

  Label if_normal(this), if_generator(this), if_async(this),
      if_class_constructor(this), if_function_without_prototype(this),
      load_map(this);
  VARIABLE(map_index, MachineType::PointerRepresentation());

  STATIC_ASSERT(FunctionKind::kNormalFunction == 0);
  Node* is_not_normal =
      Word32And(compiler_hints,
                Int32Constant(SharedFunctionInfo::kAllFunctionKindBitsMask));
  GotoIfNot(is_not_normal, &if_normal);

  Node* is_generator = Word32And(
      compiler_hints, Int32Constant(FunctionKind::kGeneratorFunction
                                    << SharedFunctionInfo::kFunctionKindShift));
  GotoIf(is_generator, &if_generator);

  Node* is_async = Word32And(
      compiler_hints, Int32Constant(FunctionKind::kAsyncFunction
                                    << SharedFunctionInfo::kFunctionKindShift));
  GotoIf(is_async, &if_async);

  Node* is_class_constructor = Word32And(
      compiler_hints, Int32Constant(FunctionKind::kClassConstructor
                                    << SharedFunctionInfo::kFunctionKindShift));
  GotoIf(is_class_constructor, &if_class_constructor);

  if (FLAG_debug_code) {
    // Function must be a function without a prototype.
    CSA_ASSERT(
        this,
        Word32And(compiler_hints,
                  Int32Constant((FunctionKind::kAccessorFunction |
                                 FunctionKind::kArrowFunction |
                                 FunctionKind::kConciseMethod)
                                << SharedFunctionInfo::kFunctionKindShift)));
  }
  Goto(&if_function_without_prototype);

  BIND(&if_normal);
  {
    map_index.Bind(SelectIntPtrConstant(is_strict,
                                        Context::STRICT_FUNCTION_MAP_INDEX,
                                        Context::SLOPPY_FUNCTION_MAP_INDEX));
    Goto(&load_map);
  }

  BIND(&if_generator);
  {
    Node* is_async =
        Word32And(compiler_hints,
                  Int32Constant(FunctionKind::kAsyncFunction
                                << SharedFunctionInfo::kFunctionKindShift));
    map_index.Bind(SelectIntPtrConstant(
        is_async, Context::ASYNC_GENERATOR_FUNCTION_MAP_INDEX,
        Context::GENERATOR_FUNCTION_MAP_INDEX));
    Goto(&load_map);
  }

  BIND(&if_async);
  {
    map_index.Bind(IntPtrConstant(Context::ASYNC_FUNCTION_MAP_INDEX));
    Goto(&load_map);
  }

  BIND(&if_class_constructor);
  {
    map_index.Bind(IntPtrConstant(Context::CLASS_FUNCTION_MAP_INDEX));
    Goto(&load_map);
  }

  BIND(&if_function_without_prototype);
  {
    map_index.Bind(
        IntPtrConstant(Context::STRICT_FUNCTION_WITHOUT_PROTOTYPE_MAP_INDEX));
    Goto(&load_map);
  }

  BIND(&load_map);

  // Get the function map in the current native context and set that
  // as the map of the allocated object.
  Node* native_context = LoadNativeContext(context);
  Node* map_slot_value =
      LoadFixedArrayElement(native_context, map_index.value());
  StoreMapNoWriteBarrier(result, map_slot_value);

  // Initialize the rest of the function.
  Node* empty_fixed_array = HeapConstant(factory->empty_fixed_array());
  StoreObjectFieldNoWriteBarrier(result, JSObject::kPropertiesOffset,
                                 empty_fixed_array);
  StoreObjectFieldNoWriteBarrier(result, JSObject::kElementsOffset,
                                 empty_fixed_array);
  Node* literals_cell = LoadFixedArrayElement(
      feedback_vector, slot, 0, CodeStubAssembler::SMI_PARAMETERS);
  {
    // Bump the closure counter encoded in the cell's map.
    Node* cell_map = LoadMap(literals_cell);
    Label no_closures(this), one_closure(this), cell_done(this);

    GotoIf(IsNoClosuresCellMap(cell_map), &no_closures);
    GotoIf(IsOneClosureCellMap(cell_map), &one_closure);
    CSA_ASSERT(this, IsManyClosuresCellMap(cell_map));
    Goto(&cell_done);

    BIND(&no_closures);
    StoreMapNoWriteBarrier(literals_cell, Heap::kOneClosureCellMapRootIndex);
    Goto(&cell_done);

    BIND(&one_closure);
    StoreMapNoWriteBarrier(literals_cell, Heap::kManyClosuresCellMapRootIndex);
    Goto(&cell_done);

    BIND(&cell_done);
  }
  StoreObjectFieldNoWriteBarrier(result, JSFunction::kFeedbackVectorOffset,
                                 literals_cell);
  StoreObjectFieldNoWriteBarrier(
      result, JSFunction::kPrototypeOrInitialMapOffset, TheHoleConstant());
  StoreObjectFieldNoWriteBarrier(result, JSFunction::kSharedFunctionInfoOffset,
                                 shared_info);
  StoreObjectFieldNoWriteBarrier(result, JSFunction::kContextOffset, context);
  Handle<Code> lazy_builtin_handle(
      isolate->builtins()->builtin(Builtins::kCompileLazy));
  Node* lazy_builtin = HeapConstant(lazy_builtin_handle);
  Node* lazy_builtin_entry =
      IntPtrAdd(BitcastTaggedToWord(lazy_builtin),
                IntPtrConstant(Code::kHeaderSize - kHeapObjectTag));
  StoreObjectFieldNoWriteBarrier(result, JSFunction::kCodeEntryOffset,
                                 lazy_builtin_entry,
                                 MachineType::PointerRepresentation());
  StoreObjectFieldNoWriteBarrier(result, JSFunction::kNextFunctionLinkOffset,
                                 UndefinedConstant());

  return result;
}

TF_BUILTIN(FastNewClosure, ConstructorBuiltinsAssembler) {
  Node* shared = Parameter(FastNewClosureDescriptor::kSharedFunctionInfo);
  Node* context = Parameter(FastNewClosureDescriptor::kContext);
  Node* vector = Parameter(FastNewClosureDescriptor::kVector);
  Node* slot = Parameter(FastNewClosureDescriptor::kSlot);
  Return(EmitFastNewClosure(shared, vector, slot, context));
}

TF_BUILTIN(FastNewObject, ConstructorBuiltinsAssembler) {
  Node* context = Parameter(Descriptor::kContext);
  Node* target = Parameter(Descriptor::kTarget);
  Node* new_target = Parameter(Descriptor::kNewTarget);

  Label call_runtime(this);

  Node* result = EmitFastNewObject(context, target, new_target, &call_runtime);
  Return(result);

  BIND(&call_runtime);
  TailCallRuntime(Runtime::kNewObject, context, target, new_target);
}

Node* ConstructorBuiltinsAssembler::EmitFastNewObject(Node* context,
                                                      Node* target,
                                                      Node* new_target) {
  VARIABLE(var_obj, MachineRepresentation::kTagged);
  Label call_runtime(this), end(this);

  Node* result = EmitFastNewObject(context, target, new_target, &call_runtime);
  var_obj.Bind(result);
  Goto(&end);

  BIND(&call_runtime);
  var_obj.Bind(CallRuntime(Runtime::kNewObject, context, target, new_target));
  Goto(&end);

  BIND(&end);
  return var_obj.value();
}

Node* ConstructorBuiltinsAssembler::EmitFastNewObject(Node* context,
                                                      Node* target,
                                                      Node* new_target,
                                                      Label* call_runtime) {
  CSA_ASSERT(this, HasInstanceType(target, JS_FUNCTION_TYPE));
  CSA_ASSERT(this, IsJSReceiver(new_target));

  // Verify that the new target is a JSFunction.
  Label fast(this), end(this);
  GotoIf(HasInstanceType(new_target, JS_FUNCTION_TYPE), &fast);
  Goto(call_runtime);

  BIND(&fast);

  // Load the initial map and verify that it's in fact a map.
  Node* initial_map =
      LoadObjectField(new_target, JSFunction::kPrototypeOrInitialMapOffset);
  GotoIf(TaggedIsSmi(initial_map), call_runtime);
  GotoIf(DoesntHaveInstanceType(initial_map, MAP_TYPE), call_runtime);

  // Fall back to runtime if the target differs from the new target's
  // initial map constructor.
  Node* new_target_constructor =
      LoadObjectField(initial_map, Map::kConstructorOrBackPointerOffset);
  GotoIf(WordNotEqual(target, new_target_constructor), call_runtime);

  VARIABLE(properties, MachineRepresentation::kTagged);

  Label instantiate_map(this), allocate_properties(this);
  GotoIf(IsDictionaryMap(initial_map), &allocate_properties);
  {
    properties.Bind(EmptyFixedArrayConstant());
    Goto(&instantiate_map);
  }
  BIND(&allocate_properties);
  {
    properties.Bind(AllocateNameDictionary(NameDictionary::kInitialCapacity));
    Goto(&instantiate_map);
  }

  BIND(&instantiate_map);

  Node* object = AllocateJSObjectFromMap(initial_map, properties.value());

  // Perform in-object slack tracking if requested.
  HandleSlackTracking(context, object, initial_map, JSObject::kHeaderSize);
  return object;
}

Node* ConstructorBuiltinsAssembler::EmitFastNewFunctionContext(
    Node* function, Node* slots, Node* context, ScopeType scope_type) {
  slots = ChangeUint32ToWord(slots);

  // TODO(ishell): Use CSA::OptimalParameterMode() here.
  ParameterMode mode = INTPTR_PARAMETERS;
  Node* min_context_slots = IntPtrConstant(Context::MIN_CONTEXT_SLOTS);
  Node* length = IntPtrAdd(slots, min_context_slots);
  Node* size = GetFixedArrayAllocationSize(length, FAST_ELEMENTS, mode);

  // Create a new closure from the given function info in new space
  Node* function_context = AllocateInNewSpace(size);

  Heap::RootListIndex context_type;
  switch (scope_type) {
    case EVAL_SCOPE:
      context_type = Heap::kEvalContextMapRootIndex;
      break;
    case FUNCTION_SCOPE:
      context_type = Heap::kFunctionContextMapRootIndex;
      break;
    default:
      UNREACHABLE();
  }
  StoreMapNoWriteBarrier(function_context, context_type);
  StoreObjectFieldNoWriteBarrier(function_context, Context::kLengthOffset,
                                 SmiTag(length));

  // Set up the fixed slots.
  StoreFixedArrayElement(function_context, Context::CLOSURE_INDEX, function,
                         SKIP_WRITE_BARRIER);
  StoreFixedArrayElement(function_context, Context::PREVIOUS_INDEX, context,
                         SKIP_WRITE_BARRIER);
  StoreFixedArrayElement(function_context, Context::EXTENSION_INDEX,
                         TheHoleConstant(), SKIP_WRITE_BARRIER);

  // Copy the native context from the previous context.
  Node* native_context = LoadNativeContext(context);
  StoreFixedArrayElement(function_context, Context::NATIVE_CONTEXT_INDEX,
                         native_context, SKIP_WRITE_BARRIER);

  // Initialize the rest of the slots to undefined.
  Node* undefined = UndefinedConstant();
  BuildFastFixedArrayForEach(
      function_context, FAST_ELEMENTS, min_context_slots, length,
      [this, undefined](Node* context, Node* offset) {
        StoreNoWriteBarrier(MachineRepresentation::kTagged, context, offset,
                            undefined);
      },
      mode);

  return function_context;
}

TF_BUILTIN(FastNewFunctionContextEval, ConstructorBuiltinsAssembler) {
  Node* function = Parameter(FastNewFunctionContextDescriptor::kFunction);
  Node* slots = Parameter(FastNewFunctionContextDescriptor::kSlots);
  Node* context = Parameter(FastNewFunctionContextDescriptor::kContext);
  Return(EmitFastNewFunctionContext(function, slots, context,
                                    ScopeType::EVAL_SCOPE));
}

TF_BUILTIN(FastNewFunctionContextFunction, ConstructorBuiltinsAssembler) {
  Node* function = Parameter(FastNewFunctionContextDescriptor::kFunction);
  Node* slots = Parameter(FastNewFunctionContextDescriptor::kSlots);
  Node* context = Parameter(FastNewFunctionContextDescriptor::kContext);
  Return(EmitFastNewFunctionContext(function, slots, context,
                                    ScopeType::FUNCTION_SCOPE));
}

Node* ConstructorBuiltinsAssembler::EmitFastCloneRegExp(Node* closure,
                                                        Node* literal_index,
                                                        Node* pattern,
                                                        Node* flags,
                                                        Node* context) {
  Label call_runtime(this, Label::kDeferred), end(this);

  VARIABLE(result, MachineRepresentation::kTagged);

  Node* cell = LoadObjectField(closure, JSFunction::kFeedbackVectorOffset);
  Node* feedback_vector = LoadObjectField(cell, Cell::kValueOffset);
  Node* boilerplate = LoadFixedArrayElement(feedback_vector, literal_index, 0,
                                            CodeStubAssembler::SMI_PARAMETERS);
  GotoIf(IsUndefined(boilerplate), &call_runtime);

  {
    int size = JSRegExp::kSize + JSRegExp::kInObjectFieldCount * kPointerSize;
    Node* copy = Allocate(size);
    for (int offset = 0; offset < size; offset += kPointerSize) {
      Node* value = LoadObjectField(boilerplate, offset);
      StoreObjectFieldNoWriteBarrier(copy, offset, value);
    }
    result.Bind(copy);
    Goto(&end);
  }

  BIND(&call_runtime);
  {
    result.Bind(CallRuntime(Runtime::kCreateRegExpLiteral, context, closure,
                            literal_index, pattern, flags));
    Goto(&end);
  }

  BIND(&end);
  return result.value();
}

TF_BUILTIN(FastCloneRegExp, ConstructorBuiltinsAssembler) {
  Node* closure = Parameter(FastCloneRegExpDescriptor::kClosure);
  Node* literal_index = Parameter(FastCloneRegExpDescriptor::kLiteralIndex);
  Node* pattern = Parameter(FastCloneRegExpDescriptor::kPattern);
  Node* flags = Parameter(FastCloneRegExpDescriptor::kFlags);
  Node* context = Parameter(FastCloneRegExpDescriptor::kContext);

  Return(EmitFastCloneRegExp(closure, literal_index, pattern, flags, context));
}

Node* ConstructorBuiltinsAssembler::NonEmptyShallowClone(
    Node* boilerplate, Node* boilerplate_map, Node* boilerplate_elements,
    Node* allocation_site, Node* capacity, ElementsKind kind) {
  ParameterMode param_mode = OptimalParameterMode();

  Node* length = LoadJSArrayLength(boilerplate);
  capacity = TaggedToParameter(capacity, param_mode);

  Node *array, *elements;
  std::tie(array, elements) = AllocateUninitializedJSArrayWithElements(
      kind, boilerplate_map, length, allocation_site, capacity, param_mode);

  Comment("copy elements header");
  // Header consists of map and length.
  STATIC_ASSERT(FixedArrayBase::kHeaderSize == 2 * kPointerSize);
  StoreMap(elements, LoadMap(boilerplate_elements));
  {
    int offset = FixedArrayBase::kLengthOffset;
    StoreObjectFieldNoWriteBarrier(
        elements, offset, LoadObjectField(boilerplate_elements, offset));
  }

  length = TaggedToParameter(length, param_mode);

  Comment("copy boilerplate elements");
  CopyFixedArrayElements(kind, boilerplate_elements, elements, length,
                         SKIP_WRITE_BARRIER, param_mode);
  IncrementCounter(isolate()->counters()->inlined_copied_elements(), 1);

  return array;
}

Node* ConstructorBuiltinsAssembler::EmitFastCloneShallowArray(
    Node* closure, Node* literal_index, Node* context, Label* call_runtime,
    AllocationSiteMode allocation_site_mode) {
  Label zero_capacity(this), cow_elements(this), fast_elements(this),
      return_result(this);
  VARIABLE(result, MachineRepresentation::kTagged);

  Node* cell = LoadObjectField(closure, JSFunction::kFeedbackVectorOffset);
  Node* feedback_vector = LoadObjectField(cell, Cell::kValueOffset);
  Node* allocation_site = LoadFixedArrayElement(
      feedback_vector, literal_index, 0, CodeStubAssembler::SMI_PARAMETERS);

  GotoIf(IsUndefined(allocation_site), call_runtime);
  allocation_site = LoadFixedArrayElement(feedback_vector, literal_index, 0,
                                          CodeStubAssembler::SMI_PARAMETERS);

  Node* boilerplate =
      LoadObjectField(allocation_site, AllocationSite::kTransitionInfoOffset);
  Node* boilerplate_map = LoadMap(boilerplate);
  Node* boilerplate_elements = LoadElements(boilerplate);
  Node* capacity = LoadFixedArrayBaseLength(boilerplate_elements);
  allocation_site =
      allocation_site_mode == TRACK_ALLOCATION_SITE ? allocation_site : nullptr;

  Node* zero = SmiConstant(Smi::kZero);
  GotoIf(SmiEqual(capacity, zero), &zero_capacity);

  Node* elements_map = LoadMap(boilerplate_elements);
  GotoIf(IsFixedCOWArrayMap(elements_map), &cow_elements);

  GotoIf(IsFixedArrayMap(elements_map), &fast_elements);
  {
    Comment("fast double elements path");
    if (FLAG_debug_code) {
      Label correct_elements_map(this), abort(this, Label::kDeferred);
      Branch(IsFixedDoubleArrayMap(elements_map), &correct_elements_map,
             &abort);

      BIND(&abort);
      {
        Node* abort_id = SmiConstant(
            Smi::FromInt(BailoutReason::kExpectedFixedDoubleArrayMap));
        CallRuntime(Runtime::kAbort, context, abort_id);
        result.Bind(UndefinedConstant());
        Goto(&return_result);
      }
      BIND(&correct_elements_map);
    }

    Node* array =
        NonEmptyShallowClone(boilerplate, boilerplate_map, boilerplate_elements,
                             allocation_site, capacity, FAST_DOUBLE_ELEMENTS);
    result.Bind(array);
    Goto(&return_result);
  }

  BIND(&fast_elements);
  {
    Comment("fast elements path");
    Node* array =
        NonEmptyShallowClone(boilerplate, boilerplate_map, boilerplate_elements,
                             allocation_site, capacity, FAST_ELEMENTS);
    result.Bind(array);
    Goto(&return_result);
  }

  VARIABLE(length, MachineRepresentation::kTagged);
  VARIABLE(elements, MachineRepresentation::kTagged);
  Label allocate_without_elements(this);

  BIND(&cow_elements);
  {
    Comment("fixed cow path");
    length.Bind(LoadJSArrayLength(boilerplate));
    elements.Bind(boilerplate_elements);

    Goto(&allocate_without_elements);
  }

  BIND(&zero_capacity);
  {
    Comment("zero capacity path");
    length.Bind(zero);
    elements.Bind(LoadRoot(Heap::kEmptyFixedArrayRootIndex));

    Goto(&allocate_without_elements);
  }

  BIND(&allocate_without_elements);
  {
    Node* array = AllocateUninitializedJSArrayWithoutElements(
        FAST_ELEMENTS, boilerplate_map, length.value(), allocation_site);
    StoreObjectField(array, JSObject::kElementsOffset, elements.value());
    result.Bind(array);
    Goto(&return_result);
  }

  BIND(&return_result);
  return result.value();
}

void ConstructorBuiltinsAssembler::CreateFastCloneShallowArrayBuiltin(
    AllocationSiteMode allocation_site_mode) {
  Node* closure = Parameter(FastCloneShallowArrayDescriptor::kClosure);
  Node* literal_index =
      Parameter(FastCloneShallowArrayDescriptor::kLiteralIndex);
  Node* constant_elements =
      Parameter(FastCloneShallowArrayDescriptor::kConstantElements);
  Node* context = Parameter(FastCloneShallowArrayDescriptor::kContext);
  Label call_runtime(this, Label::kDeferred);
  Return(EmitFastCloneShallowArray(closure, literal_index, context,
                                   &call_runtime, allocation_site_mode));

  BIND(&call_runtime);
  {
    Comment("call runtime");
    Node* flags =
        SmiConstant(Smi::FromInt(ArrayLiteral::kShallowElements |
                                 (allocation_site_mode == TRACK_ALLOCATION_SITE
                                      ? 0
                                      : ArrayLiteral::kDisableMementos)));
    Return(CallRuntime(Runtime::kCreateArrayLiteral, context, closure,
                       literal_index, constant_elements, flags));
  }
}

TF_BUILTIN(FastCloneShallowArrayTrack, ConstructorBuiltinsAssembler) {
  CreateFastCloneShallowArrayBuiltin(TRACK_ALLOCATION_SITE);
}

TF_BUILTIN(FastCloneShallowArrayDontTrack, ConstructorBuiltinsAssembler) {
  CreateFastCloneShallowArrayBuiltin(DONT_TRACK_ALLOCATION_SITE);
}

Node* ConstructorBuiltinsAssembler::EmitFastCloneShallowObject(
    Label* call_runtime, Node* closure, Node* literals_index,
    Node* fast_properties_count) {
  Node* cell = LoadObjectField(closure, JSFunction::kFeedbackVectorOffset);
  Node* feedback_vector = LoadObjectField(cell, Cell::kValueOffset);
  Node* allocation_site = LoadFixedArrayElement(
      feedback_vector, literals_index, 0, CodeStubAssembler::SMI_PARAMETERS);
  GotoIf(IsUndefined(allocation_site), call_runtime);

  Node* boilerplate =
      LoadObjectField(allocation_site, AllocationSite::kTransitionInfoOffset);
  Node* boilerplate_map = LoadMap(boilerplate);
  Variable properties(this, MachineRepresentation::kTagged,
                      EmptyFixedArrayConstant());
  // TODO(cbruni): directly use the property count from the boilerplate map.
  Variable in_object_property_count(this, MachineType::PointerRepresentation(),
                                    fast_properties_count);
  // Directly copy over the property store for dict-mode boilerplates.
  Label dict_properties(this), allocate_object(this);
  Branch(IsDictionaryMap(boilerplate_map), &dict_properties, &allocate_object);
  Bind(&dict_properties);
  {
    properties.Bind(
        CopyNameDictionary(LoadProperties(boilerplate), call_runtime));
    in_object_property_count.Bind(IntPtrConstant(0));
    Goto(&allocate_object);
  }
  Bind(&allocate_object);

  // Calculate the object and allocation size based on the properties count.
  Node* object_size =
      IntPtrAdd(WordShl(in_object_property_count.value(), kPointerSizeLog2),
                IntPtrConstant(JSObject::kHeaderSize));
  Node* allocation_size = object_size;
  if (FLAG_allocation_site_pretenuring) {
    allocation_size =
        IntPtrAdd(object_size, IntPtrConstant(AllocationMemento::kSize));
  }

  Node* instance_size = LoadMapInstanceSize(boilerplate_map);
  Node* size_in_words = WordShr(object_size, kPointerSizeLog2);
  GotoIfNot(WordEqual(instance_size, size_in_words), call_runtime);

  Node* copy = AllocateInNewSpace(allocation_size);
  // Copy boilerplate elements.
  VARIABLE(offset, MachineType::PointerRepresentation());
  offset.Bind(IntPtrConstant(-kHeapObjectTag));
  Node* end_offset = IntPtrAdd(object_size, offset.value());
  Label loop_body(this, &offset), loop_check(this, &offset);
  // We should always have an object size greater than zero.
  Goto(&loop_body);
  BIND(&loop_body);
  {
    // The Allocate above guarantees that the copy lies in new space. This
    // allows us to skip write barriers. This is necessary since we may also be
    // copying unboxed doubles.
    Node* field = Load(MachineType::IntPtr(), boilerplate, offset.value());
    StoreNoWriteBarrier(MachineType::PointerRepresentation(), copy,
                        offset.value(), field);
    Goto(&loop_check);
  }
  BIND(&loop_check);
  {
    offset.Bind(IntPtrAdd(offset.value(), IntPtrConstant(kPointerSize)));
    GotoIfNot(IntPtrGreaterThanOrEqual(offset.value(), end_offset), &loop_body);
  }

  StoreObjectFieldNoWriteBarrier(copy, JSObject::kPropertiesOffset,
                                 properties.value());

  if (FLAG_allocation_site_pretenuring) {
    Node* memento = InnerAllocate(copy, object_size);
    StoreMapNoWriteBarrier(memento, Heap::kAllocationMementoMapRootIndex);
    StoreObjectFieldNoWriteBarrier(
        memento, AllocationMemento::kAllocationSiteOffset, allocation_site);
    Node* memento_create_count = LoadObjectField(
        allocation_site, AllocationSite::kPretenureCreateCountOffset);
    memento_create_count =
        SmiAdd(memento_create_count, SmiConstant(Smi::FromInt(1)));
    StoreObjectFieldNoWriteBarrier(allocation_site,
                                   AllocationSite::kPretenureCreateCountOffset,
                                   memento_create_count);
  }

  // TODO(verwaest): Allocate and fill in double boxes.
  return copy;
}

template <typename Descriptor>
void ConstructorBuiltinsAssembler::CreateFastCloneShallowObjectBuiltin(
    int properties_count) {
  DCHECK_GE(properties_count, 0);
  DCHECK_LE(properties_count,
            ConstructorBuiltins::kMaximumClonedShallowObjectProperties);
  Label call_runtime(this);
  Node* closure = Parameter(Descriptor::kClosure);
  Node* literals_index = Parameter(Descriptor::kLiteralIndex);

  Node* properties_count_node =
      IntPtrConstant(ConstructorBuiltins::FastCloneShallowObjectPropertiesCount(
          properties_count));
  Node* copy = EmitFastCloneShallowObject(
      &call_runtime, closure, literals_index, properties_count_node);
  Return(copy);

  BIND(&call_runtime);
  Node* constant_properties = Parameter(Descriptor::kConstantProperties);
  Node* flags = Parameter(Descriptor::kFlags);
  Node* context = Parameter(Descriptor::kContext);
  TailCallRuntime(Runtime::kCreateObjectLiteral, context, closure,
                  literals_index, constant_properties, flags);
}

#define SHALLOW_OBJECT_BUILTIN(props)                                       \
  TF_BUILTIN(FastCloneShallowObject##props, ConstructorBuiltinsAssembler) { \
    CreateFastCloneShallowObjectBuiltin<Descriptor>(props);                 \
  }

SHALLOW_OBJECT_BUILTIN(0);
SHALLOW_OBJECT_BUILTIN(1);
SHALLOW_OBJECT_BUILTIN(2);
SHALLOW_OBJECT_BUILTIN(3);
SHALLOW_OBJECT_BUILTIN(4);
SHALLOW_OBJECT_BUILTIN(5);
SHALLOW_OBJECT_BUILTIN(6);

}  // namespace internal
}  // namespace v8
