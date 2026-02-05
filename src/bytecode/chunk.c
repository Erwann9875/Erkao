#include "chunk.h"

#include <limits.h>
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
    fprintf(stderr, "RuntimeError: Out of memory.\n");
    return NULL;
  }
  initChunk(copy);

  if (chunk->count > 0) {
    copy->capacity = chunk->count;
    copy->count = chunk->count;
    copy->code = GROW_ARRAY(uint8_t, copy->code, 0, copy->capacity);
    copy->tokens = GROW_ARRAY(Token, copy->tokens, 0, copy->capacity);
    copy->caches = GROW_ARRAY(InlineCache, copy->caches, 0, copy->capacity);
    if (!copy->code || !copy->tokens || !copy->caches) {
      fprintf(stderr, "RuntimeError: Out of memory.\n");
      freeChunk(copy);
      free(copy);
      return NULL;
    }
    memcpy(copy->code, chunk->code, (size_t)chunk->count);
    memcpy(copy->tokens, chunk->tokens, sizeof(Token) * (size_t)chunk->count);
    memset(copy->caches, 0, sizeof(InlineCache) * (size_t)copy->capacity);
  }

  if (chunk->constantsCount > 0) {
    copy->constantsCapacity = chunk->constantsCount;
    copy->constantsCount = chunk->constantsCount;
    copy->constants = GROW_ARRAY(Value, copy->constants, 0, copy->constantsCapacity);
    if (!copy->constants) {
      fprintf(stderr, "RuntimeError: Out of memory.\n");
      freeChunk(copy);
      free(copy);
      return NULL;
    }
    memcpy(copy->constants, chunk->constants, sizeof(Value) * (size_t)chunk->constantsCount);
  }

  return copy;
}

void writeChunk(Chunk* chunk, uint8_t byte, Token token) {
  if (chunk->capacity < chunk->count + 1) {
    int oldCapacity = chunk->capacity;
    int newCapacity = GROW_CAPACITY(oldCapacity);
    uint8_t* newCode = GROW_ARRAY(uint8_t, chunk->code, oldCapacity, newCapacity);
    if (!newCode) {
      fprintf(stderr, "RuntimeError: Out of memory.\n");
      return;
    }
    chunk->code = newCode;

    Token* newTokens = GROW_ARRAY(Token, chunk->tokens, oldCapacity, newCapacity);
    if (!newTokens) {
      fprintf(stderr, "RuntimeError: Out of memory.\n");
      return;
    }
    chunk->tokens = newTokens;

    InlineCache* newCaches = GROW_ARRAY(InlineCache, chunk->caches, oldCapacity, newCapacity);
    if (!newCaches) {
      fprintf(stderr, "RuntimeError: Out of memory.\n");
      return;
    }
    chunk->caches = newCaches;
    chunk->capacity = newCapacity;
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
    Value* newConstants = GROW_ARRAY(Value, chunk->constants, oldCapacity, chunk->constantsCapacity);
    if (!newConstants) {
      fprintf(stderr, "RuntimeError: Out of memory.\n");
      chunk->constantsCapacity = oldCapacity;
      return INT_MAX;
    }
    chunk->constants = newConstants;
  }
  chunk->constants[chunk->constantsCount] = value;
  return chunk->constantsCount++;
}
