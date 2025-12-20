#include "chunk.h"

#include <stdlib.h>
#include <string.h>

void initChunk(Chunk* chunk) {
  chunk->count = 0;
  chunk->capacity = 0;
  chunk->code = NULL;
  chunk->tokens = NULL;
  chunk->caches = NULL;
  chunk->constantsCount = 0;
  chunk->constantsCapacity = 0;
  chunk->constants = NULL;
}

void freeChunk(Chunk* chunk) {
  FREE_ARRAY(uint8_t, chunk->code, chunk->capacity);
  FREE_ARRAY(Token, chunk->tokens, chunk->capacity);
  FREE_ARRAY(InlineCache, chunk->caches, chunk->capacity);
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
    copy->caches = GROW_ARRAY(InlineCache, copy->caches, 0, copy->capacity);
    memcpy(copy->code, chunk->code, (size_t)chunk->count);
    memcpy(copy->tokens, chunk->tokens, sizeof(Token) * (size_t)chunk->count);
    memset(copy->caches, 0, sizeof(InlineCache) * (size_t)copy->capacity);
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
    chunk->caches = GROW_ARRAY(InlineCache, chunk->caches, oldCapacity, chunk->capacity);
    if (chunk->caches) {
      memset(chunk->caches + oldCapacity, 0,
             sizeof(InlineCache) * (size_t)(chunk->capacity - oldCapacity));
    }
  }
  chunk->code[chunk->count] = byte;
  chunk->tokens[chunk->count] = token;
  if (chunk->caches) {
    memset(&chunk->caches[chunk->count], 0, sizeof(InlineCache));
  }
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
