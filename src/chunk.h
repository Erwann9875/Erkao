#ifndef ERKAO_CHUNK_H
#define ERKAO_CHUNK_H

#include "value.h"

typedef enum {
  OP_CONSTANT,
  OP_NULL,
  OP_TRUE,
  OP_FALSE,
  OP_POP,
  OP_GET_VAR,
  OP_SET_VAR,
  OP_DEFINE_VAR,
  OP_GET_PROPERTY,
  OP_SET_PROPERTY,
  OP_GET_THIS,
  OP_GET_INDEX,
  OP_SET_INDEX,
  OP_EQUAL,
  OP_GREATER,
  OP_GREATER_EQUAL,
  OP_LESS,
  OP_LESS_EQUAL,
  OP_ADD,
  OP_SUBTRACT,
  OP_MULTIPLY,
  OP_DIVIDE,
  OP_NOT,
  OP_NEGATE,
  OP_JUMP,
  OP_JUMP_IF_FALSE,
  OP_LOOP,
  OP_CALL,
  OP_CLOSURE,
  OP_RETURN,
  OP_BEGIN_SCOPE,
  OP_END_SCOPE,
  OP_CLASS,
  OP_IMPORT,
  OP_ARRAY,
  OP_ARRAY_APPEND,
  OP_MAP,
  OP_MAP_SET,
  OP_GC
} OpCode;

typedef struct Chunk {
  int count;
  int capacity;
  uint8_t* code;
  Token* tokens;
  int constantsCount;
  int constantsCapacity;
  Value* constants;
} Chunk;

void initChunk(Chunk* chunk);
void freeChunk(Chunk* chunk);
Chunk* cloneChunk(const Chunk* chunk);
void writeChunk(Chunk* chunk, uint8_t byte, Token token);
int addConstant(Chunk* chunk, Value value);

#endif
