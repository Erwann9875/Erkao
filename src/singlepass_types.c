#include "singlepass_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

bool typeNamesEqual(ObjString* a, ObjString* b);

static Type TYPE_ANY_VALUE = { TYPE_ANY, NULL, NULL, NULL, NULL, NULL, 0, NULL,
                               NULL, 0, NULL, 0, NULL, 0, false };
static Type TYPE_UNKNOWN_VALUE = { TYPE_UNKNOWN, NULL, NULL, NULL, NULL, NULL, 0, NULL,
                                   NULL, 0, NULL, 0, NULL, 0, false };
static Type TYPE_NUMBER_VALUE = { TYPE_NUMBER, NULL, NULL, NULL, NULL, NULL, 0, NULL,
                                  NULL, 0, NULL, 0, NULL, 0, false };
static Type TYPE_STRING_VALUE = { TYPE_STRING, NULL, NULL, NULL, NULL, NULL, 0, NULL,
                                  NULL, 0, NULL, 0, NULL, 0, false };
static Type TYPE_BOOL_VALUE = { TYPE_BOOL, NULL, NULL, NULL, NULL, NULL, 0, NULL,
                                NULL, 0, NULL, 0, NULL, 0, false };
static Type TYPE_NULL_VALUE = { TYPE_NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL,
                                NULL, 0, NULL, 0, NULL, 0, false };

TypeRegistry* gTypeRegistry = NULL;

bool typecheckEnabled(Compiler* c) {
  return c->typecheck && c->typecheck->enabled;
}

Type* typeAny(void) { return &TYPE_ANY_VALUE; }
Type* typeUnknown(void) { return &TYPE_UNKNOWN_VALUE; }
Type* typeNumber(void) { return &TYPE_NUMBER_VALUE; }
Type* typeString(void) { return &TYPE_STRING_VALUE; }
Type* typeBool(void) { return &TYPE_BOOL_VALUE; }
Type* typeNull(void) { return &TYPE_NULL_VALUE; }

Type* typeAlloc(TypeChecker* tc, TypeKind kind) {
  if (!tc) return typeAny();
  Type* type = (Type*)malloc(sizeof(Type));
  if (!type) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }
  memset(type, 0, sizeof(Type));
  type->kind = kind;
  if (tc->allocatedCount >= tc->allocatedCapacity) {
    int oldCap = tc->allocatedCapacity;
    tc->allocatedCapacity = GROW_CAPACITY(oldCap);
    tc->allocated = GROW_ARRAY(Type*, tc->allocated, oldCap, tc->allocatedCapacity);
    if (!tc->allocated) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  tc->allocated[tc->allocatedCount++] = type;
  return type;
}

  void typeCheckerInit(TypeChecker* tc, TypeChecker* enclosing, bool enabled) {
    tc->enabled = enabled;
    tc->errorCount = 0;
    tc->scopeDepth = 0;
    tc->enclosing = enclosing;
    tc->entries = NULL;
    tc->count = 0;
    tc->capacity = 0;
    tc->aliases = NULL;
    tc->aliasCount = 0;
    tc->aliasCapacity = 0;
    tc->stack = NULL;
    tc->stackCount = 0;
    tc->stackCapacity = 0;
    tc->allocated = NULL;
    tc->allocatedCount = 0;
    tc->allocatedCapacity = 0;
    tc->currentReturn = NULL;
    tc->typeParams = NULL;
    tc->typeParamCount = 0;
    tc->typeParamCapacity = 0;
  }

  void typeCheckerFree(TypeChecker* tc) {
    if (!tc) return;
    for (int i = 0; i < tc->allocatedCount; i++) {
      if (tc->allocated[i]) {
        if (tc->allocated[i]->params) {
          free(tc->allocated[i]->params);
        }
        if (tc->allocated[i]->typeArgs) {
          free(tc->allocated[i]->typeArgs);
        }
        if (tc->allocated[i]->typeParams) {
          free(tc->allocated[i]->typeParams);
        }
        if (tc->allocated[i]->unionTypes) {
          free(tc->allocated[i]->unionTypes);
        }
        free(tc->allocated[i]);
      }
    }
    FREE_ARRAY(Type*, tc->allocated, tc->allocatedCapacity);
    FREE_ARRAY(TypeEntry, tc->entries, tc->capacity);
    FREE_ARRAY(TypeAlias, tc->aliases, tc->aliasCapacity);
    FREE_ARRAY(Type*, tc->stack, tc->stackCapacity);
    FREE_ARRAY(TypeParam, tc->typeParams, tc->typeParamCapacity);
    tc->allocated = NULL;
    tc->allocatedCount = 0;
    tc->allocatedCapacity = 0;
    tc->entries = NULL;
    tc->count = 0;
    tc->capacity = 0;
    tc->aliases = NULL;
    tc->aliasCount = 0;
    tc->aliasCapacity = 0;
    tc->stack = NULL;
    tc->stackCount = 0;
    tc->stackCapacity = 0;
    tc->typeParams = NULL;
    tc->typeParamCount = 0;
    tc->typeParamCapacity = 0;
  }

  void typeRegistryInit(TypeRegistry* registry) {
    registry->interfaces = NULL;
    registry->interfaceCount = 0;
    registry->interfaceCapacity = 0;
    registry->classes = NULL;
    registry->classCount = 0;
    registry->classCapacity = 0;
  }

  void typeRegistryFree(TypeRegistry* registry) {
    if (!registry) return;
    for (int i = 0; i < registry->interfaceCount; i++) {
      InterfaceDef* def = &registry->interfaces[i];
      if (def->methods) {
        free(def->methods);
        def->methods = NULL;
      }
      if (def->typeParams) {
        free(def->typeParams);
        def->typeParams = NULL;
      }
    }
    for (int i = 0; i < registry->classCount; i++) {
      ClassDef* def = &registry->classes[i];
      if (def->interfaces) {
        free(def->interfaces);
        def->interfaces = NULL;
      }
    }
    free(registry->interfaces);
    free(registry->classes);
    registry->interfaces = NULL;
    registry->classes = NULL;
    registry->interfaceCount = 0;
    registry->interfaceCapacity = 0;
    registry->classCount = 0;
    registry->classCapacity = 0;
  }

  InterfaceDef* typeRegistryFindInterface(TypeRegistry* registry, ObjString* name) {
    if (!registry || !name) return NULL;
    for (int i = 0; i < registry->interfaceCount; i++) {
      if (typeNamesEqual(registry->interfaces[i].name, name)) {
        return &registry->interfaces[i];
      }
    }
    return NULL;
  }

  ClassDef* typeRegistryFindClass(TypeRegistry* registry, ObjString* name) {
    if (!registry || !name) return NULL;
    for (int i = 0; i < registry->classCount; i++) {
      if (typeNamesEqual(registry->classes[i].name, name)) {
        return &registry->classes[i];
      }
    }
    return NULL;
  }

  void typeRegistryAddInterface(TypeRegistry* registry, const InterfaceDef* def) {
    if (!registry || !def) return;
    if (registry->interfaceCount >= registry->interfaceCapacity) {
      int oldCap = registry->interfaceCapacity;
      registry->interfaceCapacity = GROW_CAPACITY(oldCap);
      registry->interfaces = GROW_ARRAY(InterfaceDef, registry->interfaces,
                                        oldCap, registry->interfaceCapacity);
      if (!registry->interfaces) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
      }
    }
    registry->interfaces[registry->interfaceCount++] = *def;
  }

void typeRegistryAddClass(TypeRegistry* registry, ObjString* name,
                                   ObjString** interfaces, int interfaceCount) {
    if (!registry || !name) return;
    ClassDef* existing = typeRegistryFindClass(registry, name);
    if (existing) {
      if (existing->interfaces) free(existing->interfaces);
      existing->interfaces = interfaces;
      existing->interfaceCount = interfaceCount;
      existing->interfaceCapacity = interfaceCount;
      return;
    }
    if (registry->classCount >= registry->classCapacity) {
      int oldCap = registry->classCapacity;
      registry->classCapacity = GROW_CAPACITY(oldCap);
      registry->classes = GROW_ARRAY(ClassDef, registry->classes,
                                     oldCap, registry->classCapacity);
      if (!registry->classes) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
      }
    }
    ClassDef* def = &registry->classes[registry->classCount++];
    def->name = name;
    def->interfaces = interfaces;
    def->interfaceCount = interfaceCount;
    def->interfaceCapacity = interfaceCount;
  }

bool typeRegistryClassImplements(TypeRegistry* registry, ObjString* className,
                                          ObjString* interfaceName) {
    if (!registry || !className || !interfaceName) return false;
    ClassDef* def = typeRegistryFindClass(registry, className);
    if (!def) return false;
    for (int i = 0; i < def->interfaceCount; i++) {
      if (typeNamesEqual(def->interfaces[i], interfaceName)) {
        return true;
      }
    }
    return false;
  }

  void typeParamsEnsure(TypeChecker* tc, int needed) {
    if (!tc) return;
    if (tc->typeParamCount + needed <= tc->typeParamCapacity) return;
    int oldCap = tc->typeParamCapacity;
    while (tc->typeParamCount + needed > tc->typeParamCapacity) {
      tc->typeParamCapacity = GROW_CAPACITY(tc->typeParamCapacity);
    }
    tc->typeParams = GROW_ARRAY(TypeParam, tc->typeParams, oldCap, tc->typeParamCapacity);
    if (!tc->typeParams) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }

  void typeParamsPushList(TypeChecker* tc, const TypeParam* params, int count) {
    if (!tc || !params || count <= 0) return;
    typeParamsEnsure(tc, count);
    for (int i = 0; i < count; i++) {
      tc->typeParams[tc->typeParamCount++] = params[i];
    }
  }

  void typeParamsTruncate(TypeChecker* tc, int count) {
    if (!tc) return;
    if (count < 0) count = 0;
    if (count > tc->typeParamCount) count = tc->typeParamCount;
    tc->typeParamCount = count;
  }

  TypeParam* typeParamFindToken(TypeChecker* tc, Token token) {
    if (!tc) return NULL;
    for (TypeChecker* cur = tc; cur != NULL; cur = cur->enclosing) {
      for (int i = cur->typeParamCount - 1; i >= 0; i--) {
        TypeParam* param = &cur->typeParams[i];
        if (!param->name) continue;
        if (param->name->length == token.length &&
            memcmp(param->name->chars, token.start, (size_t)token.length) == 0) {
          return param;
        }
      }
    }
    return NULL;
  }

void typePush(Compiler* c, Type* type) {
  if (!typecheckEnabled(c)) return;
  TypeChecker* tc = c->typecheck;
  if (tc->stackCount >= tc->stackCapacity) {
    int oldCap = tc->stackCapacity;
    tc->stackCapacity = GROW_CAPACITY(oldCap);
    tc->stack = GROW_ARRAY(Type*, tc->stack, oldCap, tc->stackCapacity);
    if (!tc->stack) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  tc->stack[tc->stackCount++] = type;
}

Type* typePop(Compiler* c) {
  if (!typecheckEnabled(c)) return typeAny();
  TypeChecker* tc = c->typecheck;
  if (tc->stackCount <= 0) return typeAny();
  return tc->stack[--tc->stackCount];
}

bool typeIsAny(Type* type) {
  if (!type) return true;
  return type->kind == TYPE_ANY || type->kind == TYPE_UNKNOWN || type->kind == TYPE_GENERIC;
}

bool typeIsNullable(Type* type) {
  if (!type) return false;
  if (type->kind == TYPE_NULL) return true;
  if (type->kind == TYPE_UNION) {
    if (type->nullable) return true;
    for (int i = 0; i < type->unionCount; i++) {
      if (typeIsNullable(type->unionTypes[i])) return true;
    }
    return false;
  }
  return type->nullable;
}

Type* typeClone(TypeChecker* tc, Type* src) {
  if (!src) return typeAny();
  switch (src->kind) {
    case TYPE_ANY:
      return typeAny();
    case TYPE_UNKNOWN:
      return typeUnknown();
    case TYPE_NUMBER: {
      if (!src->nullable) return typeNumber();
      Type* type = typeAlloc(tc, TYPE_NUMBER);
      type->nullable = true;
      return type;
    }
    case TYPE_STRING: {
      if (!src->nullable) return typeString();
      Type* type = typeAlloc(tc, TYPE_STRING);
      type->nullable = true;
      return type;
    }
    case TYPE_BOOL: {
      if (!src->nullable) return typeBool();
      Type* type = typeAlloc(tc, TYPE_BOOL);
      type->nullable = true;
      return type;
    }
    case TYPE_NULL:
      return typeNull();
      case TYPE_NAMED: {
        Type* type = typeAlloc(tc, TYPE_NAMED);
        type->name = src->name;
        type->nullable = src->nullable;
        if (src->typeArgCount > 0 && src->typeArgs) {
          type->typeArgCount = src->typeArgCount;
          type->typeArgs = (Type**)malloc(sizeof(Type*) * (size_t)src->typeArgCount);
          if (!type->typeArgs) {
            fprintf(stderr, "Out of memory.\n");
            exit(1);
          }
          for (int i = 0; i < src->typeArgCount; i++) {
            type->typeArgs[i] = typeClone(tc, src->typeArgs[i]);
          }
        }
        return type;
      }
      case TYPE_GENERIC: {
        Type* type = typeAlloc(tc, TYPE_GENERIC);
        type->name = src->name;
        type->nullable = src->nullable;
        return type;
      }
      case TYPE_ARRAY: {
        Type* type = typeAlloc(tc, TYPE_ARRAY);
        type->elem = typeClone(tc, src->elem);
        type->nullable = src->nullable;
        return type;
    }
    case TYPE_MAP: {
      Type* type = typeAlloc(tc, TYPE_MAP);
      type->key = typeClone(tc, src->key);
      type->value = typeClone(tc, src->value);
      type->nullable = src->nullable;
      return type;
    }
    case TYPE_UNION: {
      Type* type = typeAlloc(tc, TYPE_UNION);
      type->unionCount = src->unionCount;
      type->nullable = src->nullable;
      if (src->unionCount > 0 && src->unionTypes) {
        type->unionTypes = (Type**)malloc(sizeof(Type*) * (size_t)src->unionCount);
        if (!type->unionTypes) {
          fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
        for (int i = 0; i < src->unionCount; i++) {
          type->unionTypes[i] = typeClone(tc, src->unionTypes[i]);
        }
      }
      return type;
    }
      case TYPE_FUNCTION: {
        Type* type = typeAlloc(tc, TYPE_FUNCTION);
        type->paramCount = src->paramCount;
        type->returnType = typeClone(tc, src->returnType);
        type->nullable = src->nullable;
        if (src->typeParamCount > 0 && src->typeParams) {
          type->typeParamCount = src->typeParamCount;
          type->typeParams = (TypeParam*)malloc(sizeof(TypeParam) * (size_t)src->typeParamCount);
          if (!type->typeParams) {
            fprintf(stderr, "Out of memory.\n");
            exit(1);
          }
          for (int i = 0; i < src->typeParamCount; i++) {
            type->typeParams[i] = src->typeParams[i];
          }
        }
        if (src->paramCount > 0 && src->params) {
          type->params = (Type**)malloc(sizeof(Type*) * (size_t)src->paramCount);
          if (!type->params) {
            fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
        for (int i = 0; i < src->paramCount; i++) {
          type->params[i] = typeClone(tc, src->params[i]);
        }
      }
      return type;
    }
  }
  return typeAny();
}

Type* typeMakeNullable(TypeChecker* tc, Type* type) {
  if (!type) return typeAny();
  if (type->kind == TYPE_ANY) return typeAny();
  if (type->kind == TYPE_UNKNOWN) return typeUnknown();
  if (type->kind == TYPE_NULL) return typeNull();
  if (type->kind == TYPE_UNION) {
    if (typeIsNullable(type)) return type;
    Type* copy = typeClone(tc, type);
    if (copy) copy->nullable = true;
    return copy;
  }
  if (type->nullable) return type;
  if (!tc) return type;
  if (type->kind == TYPE_NUMBER || type->kind == TYPE_STRING || type->kind == TYPE_BOOL) {
    Type* copy = typeAlloc(tc, type->kind);
    copy->nullable = true;
    return copy;
  }
  Type* copy = typeClone(tc, type);
  if (copy) copy->nullable = true;
  return copy;
}

bool typeNamesEqual(ObjString* a, ObjString* b) {
  if (a == b) return true;
  if (!a || !b) return false;
  if (a->length != b->length) return false;
  return memcmp(a->chars, b->chars, (size_t)a->length) == 0;
}

bool typeEquals(Type* a, Type* b) {
  if (a == b) return true;
  if (!a || !b) return false;
  if (a->kind != b->kind) return false;
  if (a->kind != TYPE_NULL && a->nullable != b->nullable) return false;
  switch (a->kind) {
    case TYPE_ANY:
    case TYPE_UNKNOWN:
    case TYPE_NUMBER:
    case TYPE_STRING:
    case TYPE_BOOL:
    case TYPE_NULL:
      return true;
      case TYPE_NAMED:
        if (!typeNamesEqual(a->name, b->name)) return false;
        if (a->typeArgCount == 0 && b->typeArgCount == 0) return true;
        if (a->typeArgCount != b->typeArgCount) return false;
        for (int i = 0; i < a->typeArgCount; i++) {
          if (!typeEquals(a->typeArgs[i], b->typeArgs[i])) return false;
        }
        return true;
      case TYPE_GENERIC:
        return typeNamesEqual(a->name, b->name);
    case TYPE_ARRAY:
      return typeEquals(a->elem, b->elem);
    case TYPE_MAP:
      return typeEquals(a->key, b->key) && typeEquals(a->value, b->value);
    case TYPE_UNION:
      if (a->nullable != b->nullable) return false;
      if (a->unionCount != b->unionCount) return false;
      for (int i = 0; i < a->unionCount; i++) {
        bool found = false;
        for (int j = 0; j < b->unionCount; j++) {
          if (typeEquals(a->unionTypes[i], b->unionTypes[j])) {
            found = true;
            break;
          }
        }
        if (!found) return false;
      }
      return true;
    case TYPE_FUNCTION:
      if (a->paramCount != b->paramCount) return false;
      for (int i = 0; i < a->paramCount; i++) {
        if (!typeEquals(a->params[i], b->params[i])) return false;
      }
      return typeEquals(a->returnType, b->returnType);
  }
  return false;
}

  bool typeAssignable(Type* dst, Type* src) {
    if (typeIsAny(dst) || typeIsAny(src)) return true;
    if (!dst || !src) return true;
    if (dst->kind == TYPE_NULL) return src->kind == TYPE_NULL;
    if (src->kind == TYPE_NULL) return typeIsNullable(dst);
    if (typeIsNullable(src) && !typeIsNullable(dst)) return false;
    if (dst->kind == TYPE_UNION) {
      if (src->kind == TYPE_UNION) {
        for (int i = 0; i < src->unionCount; i++) {
          if (!typeAssignable(dst, src->unionTypes[i])) return false;
        }
        return true;
      }
      for (int i = 0; i < dst->unionCount; i++) {
        if (typeAssignable(dst->unionTypes[i], src)) return true;
      }
      return false;
    }
    if (src->kind == TYPE_UNION) {
      for (int i = 0; i < src->unionCount; i++) {
        if (!typeAssignable(dst, src->unionTypes[i])) return false;
      }
      return true;
    }
    if (dst->kind == TYPE_NAMED && src->kind == TYPE_NAMED) {
      if (typeNamesEqual(dst->name, src->name)) {
        if (dst->typeArgCount == 0 || src->typeArgCount == 0) return true;
        if (dst->typeArgCount != src->typeArgCount) return false;
        for (int i = 0; i < dst->typeArgCount; i++) {
          if (!typeAssignable(dst->typeArgs[i], src->typeArgs[i])) return false;
        }
        return true;
      }
      if (gTypeRegistry &&
          typeRegistryClassImplements(gTypeRegistry, src->name, dst->name)) {
        return true;
      }
      return false;
    }
    if (dst->kind != src->kind) return false;
    switch (dst->kind) {
      case TYPE_ANY:
      case TYPE_UNKNOWN:
      case TYPE_NUMBER:
      case TYPE_STRING:
      case TYPE_BOOL:
      case TYPE_NULL:
        return true;
      case TYPE_NAMED:
        return false;
      case TYPE_GENERIC:
        return true;
      case TYPE_ARRAY:
        return typeAssignable(dst->elem, src->elem);
      case TYPE_MAP:
        return typeAssignable(dst->key, src->key) && typeAssignable(dst->value, src->value);
      case TYPE_UNION:
        return false;
      case TYPE_FUNCTION:
      if (dst->paramCount != src->paramCount) return false;
      for (int i = 0; i < dst->paramCount; i++) {
        if (!typeAssignable(dst->params[i], src->params[i])) return false;
      }
      return typeAssignable(dst->returnType, src->returnType);
  }
    return true;
  }

  TypeBinding* typeBindingFind(TypeBinding* bindings, int count, ObjString* name) {
    if (!bindings || !name) return NULL;
    for (int i = 0; i < count; i++) {
      if (typeNamesEqual(bindings[i].name, name)) {
        return &bindings[i];
      }
    }
    return NULL;
  }

  bool typeSatisfiesConstraint(Type* actual, ObjString* constraint) {
    if (!constraint) return true;
    if (!actual || typeIsAny(actual)) return true;
    if (actual->kind == TYPE_NAMED) {
      if (typeNamesEqual(actual->name, constraint)) return true;
      return gTypeRegistry && typeRegistryClassImplements(gTypeRegistry, actual->name, constraint);
    }
    return false;
  }

bool typeUnify(Compiler* c, Type* pattern, Type* actual,
                        TypeBinding* bindings, int bindingCount, Token token) {
    if (!pattern || !actual) return true;
    if (pattern->kind == TYPE_GENERIC) {
      TypeBinding* binding = typeBindingFind(bindings, bindingCount, pattern->name);
      if (!binding) return true;
      if (!binding->bound) {
        if (!typeSatisfiesConstraint(actual, binding->constraint)) {
          char expected[64];
          typeToString(typeNamed(c->typecheck, binding->constraint), expected, sizeof(expected));
          typeErrorAt(c, token, "Type argument for '%s' must implement %s.",
                      binding->name ? binding->name->chars : "T", expected);
          return false;
        }
        binding->bound = actual;
        return true;
      }
      return typeAssignable(binding->bound, actual);
    }
    if (pattern->kind == TYPE_UNION) {
      for (int i = 0; i < pattern->unionCount; i++) {
        if (typeUnify(c, pattern->unionTypes[i], actual, bindings, bindingCount, token)) {
          return true;
        }
      }
      return false;
    }
    if (actual->kind == TYPE_UNION) {
      for (int i = 0; i < actual->unionCount; i++) {
        if (!typeUnify(c, pattern, actual->unionTypes[i], bindings, bindingCount, token)) {
          return false;
        }
      }
      return true;
    }
    if (pattern->kind == TYPE_ARRAY) {
      if (actual->kind != TYPE_ARRAY && !typeIsAny(actual)) return false;
      if (actual->kind == TYPE_ARRAY) {
        return typeUnify(c, pattern->elem, actual->elem, bindings, bindingCount, token);
      }
      return true;
    }
    if (pattern->kind == TYPE_MAP) {
      if (actual->kind != TYPE_MAP && !typeIsAny(actual)) return false;
      if (actual->kind == TYPE_MAP) {
        if (!typeUnify(c, pattern->key, actual->key, bindings, bindingCount, token)) return false;
        return typeUnify(c, pattern->value, actual->value, bindings, bindingCount, token);
      }
      return true;
    }
    if (pattern->kind == TYPE_FUNCTION && actual->kind == TYPE_FUNCTION) {
      if (pattern->paramCount >= 0 && actual->paramCount >= 0 &&
          pattern->paramCount != actual->paramCount) {
        return false;
      }
      int count = pattern->paramCount >= 0 ? pattern->paramCount : actual->paramCount;
      for (int i = 0; i < count; i++) {
        if (!typeUnify(c, pattern->params[i], actual->params[i], bindings, bindingCount, token)) {
          return false;
        }
      }
      return typeUnify(c, pattern->returnType, actual->returnType, bindings, bindingCount, token);
    }
    if (pattern->kind == TYPE_NAMED && actual->kind == TYPE_NAMED) {
      if (!typeNamesEqual(pattern->name, actual->name)) return false;
      if (pattern->typeArgCount == 0 || actual->typeArgCount == 0) return true;
      if (pattern->typeArgCount != actual->typeArgCount) return false;
      for (int i = 0; i < pattern->typeArgCount; i++) {
        if (!typeUnify(c, pattern->typeArgs[i], actual->typeArgs[i], bindings, bindingCount, token)) {
          return false;
        }
      }
      return true;
    }
    return typeAssignable(pattern, actual);
  }

Type* typeSubstitute(TypeChecker* tc, Type* type,
                              TypeBinding* bindings, int bindingCount) {
    if (!type) return typeAny();
    if (type->kind == TYPE_GENERIC) {
      TypeBinding* binding = typeBindingFind(bindings, bindingCount, type->name);
      if (binding && binding->bound) return binding->bound;
      return typeAny();
    }
    if (type->kind == TYPE_UNION) {
      Type* result = typeAlloc(tc, TYPE_UNION);
      result->unionCount = type->unionCount;
      result->nullable = type->nullable;
      if (type->unionCount > 0 && type->unionTypes) {
        result->unionTypes = (Type**)malloc(sizeof(Type*) * (size_t)type->unionCount);
        if (!result->unionTypes) {
          fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
        for (int i = 0; i < type->unionCount; i++) {
          result->unionTypes[i] = typeSubstitute(tc, type->unionTypes[i], bindings, bindingCount);
        }
      }
      return result;
    }
    if (type->kind == TYPE_ARRAY) {
      Type* elem = typeSubstitute(tc, type->elem, bindings, bindingCount);
      Type* result = typeArray(tc, elem);
      result->nullable = type->nullable;
      return result;
    }
    if (type->kind == TYPE_MAP) {
      Type* key = typeSubstitute(tc, type->key, bindings, bindingCount);
      Type* value = typeSubstitute(tc, type->value, bindings, bindingCount);
      Type* result = typeMap(tc, key, value);
      result->nullable = type->nullable;
      return result;
    }
    if (type->kind == TYPE_NAMED) {
      Type* result = typeNamed(tc, type->name);
      result->nullable = type->nullable;
      if (type->typeArgCount > 0 && type->typeArgs) {
        result->typeArgCount = type->typeArgCount;
        result->typeArgs = (Type**)malloc(sizeof(Type*) * (size_t)type->typeArgCount);
        if (!result->typeArgs) {
          fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
        for (int i = 0; i < type->typeArgCount; i++) {
          result->typeArgs[i] = typeSubstitute(tc, type->typeArgs[i], bindings, bindingCount);
        }
      }
      return result;
    }
    if (type->kind == TYPE_FUNCTION) {
      Type* result = typeAlloc(tc, TYPE_FUNCTION);
      result->paramCount = type->paramCount;
      result->returnType = typeSubstitute(tc, type->returnType, bindings, bindingCount);
      result->nullable = type->nullable;
      if (type->paramCount > 0 && type->params) {
        result->params = (Type**)malloc(sizeof(Type*) * (size_t)type->paramCount);
        if (!result->params) {
          fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
        for (int i = 0; i < type->paramCount; i++) {
          result->params[i] = typeSubstitute(tc, type->params[i], bindings, bindingCount);
        }
      }
      return result;
    }
    return typeClone(tc, type);
  }

void typeToString(Type* type, char* buffer, size_t size) {
  if (!buffer || size == 0) return;
  if (!type) {
    snprintf(buffer, size, "any");
    return;
  }
  switch (type->kind) {
    case TYPE_ANY:
      snprintf(buffer, size, "any");
      return;
    case TYPE_UNKNOWN:
      snprintf(buffer, size, "unknown");
      return;
    case TYPE_NUMBER:
      snprintf(buffer, size, type->nullable ? "number?" : "number");
      return;
    case TYPE_STRING:
      snprintf(buffer, size, type->nullable ? "string?" : "string");
      return;
    case TYPE_BOOL:
      snprintf(buffer, size, type->nullable ? "bool?" : "bool");
      return;
    case TYPE_NULL:
      snprintf(buffer, size, "null");
      return;
      case TYPE_NAMED:
        if (type->name) {
          size_t used = (size_t)snprintf(buffer, size, "%s", type->name->chars);
          if (type->typeArgCount > 0 && type->typeArgs && used < size) {
            used += (size_t)snprintf(buffer + used, size - used, "<");
            for (int i = 0; i < type->typeArgCount && used < size; i++) {
              char arg[32];
              typeToString(type->typeArgs[i], arg, sizeof(arg));
              used += (size_t)snprintf(buffer + used, size - used, "%s%s",
                                       i > 0 ? ", " : "", arg);
            }
            if (used < size) {
              used += (size_t)snprintf(buffer + used, size - used, ">");
            }
          }
          if (type->nullable && used < size) {
            (void)snprintf(buffer + used, size - used, "?");
          }
        } else {
          snprintf(buffer, size, "named");
        }
        return;
      case TYPE_GENERIC:
        if (type->name) {
          if (type->nullable) {
            snprintf(buffer, size, "%s?", type->name->chars);
          } else {
            snprintf(buffer, size, "%s", type->name->chars);
          }
        } else {
          snprintf(buffer, size, "T");
        }
        return;
    case TYPE_ARRAY: {
      char inner[64];
      typeToString(type->elem, inner, sizeof(inner));
      if (type->nullable) {
        snprintf(buffer, size, "array<%s>?", inner);
      } else {
        snprintf(buffer, size, "array<%s>", inner);
      }
      return;
    }
    case TYPE_MAP: {
      char key[64];
      char value[64];
      typeToString(type->key, key, sizeof(key));
      typeToString(type->value, value, sizeof(value));
      if (type->nullable) {
        snprintf(buffer, size, "map<%s, %s>?", key, value);
      } else {
        snprintf(buffer, size, "map<%s, %s>", key, value);
      }
      return;
    }
    case TYPE_UNION: {
      size_t used = 0;
      for (int i = 0; i < type->unionCount && used < size; i++) {
        char part[64];
        typeToString(type->unionTypes[i], part, sizeof(part));
        used += (size_t)snprintf(buffer + used, size - used, "%s%s",
                                 i > 0 ? " | " : "", part);
      }
      if (type->nullable && used < size) {
        (void)snprintf(buffer + used, size - used, "?");
      }
      return;
    }
    case TYPE_FUNCTION:
      snprintf(buffer, size, type->nullable ? "fun?" : "fun");
      return;
  }
  snprintf(buffer, size, "any");
}

void typeErrorAt(Compiler* c, Token token, const char* format, ...) {
  if (!typecheckEnabled(c)) return;
  if (c->panicMode) return;
  char message[256];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  c->hadError = true;
#ifdef ERKAO_FUZZING
  (void)token;
#else
  const char* path = c->path ? c->path : "<repl>";
  fprintf(stderr, "%s:%d:%d: Error", path, token.line, token.column);
  if (token.type == TOKEN_EOF) {
    fprintf(stderr, " at end");
  } else if (token.type != TOKEN_ERROR) {
    fprintf(stderr, " at '%.*s'", token.length, token.start);
  }
  fprintf(stderr, ": %s\n", message);
  printErrorContext(c->source, token.line, token.column,
                    token.length > 0 ? token.length : 1);
#endif
  if (c->typecheck) {
    c->typecheck->errorCount++;
  }
}

void typeCheckerEnterScope(Compiler* c) {
  if (!typecheckEnabled(c)) return;
  c->typecheck->scopeDepth = c->scopeDepth;
}

void typeCheckerExitScope(Compiler* c) {
  if (!typecheckEnabled(c)) return;
  TypeChecker* tc = c->typecheck;
  int targetDepth = c->scopeDepth;
  while (tc->count > 0 && tc->entries[tc->count - 1].depth > targetDepth) {
    tc->count--;
  }
  while (tc->aliasCount > 0 && tc->aliases[tc->aliasCount - 1].depth > targetDepth) {
    tc->aliasCount--;
  }
  tc->scopeDepth = targetDepth;
}

void typeDefine(Compiler* c, Token name, Type* type, bool explicitType) {
  if (!typecheckEnabled(c)) return;
  TypeChecker* tc = c->typecheck;
  if (tc->count >= tc->capacity) {
    int oldCap = tc->capacity;
    tc->capacity = GROW_CAPACITY(oldCap);
    tc->entries = GROW_ARRAY(TypeEntry, tc->entries, oldCap, tc->capacity);
    if (!tc->entries) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  ObjString* nameStr = stringFromToken(c->vm, name);
  tc->entries[tc->count].name = nameStr;
  tc->entries[tc->count].type = type ? type : typeAny();
  tc->entries[tc->count].explicitType = explicitType;
  tc->entries[tc->count].depth = c->scopeDepth;
  tc->count++;
}

TypeAlias* typeAliasLookup(TypeChecker* tc, ObjString* name) {
  if (!tc || !name) return NULL;
  for (int i = tc->aliasCount - 1; i >= 0; i--) {
    if (tc->aliases[i].name == name) {
      return &tc->aliases[i];
    }
  }
  if (tc->enclosing) {
    return typeAliasLookup(tc->enclosing, name);
  }
  return NULL;
}

void typeAliasDefine(Compiler* c, Token name, Type* type) {
  if (!typecheckEnabled(c)) return;
  TypeChecker* tc = c->typecheck;
  if (tc->aliasCount >= tc->aliasCapacity) {
    int oldCap = tc->aliasCapacity;
    tc->aliasCapacity = GROW_CAPACITY(oldCap);
    tc->aliases = GROW_ARRAY(TypeAlias, tc->aliases, oldCap, tc->aliasCapacity);
    if (!tc->aliases) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  ObjString* nameStr = stringFromToken(c->vm, name);
  tc->aliases[tc->aliasCount].name = nameStr;
  tc->aliases[tc->aliasCount].type = type ? type : typeAny();
  tc->aliases[tc->aliasCount].depth = c->scopeDepth;
  tc->aliasCount++;
}

TypeEntry* typeLookupEntry(TypeChecker* tc, ObjString* name) {
  if (!tc) return NULL;
  for (int i = tc->count - 1; i >= 0; i--) {
    if (tc->entries[i].name == name) {
      return &tc->entries[i];
    }
  }
  if (tc->enclosing) {
    return typeLookupEntry(tc->enclosing, name);
  }
  return NULL;
}

Type* typeLookup(Compiler* c, Token name) {
  if (!typecheckEnabled(c)) return typeAny();
  ObjString* nameStr = stringFromToken(c->vm, name);
  TypeEntry* entry = typeLookupEntry(c->typecheck, nameStr);
  if (!entry) return typeAny();
  return entry->type ? entry->type : typeAny();
}

void typeAssign(Compiler* c, Token name, Type* valueType) {
  if (!typecheckEnabled(c)) return;
  ObjString* nameStr = stringFromToken(c->vm, name);
  TypeEntry* entry = typeLookupEntry(c->typecheck, nameStr);
  if (!entry) {
    return;
  }
  Type* target = entry->type ? entry->type : typeAny();
  if (entry->explicitType) {
    if (!typeAssignable(target, valueType)) {
      char expected[64];
      char got[64];
      typeToString(target, expected, sizeof(expected));
      typeToString(valueType, got, sizeof(got));
      typeErrorAt(c, name, "Type mismatch. Expected %s but got %s.", expected, got);
    }
    return;
  }
  if (target->kind == TYPE_UNKNOWN) {
    entry->type = valueType ? valueType : typeAny();
    return;
  }
  if (typeIsAny(target) || typeIsAny(valueType)) {
    return;
  }
  if (valueType->kind == TYPE_NULL && target->kind != TYPE_NULL) {
    entry->type = typeMakeNullable(c->typecheck, target);
    return;
  }
  if (target->kind == TYPE_NULL && valueType->kind != TYPE_NULL) {
    entry->type = typeMakeNullable(c->typecheck, valueType);
    return;
  }
  if (!typeAssignable(target, valueType)) {
    if (target->kind == valueType->kind && typeIsNullable(valueType)) {
      entry->type = typeMakeNullable(c->typecheck, target);
      return;
    }
    {
      char expected[64];
      char got[64];
      typeToString(target, expected, sizeof(expected));
      typeToString(valueType, got, sizeof(got));
      typeErrorAt(c, name, "Type mismatch. Expected %s but got %s.", expected, got);
    }
    return;
  }
}

bool tokenMatches(Token token, const char* text) {
  int length = (int)strlen(text);
  if (token.length != length) return false;
  return memcmp(token.start, text, (size_t)length) == 0;
}


void typeDefineSynthetic(Compiler* c, const char* name, Type* type) {
  if (!typecheckEnabled(c)) return;
  Token token = syntheticToken(name);
  typeDefine(c, token, type, true);
}

bool typeNamedIs(Type* type, const char* name) {
  if (!type || type->kind != TYPE_NAMED || !type->name || !name) return false;
  size_t length = strlen(name);
  if ((size_t)type->name->length != length) return false;
  return memcmp(type->name->chars, name, length) == 0;
}

Type* typeFunctionN(TypeChecker* tc, int paramCount, Type* returnType, ...) {
  if (paramCount < 0) {
    return typeFunction(tc, NULL, -1, returnType);
  }
  Type* params[8];
  if (paramCount > (int)(sizeof(params) / sizeof(params[0]))) {
    paramCount = (int)(sizeof(params) / sizeof(params[0]));
  }
  va_list args;
  va_start(args, returnType);
  for (int i = 0; i < paramCount; i++) {
    params[i] = va_arg(args, Type*);
  }
  va_end(args);
  return typeFunction(tc, params, paramCount, returnType);
}


Type* typeLookupStdlibMember(Compiler* c, Type* objectType, Token name) {
  if (!typecheckEnabled(c)) return typeAny();
  if (!objectType || typeIsAny(objectType)) return typeAny();
  if (objectType->kind != TYPE_NAMED || !objectType->name) return typeAny();
  TypeChecker* tc = c->typecheck;
  Type* any = typeAny();
  Type* number = typeNumber();
  Type* string = typeString();
  Type* boolean = typeBool();

  if (typeNamedIs(objectType, "fs")) {
    Type* arrayString = typeArray(tc, string);
    if (tokenMatches(name, "readText")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "writeText")) return typeFunctionN(tc, 2, boolean, string, string);
    if (tokenMatches(name, "exists")) return typeFunctionN(tc, 1, boolean, string);
    if (tokenMatches(name, "cwd")) return typeFunctionN(tc, 0, string);
    if (tokenMatches(name, "listDir")) return typeFunctionN(tc, 1, arrayString, string);
    if (tokenMatches(name, "isFile")) return typeFunctionN(tc, 1, boolean, string);
    if (tokenMatches(name, "isDir")) return typeFunctionN(tc, 1, boolean, string);
    if (tokenMatches(name, "size")) return typeFunctionN(tc, 1, number, string);
    if (tokenMatches(name, "glob")) return typeFunctionN(tc, 1, arrayString, string);
  }

  if (typeNamedIs(objectType, "path")) {
    Type* arrayString = typeArray(tc, string);
    if (tokenMatches(name, "join")) return typeFunctionN(tc, 2, string, string, string);
    if (tokenMatches(name, "dirname")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "basename")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "extname")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "isAbs")) return typeFunctionN(tc, 1, boolean, string);
    if (tokenMatches(name, "normalize")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "stem")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "split")) return typeFunctionN(tc, 1, arrayString, string);
  }

  if (typeNamedIs(objectType, "json") || typeNamedIs(objectType, "yaml")) {
    if (tokenMatches(name, "parse")) return typeFunctionN(tc, 1, any, string);
    if (tokenMatches(name, "stringify")) return typeFunctionN(tc, 1, string, any);
  }

  if (typeNamedIs(objectType, "math")) {
    if (tokenMatches(name, "abs")) return typeFunctionN(tc, 1, number, number);
    if (tokenMatches(name, "floor")) return typeFunctionN(tc, 1, number, number);
    if (tokenMatches(name, "ceil")) return typeFunctionN(tc, 1, number, number);
    if (tokenMatches(name, "round")) return typeFunctionN(tc, 1, number, number);
    if (tokenMatches(name, "sqrt")) return typeFunctionN(tc, 1, number, number);
    if (tokenMatches(name, "pow")) return typeFunctionN(tc, 2, number, number, number);
    if (tokenMatches(name, "min")) return typeFunctionN(tc, -1, number);
    if (tokenMatches(name, "max")) return typeFunctionN(tc, -1, number);
    if (tokenMatches(name, "clamp")) return typeFunctionN(tc, 3, number, number, number, number);
    if (tokenMatches(name, "PI") || tokenMatches(name, "E")) return number;
  }

  if (typeNamedIs(objectType, "random")) {
    Type* arrayAny = typeArray(tc, any);
    if (tokenMatches(name, "seed")) return typeFunctionN(tc, 1, typeNull(), number);
    if (tokenMatches(name, "int")) return typeFunctionN(tc, -1, number);
    if (tokenMatches(name, "float")) return typeFunctionN(tc, -1, number);
    if (tokenMatches(name, "choice")) return typeFunctionN(tc, 1, any, arrayAny);
    if (tokenMatches(name, "normal")) return typeFunctionN(tc, 2, number, number, number);
    if (tokenMatches(name, "gaussian")) return typeFunctionN(tc, 2, number, number, number);
    if (tokenMatches(name, "exponential")) return typeFunctionN(tc, 1, number, number);
    if (tokenMatches(name, "uniform")) return typeFunctionN(tc, -1, number);
  }

  if (typeNamedIs(objectType, "str")) {
    Type* arrayString = typeArray(tc, string);
    if (tokenMatches(name, "upper")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "lower")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "trim")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "trimStart")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "trimEnd")) return typeFunctionN(tc, 1, string, string);
    if (tokenMatches(name, "startsWith")) return typeFunctionN(tc, 2, boolean, string, string);
    if (tokenMatches(name, "endsWith")) return typeFunctionN(tc, 2, boolean, string, string);
    if (tokenMatches(name, "contains")) return typeFunctionN(tc, 2, boolean, string, string);
    if (tokenMatches(name, "split")) return typeFunctionN(tc, 2, arrayString, string, string);
    if (tokenMatches(name, "join")) return typeFunctionN(tc, 2, string, arrayString, string);
    if (tokenMatches(name, "builder")) return typeFunctionN(tc, 0, arrayString);
    if (tokenMatches(name, "append")) return typeFunctionN(tc, 2, arrayString, arrayString, string);
    if (tokenMatches(name, "build")) return typeFunctionN(tc, -1, string);
    if (tokenMatches(name, "replace")) return typeFunctionN(tc, 3, string, string, string, string);
    if (tokenMatches(name, "replaceAll")) return typeFunctionN(tc, 3, string, string, string, string);
    if (tokenMatches(name, "repeat")) return typeFunctionN(tc, 2, string, string, number);
  }

  if (typeNamedIs(objectType, "array")) {
    Type* arrayAny = typeArray(tc, any);
    if (tokenMatches(name, "slice")) return typeFunctionN(tc, -1, arrayAny);
    if (tokenMatches(name, "map")) {
      Type* params[2] = { arrayAny, typeFunctionN(tc, 1, any, any) };
      return typeFunction(tc, params, 2, arrayAny);
    }
    if (tokenMatches(name, "filter")) {
      Type* params[2] = { arrayAny, typeFunctionN(tc, 1, boolean, any) };
      return typeFunction(tc, params, 2, arrayAny);
    }
    if (tokenMatches(name, "reduce")) return typeFunctionN(tc, -1, any);
    if (tokenMatches(name, "contains")) return typeFunctionN(tc, 2, boolean, arrayAny, any);
    if (tokenMatches(name, "indexOf")) return typeFunctionN(tc, 2, number, arrayAny, any);
    if (tokenMatches(name, "concat")) return typeFunctionN(tc, 2, arrayAny, arrayAny, arrayAny);
    if (tokenMatches(name, "reverse")) return typeFunctionN(tc, 1, arrayAny, arrayAny);
  }

  if (typeNamedIs(objectType, "os")) {
    if (tokenMatches(name, "platform")) return typeFunctionN(tc, 0, string);
    if (tokenMatches(name, "arch")) return typeFunctionN(tc, 0, string);
    if (tokenMatches(name, "sep")) return typeFunctionN(tc, 0, string);
    if (tokenMatches(name, "eol")) return typeFunctionN(tc, 0, string);
    if (tokenMatches(name, "cwd")) return typeFunctionN(tc, 0, string);
    if (tokenMatches(name, "home")) return typeFunctionN(tc, 0, string);
    if (tokenMatches(name, "tmp")) return typeFunctionN(tc, 0, string);
  }

  if (typeNamedIs(objectType, "time")) {
    Type* mapAny = typeMap(tc, string, any);
    if (tokenMatches(name, "now")) return typeFunctionN(tc, 0, number);
    if (tokenMatches(name, "sleep")) return typeFunctionN(tc, 1, typeNull(), number);
    if (tokenMatches(name, "format")) return typeFunctionN(tc, -1, string);
    if (tokenMatches(name, "iso")) return typeFunctionN(tc, -1, string);
    if (tokenMatches(name, "parts")) return typeFunctionN(tc, -1, mapAny);
  }

  if (typeNamedIs(objectType, "di")) {
    Type* mapAny = typeMap(tc, string, any);
    if (tokenMatches(name, "container")) return typeFunctionN(tc, 0, mapAny);
    if (tokenMatches(name, "bind")) return typeFunctionN(tc, 3, typeNull(), mapAny, string, any);
    if (tokenMatches(name, "singleton")) return typeFunctionN(tc, 3, typeNull(), mapAny, string, any);
    if (tokenMatches(name, "value")) return typeFunctionN(tc, 3, typeNull(), mapAny, string, any);
    if (tokenMatches(name, "resolve")) return typeFunctionN(tc, 2, any, mapAny, string);
  }

  if (typeNamedIs(objectType, "vec2")) {
    Type* arrayNumber = typeArray(tc, number);
    if (tokenMatches(name, "make")) return typeFunctionN(tc, 2, arrayNumber, number, number);
    if (tokenMatches(name, "add")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, arrayNumber);
    if (tokenMatches(name, "sub")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, arrayNumber);
    if (tokenMatches(name, "scale")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, number);
    if (tokenMatches(name, "dot")) return typeFunctionN(tc, 2, number, arrayNumber, arrayNumber);
    if (tokenMatches(name, "len")) return typeFunctionN(tc, 1, number, arrayNumber);
    if (tokenMatches(name, "norm")) return typeFunctionN(tc, 1, arrayNumber, arrayNumber);
    if (tokenMatches(name, "lerp")) return typeFunctionN(tc, 3, arrayNumber, arrayNumber, arrayNumber, number);
    if (tokenMatches(name, "dist")) return typeFunctionN(tc, 2, number, arrayNumber, arrayNumber);
  }

  if (typeNamedIs(objectType, "vec3")) {
    Type* arrayNumber = typeArray(tc, number);
    if (tokenMatches(name, "make")) return typeFunctionN(tc, 3, arrayNumber, number, number, number);
    if (tokenMatches(name, "add")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, arrayNumber);
    if (tokenMatches(name, "sub")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, arrayNumber);
    if (tokenMatches(name, "scale")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, number);
    if (tokenMatches(name, "dot")) return typeFunctionN(tc, 2, number, arrayNumber, arrayNumber);
    if (tokenMatches(name, "len")) return typeFunctionN(tc, 1, number, arrayNumber);
    if (tokenMatches(name, "norm")) return typeFunctionN(tc, 1, arrayNumber, arrayNumber);
    if (tokenMatches(name, "lerp")) return typeFunctionN(tc, 3, arrayNumber, arrayNumber, arrayNumber, number);
    if (tokenMatches(name, "dist")) return typeFunctionN(tc, 2, number, arrayNumber, arrayNumber);
    if (tokenMatches(name, "cross")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, arrayNumber);
  }

  if (typeNamedIs(objectType, "vec4")) {
    Type* arrayNumber = typeArray(tc, number);
    if (tokenMatches(name, "make")) return typeFunctionN(tc, 4, arrayNumber, number, number, number, number);
    if (tokenMatches(name, "add")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, arrayNumber);
    if (tokenMatches(name, "sub")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, arrayNumber);
    if (tokenMatches(name, "scale")) return typeFunctionN(tc, 2, arrayNumber, arrayNumber, number);
    if (tokenMatches(name, "dot")) return typeFunctionN(tc, 2, number, arrayNumber, arrayNumber);
    if (tokenMatches(name, "len")) return typeFunctionN(tc, 1, number, arrayNumber);
    if (tokenMatches(name, "norm")) return typeFunctionN(tc, 1, arrayNumber, arrayNumber);
    if (tokenMatches(name, "lerp")) return typeFunctionN(tc, 3, arrayNumber, arrayNumber, arrayNumber, number);
    if (tokenMatches(name, "dist")) return typeFunctionN(tc, 2, number, arrayNumber, arrayNumber);
  }

  if (typeNamedIs(objectType, "http")) {
    Type* mapAny = typeMap(tc, string, any);
    if (tokenMatches(name, "get")) return typeFunctionN(tc, 1, mapAny, string);
    if (tokenMatches(name, "post")) return typeFunctionN(tc, 2, mapAny, string, string);
    if (tokenMatches(name, "request")) return typeFunctionN(tc, 3, mapAny, string, string, any);
    if (tokenMatches(name, "serve")) return typeFunctionN(tc, -1, typeNull());
  }

  if (typeNamedIs(objectType, "proc")) {
    if (tokenMatches(name, "run")) return typeFunctionN(tc, 1, number, string);
  }

  if (typeNamedIs(objectType, "env")) {
    Type* arrayString = typeArray(tc, string);
    Type* mapString = typeMap(tc, string, string);
    if (tokenMatches(name, "args")) return typeFunctionN(tc, 0, arrayString);
    if (tokenMatches(name, "get")) return typeFunctionN(tc, 1, typeMakeNullable(tc, string), string);
    if (tokenMatches(name, "set")) return typeFunctionN(tc, 2, boolean, string, string);
    if (tokenMatches(name, "has")) return typeFunctionN(tc, 1, boolean, string);
    if (tokenMatches(name, "unset")) return typeFunctionN(tc, 1, boolean, string);
    if (tokenMatches(name, "all")) return typeFunctionN(tc, 0, mapString);
  }

  if (typeNamedIs(objectType, "plugin")) {
    if (tokenMatches(name, "load")) return typeFunctionN(tc, 1, boolean, string);
  }

  return typeAny();
}

void typeDefineStdlib(Compiler* c) {
  if (typecheckEnabled(c)) {
    TypeChecker* tc = c->typecheck;

    typeDefineSynthetic(c, "print", typeFunctionN(tc, -1, typeNull()));
    typeDefineSynthetic(c, "clock", typeFunctionN(tc, 0, typeNumber()));
    typeDefineSynthetic(c, "type", typeFunctionN(tc, 1, typeString(), typeAny()));
    typeDefineSynthetic(c, "len", typeFunctionN(tc, 1, typeNumber(), typeAny()));
    typeDefineSynthetic(c, "args", typeFunctionN(tc, 0, typeArray(tc, typeString())));
    {
      Type* params[2] = { typeArray(tc, typeAny()), typeAny() };
      typeDefineSynthetic(c, "push", typeFunction(tc, params, 2, typeNumber()));
    }
    typeDefineSynthetic(c, "keys", typeFunctionN(tc, 1, typeArray(tc, typeString()),
                                                 typeMap(tc, typeString(), typeAny())));
    typeDefineSynthetic(c, "values", typeFunctionN(tc, 1, typeArray(tc, typeAny()),
                                                   typeMap(tc, typeString(), typeAny())));
    {
      Type* any = typeAny();
      Type* number = typeNumber();
      Type* string = typeString();
      Type* arrayAny = typeArray(tc, any);
      Type* arrayString = typeArray(tc, string);
      Type* mapStringAny = typeMap(tc, string, any);
      Type* rangeType = typeNamed(tc, copyString(c->vm, "range"));
      typeDefineSynthetic(c, "range", typeFunctionN(tc, 2, rangeType, number, number));
      typeDefineSynthetic(c, "iter", typeFunctionN(tc, 1, any, any));
      typeDefineSynthetic(c, "next", typeFunctionN(tc, 1, mapStringAny, any));
      typeDefineSynthetic(c, "arrayRest", typeFunctionN(tc, 2, arrayAny, arrayAny, number));
      typeDefineSynthetic(c, "mapRest", typeFunctionN(tc, 2, mapStringAny, mapStringAny, arrayString));
      typeDefineSynthetic(c, "spawn", typeFunctionN(tc, -1, mapStringAny));
      typeDefineSynthetic(c, "await", typeFunctionN(tc, 1, any, mapStringAny));
      typeDefineSynthetic(c, "channel", typeFunctionN(tc, 0, mapStringAny));
      typeDefineSynthetic(c, "send", typeFunctionN(tc, 2, typeNull(), mapStringAny, any));
      typeDefineSynthetic(c, "recv", typeFunctionN(tc, 1, any, mapStringAny));
      typeDefineSynthetic(c, "sleep", typeFunctionN(tc, 1, typeNull(), number));
    }
    typeDefineSynthetic(c, "Option", typeNamed(tc, copyString(c->vm, "Option")));
    typeDefineSynthetic(c, "Result", typeNamed(tc, copyString(c->vm, "Result")));

    typeDefineSynthetic(c, "fs", typeNamed(tc, copyString(c->vm, "fs")));
    typeDefineSynthetic(c, "path", typeNamed(tc, copyString(c->vm, "path")));
    typeDefineSynthetic(c, "json", typeNamed(tc, copyString(c->vm, "json")));
    typeDefineSynthetic(c, "yaml", typeNamed(tc, copyString(c->vm, "yaml")));
    typeDefineSynthetic(c, "math", typeNamed(tc, copyString(c->vm, "math")));
    typeDefineSynthetic(c, "random", typeNamed(tc, copyString(c->vm, "random")));
    typeDefineSynthetic(c, "str", typeNamed(tc, copyString(c->vm, "str")));
    typeDefineSynthetic(c, "array", typeNamed(tc, copyString(c->vm, "array")));
    typeDefineSynthetic(c, "os", typeNamed(tc, copyString(c->vm, "os")));
    typeDefineSynthetic(c, "time", typeNamed(tc, copyString(c->vm, "time")));
    typeDefineSynthetic(c, "vec2", typeNamed(tc, copyString(c->vm, "vec2")));
    typeDefineSynthetic(c, "vec3", typeNamed(tc, copyString(c->vm, "vec3")));
    typeDefineSynthetic(c, "vec4", typeNamed(tc, copyString(c->vm, "vec4")));
    typeDefineSynthetic(c, "http", typeNamed(tc, copyString(c->vm, "http")));
    typeDefineSynthetic(c, "proc", typeNamed(tc, copyString(c->vm, "proc")));
    typeDefineSynthetic(c, "env", typeNamed(tc, copyString(c->vm, "env")));
    typeDefineSynthetic(c, "plugin", typeNamed(tc, copyString(c->vm, "plugin")));
    typeDefineSynthetic(c, "di", typeNamed(tc, copyString(c->vm, "di")));
  }
  {
    Token optionToken;
    memset(&optionToken, 0, sizeof(Token));
    optionToken.start = "Option";
    optionToken.length = 6;
    EnumInfo* optionInfo = compilerAddEnum(c, optionToken);
    if (optionInfo) {
      enumInfoSetAdt(optionInfo, true);
      Token someToken;
      memset(&someToken, 0, sizeof(Token));
      someToken.start = "Some";
      someToken.length = 4;
      enumInfoAddVariant(optionInfo, someToken, 1);
      Token noneToken;
      memset(&noneToken, 0, sizeof(Token));
      noneToken.start = "None";
      noneToken.length = 4;
      enumInfoAddVariant(optionInfo, noneToken, 0);
    }

    Token resultToken;
    memset(&resultToken, 0, sizeof(Token));
    resultToken.start = "Result";
    resultToken.length = 6;
    EnumInfo* resultInfo = compilerAddEnum(c, resultToken);
    if (resultInfo) {
      enumInfoSetAdt(resultInfo, true);
      Token okToken;
      memset(&okToken, 0, sizeof(Token));
      okToken.start = "Ok";
      okToken.length = 2;
      enumInfoAddVariant(resultInfo, okToken, 1);
      Token errToken;
      memset(&errToken, 0, sizeof(Token));
      errToken.start = "Err";
      errToken.length = 3;
      enumInfoAddVariant(resultInfo, errToken, 1);
    }
  }
  compilerPluginTypeHooks(c);
}

Type* typeNamed(TypeChecker* tc, ObjString* name) {
  Type* type = typeAlloc(tc, TYPE_NAMED);
  type->name = name;
  return type;
}

Type* typeGeneric(TypeChecker* tc, ObjString* name) {
  Type* type = typeAlloc(tc, TYPE_GENERIC);
  type->name = name;
  return type;
}

Type* typeArray(TypeChecker* tc, Type* elem) {
  Type* type = typeAlloc(tc, TYPE_ARRAY);
  type->elem = elem ? elem : typeAny();
  return type;
}

Type* typeMap(TypeChecker* tc, Type* key, Type* value) {
  Type* type = typeAlloc(tc, TYPE_MAP);
  type->key = key ? key : typeString();
  type->value = value ? value : typeAny();
  return type;
}

bool typeListContains(Type** list, int count, Type* candidate) {
  for (int i = 0; i < count; i++) {
    if (typeEquals(list[i], candidate)) return true;
  }
  return false;
}

void typeListAdd(TypeChecker* tc, Type*** list, int* count, int* capacity, Type* candidate) {
  if (!candidate) return;
  if (typeListContains(*list, *count, candidate)) return;
  if (*capacity < *count + 1) {
    int oldCap = *capacity;
    *capacity = GROW_CAPACITY(oldCap);
    *list = GROW_ARRAY(Type*, *list, oldCap, *capacity);
    if (!*list) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  (*list)[*count] = typeClone(tc, candidate);
  (*count)++;
}

static void typeUnionCollect(TypeChecker* tc, Type* type, Type*** list,
                             int* count, int* capacity, bool* nullable) {
  if (!type) return;
  if (type->kind == TYPE_UNION) {
    if (type->nullable) *nullable = true;
    for (int i = 0; i < type->unionCount; i++) {
      typeListAdd(tc, list, count, capacity, type->unionTypes[i]);
    }
    return;
  }
  typeListAdd(tc, list, count, capacity, type);
}

Type* typeUnion(TypeChecker* tc, Type* a, Type* b) {
  if (!tc) return typeAny();
  if (!a) return b ? typeClone(tc, b) : typeAny();
  if (!b) return typeClone(tc, a);
  if (typeIsAny(a) || typeIsAny(b)) return typeAny();
  if (typeEquals(a, b)) return typeClone(tc, a);

  Type** members = NULL;
  int count = 0;
  int capacity = 0;
  bool nullable = false;
  typeUnionCollect(tc, a, &members, &count, &capacity, &nullable);
  typeUnionCollect(tc, b, &members, &count, &capacity, &nullable);

  if (count == 0) {
    FREE_ARRAY(Type*, members, capacity);
    return typeAny();
  }
  if (count == 1 && !nullable) {
    Type* only = members[0];
    FREE_ARRAY(Type*, members, capacity);
    return only;
  }
  Type* type = typeAlloc(tc, TYPE_UNION);
  type->unionTypes = members;
  type->unionCount = count;
  type->nullable = nullable;
  return type;
}

Type* typeFunction(TypeChecker* tc, Type** params, int paramCount, Type* returnType) {
  Type* type = typeAlloc(tc, TYPE_FUNCTION);
  type->paramCount = paramCount;
  type->returnType = returnType ? returnType : typeAny();
  if (paramCount > 0) {
    type->params = (Type**)malloc(sizeof(Type*) * (size_t)paramCount);
    if (!type->params) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
    for (int i = 0; i < paramCount; i++) {
      type->params[i] = params[i] ? params[i] : typeAny();
    }
  }
  return type;
}


TypeParam* parseTypeParams(Compiler* c, int* outCount) {
  if (outCount) *outCount = 0;
  if (!match(c, TOKEN_LESS)) return NULL;
  int count = 0;
  int capacity = 0;
  TypeParam* params = NULL;
  do {
    Token name = consume(c, TOKEN_IDENTIFIER, "Expect type parameter name.");
    ObjString* nameStr = stringFromToken(c->vm, name);
    ObjString* constraint = NULL;
    if (match(c, TOKEN_COLON)) {
      Token constraintName = consume(c, TOKEN_IDENTIFIER, "Expect interface name after ':'.");
      constraint = stringFromToken(c->vm, constraintName);
    }
    if (count >= capacity) {
      int oldCap = capacity;
      capacity = GROW_CAPACITY(oldCap);
      params = GROW_ARRAY(TypeParam, params, oldCap, capacity);
      if (!params) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
      }
    }
    params[count].name = nameStr;
    params[count].constraint = constraint;
    count++;
  } while (match(c, TOKEN_COMMA));
  consume(c, TOKEN_GREATER, "Expect '>' after type parameters.");
  if (outCount) *outCount = count;
  return params;
}

Type* typeFromToken(Compiler* c, Token token) {
  if (tokenMatches(token, "number")) return typeNumber();
  if (tokenMatches(token, "string")) return typeString();
  if (tokenMatches(token, "bool") || tokenMatches(token, "boolean")) return typeBool();
  if (tokenMatches(token, "null") || tokenMatches(token, "void")) return typeNull();
  if (tokenMatches(token, "any")) return typeAny();
  if (tokenMatches(token, "array")) return typeArray(c->typecheck, typeAny());
  if (tokenMatches(token, "map")) return typeMap(c->typecheck, typeString(), typeAny());
  ObjString* name = stringFromToken(c->vm, token);
  TypeAlias* alias = typeAliasLookup(c->typecheck, name);
  if (alias && alias->type) {
    return typeClone(c->typecheck, alias->type);
  }
  return typeNamed(c->typecheck, name);
}

Type* parseTypeArguments(Compiler* c, Type* base, Token typeToken) {
  if (!match(c, TOKEN_LESS)) {
    return base;
  }

  if (base->kind == TYPE_ARRAY) {
    Type* elem = parseType(c);
    consume(c, TOKEN_GREATER, "Expect '>' after array type.");
    return typeArray(c->typecheck, elem);
  }

  if (base->kind == TYPE_MAP) {
    Type* key = parseType(c);
    Type* value = NULL;
    if (match(c, TOKEN_COMMA)) {
      value = parseType(c);
    } else {
      value = key;
      key = typeString();
    }
    if (!typeIsAny(key) && key->kind != TYPE_STRING) {
      typeErrorAt(c, typeToken, "Map keys must be string.");
      key = typeString();
    }
    consume(c, TOKEN_GREATER, "Expect '>' after map type.");
    return typeMap(c->typecheck, key, value);
  }

  if (base->kind == TYPE_NAMED) {
    int count = 0;
    int capacity = 0;
    Type** args = NULL;
    if (!check(c, TOKEN_GREATER)) {
      do {
        Type* arg = parseType(c);
        if (count >= capacity) {
          int oldCap = capacity;
          capacity = GROW_CAPACITY(oldCap);
          args = GROW_ARRAY(Type*, args, oldCap, capacity);
          if (!args) {
            fprintf(stderr, "Out of memory.\n");
            exit(1);
          }
        }
        args[count++] = arg;
      } while (match(c, TOKEN_COMMA));
    }
    consume(c, TOKEN_GREATER, "Expect '>' after type arguments.");
    base->typeArgs = args;
    base->typeArgCount = count;
    return base;
  }

  typeErrorAt(c, typeToken, "Only array/map/named types accept type arguments.");
  int depth = 1;
  while (!isAtEnd(c) && depth > 0) {
    if (match(c, TOKEN_LESS)) depth++;
    else if (match(c, TOKEN_GREATER)) depth--;
    else advance(c);
  }
  return base;
}

Type* parseTypePrimary(Compiler* c) {
  if (!check(c, TOKEN_IDENTIFIER) && !check(c, TOKEN_NULL)) {
    errorAtCurrent(c, "Expect type name.");
    return typeAny();
  }
  Token name = advance(c);
  TypeParam* param = typeParamFindToken(c->typecheck, name);
  Type* base = param ? typeGeneric(c->typecheck, param->name) : typeFromToken(c, name);
  if (check(c, TOKEN_LESS)) {
    base = parseTypeArguments(c, base, name);
  }
  if (match(c, TOKEN_QUESTION)) {
    base = typeMakeNullable(c->typecheck, base);
  }
  return base;
}

Type* parseType(Compiler* c) {
  Type* type = parseTypePrimary(c);
  while (match(c, TOKEN_PIPE)) {
    Type* next = parseTypePrimary(c);
    type = typeUnion(c->typecheck, type, next);
  }
  return type;
}

Type* typeMerge(TypeChecker* tc, Type* current, Type* next) {
  if (!current) return next;
  if (!next) return current;
  if (current->kind == TYPE_UNKNOWN) return next;
  if (next->kind == TYPE_UNKNOWN) return current;
  if (typeEquals(current, next)) return current;
  if (current->kind == TYPE_NULL) {
    return typeMakeNullable(tc, next);
  }
  if (next->kind == TYPE_NULL) {
    return typeMakeNullable(tc, current);
  }
  if (current->kind == TYPE_UNION || next->kind == TYPE_UNION) {
    return typeUnion(tc, current, next);
  }
  if (current->kind == next->kind) {
    if (typeEquals(current, next)) return current;
    switch (current->kind) {
      case TYPE_NUMBER:
      case TYPE_STRING:
      case TYPE_BOOL:
        return typeMakeNullable(tc, current);
      case TYPE_UNION:
        break;
      case TYPE_NAMED:
        if (typeNamesEqual(current->name, next->name)) {
          return typeMakeNullable(tc, current);
        }
        break;
        case TYPE_GENERIC:
          if (typeNamesEqual(current->name, next->name)) {
            return typeMakeNullable(tc, current);
          }
          break;
      case TYPE_ARRAY:
        if (typeEquals(current->elem, next->elem)) {
          return typeMakeNullable(tc, current);
        }
        break;
      case TYPE_MAP:
        if (typeEquals(current->key, next->key) &&
            typeEquals(current->value, next->value)) {
          return typeMakeNullable(tc, current);
        }
        break;
      case TYPE_FUNCTION:
        if (typeEquals(current, next)) return current;
        break;
      case TYPE_ANY:
      case TYPE_UNKNOWN:
      case TYPE_NULL:
        break;
    }
  }
  if (typeIsAny(current) || typeIsAny(next)) return typeAny();
  return typeUnion(tc, current, next);
}

bool typeEnsureNonNull(Compiler* c, Token token, Type* type, const char* message) {
  if (!typecheckEnabled(c)) return true;
  if (typeIsAny(type)) return true;
  if (typeIsNullable(type)) {
    typeErrorAt(c, token, "%s", message);
    return false;
  }
  return true;
}

Type* typeUnaryResult(Compiler* c, Token op, Type* right) {
  if (right && right->kind == TYPE_UNION) return typeAny();
  if (op.type == TOKEN_MINUS) {
    typeEnsureNonNull(c, op, right, "Unary '-' expects a non-null number.");
    if (!typeIsAny(right) && right->kind != TYPE_NUMBER) {
      typeErrorAt(c, op, "Unary '-' expects a number.");
    }
    return typeNumber();
  }
  if (op.type == TOKEN_BANG) {
    return typeBool();
  }
  return typeAny();
}

Type* typeBinaryResult(Compiler* c, Token op, Type* left, Type* right) {
  if ((left && left->kind == TYPE_UNION) || (right && right->kind == TYPE_UNION)) {
    return typeAny();
  }
  switch (op.type) {
    case TOKEN_DOT_DOT:
      typeEnsureNonNull(c, op, left, "Range expects non-null numbers.");
      typeEnsureNonNull(c, op, right, "Range expects non-null numbers.");
      if (!typeIsAny(left) && left->kind != TYPE_NUMBER) {
        typeErrorAt(c, op, "Range expects numbers.");
      }
      if (!typeIsAny(right) && right->kind != TYPE_NUMBER) {
        typeErrorAt(c, op, "Range expects numbers.");
      }
      return typeNamed(c->typecheck, copyString(c->vm, "range"));
    case TOKEN_PLUS:
      typeEnsureNonNull(c, op, left, "Operator '+' expects non-null operands.");
      typeEnsureNonNull(c, op, right, "Operator '+' expects non-null operands.");
      if (left->kind == TYPE_NUMBER && right->kind == TYPE_NUMBER) return typeNumber();
      if (left->kind == TYPE_STRING && right->kind == TYPE_STRING) return typeString();
      if (typeIsAny(left) || typeIsAny(right)) return typeAny();
      typeErrorAt(c, op, "Operator '+' expects two numbers or two strings.");
      return typeAny();
    case TOKEN_MINUS:
    case TOKEN_STAR:
    case TOKEN_SLASH:
      typeEnsureNonNull(c, op, left, "Operator expects non-null numbers.");
      typeEnsureNonNull(c, op, right, "Operator expects non-null numbers.");
      if (!typeIsAny(left) && left->kind != TYPE_NUMBER) {
        typeErrorAt(c, op, "Operator expects numbers.");
      }
      if (!typeIsAny(right) && right->kind != TYPE_NUMBER) {
        typeErrorAt(c, op, "Operator expects numbers.");
      }
      return typeNumber();
    case TOKEN_GREATER:
    case TOKEN_GREATER_EQUAL:
    case TOKEN_LESS:
    case TOKEN_LESS_EQUAL:
      typeEnsureNonNull(c, op, left, "Comparison expects non-null numbers.");
      typeEnsureNonNull(c, op, right, "Comparison expects non-null numbers.");
      if (!typeIsAny(left) && left->kind != TYPE_NUMBER) {
        typeErrorAt(c, op, "Comparison expects numbers.");
      }
      if (!typeIsAny(right) && right->kind != TYPE_NUMBER) {
        typeErrorAt(c, op, "Comparison expects numbers.");
      }
      return typeBool();
    case TOKEN_BANG_EQUAL:
    case TOKEN_EQUAL_EQUAL:
      return typeBool();
    default:
      return typeAny();
  }
}

Type* typeLogicalResult(Type* left, Type* right) {
  if (typeIsAny(left) || typeIsAny(right)) return typeAny();
  if (typeEquals(left, right)) return left;
  return typeAny();
}

Type* typeIndexResult(Compiler* c, Token op, Type* objectType, Type* indexType) {
  if (typeIsAny(objectType)) return typeAny();
  if (objectType->kind == TYPE_NULL) return typeNull();
  if (objectType->kind == TYPE_UNION) return typeAny();
  if (objectType->kind == TYPE_ARRAY) {
    if (!typeIsAny(indexType) && indexType->kind != TYPE_NUMBER) {
      typeErrorAt(c, op, "Array index expects a number.");
    }
    return objectType->elem ? objectType->elem : typeAny();
  }
  if (objectType->kind == TYPE_MAP) {
    if (!typeIsAny(indexType) && indexType->kind != TYPE_STRING) {
      typeErrorAt(c, op, "Map index expects a string.");
    }
    return objectType->value ? objectType->value : typeAny();
  }
  return typeAny();
}

void typeCheckIndexAssign(Compiler* c, Token op, Type* objectType, Type* indexType,
                                 Type* valueType) {
  if (typeIsAny(objectType)) return;
  if (objectType->kind == TYPE_UNION) return;
  if (!typeEnsureNonNull(c, op, objectType,
                         "Cannot index nullable value. Use '?.['.")) {
    return;
  }
  if (objectType->kind == TYPE_ARRAY) {
    if (!typeIsAny(indexType) && indexType->kind != TYPE_NUMBER) {
      typeErrorAt(c, op, "Array index expects a number.");
    }
    if (objectType->elem && !typeAssignable(objectType->elem, valueType)) {
      char expected[64];
      char got[64];
      typeToString(objectType->elem, expected, sizeof(expected));
      typeToString(valueType, got, sizeof(got));
      typeErrorAt(c, op, "Array element expects %s but got %s.", expected, got);
    }
    return;
  }
  if (objectType->kind == TYPE_MAP) {
    if (!typeIsAny(indexType) && indexType->kind != TYPE_STRING) {
      typeErrorAt(c, op, "Map index expects a string.");
    }
    if (objectType->value && !typeAssignable(objectType->value, valueType)) {
      char expected[64];
      char got[64];
      typeToString(objectType->value, expected, sizeof(expected));
      typeToString(valueType, got, sizeof(got));
      typeErrorAt(c, op, "Map value expects %s but got %s.", expected, got);
    }
  }
}


