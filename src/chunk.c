#include "chunk.h"

#include <stdlib.h>
#include <string.h>

void initChunk(Chunk* chunk) {
  chunk->count = 0;
  chunk->capacity = 0;
  chunk->code = NULL;
  chunk->tokens = NULL;
  chunk->constantsCount = 0;
  chunk->constantsCapacity = 0;
  chunk->constants = NULL;
}

void freeChunk(Chunk* chunk) {
  FREE_ARRAY(uint8_t, chunk->code, chunk->capacity);
  FREE_ARRAY(Token, chunk->tokens, chunk->capacity);
  FREE_ARRAY(Value, chunk->constants, chunk->constantsCapacity);
  initChunk(chunk);
}

Chunk* cloneChunk(const Chunk* chunk) {
  Chunk* copy = (Chunk*)malloc(sizeof(Chunk));
  if (!copy) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  initChunk(copy);

  if (chunk->count > 0) {
    copy->capacity = chunk->count;
    copy->count = chunk->count;
    copy->code = GROW_ARRAY(uint8_t, copy->code, 0, copy->capacity);
    copy->tokens = GROW_ARRAY(Token, copy->tokens, 0, copy->capacity);
    memcpy(copy->code, chunk->code, (size_t)chunk->count);
    memcpy(copy->tokens, chunk->tokens, sizeof(Token) * (size_t)chunk->count);
  }

  if (chunk->constantsCount > 0) {
    copy->constantsCapacity = chunk->constantsCount;
    copy->constantsCount = chunk->constantsCount;
    copy->constants = GROW_ARRAY(Value, copy->constants, 0, copy->constantsCapacity);
    memcpy(copy->constants, chunk->constants, sizeof(Value) * (size_t)chunk->constantsCount);
  }

  return copy;
}

void writeChunk(Chunk* chunk, uint8_t byte, Token token) {
  if (chunk->capacity < chunk->count + 1) {
    int oldCapacity = chunk->capacity;
    chunk->capacity = GROW_CAPACITY(oldCapacity);
    chunk->code = GROW_ARRAY(uint8_t, chunk->code, oldCapacity, chunk->capacity);
    chunk->tokens = GROW_ARRAY(Token, chunk->tokens, oldCapacity, chunk->capacity);
  }
  chunk->code[chunk->count] = byte;
  chunk->tokens[chunk->count] = token;
  chunk->count++;
}

int addConstant(Chunk* chunk, Value value) {
  if (chunk->constantsCapacity < chunk->constantsCount + 1) {
    int oldCapacity = chunk->constantsCapacity;
    chunk->constantsCapacity = GROW_CAPACITY(oldCapacity);
    chunk->constants = GROW_ARRAY(Value, chunk->constants, oldCapacity, chunk->constantsCapacity);
  }
  chunk->constants[chunk->constantsCount] = value;
  return chunk->constantsCount++;
}
