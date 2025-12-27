#include "disasm.h"
#include "value.h"

#include <stdio.h>
#include <string.h>

static void printLine(const Chunk* chunk, int offset) {
  if (!chunk->tokens) {
    printf("   | ");
    return;
  }
  Token token = chunk->tokens[offset];
  if (offset > 0 && token.line == chunk->tokens[offset - 1].line) {
    printf("   | ");
  } else if (token.line > 0) {
    printf("%4d ", token.line);
  } else {
    printf("   | ");
  }
}

static int simpleInstruction(const char* name, const Chunk* chunk, int offset) {
  (void)chunk;
  printf("%-16s\n", name);
  return offset + 1;
}

static int constantInstruction(const char* name, const Chunk* chunk, int offset) {
  uint16_t constant = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
  printf("%-16s %4u '", name, constant);
  if (constant < (uint16_t)chunk->constantsCount) {
    printValue(chunk->constants[constant]);
  } else {
    printf("<invalid>");
  }
  printf("'\n");
  return offset + 3;
}

static int shortInstruction(const char* name, const Chunk* chunk, int offset) {
  uint16_t value = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
  printf("%-16s %4u\n", name, value);
  return offset + 3;
}

static int byteInstruction(const char* name, const Chunk* chunk, int offset) {
  uint8_t value = chunk->code[offset + 1];
  printf("%-16s %4u\n", name, value);
  return offset + 2;
}

static int invokeInstruction(const char* name, const Chunk* chunk, int offset) {
  uint16_t constant = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
  uint8_t argc = chunk->code[offset + 3];
  printf("%-16s %4u argc=%u '", name, constant, argc);
  if (constant < (uint16_t)chunk->constantsCount) {
    printValue(chunk->constants[constant]);
  } else {
    printf("<invalid>");
  }
  printf("'\n");
  return offset + 4;
}

static int jumpInstruction(const char* name, int sign, const Chunk* chunk, int offset) {
  uint16_t jump = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
  int destination = offset + 3 + sign * (int)jump;
  printf("%-16s %4d -> %d\n", name, offset, destination);
  return offset + 3;
}

static int disassembleInstruction(const Chunk* chunk, int offset) {
  printf("%04d ", offset);
  printLine(chunk, offset);

  uint8_t instruction = chunk->code[offset];
  switch (instruction) {
    case OP_CONSTANT:
      return constantInstruction("OP_CONSTANT", chunk, offset);
    case OP_NULL:
      return simpleInstruction("OP_NULL", chunk, offset);
    case OP_TRUE:
      return simpleInstruction("OP_TRUE", chunk, offset);
    case OP_FALSE:
      return simpleInstruction("OP_FALSE", chunk, offset);
    case OP_POP:
      return simpleInstruction("OP_POP", chunk, offset);
    case OP_GET_VAR:
      return constantInstruction("OP_GET_VAR", chunk, offset);
    case OP_SET_VAR:
      return constantInstruction("OP_SET_VAR", chunk, offset);
    case OP_DEFINE_VAR:
      return constantInstruction("OP_DEFINE_VAR", chunk, offset);
    case OP_DEFINE_CONST:
      return constantInstruction("OP_DEFINE_CONST", chunk, offset);
    case OP_GET_PROPERTY:
      return constantInstruction("OP_GET_PROPERTY", chunk, offset);
    case OP_GET_PROPERTY_OPTIONAL:
      return constantInstruction("OP_GET_PROPERTY_OPTIONAL", chunk, offset);
    case OP_SET_PROPERTY:
      return constantInstruction("OP_SET_PROPERTY", chunk, offset);
    case OP_GET_THIS:
      return constantInstruction("OP_GET_THIS", chunk, offset);
    case OP_GET_INDEX:
      return simpleInstruction("OP_GET_INDEX", chunk, offset);
    case OP_SET_INDEX:
      return simpleInstruction("OP_SET_INDEX", chunk, offset);
    case OP_EQUAL:
      return simpleInstruction("OP_EQUAL", chunk, offset);
    case OP_GREATER:
      return simpleInstruction("OP_GREATER", chunk, offset);
    case OP_GREATER_EQUAL:
      return simpleInstruction("OP_GREATER_EQUAL", chunk, offset);
    case OP_LESS:
      return simpleInstruction("OP_LESS", chunk, offset);
    case OP_LESS_EQUAL:
      return simpleInstruction("OP_LESS_EQUAL", chunk, offset);
    case OP_ADD:
      return simpleInstruction("OP_ADD", chunk, offset);
    case OP_SUBTRACT:
      return simpleInstruction("OP_SUBTRACT", chunk, offset);
    case OP_MULTIPLY:
      return simpleInstruction("OP_MULTIPLY", chunk, offset);
    case OP_DIVIDE:
      return simpleInstruction("OP_DIVIDE", chunk, offset);
    case OP_NOT:
      return simpleInstruction("OP_NOT", chunk, offset);
    case OP_NEGATE:
      return simpleInstruction("OP_NEGATE", chunk, offset);
    case OP_STRINGIFY:
      return simpleInstruction("OP_STRINGIFY", chunk, offset);
    case OP_JUMP:
      return jumpInstruction("OP_JUMP", 1, chunk, offset);
    case OP_JUMP_IF_FALSE:
      return jumpInstruction("OP_JUMP_IF_FALSE", 1, chunk, offset);
    case OP_LOOP:
      return jumpInstruction("OP_LOOP", -1, chunk, offset);
    case OP_CALL:
      return byteInstruction("OP_CALL", chunk, offset);
    case OP_CALL_OPTIONAL:
      return byteInstruction("OP_CALL_OPTIONAL", chunk, offset);
    case OP_INVOKE:
      return invokeInstruction("OP_INVOKE", chunk, offset);
    case OP_ARG_COUNT:
      return simpleInstruction("OP_ARG_COUNT", chunk, offset);
    case OP_CLOSURE:
      return constantInstruction("OP_CLOSURE", chunk, offset);
    case OP_RETURN:
      return simpleInstruction("OP_RETURN", chunk, offset);
    case OP_BEGIN_SCOPE:
      return simpleInstruction("OP_BEGIN_SCOPE", chunk, offset);
    case OP_END_SCOPE:
      return simpleInstruction("OP_END_SCOPE", chunk, offset);
    case OP_CLASS: {
      uint16_t name = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
      uint16_t methods = (uint16_t)((chunk->code[offset + 3] << 8) | chunk->code[offset + 4]);
      printf("%-16s %4u methods=%u '", "OP_CLASS", name, methods);
      if (name < (uint16_t)chunk->constantsCount) {
        printValue(chunk->constants[name]);
      } else {
        printf("<invalid>");
      }
      printf("'\n");
      return offset + 5;
    }
    case OP_IMPORT: {
      uint8_t hasAlias = chunk->code[offset + 1];
      uint16_t alias = (uint16_t)((chunk->code[offset + 2] << 8) | chunk->code[offset + 3]);
      printf("%-16s hasAlias=%u alias=%u", "OP_IMPORT", hasAlias, alias);
      if (hasAlias && alias < (uint16_t)chunk->constantsCount) {
        printf(" '");
        printValue(chunk->constants[alias]);
        printf("'");
      }
      printf("\n");
      return offset + 4;
    }
    case OP_EXPORT:
      return constantInstruction("OP_EXPORT", chunk, offset);
    case OP_ARRAY:
      return shortInstruction("OP_ARRAY", chunk, offset);
    case OP_ARRAY_APPEND:
      return simpleInstruction("OP_ARRAY_APPEND", chunk, offset);
    case OP_MAP:
      return shortInstruction("OP_MAP", chunk, offset);
    case OP_MAP_SET:
      return simpleInstruction("OP_MAP_SET", chunk, offset);
    case OP_GC:
      return simpleInstruction("OP_GC", chunk, offset);
    default:
      printf("OP_UNKNOWN %d\n", instruction);
      return offset + 1;
  }
}

void disassembleChunk(const Chunk* chunk, const char* name) {
  printf("== %s ==\n", name ? name : "<chunk>");
  for (int offset = 0; offset < chunk->count;) {
    offset = disassembleInstruction(chunk, offset);
  }
}

void disassembleFunction(const ObjFunction* function) {
  if (!function || !function->chunk) return;
  const char* name = function->name ? function->name->chars : "<script>";
  disassembleChunk(function->chunk, name);
}
