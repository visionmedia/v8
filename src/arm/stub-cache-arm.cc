// Copyright 2006-2009 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "v8.h"

#if defined(V8_TARGET_ARCH_ARM)

#include "ic-inl.h"
#include "codegen-inl.h"
#include "stub-cache.h"

namespace v8 {
namespace internal {

#define __ ACCESS_MASM(masm)


static void ProbeTable(MacroAssembler* masm,
                       Code::Flags flags,
                       StubCache::Table table,
                       Register name,
                       Register offset,
                       Register scratch,
                       Register scratch2) {
  ExternalReference key_offset(SCTableReference::keyReference(table));
  ExternalReference value_offset(SCTableReference::valueReference(table));

  uint32_t key_off_addr = reinterpret_cast<uint32_t>(key_offset.address());
  uint32_t value_off_addr = reinterpret_cast<uint32_t>(value_offset.address());

  // Check the relative positions of the address fields.
  ASSERT(value_off_addr > key_off_addr);
  ASSERT((value_off_addr - key_off_addr) % 4 == 0);
  ASSERT((value_off_addr - key_off_addr) < (256 * 4));

  Label miss;
  Register offsets_base_addr = scratch;

  // Check that the key in the entry matches the name.
  __ mov(offsets_base_addr, Operand(key_offset));
  __ ldr(ip, MemOperand(offsets_base_addr, offset, LSL, 1));
  __ cmp(name, ip);
  __ b(ne, &miss);

  // Get the code entry from the cache.
  __ add(offsets_base_addr, offsets_base_addr,
         Operand(value_off_addr - key_off_addr));
  __ ldr(scratch2, MemOperand(offsets_base_addr, offset, LSL, 1));

  // Check that the flags match what we're looking for.
  __ ldr(scratch2, FieldMemOperand(scratch2, Code::kFlagsOffset));
  __ bic(scratch2, scratch2, Operand(Code::kFlagsNotUsedInLookup));
  __ cmp(scratch2, Operand(flags));
  __ b(ne, &miss);

  // Re-load code entry from cache.
  __ ldr(offset, MemOperand(offsets_base_addr, offset, LSL, 1));

  // Jump to the first instruction in the code stub.
  __ add(offset, offset, Operand(Code::kHeaderSize - kHeapObjectTag));
  __ Jump(offset);

  // Miss: fall through.
  __ bind(&miss);
}


// Helper function used to check that the dictionary doesn't contain
// the property. This function may return false negatives, so miss_label
// must always call a backup property check that is complete.
// This function is safe to call if the receiver has fast properties.
// Name must be a symbol and receiver must be a heap object.
static void GenerateDictionaryNegativeLookup(MacroAssembler* masm,
                                             Label* miss_label,
                                             Register receiver,
                                             String* name,
                                             Register scratch0,
                                             Register scratch1) {
  ASSERT(name->IsSymbol());
  __ IncrementCounter(&Counters::negative_lookups, 1, scratch0, scratch1);
  __ IncrementCounter(&Counters::negative_lookups_miss, 1, scratch0, scratch1);

  Label done;

  const int kInterceptorOrAccessCheckNeededMask =
      (1 << Map::kHasNamedInterceptor) | (1 << Map::kIsAccessCheckNeeded);

  // Bail out if the receiver has a named interceptor or requires access checks.
  Register map = scratch1;
  __ ldr(map, FieldMemOperand(receiver, HeapObject::kMapOffset));
  __ ldrb(scratch0, FieldMemOperand(map, Map::kBitFieldOffset));
  __ tst(scratch0, Operand(kInterceptorOrAccessCheckNeededMask));
  __ b(ne, miss_label);

  // Check that receiver is a JSObject.
  __ ldrb(scratch0, FieldMemOperand(map, Map::kInstanceTypeOffset));
  __ cmp(scratch0, Operand(FIRST_JS_OBJECT_TYPE));
  __ b(lt, miss_label);

  // Load properties array.
  Register properties = scratch0;
  __ ldr(properties, FieldMemOperand(receiver, JSObject::kPropertiesOffset));
  // Check that the properties array is a dictionary.
  __ ldr(map, FieldMemOperand(properties, HeapObject::kMapOffset));
  Register tmp = properties;
  __ LoadRoot(tmp, Heap::kHashTableMapRootIndex);
  __ cmp(map, tmp);
  __ b(ne, miss_label);

  // Restore the temporarily used register.
  __ ldr(properties, FieldMemOperand(receiver, JSObject::kPropertiesOffset));

  // Compute the capacity mask.
  const int kCapacityOffset =
      StringDictionary::kHeaderSize +
      StringDictionary::kCapacityIndex * kPointerSize;

  // Generate an unrolled loop that performs a few probes before
  // giving up.
  static const int kProbes = 4;
  const int kElementsStartOffset =
      StringDictionary::kHeaderSize +
      StringDictionary::kElementsStartIndex * kPointerSize;

  // If names of slots in range from 1 to kProbes - 1 for the hash value are
  // not equal to the name and kProbes-th slot is not used (its name is the
  // undefined value), it guarantees the hash table doesn't contain the
  // property. It's true even if some slots represent deleted properties
  // (their names are the null value).
  for (int i = 0; i < kProbes; i++) {
    // scratch0 points to properties hash.
    // Compute the masked index: (hash + i + i * i) & mask.
    Register index = scratch1;
    // Capacity is smi 2^n.
    __ ldr(index, FieldMemOperand(properties, kCapacityOffset));
    __ sub(index, index, Operand(1));
    __ and_(index, index, Operand(
        Smi::FromInt(name->Hash() + StringDictionary::GetProbeOffset(i))));

    // Scale the index by multiplying by the entry size.
    ASSERT(StringDictionary::kEntrySize == 3);
    __ add(index, index, Operand(index, LSL, 1));  // index *= 3.

    Register entity_name = scratch1;
    // Having undefined at this place means the name is not contained.
    ASSERT_EQ(kSmiTagSize, 1);
    Register tmp = properties;
    __ add(tmp, properties, Operand(index, LSL, 1));
    __ ldr(entity_name, FieldMemOperand(tmp, kElementsStartOffset));

    ASSERT(!tmp.is(entity_name));
    __ LoadRoot(tmp, Heap::kUndefinedValueRootIndex);
    __ cmp(entity_name, tmp);
    if (i != kProbes - 1) {
      __ b(eq, &done);

      // Stop if found the property.
      __ cmp(entity_name, Operand(Handle<String>(name)));
      __ b(eq, miss_label);

      // Check if the entry name is not a symbol.
      __ ldr(entity_name, FieldMemOperand(entity_name, HeapObject::kMapOffset));
      __ ldrb(entity_name,
              FieldMemOperand(entity_name, Map::kInstanceTypeOffset));
      __ tst(entity_name, Operand(kIsSymbolMask));
      __ b(eq, miss_label);

      // Restore the properties.
      __ ldr(properties,
             FieldMemOperand(receiver, JSObject::kPropertiesOffset));
    } else {
      // Give up probing if still not found the undefined value.
      __ b(ne, miss_label);
    }
  }
  __ bind(&done);
  __ DecrementCounter(&Counters::negative_lookups_miss, 1, scratch0, scratch1);
}


void StubCache::GenerateProbe(MacroAssembler* masm,
                              Code::Flags flags,
                              Register receiver,
                              Register name,
                              Register scratch,
                              Register extra,
                              Register extra2) {
  Label miss;

  // Make sure that code is valid. The shifting code relies on the
  // entry size being 8.
  ASSERT(sizeof(Entry) == 8);

  // Make sure the flags does not name a specific type.
  ASSERT(Code::ExtractTypeFromFlags(flags) == 0);

  // Make sure that there are no register conflicts.
  ASSERT(!scratch.is(receiver));
  ASSERT(!scratch.is(name));
  ASSERT(!extra.is(receiver));
  ASSERT(!extra.is(name));
  ASSERT(!extra.is(scratch));
  ASSERT(!extra2.is(receiver));
  ASSERT(!extra2.is(name));
  ASSERT(!extra2.is(scratch));
  ASSERT(!extra2.is(extra));

  // Check scratch, extra and extra2 registers are valid.
  ASSERT(!scratch.is(no_reg));
  ASSERT(!extra.is(no_reg));
  ASSERT(!extra2.is(no_reg));

  // Check that the receiver isn't a smi.
  __ tst(receiver, Operand(kSmiTagMask));
  __ b(eq, &miss);

  // Get the map of the receiver and compute the hash.
  __ ldr(scratch, FieldMemOperand(name, String::kHashFieldOffset));
  __ ldr(ip, FieldMemOperand(receiver, HeapObject::kMapOffset));
  __ add(scratch, scratch, Operand(ip));
  __ eor(scratch, scratch, Operand(flags));
  __ and_(scratch,
          scratch,
          Operand((kPrimaryTableSize - 1) << kHeapObjectTagSize));

  // Probe the primary table.
  ProbeTable(masm, flags, kPrimary, name, scratch, extra, extra2);

  // Primary miss: Compute hash for secondary probe.
  __ sub(scratch, scratch, Operand(name));
  __ add(scratch, scratch, Operand(flags));
  __ and_(scratch,
          scratch,
          Operand((kSecondaryTableSize - 1) << kHeapObjectTagSize));

  // Probe the secondary table.
  ProbeTable(masm, flags, kSecondary, name, scratch, extra, extra2);

  // Cache miss: Fall-through and let caller handle the miss by
  // entering the runtime system.
  __ bind(&miss);
}


void StubCompiler::GenerateLoadGlobalFunctionPrototype(MacroAssembler* masm,
                                                       int index,
                                                       Register prototype) {
  // Load the global or builtins object from the current context.
  __ ldr(prototype, MemOperand(cp, Context::SlotOffset(Context::GLOBAL_INDEX)));
  // Load the global context from the global or builtins object.
  __ ldr(prototype,
         FieldMemOperand(prototype, GlobalObject::kGlobalContextOffset));
  // Load the function from the global context.
  __ ldr(prototype, MemOperand(prototype, Context::SlotOffset(index)));
  // Load the initial map.  The global functions all have initial maps.
  __ ldr(prototype,
         FieldMemOperand(prototype, JSFunction::kPrototypeOrInitialMapOffset));
  // Load the prototype from the initial map.
  __ ldr(prototype, FieldMemOperand(prototype, Map::kPrototypeOffset));
}


void StubCompiler::GenerateDirectLoadGlobalFunctionPrototype(
    MacroAssembler* masm, int index, Register prototype, Label* miss) {
  // Check we're still in the same context.
  __ ldr(prototype, MemOperand(cp, Context::SlotOffset(Context::GLOBAL_INDEX)));
  __ Move(ip, Top::global());
  __ cmp(prototype, ip);
  __ b(ne, miss);
  // Get the global function with the given index.
  JSFunction* function = JSFunction::cast(Top::global_context()->get(index));
  // Load its initial map. The global functions all have initial maps.
  __ Move(prototype, Handle<Map>(function->initial_map()));
  // Load the prototype from the initial map.
  __ ldr(prototype, FieldMemOperand(prototype, Map::kPrototypeOffset));
}


// Load a fast property out of a holder object (src). In-object properties
// are loaded directly otherwise the property is loaded from the properties
// fixed array.
void StubCompiler::GenerateFastPropertyLoad(MacroAssembler* masm,
                                            Register dst, Register src,
                                            JSObject* holder, int index) {
  // Adjust for the number of properties stored in the holder.
  index -= holder->map()->inobject_properties();
  if (index < 0) {
    // Get the property straight out of the holder.
    int offset = holder->map()->instance_size() + (index * kPointerSize);
    __ ldr(dst, FieldMemOperand(src, offset));
  } else {
    // Calculate the offset into the properties array.
    int offset = index * kPointerSize + FixedArray::kHeaderSize;
    __ ldr(dst, FieldMemOperand(src, JSObject::kPropertiesOffset));
    __ ldr(dst, FieldMemOperand(dst, offset));
  }
}


void StubCompiler::GenerateLoadArrayLength(MacroAssembler* masm,
                                           Register receiver,
                                           Register scratch,
                                           Label* miss_label) {
  // Check that the receiver isn't a smi.
  __ tst(receiver, Operand(kSmiTagMask));
  __ b(eq, miss_label);

  // Check that the object is a JS array.
  __ CompareObjectType(receiver, scratch, scratch, JS_ARRAY_TYPE);
  __ b(ne, miss_label);

  // Load length directly from the JS array.
  __ ldr(r0, FieldMemOperand(receiver, JSArray::kLengthOffset));
  __ Ret();
}


// Generate code to check if an object is a string.  If the object is a
// heap object, its map's instance type is left in the scratch1 register.
// If this is not needed, scratch1 and scratch2 may be the same register.
static void GenerateStringCheck(MacroAssembler* masm,
                                Register receiver,
                                Register scratch1,
                                Register scratch2,
                                Label* smi,
                                Label* non_string_object) {
  // Check that the receiver isn't a smi.
  __ tst(receiver, Operand(kSmiTagMask));
  __ b(eq, smi);

  // Check that the object is a string.
  __ ldr(scratch1, FieldMemOperand(receiver, HeapObject::kMapOffset));
  __ ldrb(scratch1, FieldMemOperand(scratch1, Map::kInstanceTypeOffset));
  __ and_(scratch2, scratch1, Operand(kIsNotStringMask));
  // The cast is to resolve the overload for the argument of 0x0.
  __ cmp(scratch2, Operand(static_cast<int32_t>(kStringTag)));
  __ b(ne, non_string_object);
}


// Generate code to load the length from a string object and return the length.
// If the receiver object is not a string or a wrapped string object the
// execution continues at the miss label. The register containing the
// receiver is potentially clobbered.
void StubCompiler::GenerateLoadStringLength(MacroAssembler* masm,
                                            Register receiver,
                                            Register scratch1,
                                            Register scratch2,
                                            Label* miss) {
  Label check_wrapper;

  // Check if the object is a string leaving the instance type in the
  // scratch1 register.
  GenerateStringCheck(masm, receiver, scratch1, scratch2, miss, &check_wrapper);

  // Load length directly from the string.
  __ ldr(r0, FieldMemOperand(receiver, String::kLengthOffset));
  __ Ret();

  // Check if the object is a JSValue wrapper.
  __ bind(&check_wrapper);
  __ cmp(scratch1, Operand(JS_VALUE_TYPE));
  __ b(ne, miss);

  // Unwrap the value and check if the wrapped value is a string.
  __ ldr(scratch1, FieldMemOperand(receiver, JSValue::kValueOffset));
  GenerateStringCheck(masm, scratch1, scratch2, scratch2, miss, miss);
  __ ldr(r0, FieldMemOperand(scratch1, String::kLengthOffset));
  __ Ret();
}


void StubCompiler::GenerateLoadFunctionPrototype(MacroAssembler* masm,
                                                 Register receiver,
                                                 Register scratch1,
                                                 Register scratch2,
                                                 Label* miss_label) {
  __ TryGetFunctionPrototype(receiver, scratch1, scratch2, miss_label);
  __ mov(r0, scratch1);
  __ Ret();
}


// Generate StoreField code, value is passed in r0 register.
// When leaving generated code after success, the receiver_reg and name_reg
// may be clobbered.  Upon branch to miss_label, the receiver and name
// registers have their original values.
void StubCompiler::GenerateStoreField(MacroAssembler* masm,
                                      JSObject* object,
                                      int index,
                                      Map* transition,
                                      Register receiver_reg,
                                      Register name_reg,
                                      Register scratch,
                                      Label* miss_label) {
  // r0 : value
  Label exit;

  // Check that the receiver isn't a smi.
  __ tst(receiver_reg, Operand(kSmiTagMask));
  __ b(eq, miss_label);

  // Check that the map of the receiver hasn't changed.
  __ ldr(scratch, FieldMemOperand(receiver_reg, HeapObject::kMapOffset));
  __ cmp(scratch, Operand(Handle<Map>(object->map())));
  __ b(ne, miss_label);

  // Perform global security token check if needed.
  if (object->IsJSGlobalProxy()) {
    __ CheckAccessGlobalProxy(receiver_reg, scratch, miss_label);
  }

  // Stub never generated for non-global objects that require access
  // checks.
  ASSERT(object->IsJSGlobalProxy() || !object->IsAccessCheckNeeded());

  // Perform map transition for the receiver if necessary.
  if ((transition != NULL) && (object->map()->unused_property_fields() == 0)) {
    // The properties must be extended before we can store the value.
    // We jump to a runtime call that extends the properties array.
    __ push(receiver_reg);
    __ mov(r2, Operand(Handle<Map>(transition)));
    __ Push(r2, r0);
    __ TailCallExternalReference(
           ExternalReference(IC_Utility(IC::kSharedStoreIC_ExtendStorage)),
           3, 1);
    return;
  }

  if (transition != NULL) {
    // Update the map of the object; no write barrier updating is
    // needed because the map is never in new space.
    __ mov(ip, Operand(Handle<Map>(transition)));
    __ str(ip, FieldMemOperand(receiver_reg, HeapObject::kMapOffset));
  }

  // Adjust for the number of properties stored in the object. Even in the
  // face of a transition we can use the old map here because the size of the
  // object and the number of in-object properties is not going to change.
  index -= object->map()->inobject_properties();

  if (index < 0) {
    // Set the property straight into the object.
    int offset = object->map()->instance_size() + (index * kPointerSize);
    __ str(r0, FieldMemOperand(receiver_reg, offset));

    // Skip updating write barrier if storing a smi.
    __ tst(r0, Operand(kSmiTagMask));
    __ b(eq, &exit);

    // Update the write barrier for the array address.
    // Pass the now unused name_reg as a scratch register.
    __ RecordWrite(receiver_reg, Operand(offset), name_reg, scratch);
  } else {
    // Write to the properties array.
    int offset = index * kPointerSize + FixedArray::kHeaderSize;
    // Get the properties array
    __ ldr(scratch, FieldMemOperand(receiver_reg, JSObject::kPropertiesOffset));
    __ str(r0, FieldMemOperand(scratch, offset));

    // Skip updating write barrier if storing a smi.
    __ tst(r0, Operand(kSmiTagMask));
    __ b(eq, &exit);

    // Update the write barrier for the array address.
    // Ok to clobber receiver_reg and name_reg, since we return.
    __ RecordWrite(scratch, Operand(offset), name_reg, receiver_reg);
  }

  // Return the value (register r0).
  __ bind(&exit);
  __ Ret();
}


void StubCompiler::GenerateLoadMiss(MacroAssembler* masm, Code::Kind kind) {
  ASSERT(kind == Code::LOAD_IC || kind == Code::KEYED_LOAD_IC);
  Code* code = NULL;
  if (kind == Code::LOAD_IC) {
    code = Builtins::builtin(Builtins::LoadIC_Miss);
  } else {
    code = Builtins::builtin(Builtins::KeyedLoadIC_Miss);
  }

  Handle<Code> ic(code);
  __ Jump(ic, RelocInfo::CODE_TARGET);
}


static void GenerateCallFunction(MacroAssembler* masm,
                                 Object* object,
                                 const ParameterCount& arguments,
                                 Label* miss) {
  // ----------- S t a t e -------------
  //  -- r0: receiver
  //  -- r1: function to call
  // -----------------------------------

  // Check that the function really is a function.
  __ BranchOnSmi(r1, miss);
  __ CompareObjectType(r1, r3, r3, JS_FUNCTION_TYPE);
  __ b(ne, miss);

  // Patch the receiver on the stack with the global proxy if
  // necessary.
  if (object->IsGlobalObject()) {
    __ ldr(r3, FieldMemOperand(r0, GlobalObject::kGlobalReceiverOffset));
    __ str(r3, MemOperand(sp, arguments.immediate() * kPointerSize));
  }

  // Invoke the function.
  __ InvokeFunction(r1, arguments, JUMP_FUNCTION);
}


static void PushInterceptorArguments(MacroAssembler* masm,
                                     Register receiver,
                                     Register holder,
                                     Register name,
                                     JSObject* holder_obj) {
  __ push(name);
  InterceptorInfo* interceptor = holder_obj->GetNamedInterceptor();
  ASSERT(!Heap::InNewSpace(interceptor));
  Register scratch = name;
  __ mov(scratch, Operand(Handle<Object>(interceptor)));
  __ push(scratch);
  __ push(receiver);
  __ push(holder);
  __ ldr(scratch, FieldMemOperand(scratch, InterceptorInfo::kDataOffset));
  __ push(scratch);
}


static void CompileCallLoadPropertyWithInterceptor(MacroAssembler* masm,
                                                   Register receiver,
                                                   Register holder,
                                                   Register name,
                                                   JSObject* holder_obj) {
  PushInterceptorArguments(masm, receiver, holder, name, holder_obj);

  ExternalReference ref =
      ExternalReference(IC_Utility(IC::kLoadPropertyWithInterceptorOnly));
  __ mov(r0, Operand(5));
  __ mov(r1, Operand(ref));

  CEntryStub stub(1);
  __ CallStub(&stub);
}


// Reserves space for the extra arguments to FastHandleApiCall in the
// caller's frame.
//
// These arguments are set by CheckPrototypes and GenerateFastApiCall.
static void ReserveSpaceForFastApiCall(MacroAssembler* masm,
                                       Register scratch) {
  __ mov(scratch, Operand(Smi::FromInt(0)));
  __ push(scratch);
  __ push(scratch);
  __ push(scratch);
  __ push(scratch);
}


// Undoes the effects of ReserveSpaceForFastApiCall.
static void FreeSpaceForFastApiCall(MacroAssembler* masm) {
  __ Drop(4);
}


// Generates call to FastHandleApiCall builtin.
static void GenerateFastApiCall(MacroAssembler* masm,
                                const CallOptimization& optimization,
                                int argc) {
  // Get the function and setup the context.
  JSFunction* function = optimization.constant_function();
  __ mov(r5, Operand(Handle<JSFunction>(function)));
  __ ldr(cp, FieldMemOperand(r5, JSFunction::kContextOffset));

  // Pass the additional arguments FastHandleApiCall expects.
  bool info_loaded = false;
  Object* callback = optimization.api_call_info()->callback();
  if (Heap::InNewSpace(callback)) {
    info_loaded = true;
    __ Move(r0, Handle<CallHandlerInfo>(optimization.api_call_info()));
    __ ldr(r7, FieldMemOperand(r0, CallHandlerInfo::kCallbackOffset));
  } else {
    __ Move(r7, Handle<Object>(callback));
  }
  Object* call_data = optimization.api_call_info()->data();
  if (Heap::InNewSpace(call_data)) {
    if (!info_loaded) {
      __ Move(r0, Handle<CallHandlerInfo>(optimization.api_call_info()));
    }
    __ ldr(r6, FieldMemOperand(r0, CallHandlerInfo::kDataOffset));
  } else {
    __ Move(r6, Handle<Object>(call_data));
  }

  __ add(sp, sp, Operand(1 * kPointerSize));
  __ stm(ia, sp, r5.bit() | r6.bit() | r7.bit());
  __ sub(sp, sp, Operand(1 * kPointerSize));

  // Set the number of arguments.
  __ mov(r0, Operand(argc + 4));

  // Jump to the fast api call builtin (tail call).
  Handle<Code> code = Handle<Code>(
      Builtins::builtin(Builtins::FastHandleApiCall));
  ParameterCount expected(0);
  __ InvokeCode(code, expected, expected,
                RelocInfo::CODE_TARGET, JUMP_FUNCTION);
}


class CallInterceptorCompiler BASE_EMBEDDED {
 public:
  CallInterceptorCompiler(StubCompiler* stub_compiler,
                          const ParameterCount& arguments,
                          Register name)
      : stub_compiler_(stub_compiler),
        arguments_(arguments),
        name_(name) {}

  void Compile(MacroAssembler* masm,
               JSObject* object,
               JSObject* holder,
               String* name,
               LookupResult* lookup,
               Register receiver,
               Register scratch1,
               Register scratch2,
               Register scratch3,
               Label* miss) {
    ASSERT(holder->HasNamedInterceptor());
    ASSERT(!holder->GetNamedInterceptor()->getter()->IsUndefined());

    // Check that the receiver isn't a smi.
    __ BranchOnSmi(receiver, miss);

    CallOptimization optimization(lookup);

    if (optimization.is_constant_call()) {
      CompileCacheable(masm,
                       object,
                       receiver,
                       scratch1,
                       scratch2,
                       scratch3,
                       holder,
                       lookup,
                       name,
                       optimization,
                       miss);
    } else {
      CompileRegular(masm,
                     object,
                     receiver,
                     scratch1,
                     scratch2,
                     scratch3,
                     name,
                     holder,
                     miss);
    }
  }

 private:
  void CompileCacheable(MacroAssembler* masm,
                       JSObject* object,
                       Register receiver,
                       Register scratch1,
                       Register scratch2,
                       Register scratch3,
                       JSObject* interceptor_holder,
                       LookupResult* lookup,
                       String* name,
                       const CallOptimization& optimization,
                       Label* miss_label) {
    ASSERT(optimization.is_constant_call());
    ASSERT(!lookup->holder()->IsGlobalObject());

    int depth1 = kInvalidProtoDepth;
    int depth2 = kInvalidProtoDepth;
    bool can_do_fast_api_call = false;
    if (optimization.is_simple_api_call() &&
       !lookup->holder()->IsGlobalObject()) {
     depth1 =
         optimization.GetPrototypeDepthOfExpectedType(object,
                                                      interceptor_holder);
     if (depth1 == kInvalidProtoDepth) {
       depth2 =
           optimization.GetPrototypeDepthOfExpectedType(interceptor_holder,
                                                        lookup->holder());
     }
     can_do_fast_api_call = (depth1 != kInvalidProtoDepth) ||
                            (depth2 != kInvalidProtoDepth);
    }

    __ IncrementCounter(&Counters::call_const_interceptor, 1,
                      scratch1, scratch2);

    if (can_do_fast_api_call) {
      __ IncrementCounter(&Counters::call_const_interceptor_fast_api, 1,
                          scratch1, scratch2);
      ReserveSpaceForFastApiCall(masm, scratch1);
    }

    // Check that the maps from receiver to interceptor's holder
    // haven't changed and thus we can invoke interceptor.
    Label miss_cleanup;
    Label* miss = can_do_fast_api_call ? &miss_cleanup : miss_label;
    Register holder =
        stub_compiler_->CheckPrototypes(object, receiver,
                                        interceptor_holder, scratch1,
                                        scratch2, scratch3, name, depth1, miss);

    // Invoke an interceptor and if it provides a value,
    // branch to |regular_invoke|.
    Label regular_invoke;
    LoadWithInterceptor(masm, receiver, holder, interceptor_holder, scratch2,
                        &regular_invoke);

    // Interceptor returned nothing for this property.  Try to use cached
    // constant function.

    // Check that the maps from interceptor's holder to constant function's
    // holder haven't changed and thus we can use cached constant function.
    if (interceptor_holder != lookup->holder()) {
      stub_compiler_->CheckPrototypes(interceptor_holder, receiver,
                                      lookup->holder(), scratch1,
                                      scratch2, scratch3, name, depth2, miss);
    } else {
      // CheckPrototypes has a side effect of fetching a 'holder'
      // for API (object which is instanceof for the signature).  It's
      // safe to omit it here, as if present, it should be fetched
      // by the previous CheckPrototypes.
      ASSERT(depth2 == kInvalidProtoDepth);
    }

    // Invoke function.
    if (can_do_fast_api_call) {
      GenerateFastApiCall(masm, optimization, arguments_.immediate());
    } else {
      __ InvokeFunction(optimization.constant_function(), arguments_,
                        JUMP_FUNCTION);
    }

    // Deferred code for fast API call case---clean preallocated space.
    if (can_do_fast_api_call) {
      __ bind(&miss_cleanup);
      FreeSpaceForFastApiCall(masm);
      __ b(miss_label);
    }

    // Invoke a regular function.
    __ bind(&regular_invoke);
    if (can_do_fast_api_call) {
      FreeSpaceForFastApiCall(masm);
    }
  }

  void CompileRegular(MacroAssembler* masm,
                      JSObject* object,
                      Register receiver,
                      Register scratch1,
                      Register scratch2,
                      Register scratch3,
                      String* name,
                      JSObject* interceptor_holder,
                      Label* miss_label) {
    Register holder =
        stub_compiler_->CheckPrototypes(object, receiver, interceptor_holder,
                                        scratch1, scratch2, scratch3, name,
                                        miss_label);

    // Call a runtime function to load the interceptor property.
    __ EnterInternalFrame();
    // Save the name_ register across the call.
    __ push(name_);

    PushInterceptorArguments(masm,
                             receiver,
                             holder,
                             name_,
                             interceptor_holder);

    __ CallExternalReference(
          ExternalReference(
              IC_Utility(IC::kLoadPropertyWithInterceptorForCall)),
          5);

    // Restore the name_ register.
    __ pop(name_);
    __ LeaveInternalFrame();
  }

  void LoadWithInterceptor(MacroAssembler* masm,
                           Register receiver,
                           Register holder,
                           JSObject* holder_obj,
                           Register scratch,
                           Label* interceptor_succeeded) {
    __ EnterInternalFrame();
    __ Push(holder, name_);

    CompileCallLoadPropertyWithInterceptor(masm,
                                           receiver,
                                           holder,
                                           name_,
                                           holder_obj);

    __ pop(name_);  // Restore the name.
    __ pop(receiver);  // Restore the holder.
    __ LeaveInternalFrame();

    // If interceptor returns no-result sentinel, call the constant function.
    __ LoadRoot(scratch, Heap::kNoInterceptorResultSentinelRootIndex);
    __ cmp(r0, scratch);
    __ b(ne, interceptor_succeeded);
  }

  StubCompiler* stub_compiler_;
  const ParameterCount& arguments_;
  Register name_;
};


// Generate code to check that a global property cell is empty. Create
// the property cell at compilation time if no cell exists for the
// property.
MUST_USE_RESULT static MaybeObject* GenerateCheckPropertyCell(
    MacroAssembler* masm,
    GlobalObject* global,
    String* name,
    Register scratch,
    Label* miss) {
  Object* probe;
  { MaybeObject* maybe_probe = global->EnsurePropertyCell(name);
    if (!maybe_probe->ToObject(&probe)) return maybe_probe;
  }
  JSGlobalPropertyCell* cell = JSGlobalPropertyCell::cast(probe);
  ASSERT(cell->value()->IsTheHole());
  __ mov(scratch, Operand(Handle<Object>(cell)));
  __ ldr(scratch,
         FieldMemOperand(scratch, JSGlobalPropertyCell::kValueOffset));
  __ LoadRoot(ip, Heap::kTheHoleValueRootIndex);
  __ cmp(scratch, ip);
  __ b(ne, miss);
  return cell;
}

// Calls GenerateCheckPropertyCell for each global object in the prototype chain
// from object to (but not including) holder.
MUST_USE_RESULT static MaybeObject* GenerateCheckPropertyCells(
    MacroAssembler* masm,
    JSObject* object,
    JSObject* holder,
    String* name,
    Register scratch,
    Label* miss) {
  JSObject* current = object;
  while (current != holder) {
    if (current->IsGlobalObject()) {
      // Returns a cell or a failure.
      MaybeObject* result = GenerateCheckPropertyCell(
          masm,
          GlobalObject::cast(current),
          name,
          scratch,
          miss);
      if (result->IsFailure()) return result;
    }
    ASSERT(current->IsJSObject());
    current = JSObject::cast(current->GetPrototype());
  }
  return NULL;
}



#undef __
#define __ ACCESS_MASM(masm())


Register StubCompiler::CheckPrototypes(JSObject* object,
                                       Register object_reg,
                                       JSObject* holder,
                                       Register holder_reg,
                                       Register scratch1,
                                       Register scratch2,
                                       String* name,
                                       int save_at_depth,
                                       Label* miss) {
  // Make sure there's no overlap between holder and object registers.
  ASSERT(!scratch1.is(object_reg) && !scratch1.is(holder_reg));
  ASSERT(!scratch2.is(object_reg) && !scratch2.is(holder_reg)
         && !scratch2.is(scratch1));

  // Keep track of the current object in register reg.
  Register reg = object_reg;
  int depth = 0;

  if (save_at_depth == depth) {
    __ str(reg, MemOperand(sp));
  }

  // Check the maps in the prototype chain.
  // Traverse the prototype chain from the object and do map checks.
  JSObject* current = object;
  while (current != holder) {
    depth++;

    // Only global objects and objects that do not require access
    // checks are allowed in stubs.
    ASSERT(current->IsJSGlobalProxy() || !current->IsAccessCheckNeeded());

    ASSERT(current->GetPrototype()->IsJSObject());
    JSObject* prototype = JSObject::cast(current->GetPrototype());
    if (!current->HasFastProperties() &&
        !current->IsJSGlobalObject() &&
        !current->IsJSGlobalProxy()) {
      if (!name->IsSymbol()) {
        MaybeObject* maybe_lookup_result = Heap::LookupSymbol(name);
        Object* lookup_result = NULL;  // Initialization to please compiler.
        if (!maybe_lookup_result->ToObject(&lookup_result)) {
          set_failure(Failure::cast(maybe_lookup_result));
          return reg;
        }
        name = String::cast(lookup_result);
      }
      ASSERT(current->property_dictionary()->FindEntry(name) ==
             StringDictionary::kNotFound);

      GenerateDictionaryNegativeLookup(masm(),
                                       miss,
                                       reg,
                                       name,
                                       scratch1,
                                       scratch2);
      __ ldr(scratch1, FieldMemOperand(reg, HeapObject::kMapOffset));
      reg = holder_reg;  // from now the object is in holder_reg
      __ ldr(reg, FieldMemOperand(scratch1, Map::kPrototypeOffset));
    } else if (Heap::InNewSpace(prototype)) {
      // Get the map of the current object.
      __ ldr(scratch1, FieldMemOperand(reg, HeapObject::kMapOffset));
      __ cmp(scratch1, Operand(Handle<Map>(current->map())));

      // Branch on the result of the map check.
      __ b(ne, miss);

      // Check access rights to the global object.  This has to happen
      // after the map check so that we know that the object is
      // actually a global object.
      if (current->IsJSGlobalProxy()) {
        __ CheckAccessGlobalProxy(reg, scratch1, miss);
        // Restore scratch register to be the map of the object.  In the
        // new space case below, we load the prototype from the map in
        // the scratch register.
        __ ldr(scratch1, FieldMemOperand(reg, HeapObject::kMapOffset));
      }

      reg = holder_reg;  // from now the object is in holder_reg
      // The prototype is in new space; we cannot store a reference
      // to it in the code. Load it from the map.
      __ ldr(reg, FieldMemOperand(scratch1, Map::kPrototypeOffset));
    } else {
      // Check the map of the current object.
      __ ldr(scratch1, FieldMemOperand(reg, HeapObject::kMapOffset));
      __ cmp(scratch1, Operand(Handle<Map>(current->map())));
      // Branch on the result of the map check.
      __ b(ne, miss);
      // Check access rights to the global object.  This has to happen
      // after the map check so that we know that the object is
      // actually a global object.
      if (current->IsJSGlobalProxy()) {
        __ CheckAccessGlobalProxy(reg, scratch1, miss);
      }
      // The prototype is in old space; load it directly.
      reg = holder_reg;  // from now the object is in holder_reg
      __ mov(reg, Operand(Handle<JSObject>(prototype)));
    }

    if (save_at_depth == depth) {
      __ str(reg, MemOperand(sp));
    }

    // Go to the next object in the prototype chain.
    current = prototype;
  }

  // Check the holder map.
  __ ldr(scratch1, FieldMemOperand(reg, HeapObject::kMapOffset));
  __ cmp(scratch1, Operand(Handle<Map>(current->map())));
  __ b(ne, miss);

  // Log the check depth.
  LOG(IntEvent("check-maps-depth", depth + 1));

  // Perform security check for access to the global object.
  ASSERT(holder->IsJSGlobalProxy() || !holder->IsAccessCheckNeeded());
  if (holder->IsJSGlobalProxy()) {
    __ CheckAccessGlobalProxy(reg, scratch1, miss);
  };

  // If we've skipped any global objects, it's not enough to verify
  // that their maps haven't changed.  We also need to check that the
  // property cell for the property is still empty.
  MaybeObject* result = GenerateCheckPropertyCells(masm(),
                                                   object,
                                                   holder,
                                                   name,
                                                   scratch1,
                                                   miss);
  if (result->IsFailure()) set_failure(Failure::cast(result));

  // Return the register containing the holder.
  return reg;
}


void StubCompiler::GenerateLoadField(JSObject* object,
                                     JSObject* holder,
                                     Register receiver,
                                     Register scratch1,
                                     Register scratch2,
                                     Register scratch3,
                                     int index,
                                     String* name,
                                     Label* miss) {
  // Check that the receiver isn't a smi.
  __ tst(receiver, Operand(kSmiTagMask));
  __ b(eq, miss);

  // Check that the maps haven't changed.
  Register reg =
      CheckPrototypes(object, receiver, holder, scratch1, scratch2, scratch3,
                      name, miss);
  GenerateFastPropertyLoad(masm(), r0, reg, holder, index);
  __ Ret();
}


void StubCompiler::GenerateLoadConstant(JSObject* object,
                                        JSObject* holder,
                                        Register receiver,
                                        Register scratch1,
                                        Register scratch2,
                                        Register scratch3,
                                        Object* value,
                                        String* name,
                                        Label* miss) {
  // Check that the receiver isn't a smi.
  __ tst(receiver, Operand(kSmiTagMask));
  __ b(eq, miss);

  // Check that the maps haven't changed.
  Register reg =
      CheckPrototypes(object, receiver, holder,
                      scratch1, scratch2, scratch3, name, miss);

  // Return the constant value.
  __ mov(r0, Operand(Handle<Object>(value)));
  __ Ret();
}


bool StubCompiler::GenerateLoadCallback(JSObject* object,
                                        JSObject* holder,
                                        Register receiver,
                                        Register name_reg,
                                        Register scratch1,
                                        Register scratch2,
                                        Register scratch3,
                                        AccessorInfo* callback,
                                        String* name,
                                        Label* miss,
                                        Failure** failure) {
  // Check that the receiver isn't a smi.
  __ tst(receiver, Operand(kSmiTagMask));
  __ b(eq, miss);

  // Check that the maps haven't changed.
  Register reg =
      CheckPrototypes(object, receiver, holder, scratch1, scratch2, scratch3,
                      name, miss);

  // Push the arguments on the JS stack of the caller.
  __ push(receiver);  // Receiver.
  __ mov(scratch3, Operand(Handle<AccessorInfo>(callback)));  // callback data
  __ ldr(ip, FieldMemOperand(scratch3, AccessorInfo::kDataOffset));
  __ Push(reg, ip, scratch3, name_reg);

  // Do tail-call to the runtime system.
  ExternalReference load_callback_property =
      ExternalReference(IC_Utility(IC::kLoadCallbackProperty));
  __ TailCallExternalReference(load_callback_property, 5, 1);

  return true;
}


void StubCompiler::GenerateLoadInterceptor(JSObject* object,
                                           JSObject* interceptor_holder,
                                           LookupResult* lookup,
                                           Register receiver,
                                           Register name_reg,
                                           Register scratch1,
                                           Register scratch2,
                                           Register scratch3,
                                           String* name,
                                           Label* miss) {
  ASSERT(interceptor_holder->HasNamedInterceptor());
  ASSERT(!interceptor_holder->GetNamedInterceptor()->getter()->IsUndefined());

  // Check that the receiver isn't a smi.
  __ BranchOnSmi(receiver, miss);

  // So far the most popular follow ups for interceptor loads are FIELD
  // and CALLBACKS, so inline only them, other cases may be added
  // later.
  bool compile_followup_inline = false;
  if (lookup->IsProperty() && lookup->IsCacheable()) {
    if (lookup->type() == FIELD) {
      compile_followup_inline = true;
    } else if (lookup->type() == CALLBACKS &&
        lookup->GetCallbackObject()->IsAccessorInfo() &&
        AccessorInfo::cast(lookup->GetCallbackObject())->getter() != NULL) {
      compile_followup_inline = true;
    }
  }

  if (compile_followup_inline) {
    // Compile the interceptor call, followed by inline code to load the
    // property from further up the prototype chain if the call fails.
    // Check that the maps haven't changed.
    Register holder_reg = CheckPrototypes(object, receiver, interceptor_holder,
                                          scratch1, scratch2, scratch3,
                                          name, miss);
    ASSERT(holder_reg.is(receiver) || holder_reg.is(scratch1));

    // Save necessary data before invoking an interceptor.
    // Requires a frame to make GC aware of pushed pointers.
    __ EnterInternalFrame();

    if (lookup->type() == CALLBACKS && !receiver.is(holder_reg)) {
      // CALLBACKS case needs a receiver to be passed into C++ callback.
      __ Push(receiver, holder_reg, name_reg);
    } else {
      __ Push(holder_reg, name_reg);
    }

    // Invoke an interceptor.  Note: map checks from receiver to
    // interceptor's holder has been compiled before (see a caller
    // of this method.)
    CompileCallLoadPropertyWithInterceptor(masm(),
                                           receiver,
                                           holder_reg,
                                           name_reg,
                                           interceptor_holder);

    // Check if interceptor provided a value for property.  If it's
    // the case, return immediately.
    Label interceptor_failed;
    __ LoadRoot(scratch1, Heap::kNoInterceptorResultSentinelRootIndex);
    __ cmp(r0, scratch1);
    __ b(eq, &interceptor_failed);
    __ LeaveInternalFrame();
    __ Ret();

    __ bind(&interceptor_failed);
    __ pop(name_reg);
    __ pop(holder_reg);
    if (lookup->type() == CALLBACKS && !receiver.is(holder_reg)) {
      __ pop(receiver);
    }

    __ LeaveInternalFrame();

    // Check that the maps from interceptor's holder to lookup's holder
    // haven't changed.  And load lookup's holder into |holder| register.
    if (interceptor_holder != lookup->holder()) {
      holder_reg = CheckPrototypes(interceptor_holder,
                                   holder_reg,
                                   lookup->holder(),
                                   scratch1,
                                   scratch2,
                                   scratch3,
                                   name,
                                   miss);
    }

    if (lookup->type() == FIELD) {
      // We found FIELD property in prototype chain of interceptor's holder.
      // Retrieve a field from field's holder.
      GenerateFastPropertyLoad(masm(), r0, holder_reg,
                               lookup->holder(), lookup->GetFieldIndex());
      __ Ret();
    } else {
      // We found CALLBACKS property in prototype chain of interceptor's
      // holder.
      ASSERT(lookup->type() == CALLBACKS);
      ASSERT(lookup->GetCallbackObject()->IsAccessorInfo());
      AccessorInfo* callback = AccessorInfo::cast(lookup->GetCallbackObject());
      ASSERT(callback != NULL);
      ASSERT(callback->getter() != NULL);

      // Tail call to runtime.
      // Important invariant in CALLBACKS case: the code above must be
      // structured to never clobber |receiver| register.
      __ Move(scratch2, Handle<AccessorInfo>(callback));
      // holder_reg is either receiver or scratch1.
      if (!receiver.is(holder_reg)) {
        ASSERT(scratch1.is(holder_reg));
        __ Push(receiver, holder_reg);
        __ ldr(scratch3,
               FieldMemOperand(scratch2, AccessorInfo::kDataOffset));
        __ Push(scratch3, scratch2, name_reg);
      } else {
        __ push(receiver);
        __ ldr(scratch3,
               FieldMemOperand(scratch2, AccessorInfo::kDataOffset));
        __ Push(holder_reg, scratch3, scratch2, name_reg);
      }

      ExternalReference ref =
          ExternalReference(IC_Utility(IC::kLoadCallbackProperty));
      __ TailCallExternalReference(ref, 5, 1);
    }
  } else {  // !compile_followup_inline
    // Call the runtime system to load the interceptor.
    // Check that the maps haven't changed.
    Register holder_reg = CheckPrototypes(object, receiver, interceptor_holder,
                                          scratch1, scratch2, scratch3,
                                          name, miss);
    PushInterceptorArguments(masm(), receiver, holder_reg,
                             name_reg, interceptor_holder);

    ExternalReference ref = ExternalReference(
        IC_Utility(IC::kLoadPropertyWithInterceptorForLoad));
    __ TailCallExternalReference(ref, 5, 1);
  }
}


void CallStubCompiler::GenerateNameCheck(String* name, Label* miss) {
  if (kind_ == Code::KEYED_CALL_IC) {
    __ cmp(r2, Operand(Handle<String>(name)));
    __ b(ne, miss);
  }
}


void CallStubCompiler::GenerateGlobalReceiverCheck(JSObject* object,
                                                   JSObject* holder,
                                                   String* name,
                                                   Label* miss) {
  ASSERT(holder->IsGlobalObject());

  // Get the number of arguments.
  const int argc = arguments().immediate();

  // Get the receiver from the stack.
  __ ldr(r0, MemOperand(sp, argc * kPointerSize));

  // If the object is the holder then we know that it's a global
  // object which can only happen for contextual calls. In this case,
  // the receiver cannot be a smi.
  if (object != holder) {
    __ tst(r0, Operand(kSmiTagMask));
    __ b(eq, miss);
  }

  // Check that the maps haven't changed.
  CheckPrototypes(object, r0, holder, r3, r1, r4, name, miss);
}


void CallStubCompiler::GenerateLoadFunctionFromCell(JSGlobalPropertyCell* cell,
                                                    JSFunction* function,
                                                    Label* miss) {
  // Get the value from the cell.
  __ mov(r3, Operand(Handle<JSGlobalPropertyCell>(cell)));
  __ ldr(r1, FieldMemOperand(r3, JSGlobalPropertyCell::kValueOffset));

  // Check that the cell contains the same function.
  if (Heap::InNewSpace(function)) {
    // We can't embed a pointer to a function in new space so we have
    // to verify that the shared function info is unchanged. This has
    // the nice side effect that multiple closures based on the same
    // function can all use this call IC. Before we load through the
    // function, we have to verify that it still is a function.
    __ tst(r1, Operand(kSmiTagMask));
    __ b(eq, miss);
    __ CompareObjectType(r1, r3, r3, JS_FUNCTION_TYPE);
    __ b(ne, miss);

    // Check the shared function info. Make sure it hasn't changed.
    __ Move(r3, Handle<SharedFunctionInfo>(function->shared()));
    __ ldr(r4, FieldMemOperand(r1, JSFunction::kSharedFunctionInfoOffset));
    __ cmp(r4, r3);
    __ b(ne, miss);
  } else {
    __ cmp(r1, Operand(Handle<JSFunction>(function)));
    __ b(ne, miss);
  }
}


MaybeObject* CallStubCompiler::GenerateMissBranch() {
  Object* obj;
  { MaybeObject* maybe_obj =
        StubCache::ComputeCallMiss(arguments().immediate(), kind_);
    if (!maybe_obj->ToObject(&obj)) return maybe_obj;
  }
  __ Jump(Handle<Code>(Code::cast(obj)), RelocInfo::CODE_TARGET);
  return obj;
}


MaybeObject* CallStubCompiler::CompileCallField(JSObject* object,
                                                JSObject* holder,
                                                int index,
                                                String* name) {
  // ----------- S t a t e -------------
  //  -- r2    : name
  //  -- lr    : return address
  // -----------------------------------
  Label miss;

  GenerateNameCheck(name, &miss);

  const int argc = arguments().immediate();

  // Get the receiver of the function from the stack into r0.
  __ ldr(r0, MemOperand(sp, argc * kPointerSize));
  // Check that the receiver isn't a smi.
  __ tst(r0, Operand(kSmiTagMask));
  __ b(eq, &miss);

  // Do the right check and compute the holder register.
  Register reg = CheckPrototypes(object, r0, holder, r1, r3, r4, name, &miss);
  GenerateFastPropertyLoad(masm(), r1, reg, holder, index);

  GenerateCallFunction(masm(), object, arguments(), &miss);

  // Handle call cache miss.
  __ bind(&miss);
  Object* obj;
  { MaybeObject* maybe_obj = GenerateMissBranch();
    if (!maybe_obj->ToObject(&obj)) return maybe_obj;
  }

  // Return the generated code.
  return GetCode(FIELD, name);
}


MaybeObject* CallStubCompiler::CompileArrayPushCall(Object* object,
                                                    JSObject* holder,
                                                    JSGlobalPropertyCell* cell,
                                                    JSFunction* function,
                                                    String* name) {
  // ----------- S t a t e -------------
  //  -- r2    : name
  //  -- lr    : return address
  //  -- sp[(argc - n - 1) * 4] : arg[n] (zero-based)
  //  -- ...
  //  -- sp[argc * 4]           : receiver
  // -----------------------------------

  // If object is not an array, bail out to regular call.
  if (!object->IsJSArray() || cell != NULL) return Heap::undefined_value();

  Label miss;

  GenerateNameCheck(name, &miss);

  Register receiver = r1;

  // Get the receiver from the stack
  const int argc = arguments().immediate();
  __ ldr(receiver, MemOperand(sp, argc * kPointerSize));

  // Check that the receiver isn't a smi.
  __ BranchOnSmi(receiver, &miss);

  // Check that the maps haven't changed.
  CheckPrototypes(JSObject::cast(object), receiver,
                  holder, r3, r0, r4, name, &miss);

  if (argc == 0) {
    // Nothing to do, just return the length.
    __ ldr(r0, FieldMemOperand(receiver, JSArray::kLengthOffset));
    __ Drop(argc + 1);
    __ Ret();
  } else {
    Label call_builtin;

    Register elements = r3;
    Register end_elements = r5;

    // Get the elements array of the object.
    __ ldr(elements, FieldMemOperand(receiver, JSArray::kElementsOffset));

    // Check that the elements are in fast mode and writable.
    __ CheckMap(elements, r0,
                Heap::kFixedArrayMapRootIndex, &call_builtin, true);

    if (argc == 1) {  // Otherwise fall through to call the builtin.
      Label exit, with_write_barrier, attempt_to_grow_elements;

      // Get the array's length into r0 and calculate new length.
      __ ldr(r0, FieldMemOperand(receiver, JSArray::kLengthOffset));
      STATIC_ASSERT(kSmiTagSize == 1);
      STATIC_ASSERT(kSmiTag == 0);
      __ add(r0, r0, Operand(Smi::FromInt(argc)));

      // Get the element's length.
      __ ldr(r4, FieldMemOperand(elements, FixedArray::kLengthOffset));

      // Check if we could survive without allocation.
      __ cmp(r0, r4);
      __ b(gt, &attempt_to_grow_elements);

      // Save new length.
      __ str(r0, FieldMemOperand(receiver, JSArray::kLengthOffset));

      // Push the element.
      __ ldr(r4, MemOperand(sp, (argc - 1) * kPointerSize));
      // We may need a register containing the address end_elements below,
      // so write back the value in end_elements.
      __ add(end_elements, elements,
             Operand(r0, LSL, kPointerSizeLog2 - kSmiTagSize));
      const int kEndElementsOffset =
          FixedArray::kHeaderSize - kHeapObjectTag - argc * kPointerSize;
      __ str(r4, MemOperand(end_elements, kEndElementsOffset, PreIndex));

      // Check for a smi.
      __ BranchOnNotSmi(r4, &with_write_barrier);
      __ bind(&exit);
      __ Drop(argc + 1);
      __ Ret();

      __ bind(&with_write_barrier);
      __ InNewSpace(elements, r4, eq, &exit);
      __ RecordWriteHelper(elements, end_elements, r4);
      __ Drop(argc + 1);
      __ Ret();

      __ bind(&attempt_to_grow_elements);
      // r0: array's length + 1.
      // r4: elements' length.

      if (!FLAG_inline_new) {
        __ b(&call_builtin);
      }

      ExternalReference new_space_allocation_top =
          ExternalReference::new_space_allocation_top_address();
      ExternalReference new_space_allocation_limit =
          ExternalReference::new_space_allocation_limit_address();

      const int kAllocationDelta = 4;
      // Load top and check if it is the end of elements.
      __ add(end_elements, elements,
             Operand(r0, LSL, kPointerSizeLog2 - kSmiTagSize));
      __ add(end_elements, end_elements, Operand(kEndElementsOffset));
      __ mov(r7, Operand(new_space_allocation_top));
      __ ldr(r6, MemOperand(r7));
      __ cmp(end_elements, r6);
      __ b(ne, &call_builtin);

      __ mov(r9, Operand(new_space_allocation_limit));
      __ ldr(r9, MemOperand(r9));
      __ add(r6, r6, Operand(kAllocationDelta * kPointerSize));
      __ cmp(r6, r9);
      __ b(hi, &call_builtin);

      // We fit and could grow elements.
      // Update new_space_allocation_top.
      __ str(r6, MemOperand(r7));
      // Push the argument.
      __ ldr(r6, MemOperand(sp, (argc - 1) * kPointerSize));
      __ str(r6, MemOperand(end_elements));
      // Fill the rest with holes.
      __ LoadRoot(r6, Heap::kTheHoleValueRootIndex);
      for (int i = 1; i < kAllocationDelta; i++) {
        __ str(r6, MemOperand(end_elements, i * kPointerSize));
      }

      // Update elements' and array's sizes.
      __ str(r0, FieldMemOperand(receiver, JSArray::kLengthOffset));
      __ add(r4, r4, Operand(Smi::FromInt(kAllocationDelta)));
      __ str(r4, FieldMemOperand(elements, FixedArray::kLengthOffset));

      // Elements are in new space, so write barrier is not required.
      __ Drop(argc + 1);
      __ Ret();
    }
    __ bind(&call_builtin);
    __ TailCallExternalReference(ExternalReference(Builtins::c_ArrayPush),
                                 argc + 1,
                                 1);
  }

  // Handle call cache miss.
  __ bind(&miss);
  Object* obj;
  { MaybeObject* maybe_obj = GenerateMissBranch();
    if (!maybe_obj->ToObject(&obj)) return maybe_obj;
  }

  // Return the generated code.
  return GetCode(function);
}


MaybeObject* CallStubCompiler::CompileArrayPopCall(Object* object,
                                                   JSObject* holder,
                                                   JSGlobalPropertyCell* cell,
                                                   JSFunction* function,
                                                   String* name) {
  // ----------- S t a t e -------------
  //  -- r2    : name
  //  -- lr    : return address
  //  -- sp[(argc - n - 1) * 4] : arg[n] (zero-based)
  //  -- ...
  //  -- sp[argc * 4]           : receiver
  // -----------------------------------

  // If object is not an array, bail out to regular call.
  if (!object->IsJSArray() || cell != NULL) return Heap::undefined_value();

  Label miss, return_undefined, call_builtin;

  Register receiver = r1;
  Register elements = r3;

  GenerateNameCheck(name, &miss);

  // Get the receiver from the stack
  const int argc = arguments().immediate();
  __ ldr(receiver, MemOperand(sp, argc * kPointerSize));

  // Check that the receiver isn't a smi.
  __ BranchOnSmi(receiver, &miss);

  // Check that the maps haven't changed.
  CheckPrototypes(JSObject::cast(object),
                  receiver, holder, elements, r4, r0, name, &miss);

  // Get the elements array of the object.
  __ ldr(elements, FieldMemOperand(receiver, JSArray::kElementsOffset));

  // Check that the elements are in fast mode and writable.
  __ CheckMap(elements, r0, Heap::kFixedArrayMapRootIndex, &call_builtin, true);

  // Get the array's length into r4 and calculate new length.
  __ ldr(r4, FieldMemOperand(receiver, JSArray::kLengthOffset));
  __ sub(r4, r4, Operand(Smi::FromInt(1)), SetCC);
  __ b(lt, &return_undefined);

  // Get the last element.
  __ LoadRoot(r6, Heap::kTheHoleValueRootIndex);
  STATIC_ASSERT(kSmiTagSize == 1);
  STATIC_ASSERT(kSmiTag == 0);
  // We can't address the last element in one operation. Compute the more
  // expensive shift first, and use an offset later on.
  __ add(elements, elements, Operand(r4, LSL, kPointerSizeLog2 - kSmiTagSize));
  __ ldr(r0, MemOperand(elements, FixedArray::kHeaderSize - kHeapObjectTag));
  __ cmp(r0, r6);
  __ b(eq, &call_builtin);

  // Set the array's length.
  __ str(r4, FieldMemOperand(receiver, JSArray::kLengthOffset));

  // Fill with the hole.
  __ str(r6, MemOperand(elements, FixedArray::kHeaderSize - kHeapObjectTag));
  __ Drop(argc + 1);
  __ Ret();

  __ bind(&return_undefined);
  __ LoadRoot(r0, Heap::kUndefinedValueRootIndex);
  __ Drop(argc + 1);
  __ Ret();

  __ bind(&call_builtin);
  __ TailCallExternalReference(ExternalReference(Builtins::c_ArrayPop),
                               argc + 1,
                               1);

  // Handle call cache miss.
  __ bind(&miss);
  Object* obj;
  { MaybeObject* maybe_obj = GenerateMissBranch();
    if (!maybe_obj->ToObject(&obj)) return maybe_obj;
  }

  // Return the generated code.
  return GetCode(function);
}


MaybeObject* CallStubCompiler::CompileStringCharCodeAtCall(
    Object* object,
    JSObject* holder,
    JSGlobalPropertyCell* cell,
    JSFunction* function,
    String* name) {
  // ----------- S t a t e -------------
  //  -- r2                     : function name
  //  -- lr                     : return address
  //  -- sp[(argc - n - 1) * 4] : arg[n] (zero-based)
  //  -- ...
  //  -- sp[argc * 4]           : receiver
  // -----------------------------------

  // If object is not a string, bail out to regular call.
  if (!object->IsString() || cell != NULL) return Heap::undefined_value();

  const int argc = arguments().immediate();

  Label miss;
  Label index_out_of_range;
  GenerateNameCheck(name, &miss);

  // Check that the maps starting from the prototype haven't changed.
  GenerateDirectLoadGlobalFunctionPrototype(masm(),
                                            Context::STRING_FUNCTION_INDEX,
                                            r0,
                                            &miss);
  ASSERT(object != holder);
  CheckPrototypes(JSObject::cast(object->GetPrototype()), r0, holder,
                  r1, r3, r4, name, &miss);

  Register receiver = r1;
  Register index = r4;
  Register scratch = r3;
  Register result = r0;
  __ ldr(receiver, MemOperand(sp, argc * kPointerSize));
  if (argc > 0) {
    __ ldr(index, MemOperand(sp, (argc - 1) * kPointerSize));
  } else {
    __ LoadRoot(index, Heap::kUndefinedValueRootIndex);
  }

  StringCharCodeAtGenerator char_code_at_generator(receiver,
                                                   index,
                                                   scratch,
                                                   result,
                                                   &miss,  // When not a string.
                                                   &miss,  // When not a number.
                                                   &index_out_of_range,
                                                   STRING_INDEX_IS_NUMBER);
  char_code_at_generator.GenerateFast(masm());
  __ Drop(argc + 1);
  __ Ret();

  StubRuntimeCallHelper call_helper;
  char_code_at_generator.GenerateSlow(masm(), call_helper);

  __ bind(&index_out_of_range);
  __ LoadRoot(r0, Heap::kNanValueRootIndex);
  __ Drop(argc + 1);
  __ Ret();

  __ bind(&miss);
  Object* obj;
  { MaybeObject* maybe_obj = GenerateMissBranch();
    if (!maybe_obj->ToObject(&obj)) return maybe_obj;
  }

  // Return the generated code.
  return GetCode(function);
}


MaybeObject* CallStubCompiler::CompileStringCharAtCall(
    Object* object,
    JSObject* holder,
    JSGlobalPropertyCell* cell,
    JSFunction* function,
    String* name) {
  // ----------- S t a t e -------------
  //  -- r2                     : function name
  //  -- lr                     : return address
  //  -- sp[(argc - n - 1) * 4] : arg[n] (zero-based)
  //  -- ...
  //  -- sp[argc * 4]           : receiver
  // -----------------------------------

  // If object is not a string, bail out to regular call.
  if (!object->IsString() || cell != NULL) return Heap::undefined_value();

  const int argc = arguments().immediate();

  Label miss;
  Label index_out_of_range;

  GenerateNameCheck(name, &miss);

  // Check that the maps starting from the prototype haven't changed.
  GenerateDirectLoadGlobalFunctionPrototype(masm(),
                                            Context::STRING_FUNCTION_INDEX,
                                            r0,
                                            &miss);
  ASSERT(object != holder);
  CheckPrototypes(JSObject::cast(object->GetPrototype()), r0, holder,
                  r1, r3, r4, name, &miss);

  Register receiver = r0;
  Register index = r4;
  Register scratch1 = r1;
  Register scratch2 = r3;
  Register result = r0;
  __ ldr(receiver, MemOperand(sp, argc * kPointerSize));
  if (argc > 0) {
    __ ldr(index, MemOperand(sp, (argc - 1) * kPointerSize));
  } else {
    __ LoadRoot(index, Heap::kUndefinedValueRootIndex);
  }

  StringCharAtGenerator char_at_generator(receiver,
                                          index,
                                          scratch1,
                                          scratch2,
                                          result,
                                          &miss,  // When not a string.
                                          &miss,  // When not a number.
                                          &index_out_of_range,
                                          STRING_INDEX_IS_NUMBER);
  char_at_generator.GenerateFast(masm());
  __ Drop(argc + 1);
  __ Ret();

  StubRuntimeCallHelper call_helper;
  char_at_generator.GenerateSlow(masm(), call_helper);

  __ bind(&index_out_of_range);
  __ LoadRoot(r0, Heap::kEmptyStringRootIndex);
  __ Drop(argc + 1);
  __ Ret();

  __ bind(&miss);
  Object* obj;
  { MaybeObject* maybe_obj = GenerateMissBranch();
    if (!maybe_obj->ToObject(&obj)) return maybe_obj;
  }

  // Return the generated code.
  return GetCode(function);
}


MaybeObject* CallStubCompiler::CompileStringFromCharCodeCall(
    Object* object,
    JSObject* holder,
    JSGlobalPropertyCell* cell,
    JSFunction* function,
    String* name) {
  // ----------- S t a t e -------------
  //  -- r2                     : function name
  //  -- lr                     : return address
  //  -- sp[(argc - n - 1) * 4] : arg[n] (zero-based)
  //  -- ...
  //  -- sp[argc * 4]           : receiver
  // -----------------------------------

  const int argc = arguments().immediate();

  // If the object is not a JSObject or we got an unexpected number of
  // arguments, bail out to the regular call.
  if (!object->IsJSObject() || argc != 1) return Heap::undefined_value();

  Label miss;
  GenerateNameCheck(name, &miss);

  if (cell == NULL) {
    __ ldr(r1, MemOperand(sp, 1 * kPointerSize));

    STATIC_ASSERT(kSmiTag == 0);
    __ tst(r1, Operand(kSmiTagMask));
    __ b(eq, &miss);

    CheckPrototypes(JSObject::cast(object), r1, holder, r0, r3, r4, name,
                    &miss);
  } else {
    ASSERT(cell->value() == function);
    GenerateGlobalReceiverCheck(JSObject::cast(object), holder, name, &miss);
    GenerateLoadFunctionFromCell(cell, function, &miss);
  }

  // Load the char code argument.
  Register code = r1;
  __ ldr(code, MemOperand(sp, 0 * kPointerSize));

  // Check the code is a smi.
  Label slow;
  STATIC_ASSERT(kSmiTag == 0);
  __ tst(code, Operand(kSmiTagMask));
  __ b(ne, &slow);

  // Convert the smi code to uint16.
  __ and_(code, code, Operand(Smi::FromInt(0xffff)));

  StringCharFromCodeGenerator char_from_code_generator(code, r0);
  char_from_code_generator.GenerateFast(masm());
  __ Drop(argc + 1);
  __ Ret();

  StubRuntimeCallHelper call_helper;
  char_from_code_generator.GenerateSlow(masm(), call_helper);

  // Tail call the full function. We do not have to patch the receiver
  // because the function makes no use of it.
  __ bind(&slow);
  __ InvokeFunction(function, arguments(), JUMP_FUNCTION);

  __ bind(&miss);
  // r2: function name.
  Object* obj;
  { MaybeObject* maybe_obj = GenerateMissBranch();
    if (!maybe_obj->ToObject(&obj)) return maybe_obj;
  }

  // Return the generated code.
  return (cell == NULL) ? GetCode(function) : GetCode(NORMAL, name);
}


MaybeObject* CallStubCompiler::CompileMathFloorCall(Object* object,
                                                    JSObject* holder,
                                                    JSGlobalPropertyCell* cell,
                                                    JSFunction* function,
                                                    String* name) {
  // ----------- S t a t e -------------
  //  -- r2                     : function name
  //  -- lr                     : return address
  //  -- sp[(argc - n - 1) * 4] : arg[n] (zero-based)
  //  -- ...
  //  -- sp[argc * 4]           : receiver
  // -----------------------------------

  if (!CpuFeatures::IsSupported(VFP3)) return Heap::undefined_value();
  CpuFeatures::Scope scope_vfp3(VFP3);

  const int argc = arguments().immediate();

  // If the object is not a JSObject or we got an unexpected number of
  // arguments, bail out to the regular call.
  if (!object->IsJSObject() || argc != 1) return Heap::undefined_value();

  Label miss, slow;
  GenerateNameCheck(name, &miss);

  if (cell == NULL) {
    __ ldr(r1, MemOperand(sp, 1 * kPointerSize));

    STATIC_ASSERT(kSmiTag == 0);
    __ BranchOnSmi(r1, &miss);

    CheckPrototypes(JSObject::cast(object), r1, holder, r0, r3, r4, name,
                    &miss);
  } else {
    ASSERT(cell->value() == function);
    GenerateGlobalReceiverCheck(JSObject::cast(object), holder, name, &miss);
    GenerateLoadFunctionFromCell(cell, function, &miss);
  }

  // Load the (only) argument into r0.
  __ ldr(r0, MemOperand(sp, 0 * kPointerSize));

  // If the argument is a smi, just return.
  STATIC_ASSERT(kSmiTag == 0);
  __ tst(r0, Operand(kSmiTagMask));
  __ Drop(argc + 1, eq);
  __ Ret(eq);

  __ CheckMap(r0, r1, Heap::kHeapNumberMapRootIndex, &slow, true);

  Label wont_fit_smi, no_vfp_exception, restore_fpscr_and_return;

  // If vfp3 is enabled, we use the fpu rounding with the RM (round towards
  // minus infinity) mode.

  // Load the HeapNumber value.
  // We will need access to the value in the core registers, so we load it
  // with ldrd and move it to the fpu. It also spares a sub instruction for
  // updating the HeapNumber value address, as vldr expects a multiple
  // of 4 offset.
  __ Ldrd(r4, r5, FieldMemOperand(r0, HeapNumber::kValueOffset));
  __ vmov(d1, r4, r5);

  // Backup FPSCR.
  __ vmrs(r3);
  // Set custom FPCSR:
  //  - Set rounding mode to "Round towards Minus Infinity"
  //    (ie bits [23:22] = 0b10).
  //  - Clear vfp cumulative exception flags (bits [3:0]).
  //  - Make sure Flush-to-zero mode control bit is unset (bit 22).
  __ bic(r9, r3,
      Operand(kVFPExceptionMask | kVFPRoundingModeMask | kVFPFlushToZeroMask));
  __ orr(r9, r9, Operand(kVFPRoundToMinusInfinityBits));
  __ vmsr(r9);

  // Convert the argument to an integer.
  __ vcvt_s32_f64(s0, d1, Assembler::FPSCRRounding, al);

  // Use vcvt latency to start checking for special cases.
  // Get the argument exponent and clear the sign bit.
  __ bic(r6, r5, Operand(HeapNumber::kSignMask));
  __ mov(r6, Operand(r6, LSR, HeapNumber::kMantissaBitsInTopWord));

  // Retrieve FPSCR and check for vfp exceptions.
  __ vmrs(r9);
  __ tst(r9, Operand(kVFPExceptionMask));
  __ b(&no_vfp_exception, eq);

  // Check for NaN, Infinity, and -Infinity.
  // They are invariant through a Math.Floor call, so just
  // return the original argument.
  __ sub(r7, r6, Operand(HeapNumber::kExponentMask
        >> HeapNumber::kMantissaBitsInTopWord), SetCC);
  __ b(&restore_fpscr_and_return, eq);
  // We had an overflow or underflow in the conversion. Check if we
  // have a big exponent.
  __ cmp(r7, Operand(HeapNumber::kMantissaBits));
  // If greater or equal, the argument is already round and in r0.
  __ b(&restore_fpscr_and_return, ge);
  __ b(&slow);

  __ bind(&no_vfp_exception);
  // Move the result back to general purpose register r0.
  __ vmov(r0, s0);
  // Check if the result fits into a smi.
  __ add(r1, r0, Operand(0x40000000), SetCC);
  __ b(&wont_fit_smi, mi);
  // Tag the result.
  STATIC_ASSERT(kSmiTag == 0);
  __ mov(r0, Operand(r0, LSL, kSmiTagSize));

  // Check for -0.
  __ cmp(r0, Operand(0));
  __ b(&restore_fpscr_and_return, ne);
  // r5 already holds the HeapNumber exponent.
  __ tst(r5, Operand(HeapNumber::kSignMask));
  // If our HeapNumber is negative it was -0, so load its address and return.
  // Else r0 is loaded with 0, so we can also just return.
  __ ldr(r0, MemOperand(sp, 0 * kPointerSize), ne);

  __ bind(&restore_fpscr_and_return);
  // Restore FPSCR and return.
  __ vmsr(r3);
  __ Drop(argc + 1);
  __ Ret();

  __ bind(&wont_fit_smi);
  __ bind(&slow);
  // Restore FPCSR and fall to slow case.
  __ vmsr(r3);

  // Tail call the full function. We do not have to patch the receiver
  // because the function makes no use of it.
  __ InvokeFunction(function, arguments(), JUMP_FUNCTION);

  __ bind(&miss);
  // r2: function name.
  MaybeObject* obj = GenerateMissBranch();
  if (obj->IsFailure()) return obj;

  // Return the generated code.
  return (cell == NULL) ? GetCode(function) : GetCode(NORMAL, name);
}


MaybeObject* CallStubCompiler::CompileMathAbsCall(Object* object,
                                                  JSObject* holder,
                                                  JSGlobalPropertyCell* cell,
                                                  JSFunction* function,
                                                  String* name) {
  // ----------- S t a t e -------------
  //  -- r2                     : function name
  //  -- lr                     : return address
  //  -- sp[(argc - n - 1) * 4] : arg[n] (zero-based)
  //  -- ...
  //  -- sp[argc * 4]           : receiver
  // -----------------------------------

  const int argc = arguments().immediate();

  // If the object is not a JSObject or we got an unexpected number of
  // arguments, bail out to the regular call.
  if (!object->IsJSObject() || argc != 1) return Heap::undefined_value();

  Label miss;
  GenerateNameCheck(name, &miss);

  if (cell == NULL) {
    __ ldr(r1, MemOperand(sp, 1 * kPointerSize));

    STATIC_ASSERT(kSmiTag == 0);
    __ tst(r1, Operand(kSmiTagMask));
    __ b(eq, &miss);

    CheckPrototypes(JSObject::cast(object), r1, holder, r0, r3, r4, name,
                    &miss);
  } else {
    ASSERT(cell->value() == function);
    GenerateGlobalReceiverCheck(JSObject::cast(object), holder, name, &miss);
    GenerateLoadFunctionFromCell(cell, function, &miss);
  }

  // Load the (only) argument into r0.
  __ ldr(r0, MemOperand(sp, 0 * kPointerSize));

  // Check if the argument is a smi.
  Label not_smi;
  STATIC_ASSERT(kSmiTag == 0);
  __ BranchOnNotSmi(r0, &not_smi);

  // Do bitwise not or do nothing depending on the sign of the
  // argument.
  __ eor(r1, r0, Operand(r0, ASR, kBitsPerInt - 1));

  // Add 1 or do nothing depending on the sign of the argument.
  __ sub(r0, r1, Operand(r0, ASR, kBitsPerInt - 1), SetCC);

  // If the result is still negative, go to the slow case.
  // This only happens for the most negative smi.
  Label slow;
  __ b(mi, &slow);

  // Smi case done.
  __ Drop(argc + 1);
  __ Ret();

  // Check if the argument is a heap number and load its exponent and
  // sign.
  __ bind(&not_smi);
  __ CheckMap(r0, r1, Heap::kHeapNumberMapRootIndex, &slow, true);
  __ ldr(r1, FieldMemOperand(r0, HeapNumber::kExponentOffset));

  // Check the sign of the argument. If the argument is positive,
  // just return it.
  Label negative_sign;
  __ tst(r1, Operand(HeapNumber::kSignMask));
  __ b(ne, &negative_sign);
  __ Drop(argc + 1);
  __ Ret();

  // If the argument is negative, clear the sign, and return a new
  // number.
  __ bind(&negative_sign);
  __ eor(r1, r1, Operand(HeapNumber::kSignMask));
  __ ldr(r3, FieldMemOperand(r0, HeapNumber::kMantissaOffset));
  __ LoadRoot(r6, Heap::kHeapNumberMapRootIndex);
  __ AllocateHeapNumber(r0, r4, r5, r6, &slow);
  __ str(r1, FieldMemOperand(r0, HeapNumber::kExponentOffset));
  __ str(r3, FieldMemOperand(r0, HeapNumber::kMantissaOffset));
  __ Drop(argc + 1);
  __ Ret();

  // Tail call the full function. We do not have to patch the receiver
  // because the function makes no use of it.
  __ bind(&slow);
  __ InvokeFunction(function, arguments(), JUMP_FUNCTION);

  __ bind(&miss);
  // r2: function name.
  Object* obj;
  { MaybeObject* maybe_obj = GenerateMissBranch();
    if (!maybe_obj->ToObject(&obj)) return maybe_obj;
  }

  // Return the generated code.
  return (cell == NULL) ? GetCode(function) : GetCode(NORMAL, name);
}


MaybeObject* CallStubCompiler::CompileCallConstant(Object* object,
                                                   JSObject* holder,
                                                   JSFunction* function,
                                                   String* name,
                                                   CheckType check) {
  // ----------- S t a t e -------------
  //  -- r2    : name
  //  -- lr    : return address
  // -----------------------------------
  SharedFunctionInfo* function_info = function->shared();
  if (function_info->HasCustomCallGenerator()) {
    const int id = function_info->custom_call_generator_id();
    MaybeObject* maybe_result = CompileCustomCall(
        id, object, holder, NULL, function, name);
    Object* result;
    if (!maybe_result->ToObject(&result)) return maybe_result;
    // undefined means bail out to regular compiler.
    if (!result->IsUndefined()) {
      return result;
    }
  }

  Label miss_in_smi_check;

  GenerateNameCheck(name, &miss_in_smi_check);

  // Get the receiver from the stack
  const int argc = arguments().immediate();
  __ ldr(r1, MemOperand(sp, argc * kPointerSize));

  // Check that the receiver isn't a smi.
  if (check != NUMBER_CHECK) {
    __ tst(r1, Operand(kSmiTagMask));
    __ b(eq, &miss_in_smi_check);
  }

  // Make sure that it's okay not to patch the on stack receiver
  // unless we're doing a receiver map check.
  ASSERT(!object->IsGlobalObject() || check == RECEIVER_MAP_CHECK);

  CallOptimization optimization(function);
  int depth = kInvalidProtoDepth;
  Label miss;

  switch (check) {
    case RECEIVER_MAP_CHECK:
      __ IncrementCounter(&Counters::call_const, 1, r0, r3);

      if (optimization.is_simple_api_call() && !object->IsGlobalObject()) {
        depth = optimization.GetPrototypeDepthOfExpectedType(
            JSObject::cast(object), holder);
      }

      if (depth != kInvalidProtoDepth) {
        __ IncrementCounter(&Counters::call_const_fast_api, 1, r0, r3);
        ReserveSpaceForFastApiCall(masm(), r0);
      }

      // Check that the maps haven't changed.
      CheckPrototypes(JSObject::cast(object), r1, holder, r0, r3, r4, name,
                      depth, &miss);

      // Patch the receiver on the stack with the global proxy if
      // necessary.
      if (object->IsGlobalObject()) {
        ASSERT(depth == kInvalidProtoDepth);
        __ ldr(r3, FieldMemOperand(r1, GlobalObject::kGlobalReceiverOffset));
        __ str(r3, MemOperand(sp, argc * kPointerSize));
      }
      break;

    case STRING_CHECK:
      if (!function->IsBuiltin()) {
        // Calling non-builtins with a value as receiver requires boxing.
        __ jmp(&miss);
      } else {
        // Check that the object is a two-byte string or a symbol.
        __ CompareObjectType(r1, r3, r3, FIRST_NONSTRING_TYPE);
        __ b(hs, &miss);
        // Check that the maps starting from the prototype haven't changed.
        GenerateDirectLoadGlobalFunctionPrototype(
            masm(), Context::STRING_FUNCTION_INDEX, r0, &miss);
        CheckPrototypes(JSObject::cast(object->GetPrototype()), r0, holder, r3,
                        r1, r4, name, &miss);
      }
      break;

    case NUMBER_CHECK: {
      if (!function->IsBuiltin()) {
        // Calling non-builtins with a value as receiver requires boxing.
        __ jmp(&miss);
      } else {
        Label fast;
        // Check that the object is a smi or a heap number.
        __ tst(r1, Operand(kSmiTagMask));
        __ b(eq, &fast);
        __ CompareObjectType(r1, r0, r0, HEAP_NUMBER_TYPE);
        __ b(ne, &miss);
        __ bind(&fast);
        // Check that the maps starting from the prototype haven't changed.
        GenerateDirectLoadGlobalFunctionPrototype(
            masm(), Context::NUMBER_FUNCTION_INDEX, r0, &miss);
        CheckPrototypes(JSObject::cast(object->GetPrototype()), r0, holder, r3,
                        r1, r4, name, &miss);
      }
      break;
    }

    case BOOLEAN_CHECK: {
      if (!function->IsBuiltin()) {
        // Calling non-builtins with a value as receiver requires boxing.
        __ jmp(&miss);
      } else {
        Label fast;
        // Check that the object is a boolean.
        __ LoadRoot(ip, Heap::kTrueValueRootIndex);
        __ cmp(r1, ip);
        __ b(eq, &fast);
        __ LoadRoot(ip, Heap::kFalseValueRootIndex);
        __ cmp(r1, ip);
        __ b(ne, &miss);
        __ bind(&fast);
        // Check that the maps starting from the prototype haven't changed.
        GenerateDirectLoadGlobalFunctionPrototype(
            masm(), Context::BOOLEAN_FUNCTION_INDEX, r0, &miss);
        CheckPrototypes(JSObject::cast(object->GetPrototype()), r0, holder, r3,
                        r1, r4, name, &miss);
      }
      break;
    }

    default:
      UNREACHABLE();
  }

  if (depth != kInvalidProtoDepth) {
    GenerateFastApiCall(masm(), optimization, argc);
  } else {
    __ InvokeFunction(function, arguments(), JUMP_FUNCTION);
  }

  // Handle call cache miss.
  __ bind(&miss);
  if (depth != kInvalidProtoDepth) {
    FreeSpaceForFastApiCall(masm());
  }

  __ bind(&miss_in_smi_check);
  Object* obj;
  { MaybeObject* maybe_obj = GenerateMissBranch();
    if (!maybe_obj->ToObject(&obj)) return maybe_obj;
  }

  // Return the generated code.
  return GetCode(function);
}


MaybeObject* CallStubCompiler::CompileCallInterceptor(JSObject* object,
                                                      JSObject* holder,
                                                      String* name) {
  // ----------- S t a t e -------------
  //  -- r2    : name
  //  -- lr    : return address
  // -----------------------------------

  Label miss;

  GenerateNameCheck(name, &miss);

  // Get the number of arguments.
  const int argc = arguments().immediate();

  LookupResult lookup;
  LookupPostInterceptor(holder, name, &lookup);

  // Get the receiver from the stack.
  __ ldr(r1, MemOperand(sp, argc * kPointerSize));

  CallInterceptorCompiler compiler(this, arguments(), r2);
  compiler.Compile(masm(),
                   object,
                   holder,
                   name,
                   &lookup,
                   r1,
                   r3,
                   r4,
                   r0,
                   &miss);

  // Move returned value, the function to call, to r1.
  __ mov(r1, r0);
  // Restore receiver.
  __ ldr(r0, MemOperand(sp, argc * kPointerSize));

  GenerateCallFunction(masm(), object, arguments(), &miss);

  // Handle call cache miss.
  __ bind(&miss);
  Object* obj;
  { MaybeObject* maybe_obj = GenerateMissBranch();
    if (!maybe_obj->ToObject(&obj)) return maybe_obj;
  }

  // Return the generated code.
  return GetCode(INTERCEPTOR, name);
}


MaybeObject* CallStubCompiler::CompileCallGlobal(JSObject* object,
                                                 GlobalObject* holder,
                                                 JSGlobalPropertyCell* cell,
                                                 JSFunction* function,
                                                 String* name) {
  // ----------- S t a t e -------------
  //  -- r2    : name
  //  -- lr    : return address
  // -----------------------------------

  SharedFunctionInfo* function_info = function->shared();
  if (function_info->HasCustomCallGenerator()) {
    const int id = function_info->custom_call_generator_id();
    MaybeObject* maybe_result = CompileCustomCall(
        id, object, holder, cell, function, name);
    Object* result;
    if (!maybe_result->ToObject(&result)) return maybe_result;
    // undefined means bail out to regular compiler.
    if (!result->IsUndefined()) return result;
  }

  Label miss;

  GenerateNameCheck(name, &miss);

  // Get the number of arguments.
  const int argc = arguments().immediate();

  GenerateGlobalReceiverCheck(object, holder, name, &miss);

  GenerateLoadFunctionFromCell(cell, function, &miss);

  // Patch the receiver on the stack with the global proxy if
  // necessary.
  if (object->IsGlobalObject()) {
    __ ldr(r3, FieldMemOperand(r0, GlobalObject::kGlobalReceiverOffset));
    __ str(r3, MemOperand(sp, argc * kPointerSize));
  }

  // Setup the context (function already in r1).
  __ ldr(cp, FieldMemOperand(r1, JSFunction::kContextOffset));

  // Jump to the cached code (tail call).
  __ IncrementCounter(&Counters::call_global_inline, 1, r3, r4);
  ASSERT(function->is_compiled());
  Handle<Code> code(function->code());
  ParameterCount expected(function->shared()->formal_parameter_count());
  if (V8::UseCrankshaft()) {
    // TODO(kasperl): For now, we always call indirectly through the
    // code field in the function to allow recompilation to take effect
    // without changing any of the call sites.
    __ ldr(r3, FieldMemOperand(r1, JSFunction::kCodeEntryOffset));
    __ InvokeCode(r3, expected, arguments(), JUMP_FUNCTION);
  } else {
    __ InvokeCode(code, expected, arguments(),
                  RelocInfo::CODE_TARGET, JUMP_FUNCTION);
  }

  // Handle call cache miss.
  __ bind(&miss);
  __ IncrementCounter(&Counters::call_global_inline_miss, 1, r1, r3);
  Object* obj;
  { MaybeObject* maybe_obj = GenerateMissBranch();
    if (!maybe_obj->ToObject(&obj)) return maybe_obj;
  }

  // Return the generated code.
  return GetCode(NORMAL, name);
}


MaybeObject* StoreStubCompiler::CompileStoreField(JSObject* object,
                                                  int index,
                                                  Map* transition,
                                                  String* name) {
  // ----------- S t a t e -------------
  //  -- r0    : value
  //  -- r1    : receiver
  //  -- r2    : name
  //  -- lr    : return address
  // -----------------------------------
  Label miss;

  GenerateStoreField(masm(),
                     object,
                     index,
                     transition,
                     r1, r2, r3,
                     &miss);
  __ bind(&miss);
  Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Miss));
  __ Jump(ic, RelocInfo::CODE_TARGET);

  // Return the generated code.
  return GetCode(transition == NULL ? FIELD : MAP_TRANSITION, name);
}


MaybeObject* StoreStubCompiler::CompileStoreCallback(JSObject* object,
                                                     AccessorInfo* callback,
                                                     String* name) {
  // ----------- S t a t e -------------
  //  -- r0    : value
  //  -- r1    : receiver
  //  -- r2    : name
  //  -- lr    : return address
  // -----------------------------------
  Label miss;

  // Check that the object isn't a smi.
  __ tst(r1, Operand(kSmiTagMask));
  __ b(eq, &miss);

  // Check that the map of the object hasn't changed.
  __ ldr(r3, FieldMemOperand(r1, HeapObject::kMapOffset));
  __ cmp(r3, Operand(Handle<Map>(object->map())));
  __ b(ne, &miss);

  // Perform global security token check if needed.
  if (object->IsJSGlobalProxy()) {
    __ CheckAccessGlobalProxy(r1, r3, &miss);
  }

  // Stub never generated for non-global objects that require access
  // checks.
  ASSERT(object->IsJSGlobalProxy() || !object->IsAccessCheckNeeded());

  __ push(r1);  // receiver
  __ mov(ip, Operand(Handle<AccessorInfo>(callback)));  // callback info
  __ Push(ip, r2, r0);

  // Do tail-call to the runtime system.
  ExternalReference store_callback_property =
      ExternalReference(IC_Utility(IC::kStoreCallbackProperty));
  __ TailCallExternalReference(store_callback_property, 4, 1);

  // Handle store cache miss.
  __ bind(&miss);
  Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Miss));
  __ Jump(ic, RelocInfo::CODE_TARGET);

  // Return the generated code.
  return GetCode(CALLBACKS, name);
}


MaybeObject* StoreStubCompiler::CompileStoreInterceptor(JSObject* receiver,
                                                        String* name) {
  // ----------- S t a t e -------------
  //  -- r0    : value
  //  -- r1    : receiver
  //  -- r2    : name
  //  -- lr    : return address
  // -----------------------------------
  Label miss;

  // Check that the object isn't a smi.
  __ tst(r1, Operand(kSmiTagMask));
  __ b(eq, &miss);

  // Check that the map of the object hasn't changed.
  __ ldr(r3, FieldMemOperand(r1, HeapObject::kMapOffset));
  __ cmp(r3, Operand(Handle<Map>(receiver->map())));
  __ b(ne, &miss);

  // Perform global security token check if needed.
  if (receiver->IsJSGlobalProxy()) {
    __ CheckAccessGlobalProxy(r1, r3, &miss);
  }

  // Stub is never generated for non-global objects that require access
  // checks.
  ASSERT(receiver->IsJSGlobalProxy() || !receiver->IsAccessCheckNeeded());

  __ Push(r1, r2, r0);  // Receiver, name, value.

  // Do tail-call to the runtime system.
  ExternalReference store_ic_property =
      ExternalReference(IC_Utility(IC::kStoreInterceptorProperty));
  __ TailCallExternalReference(store_ic_property, 3, 1);

  // Handle store cache miss.
  __ bind(&miss);
  Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Miss));
  __ Jump(ic, RelocInfo::CODE_TARGET);

  // Return the generated code.
  return GetCode(INTERCEPTOR, name);
}


MaybeObject* StoreStubCompiler::CompileStoreGlobal(GlobalObject* object,
                                                   JSGlobalPropertyCell* cell,
                                                   String* name) {
  // ----------- S t a t e -------------
  //  -- r0    : value
  //  -- r1    : receiver
  //  -- r2    : name
  //  -- lr    : return address
  // -----------------------------------
  Label miss;

  // Check that the map of the global has not changed.
  __ ldr(r3, FieldMemOperand(r1, HeapObject::kMapOffset));
  __ cmp(r3, Operand(Handle<Map>(object->map())));
  __ b(ne, &miss);

  // Store the value in the cell.
  __ mov(r2, Operand(Handle<JSGlobalPropertyCell>(cell)));
  __ str(r0, FieldMemOperand(r2, JSGlobalPropertyCell::kValueOffset));

  __ IncrementCounter(&Counters::named_store_global_inline, 1, r4, r3);
  __ Ret();

  // Handle store cache miss.
  __ bind(&miss);
  __ IncrementCounter(&Counters::named_store_global_inline_miss, 1, r4, r3);
  Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Miss));
  __ Jump(ic, RelocInfo::CODE_TARGET);

  // Return the generated code.
  return GetCode(NORMAL, name);
}


MaybeObject* LoadStubCompiler::CompileLoadNonexistent(String* name,
                                                      JSObject* object,
                                                      JSObject* last) {
  // ----------- S t a t e -------------
  //  -- r0    : receiver
  //  -- lr    : return address
  // -----------------------------------
  Label miss;

  // Check that receiver is not a smi.
  __ tst(r0, Operand(kSmiTagMask));
  __ b(eq, &miss);

  // Check the maps of the full prototype chain.
  CheckPrototypes(object, r0, last, r3, r1, r4, name, &miss);

  // If the last object in the prototype chain is a global object,
  // check that the global property cell is empty.
  if (last->IsGlobalObject()) {
    MaybeObject* cell = GenerateCheckPropertyCell(masm(),
                                                  GlobalObject::cast(last),
                                                  name,
                                                  r1,
                                                  &miss);
    if (cell->IsFailure()) {
      miss.Unuse();
      return cell;
    }
  }

  // Return undefined if maps of the full prototype chain are still the
  // same and no global property with this name contains a value.
  __ LoadRoot(r0, Heap::kUndefinedValueRootIndex);
  __ Ret();

  __ bind(&miss);
  GenerateLoadMiss(masm(), Code::LOAD_IC);

  // Return the generated code.
  return GetCode(NONEXISTENT, Heap::empty_string());
}


MaybeObject* LoadStubCompiler::CompileLoadField(JSObject* object,
                                                JSObject* holder,
                                                int index,
                                                String* name) {
  // ----------- S t a t e -------------
  //  -- r0    : receiver
  //  -- r2    : name
  //  -- lr    : return address
  // -----------------------------------
  Label miss;

  GenerateLoadField(object, holder, r0, r3, r1, r4, index, name, &miss);
  __ bind(&miss);
  GenerateLoadMiss(masm(), Code::LOAD_IC);

  // Return the generated code.
  return GetCode(FIELD, name);
}


MaybeObject* LoadStubCompiler::CompileLoadCallback(String* name,
                                                   JSObject* object,
                                                   JSObject* holder,
                                                   AccessorInfo* callback) {
  // ----------- S t a t e -------------
  //  -- r0    : receiver
  //  -- r2    : name
  //  -- lr    : return address
  // -----------------------------------
  Label miss;

  Failure* failure = Failure::InternalError();
  bool success = GenerateLoadCallback(object, holder, r0, r2, r3, r1, r4,
                                      callback, name, &miss, &failure);
  if (!success) {
    miss.Unuse();
    return failure;
  }

  __ bind(&miss);
  GenerateLoadMiss(masm(), Code::LOAD_IC);

  // Return the generated code.
  return GetCode(CALLBACKS, name);
}


MaybeObject* LoadStubCompiler::CompileLoadConstant(JSObject* object,
                                                   JSObject* holder,
                                                   Object* value,
                                                   String* name) {
  // ----------- S t a t e -------------
  //  -- r0    : receiver
  //  -- r2    : name
  //  -- lr    : return address
  // -----------------------------------
  Label miss;

  GenerateLoadConstant(object, holder, r0, r3, r1, r4, value, name, &miss);
  __ bind(&miss);
  GenerateLoadMiss(masm(), Code::LOAD_IC);

  // Return the generated code.
  return GetCode(CONSTANT_FUNCTION, name);
}


MaybeObject* LoadStubCompiler::CompileLoadInterceptor(JSObject* object,
                                                      JSObject* holder,
                                                      String* name) {
  // ----------- S t a t e -------------
  //  -- r0    : receiver
  //  -- r2    : name
  //  -- lr    : return address
  // -----------------------------------
  Label miss;

  LookupResult lookup;
  LookupPostInterceptor(holder, name, &lookup);
  GenerateLoadInterceptor(object,
                          holder,
                          &lookup,
                          r0,
                          r2,
                          r3,
                          r1,
                          r4,
                          name,
                          &miss);
  __ bind(&miss);
  GenerateLoadMiss(masm(), Code::LOAD_IC);

  // Return the generated code.
  return GetCode(INTERCEPTOR, name);
}


MaybeObject* LoadStubCompiler::CompileLoadGlobal(JSObject* object,
                                                 GlobalObject* holder,
                                                 JSGlobalPropertyCell* cell,
                                                 String* name,
                                                 bool is_dont_delete) {
  // ----------- S t a t e -------------
  //  -- r0    : receiver
  //  -- r2    : name
  //  -- lr    : return address
  // -----------------------------------
  Label miss;

  // If the object is the holder then we know that it's a global
  // object which can only happen for contextual calls. In this case,
  // the receiver cannot be a smi.
  if (object != holder) {
    __ tst(r0, Operand(kSmiTagMask));
    __ b(eq, &miss);
  }

  // Check that the map of the global has not changed.
  CheckPrototypes(object, r0, holder, r3, r4, r1, name, &miss);

  // Get the value from the cell.
  __ mov(r3, Operand(Handle<JSGlobalPropertyCell>(cell)));
  __ ldr(r4, FieldMemOperand(r3, JSGlobalPropertyCell::kValueOffset));

  // Check for deleted property if property can actually be deleted.
  if (!is_dont_delete) {
    __ LoadRoot(ip, Heap::kTheHoleValueRootIndex);
    __ cmp(r4, ip);
    __ b(eq, &miss);
  }

  __ mov(r0, r4);
  __ IncrementCounter(&Counters::named_load_global_stub, 1, r1, r3);
  __ Ret();

  __ bind(&miss);
  __ IncrementCounter(&Counters::named_load_global_stub_miss, 1, r1, r3);
  GenerateLoadMiss(masm(), Code::LOAD_IC);

  // Return the generated code.
  return GetCode(NORMAL, name);
}


MaybeObject* KeyedLoadStubCompiler::CompileLoadField(String* name,
                                                     JSObject* receiver,
                                                     JSObject* holder,
                                                     int index) {
  // ----------- S t a t e -------------
  //  -- lr    : return address
  //  -- r0    : key
  //  -- r1    : receiver
  // -----------------------------------
  Label miss;

  // Check the key is the cached one.
  __ cmp(r0, Operand(Handle<String>(name)));
  __ b(ne, &miss);

  GenerateLoadField(receiver, holder, r1, r2, r3, r4, index, name, &miss);
  __ bind(&miss);
  GenerateLoadMiss(masm(), Code::KEYED_LOAD_IC);

  return GetCode(FIELD, name);
}


MaybeObject* KeyedLoadStubCompiler::CompileLoadCallback(
    String* name,
    JSObject* receiver,
    JSObject* holder,
    AccessorInfo* callback) {
  // ----------- S t a t e -------------
  //  -- lr    : return address
  //  -- r0    : key
  //  -- r1    : receiver
  // -----------------------------------
  Label miss;

  // Check the key is the cached one.
  __ cmp(r0, Operand(Handle<String>(name)));
  __ b(ne, &miss);

  Failure* failure = Failure::InternalError();
  bool success = GenerateLoadCallback(receiver, holder, r1, r0, r2, r3, r4,
                                      callback, name, &miss, &failure);
  if (!success) {
    miss.Unuse();
    return failure;
  }

  __ bind(&miss);
  GenerateLoadMiss(masm(), Code::KEYED_LOAD_IC);

  return GetCode(CALLBACKS, name);
}


MaybeObject* KeyedLoadStubCompiler::CompileLoadConstant(String* name,
                                                        JSObject* receiver,
                                                        JSObject* holder,
                                                        Object* value) {
  // ----------- S t a t e -------------
  //  -- lr    : return address
  //  -- r0    : key
  //  -- r1    : receiver
  // -----------------------------------
  Label miss;

  // Check the key is the cached one.
  __ cmp(r0, Operand(Handle<String>(name)));
  __ b(ne, &miss);

  GenerateLoadConstant(receiver, holder, r1, r2, r3, r4, value, name, &miss);
  __ bind(&miss);
  GenerateLoadMiss(masm(), Code::KEYED_LOAD_IC);

  // Return the generated code.
  return GetCode(CONSTANT_FUNCTION, name);
}


MaybeObject* KeyedLoadStubCompiler::CompileLoadInterceptor(JSObject* receiver,
                                                           JSObject* holder,
                                                           String* name) {
  // ----------- S t a t e -------------
  //  -- lr    : return address
  //  -- r0    : key
  //  -- r1    : receiver
  // -----------------------------------
  Label miss;

  // Check the key is the cached one.
  __ cmp(r0, Operand(Handle<String>(name)));
  __ b(ne, &miss);

  LookupResult lookup;
  LookupPostInterceptor(holder, name, &lookup);
  GenerateLoadInterceptor(receiver,
                          holder,
                          &lookup,
                          r1,
                          r0,
                          r2,
                          r3,
                          r4,
                          name,
                          &miss);
  __ bind(&miss);
  GenerateLoadMiss(masm(), Code::KEYED_LOAD_IC);

  return GetCode(INTERCEPTOR, name);
}


MaybeObject* KeyedLoadStubCompiler::CompileLoadArrayLength(String* name) {
  // ----------- S t a t e -------------
  //  -- lr    : return address
  //  -- r0    : key
  //  -- r1    : receiver
  // -----------------------------------
  Label miss;

  // Check the key is the cached one.
  __ cmp(r0, Operand(Handle<String>(name)));
  __ b(ne, &miss);

  GenerateLoadArrayLength(masm(), r1, r2, &miss);
  __ bind(&miss);
  GenerateLoadMiss(masm(), Code::KEYED_LOAD_IC);

  return GetCode(CALLBACKS, name);
}


MaybeObject* KeyedLoadStubCompiler::CompileLoadStringLength(String* name) {
  // ----------- S t a t e -------------
  //  -- lr    : return address
  //  -- r0    : key
  //  -- r1    : receiver
  // -----------------------------------
  Label miss;
  __ IncrementCounter(&Counters::keyed_load_string_length, 1, r2, r3);

  // Check the key is the cached one.
  __ cmp(r0, Operand(Handle<String>(name)));
  __ b(ne, &miss);

  GenerateLoadStringLength(masm(), r1, r2, r3, &miss);
  __ bind(&miss);
  __ DecrementCounter(&Counters::keyed_load_string_length, 1, r2, r3);

  GenerateLoadMiss(masm(), Code::KEYED_LOAD_IC);

  return GetCode(CALLBACKS, name);
}


MaybeObject* KeyedLoadStubCompiler::CompileLoadFunctionPrototype(String* name) {
  // ----------- S t a t e -------------
  //  -- lr    : return address
  //  -- r0    : key
  //  -- r1    : receiver
  // -----------------------------------
  Label miss;

  __ IncrementCounter(&Counters::keyed_load_function_prototype, 1, r2, r3);

  // Check the name hasn't changed.
  __ cmp(r0, Operand(Handle<String>(name)));
  __ b(ne, &miss);

  GenerateLoadFunctionPrototype(masm(), r1, r2, r3, &miss);
  __ bind(&miss);
  __ DecrementCounter(&Counters::keyed_load_function_prototype, 1, r2, r3);
  GenerateLoadMiss(masm(), Code::KEYED_LOAD_IC);

  return GetCode(CALLBACKS, name);
}


MaybeObject* KeyedLoadStubCompiler::CompileLoadSpecialized(JSObject* receiver) {
  // ----------- S t a t e -------------
  //  -- lr    : return address
  //  -- r0    : key
  //  -- r1    : receiver
  // -----------------------------------
  Label miss;

  // Check that the receiver isn't a smi.
  __ tst(r1, Operand(kSmiTagMask));
  __ b(eq, &miss);

  // Check that the map matches.
  __ ldr(r2, FieldMemOperand(r1, HeapObject::kMapOffset));
  __ cmp(r2, Operand(Handle<Map>(receiver->map())));
  __ b(ne, &miss);

  // Check that the key is a smi.
  __ tst(r0, Operand(kSmiTagMask));
  __ b(ne, &miss);

  // Get the elements array.
  __ ldr(r2, FieldMemOperand(r1, JSObject::kElementsOffset));
  __ AssertFastElements(r2);

  // Check that the key is within bounds.
  __ ldr(r3, FieldMemOperand(r2, FixedArray::kLengthOffset));
  __ cmp(r0, Operand(r3));
  __ b(hs, &miss);

  // Load the result and make sure it's not the hole.
  __ add(r3, r2, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  ASSERT(kSmiTag == 0 && kSmiTagSize < kPointerSizeLog2);
  __ ldr(r4,
         MemOperand(r3, r0, LSL, kPointerSizeLog2 - kSmiTagSize));
  __ LoadRoot(ip, Heap::kTheHoleValueRootIndex);
  __ cmp(r4, ip);
  __ b(eq, &miss);
  __ mov(r0, r4);
  __ Ret();

  __ bind(&miss);
  GenerateLoadMiss(masm(), Code::KEYED_LOAD_IC);

  // Return the generated code.
  return GetCode(NORMAL, NULL);
}


MaybeObject* KeyedStoreStubCompiler::CompileStoreField(JSObject* object,
                                                       int index,
                                                       Map* transition,
                                                       String* name) {
  // ----------- S t a t e -------------
  //  -- r0    : value
  //  -- r1    : name
  //  -- r2    : receiver
  //  -- lr    : return address
  // -----------------------------------
  Label miss;

  __ IncrementCounter(&Counters::keyed_store_field, 1, r3, r4);

  // Check that the name has not changed.
  __ cmp(r1, Operand(Handle<String>(name)));
  __ b(ne, &miss);

  // r3 is used as scratch register. r1 and r2 keep their values if a jump to
  // the miss label is generated.
  GenerateStoreField(masm(),
                     object,
                     index,
                     transition,
                     r2, r1, r3,
                     &miss);
  __ bind(&miss);

  __ DecrementCounter(&Counters::keyed_store_field, 1, r3, r4);
  Handle<Code> ic(Builtins::builtin(Builtins::KeyedStoreIC_Miss));

  __ Jump(ic, RelocInfo::CODE_TARGET);

  // Return the generated code.
  return GetCode(transition == NULL ? FIELD : MAP_TRANSITION, name);
}


MaybeObject* KeyedStoreStubCompiler::CompileStoreSpecialized(
    JSObject* receiver) {
  // ----------- S t a t e -------------
  //  -- r0    : value
  //  -- r1    : key
  //  -- r2    : receiver
  //  -- lr    : return address
  //  -- r3    : scratch
  //  -- r4    : scratch (elements)
  // -----------------------------------
  Label miss;

  Register value_reg = r0;
  Register key_reg = r1;
  Register receiver_reg = r2;
  Register scratch = r3;
  Register elements_reg = r4;

  // Check that the receiver isn't a smi.
  __ tst(receiver_reg, Operand(kSmiTagMask));
  __ b(eq, &miss);

  // Check that the map matches.
  __ ldr(scratch, FieldMemOperand(receiver_reg, HeapObject::kMapOffset));
  __ cmp(scratch, Operand(Handle<Map>(receiver->map())));
  __ b(ne, &miss);

  // Check that the key is a smi.
  __ tst(key_reg, Operand(kSmiTagMask));
  __ b(ne, &miss);

  // Get the elements array and make sure it is a fast element array, not 'cow'.
  __ ldr(elements_reg,
         FieldMemOperand(receiver_reg, JSObject::kElementsOffset));
  __ ldr(scratch, FieldMemOperand(elements_reg, HeapObject::kMapOffset));
  __ cmp(scratch, Operand(Handle<Map>(Factory::fixed_array_map())));
  __ b(ne, &miss);

  // Check that the key is within bounds.
  if (receiver->IsJSArray()) {
    __ ldr(scratch, FieldMemOperand(receiver_reg, JSArray::kLengthOffset));
  } else {
    __ ldr(scratch, FieldMemOperand(elements_reg, FixedArray::kLengthOffset));
  }
  // Compare smis.
  __ cmp(key_reg, scratch);
  __ b(hs, &miss);

  __ add(scratch,
         elements_reg, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  ASSERT(kSmiTag == 0 && kSmiTagSize < kPointerSizeLog2);
  __ str(value_reg,
         MemOperand(scratch, key_reg, LSL, kPointerSizeLog2 - kSmiTagSize));
  __ RecordWrite(scratch,
                 Operand(key_reg, LSL, kPointerSizeLog2 - kSmiTagSize),
                 receiver_reg , elements_reg);

  // value_reg (r0) is preserved.
  // Done.
  __ Ret();

  __ bind(&miss);
  Handle<Code> ic(Builtins::builtin(Builtins::KeyedStoreIC_Miss));
  __ Jump(ic, RelocInfo::CODE_TARGET);

  // Return the generated code.
  return GetCode(NORMAL, NULL);
}


MaybeObject* ConstructStubCompiler::CompileConstructStub(JSFunction* function) {
  // ----------- S t a t e -------------
  //  -- r0    : argc
  //  -- r1    : constructor
  //  -- lr    : return address
  //  -- [sp]  : last argument
  // -----------------------------------
  Label generic_stub_call;

  // Use r7 for holding undefined which is used in several places below.
  __ LoadRoot(r7, Heap::kUndefinedValueRootIndex);

#ifdef ENABLE_DEBUGGER_SUPPORT
  // Check to see whether there are any break points in the function code. If
  // there are jump to the generic constructor stub which calls the actual
  // code for the function thereby hitting the break points.
  __ ldr(r2, FieldMemOperand(r1, JSFunction::kSharedFunctionInfoOffset));
  __ ldr(r2, FieldMemOperand(r2, SharedFunctionInfo::kDebugInfoOffset));
  __ cmp(r2, r7);
  __ b(ne, &generic_stub_call);
#endif

  // Load the initial map and verify that it is in fact a map.
  // r1: constructor function
  // r7: undefined
  __ ldr(r2, FieldMemOperand(r1, JSFunction::kPrototypeOrInitialMapOffset));
  __ tst(r2, Operand(kSmiTagMask));
  __ b(eq, &generic_stub_call);
  __ CompareObjectType(r2, r3, r4, MAP_TYPE);
  __ b(ne, &generic_stub_call);

#ifdef DEBUG
  // Cannot construct functions this way.
  // r0: argc
  // r1: constructor function
  // r2: initial map
  // r7: undefined
  __ CompareInstanceType(r2, r3, JS_FUNCTION_TYPE);
  __ Check(ne, "Function constructed by construct stub.");
#endif

  // Now allocate the JSObject in new space.
  // r0: argc
  // r1: constructor function
  // r2: initial map
  // r7: undefined
  __ ldrb(r3, FieldMemOperand(r2, Map::kInstanceSizeOffset));
  __ AllocateInNewSpace(r3,
                        r4,
                        r5,
                        r6,
                        &generic_stub_call,
                        SIZE_IN_WORDS);

  // Allocated the JSObject, now initialize the fields. Map is set to initial
  // map and properties and elements are set to empty fixed array.
  // r0: argc
  // r1: constructor function
  // r2: initial map
  // r3: object size (in words)
  // r4: JSObject (not tagged)
  // r7: undefined
  __ LoadRoot(r6, Heap::kEmptyFixedArrayRootIndex);
  __ mov(r5, r4);
  ASSERT_EQ(0 * kPointerSize, JSObject::kMapOffset);
  __ str(r2, MemOperand(r5, kPointerSize, PostIndex));
  ASSERT_EQ(1 * kPointerSize, JSObject::kPropertiesOffset);
  __ str(r6, MemOperand(r5, kPointerSize, PostIndex));
  ASSERT_EQ(2 * kPointerSize, JSObject::kElementsOffset);
  __ str(r6, MemOperand(r5, kPointerSize, PostIndex));

  // Calculate the location of the first argument. The stack contains only the
  // argc arguments.
  __ add(r1, sp, Operand(r0, LSL, kPointerSizeLog2));

  // Fill all the in-object properties with undefined.
  // r0: argc
  // r1: first argument
  // r3: object size (in words)
  // r4: JSObject (not tagged)
  // r5: First in-object property of JSObject (not tagged)
  // r7: undefined
  // Fill the initialized properties with a constant value or a passed argument
  // depending on the this.x = ...; assignment in the function.
  SharedFunctionInfo* shared = function->shared();
  for (int i = 0; i < shared->this_property_assignments_count(); i++) {
    if (shared->IsThisPropertyAssignmentArgument(i)) {
      Label not_passed, next;
      // Check if the argument assigned to the property is actually passed.
      int arg_number = shared->GetThisPropertyAssignmentArgument(i);
      __ cmp(r0, Operand(arg_number));
      __ b(le, &not_passed);
      // Argument passed - find it on the stack.
      __ ldr(r2, MemOperand(r1, (arg_number + 1) * -kPointerSize));
      __ str(r2, MemOperand(r5, kPointerSize, PostIndex));
      __ b(&next);
      __ bind(&not_passed);
      // Set the property to undefined.
      __ str(r7, MemOperand(r5, kPointerSize, PostIndex));
      __ bind(&next);
    } else {
      // Set the property to the constant value.
      Handle<Object> constant(shared->GetThisPropertyAssignmentConstant(i));
      __ mov(r2, Operand(constant));
      __ str(r2, MemOperand(r5, kPointerSize, PostIndex));
    }
  }

  // Fill the unused in-object property fields with undefined.
  ASSERT(function->has_initial_map());
  for (int i = shared->this_property_assignments_count();
       i < function->initial_map()->inobject_properties();
       i++) {
      __ str(r7, MemOperand(r5, kPointerSize, PostIndex));
  }

  // r0: argc
  // r4: JSObject (not tagged)
  // Move argc to r1 and the JSObject to return to r0 and tag it.
  __ mov(r1, r0);
  __ mov(r0, r4);
  __ orr(r0, r0, Operand(kHeapObjectTag));

  // r0: JSObject
  // r1: argc
  // Remove caller arguments and receiver from the stack and return.
  __ add(sp, sp, Operand(r1, LSL, kPointerSizeLog2));
  __ add(sp, sp, Operand(kPointerSize));
  __ IncrementCounter(&Counters::constructed_objects, 1, r1, r2);
  __ IncrementCounter(&Counters::constructed_objects_stub, 1, r1, r2);
  __ Jump(lr);

  // Jump to the generic stub in case the specialized code cannot handle the
  // construction.
  __ bind(&generic_stub_call);
  Code* code = Builtins::builtin(Builtins::JSConstructStubGeneric);
  Handle<Code> generic_construct_stub(code);
  __ Jump(generic_construct_stub, RelocInfo::CODE_TARGET);

  // Return the generated code.
  return GetCode();
}


#undef __

} }  // namespace v8::internal

#endif  // V8_TARGET_ARCH_ARM
