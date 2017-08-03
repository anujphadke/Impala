// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <boost/algorithm/string.hpp>

#include "codegen/llvm-codegen.h"
#include "runtime/descriptors.h"
#include "runtime/mem-pool.h"
#include "runtime/runtime-state.h"
#include "runtime/string-value.h"
#include "runtime/timestamp-value.h"
#include "runtime/tuple.h"
#include "text-converter.h"
#include "util/string-parser.h"
#include "util/runtime-profile-counters.h"

#include "common/names.h"

using namespace impala;
using namespace llvm;

TextConverter::TextConverter(char escape_char, const string& null_col_val,
    bool check_null, bool strict_mode)
  : escape_char_(escape_char),
    null_col_val_(null_col_val),
    check_null_(check_null),
    strict_mode_(strict_mode) {
}

void TextConverter::UnescapeString(const char* src, char* dest, int* len,
    int64_t maxlen) {
  const char* src_end = src + *len;
  char* dest_end = dest + *len;
  if (maxlen > 0) dest_end = dest + maxlen;
  char* dest_ptr = dest;
  bool escape_next_char = false;

  while ((src < src_end) && (dest_ptr < dest_end)) {
    if (*src == escape_char_) {
      escape_next_char = !escape_next_char;
    } else {
      escape_next_char = false;
    }
    if (escape_next_char) {
      ++src;
    } else {
      *dest_ptr++ = *src++;
    }
  }
  char* dest_start = reinterpret_cast<char*>(dest);
  *len = dest_ptr - dest_start;
}

// Codegen for a function to parse one slot.  The IR for a int slot looks like:
// define i1 @WriteSlot({ i8, i32 }* %tuple_arg, i8* %data, i32 %len) {
// entry:
//   %parse_result = alloca i32
//   %0 = call i1 @IsNullString(i8* %data, i32 %len)
//   br i1 %0, label %set_null, label %check_zero
//
// set_null:                                         ; preds = %check_zero, %entry
//   call void @SetNull({ i8, i32 }* %tuple_arg)
//   ret i1 true
//
// parse_slot:                                       ; preds = %check_zero
//   %slot = getelementptr inbounds { i8, i32 }* %tuple_arg, i32 0, i32 1
//   %1 = call i32 @IrStringToInt32(i8* %data, i32 %len, i32* %parse_result)
//   %parse_result1 = load i32* %parse_result
//   %failed = icmp eq i32 %parse_result1, 1
//   br i1 %failed, label %parse_fail, label %parse_success
//
// check_zero:                                       ; preds = %entry
//   %2 = icmp eq i32 %len, 0
//   br i1 %2, label %set_null, label %parse_slot
//
// parse_success:                                    ; preds = %parse_slot
//   store i32 %1, i32* %slot
//   ret i1 true
//
// parse_fail:                                       ; preds = %parse_slot
//   call void @SetNull({ i8, i32 }* %tuple_arg)
//   ret i1 false
// }
//
// If strict_mode = true, then 'parse_slot' also treats overflows errors, e.g.:
// parse_slot:                                       ; preds = %check_zero
//   %slot = getelementptr inbounds { i8, i32 }* %tuple_arg, i32 0, i32 1
//   %1 = call i32 @IrStringToInt32(i8* %data, i32 %len, i32* %parse_result)
//   %parse_result1 = load i32, i32* %parse_result
//   %failed = icmp eq i32 %parse_result1, 1
//   %overflowed = icmp eq i32 %parse_result1, 2
//   %failed_or = or i1 %failed, %overflowed
//   br i1 %failed_or, label %parse_fail, label %parse_success
Status TextConverter::CodegenWriteSlot(LlvmCodeGen* codegen,
    TupleDescriptor* tuple_desc, SlotDescriptor* slot_desc, Function* fn,
    const char* null_col_val, int len, bool check_null, bool strict_mode) {

  if (slot_desc->type().type == TYPE_CHAR) {
    return Status("Char isn't supported for CodegenWriteSlot @@@@@@@@@@@");
  }
  SCOPED_TIMER(codegen->codegen_timer());

  // Codegen is_null_string
  bool is_default_null = (len == 2 && null_col_val[0] == '\\' && null_col_val[1] == 'N');
  Function* is_null_string_fn;
  if (is_default_null) {
    is_null_string_fn = codegen->GetFunction(IRFunction::IS_NULL_STRING, false);
  } else {
    is_null_string_fn = codegen->GetFunction(IRFunction::GENERIC_IS_NULL_STRING, false);
  }
  if (is_null_string_fn == NULL) {
    return Status("TextConverter::CodegenWriteSlot: Failed to find IRFunction for "
       "a null string");
  }

  StructType* tuple_type = tuple_desc->GetLlvmStruct(codegen);
  if (tuple_type == NULL) {
    return Status("TextConverter::CodegenWriteSlot: Failed to generate "
        "intermediate tuple type");
  }
  PointerType* tuple_ptr_type = tuple_type->getPointerTo();

  LlvmCodeGen::FnPrototype prototype(
      codegen, "WriteSlot", codegen->GetType(TYPE_BOOLEAN));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("tuple_arg", tuple_ptr_type));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("data", codegen->ptr_type()));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("len", codegen->GetType(TYPE_INT)));

  LlvmBuilder builder(codegen->context());
  Value* args[3];
  fn = prototype.GeneratePrototype(&builder, &args[0]);

  BasicBlock* set_null_block, *parse_slot_block, *check_zero_block = NULL;
  codegen->CreateIfElseBlocks(fn, "set_null", "parse_slot",
      &set_null_block, &parse_slot_block);

  if (!slot_desc->type().IsVarLenStringType()) {
    check_zero_block = BasicBlock::Create(codegen->context(), "check_zero", fn);
  }

  // Check if the data matches the configured NULL string.
  Value* is_null;
  if (check_null) {
    if (is_default_null) {
      is_null = builder.CreateCall(is_null_string_fn,
          ArrayRef<Value*>({args[1], args[2]}));
    } else {
      is_null = builder.CreateCall(is_null_string_fn, ArrayRef<Value*>({args[1], args[2],
          codegen->CastPtrToLlvmPtr(codegen->ptr_type(), const_cast<char*>(null_col_val)),
          codegen->GetIntConstant(TYPE_INT, len)}));
    }
  } else {
    // Constant FALSE as branch condition. We rely on later optimization passes
    // to remove the branch and THEN block.
    is_null = codegen->false_value();
  }
  builder.CreateCondBr(is_null, set_null_block,
      (slot_desc->type().IsVarLenStringType()) ? parse_slot_block : check_zero_block);

  if (!slot_desc->type().IsVarLenStringType()) {
    builder.SetInsertPoint(check_zero_block);
    // If len == 0 and it is not a string col, set slot to NULL
    Value* null_len = builder.CreateICmpEQ(
        args[2], codegen->GetIntConstant(TYPE_INT, 0));
    builder.CreateCondBr(null_len, set_null_block, parse_slot_block);
  }

  // Codegen parse slot block
  builder.SetInsertPoint(parse_slot_block);
  Value* slot = builder.CreateStructGEP(NULL, args[0], slot_desc->llvm_field_idx(), "slot");

  if (slot_desc->type().IsVarLenStringType()) {
    Value* ptr = builder.CreateStructGEP(NULL, slot, 0, "string_ptr");
    Value* len = builder.CreateStructGEP(NULL, slot, 1, "string_len");

    builder.CreateStore(args[1], ptr);
    // TODO codegen memory allocation for CHAR
    DCHECK(slot_desc->type().type != TYPE_CHAR);
    if (slot_desc->type().type == TYPE_VARCHAR) {
      // determine if we need to truncate the string
      Value* maxlen = codegen->GetIntConstant(TYPE_INT, slot_desc->type().len);
      Value* len_lt_maxlen = builder.CreateICmpSLT(args[2], maxlen, "len_lt_maxlen");
      Value* minlen = builder.CreateSelect(len_lt_maxlen, args[2], maxlen,
                                           "select_min_len");
      builder.CreateStore(minlen, len);
    } else {
      builder.CreateStore(args[2], len);
    }
    builder.CreateRet(codegen->true_value());
  } else {
    IRFunction::Type parse_fn_enum;
    Function* parse_fn = NULL;
    switch (slot_desc->type().type) {
      case TYPE_BOOLEAN:
        parse_fn_enum = IRFunction::STRING_TO_BOOL;
        break;
      case TYPE_TINYINT:
        parse_fn_enum = IRFunction::STRING_TO_INT8;
        break;
      case TYPE_SMALLINT:
        parse_fn_enum = IRFunction::STRING_TO_INT16;
        break;
      case TYPE_INT:
        parse_fn_enum = IRFunction::STRING_TO_INT32;
        break;
      case TYPE_BIGINT:
        parse_fn_enum = IRFunction::STRING_TO_INT64;
        break;
      case TYPE_FLOAT:
        parse_fn_enum = IRFunction::STRING_TO_FLOAT;
        break;
      case TYPE_DOUBLE:
        parse_fn_enum = IRFunction::STRING_TO_DOUBLE;
        break;
      default:
        DCHECK(false);
        return Status("TextConverter::CodegenWriteSlot: Failed to codegen since "
           "it could not determine the slot_desc type");
    }
    parse_fn = codegen->GetFunction(parse_fn_enum, false);
    DCHECK(parse_fn != NULL);

    // Set up trying to parse the string to the slot type
    BasicBlock* parse_success_block, *parse_failed_block;
    codegen->CreateIfElseBlocks(fn, "parse_success", "parse_fail",
        &parse_success_block, &parse_failed_block);
    LlvmCodeGen::NamedVariable parse_result("parse_result", codegen->GetType(TYPE_INT));
    Value* parse_result_ptr = codegen->CreateEntryBlockAlloca(fn, parse_result);

    // Call Impala's StringTo* function
    Value* result = builder.CreateCall(parse_fn,
        ArrayRef<Value*>({args[1], args[2], parse_result_ptr}));
    Value* parse_result_val = builder.CreateLoad(parse_result_ptr, "parse_result");
    Value* failed_value = codegen->GetIntConstant(TYPE_INT, StringParser::PARSE_FAILURE);

    // Check for parse error.
    Value* parse_failed = builder.CreateICmpEQ(parse_result_val, failed_value, "failed");
    if (strict_mode) {
      // In strict_mode, also check if parse_result is PARSE_OVERFLOW.
      Value* overflow_value = codegen->GetIntConstant(TYPE_INT,
          StringParser::PARSE_OVERFLOW);
      Value* parse_overflow = builder.CreateICmpEQ(parse_result_val, overflow_value,
          "overflowed");
      parse_failed = builder.CreateOr(parse_failed, parse_overflow, "failed_or");
    }
    builder.CreateCondBr(parse_failed, parse_failed_block, parse_success_block);

    // Parse succeeded
    builder.SetInsertPoint(parse_success_block);
    builder.CreateStore(result, slot);
    builder.CreateRet(codegen->true_value());

    // Parse failed, set slot to null and return false
    builder.SetInsertPoint(parse_failed_block);
    slot_desc->CodegenSetNullIndicator(codegen, &builder, args[0], codegen->true_value());
    builder.CreateRet(codegen->false_value());
  }

  // Case where data is \N or len == 0 and it is not a string col
  builder.SetInsertPoint(set_null_block);
  slot_desc->CodegenSetNullIndicator(codegen, &builder, args[0], codegen->true_value());
  builder.CreateRet(codegen->true_value());

  if (codegen->FinalizeFunction(fn) == NULL) {
    Status("TextConverter::CodegenWriteSlot:codegen'd "
       "WriteSlot function failed verification");
  }
  return Status::OK();
}
