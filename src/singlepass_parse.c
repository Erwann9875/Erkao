#include "singlepass_internal.h"

#include "interpreter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expression(Compiler* c);
static void declaration(Compiler* c);
static void statement(Compiler* c);
static void block(Compiler* c, Token open);
static void matchExpression(Compiler* c, bool canAssign);
static void deferStatement(Compiler* c);
static void map(Compiler* c, bool canAssign);

static bool isTypeDeclarationStart(Compiler* c) {
  return check(c, TOKEN_TYPE_KW) &&
         checkNext(c, TOKEN_IDENTIFIER) &&
         checkNextNext(c, TOKEN_EQUAL);
}

static void parsePrecedence(Compiler* c, Precedence prec);
static ParseRule* getRule(ErkaoTokenType type);

static void number(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token token = previous(c);
  char* temp = copyTokenLexeme(token);
  double value = strtod(temp, NULL);
  free(temp);
  emitConstant(c, NUMBER_VAL(value), token);
  typePush(c, typeNumber());
}

double parseNumberToken(Token token) {
  char* temp = copyTokenLexeme(token);
  double value = strtod(temp, NULL);
  free(temp);
  return value;
}

static void string(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token token = previous(c);
  char* value = parseStringLiteral(token);
  ObjString* str = takeStringWithLength(c->vm, value, (int)strlen(value));
  emitConstant(c, OBJ_VAL(str), token);
  typePush(c, typeString());
}

static void stringSegment(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token segment = previous(c);
  char* value = parseStringSegment(segment);
  ObjString* str = takeStringWithLength(c->vm, value, (int)strlen(value));
  emitConstant(c, OBJ_VAL(str), segment);

  while (match(c, TOKEN_INTERP_START)) {
    Token interpStart = previous(c);
    expression(c);
    typePop(c);
    consumeClosing(c, TOKEN_INTERP_END, "Expect '}' after interpolation.", interpStart);
    emitByte(c, OP_STRINGIFY, segment);
    emitByte(c, OP_ADD, segment);

    Token tail = consume(c, TOKEN_STRING_SEGMENT, "Expect string segment after interpolation.");
    char* tailValue = parseStringSegment(tail);
    ObjString* tailStr = takeStringWithLength(c->vm, tailValue, (int)strlen(tailValue));
    emitConstant(c, OBJ_VAL(tailStr), tail);
    emitByte(c, OP_ADD, tail);
  }
  typePush(c, typeString());
}

static void literal(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token token = previous(c);
  switch (token.type) {
    case TOKEN_FALSE: emitByte(c, OP_FALSE, token); break;
    case TOKEN_TRUE: emitByte(c, OP_TRUE, token); break;
    case TOKEN_NULL: emitByte(c, OP_NULL, token); break;
    default: break;
  }
  if (token.type == TOKEN_FALSE || token.type == TOKEN_TRUE) {
    typePush(c, typeBool());
  } else if (token.type == TOKEN_NULL) {
    typePush(c, typeNull());
  }
}

static void matchExpression(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token keyword = previous(c);
  Token openParen = consume(c, TOKEN_LEFT_PAREN, "Expect '(' after 'match'.");
  expression(c);
  Type* matchType = typePop(c);
  bool hasMatchVar = c->lastExprWasVar;
  Token matchVar = c->lastExprVar;
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after match value.", openParen);
  Token openBrace = consume(c, TOKEN_LEFT_BRACE, "Expect '{' after match value.");

  int matchValue = emitTempNameConstant(c, "match");
  emitDefineVarConstant(c, matchValue);
  int resultName = emitTempNameConstant(c, "match_result");
  emitByte(c, OP_NULL, noToken());
  emitDefineVarConstant(c, resultName);

  JumpList endJumps;
  initJumpList(&endJumps);
  int previousJump = -1;
  bool hasDefault = false;
  bool hasCatchAll = false;
  EnumInfo* matchEnum = NULL;
  bool* variantUsed = NULL;
  int variantUsedCount = 0;
  bool sawEnumPattern = false;
  ConstValue* literalUsed = NULL;
  int literalUsedCount = 0;
  int literalUsedCapacity = 0;
  Type* resultType = typeUnknown();

  while (!check(c, TOKEN_RIGHT_BRACE) && !isAtEnd(c)) {
    if (match(c, TOKEN_CASE)) {
      if (previousJump != -1) {
        patchJump(c, previousJump, keyword);
        emitByte(c, OP_POP, noToken());
      }
      Pattern* pattern = parsePattern(c);
      bool hasGuard = match(c, TOKEN_IF);
      PatternBindingList bindings;
      patternBindingListInit(&bindings);

      if (hasCatchAll || hasDefault) {
        errorAt(c, pattern->token, "Unreachable case.");
      }
      if (!hasGuard) {
        if (patternIsCatchAll(pattern)) {
          hasCatchAll = true;
        }
        ConstValue literalValue;
        if (patternConstValue(pattern, &literalValue)) {
          if (constValueListContains(literalUsed, literalUsedCount, &literalValue)) {
            errorAt(c, pattern->token, "Unreachable case.");
            constValueFree(&literalValue);
          } else {
            constValueListAdd(&literalUsed, &literalUsedCount,
                              &literalUsedCapacity, &literalValue);
          }
        }
      }

      if (pattern && pattern->kind == PATTERN_ENUM) {
        EnumInfo* info = findEnumInfo(c, pattern->as.enumPattern.enumToken);
        if (info && info->isAdt) {
          if (!matchEnum) {
            matchEnum = info;
            variantUsedCount = info->variantCount;
            if (variantUsedCount > 0) {
              variantUsed = (bool*)calloc((size_t)variantUsedCount, sizeof(bool));
              if (!variantUsed) {
                fprintf(stderr, "Out of memory.\n");
                exit(1);
              }
            }
          } else if (matchEnum != info) {
            errorAt(c, pattern->as.enumPattern.enumToken,
                    "Match patterns must use a single enum.");
          }

          EnumVariantInfo* variantInfo =
              findEnumVariant(info, pattern->as.enumPattern.variantToken);
          if (variantInfo && variantInfo->arity == pattern->as.enumPattern.argCount) {
            int variantIndex = enumVariantIndex(matchEnum,
                                                pattern->as.enumPattern.variantToken);
            if (variantIndex >= 0 && variantIndex < variantUsedCount) {
              if (variantUsed && variantUsed[variantIndex]) {
                errorAt(c, pattern->as.enumPattern.variantToken, "Unreachable case.");
              }
              if (!hasGuard && variantUsed) {
                variantUsed[variantIndex] = true;
              }
            }
          }
          sawEnumPattern = true;
        }
      }

      emitPatternMatchValue(c, matchValue, pattern, &bindings);
      previousJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
      emitByte(c, OP_POP, noToken());

      emitByte(c, OP_BEGIN_SCOPE, noToken());
      c->scopeDepth++;
      typeCheckerEnterScope(c);
      emitPatternBindings(c, matchValue, &bindings, OP_DEFINE_VAR, matchType);
      if (typecheckEnabled(c) && hasMatchVar &&
          !patternBindingFind(&bindings, matchVar)) {
        Type* narrowed = typeNarrowByPattern(c, matchType, pattern);
        typeDefine(c, matchVar, narrowed ? narrowed : typeAny(), true);
      }

      int guardJump = -1;
      if (hasGuard) {
        expression(c);
        Type* guardType = typePop(c);
        if (typecheckEnabled(c) && guardType &&
            guardType->kind != TYPE_BOOL &&
            guardType->kind != TYPE_ANY &&
            guardType->kind != TYPE_UNKNOWN) {
          typeErrorAt(c, previous(c), "Guard expects bool.");
        }
        guardJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
        emitByte(c, OP_POP, noToken());
      }

      consume(c, TOKEN_COLON, "Expect ':' after case pattern.");
      expression(c);
      Type* caseType = typePop(c);
      resultType = typeMerge(c->typecheck, resultType, caseType);
      emitSetVarConstant(c, resultName);
      emitByte(c, OP_POP, noToken());
      if (match(c, TOKEN_SEMICOLON)) {
      }

      emitByte(c, OP_END_SCOPE, noToken());
      c->scopeDepth--;
      typeCheckerExitScope(c);
      emitGc(c);

      int endJump = emitJump(c, OP_JUMP, keyword);
      writeJumpList(&endJumps, endJump);

      if (guardJump != -1) {
        patchJump(c, guardJump, keyword);
        emitByte(c, OP_POP, noToken());
        emitByte(c, OP_END_SCOPE, noToken());
        emitGc(c);
      }

      patternBindingListFree(&bindings);
      freePattern(pattern);
    } else if (match(c, TOKEN_DEFAULT)) {
      if (hasCatchAll || hasDefault) {
        errorAt(c, previous(c), "Unreachable default.");
      }
      hasDefault = true;
      if (previousJump != -1) {
        patchJump(c, previousJump, keyword);
        emitByte(c, OP_POP, noToken());
        previousJump = -1;
      }
      consume(c, TOKEN_COLON, "Expect ':' after default.");
      expression(c);
      Type* caseType = typePop(c);
      resultType = typeMerge(c->typecheck, resultType, caseType);
      emitSetVarConstant(c, resultName);
      emitByte(c, OP_POP, noToken());
      if (match(c, TOKEN_SEMICOLON)) {
      }
      int endJump = emitJump(c, OP_JUMP, keyword);
      writeJumpList(&endJumps, endJump);
    } else {
      errorAtCurrent(c, "Expect 'case' or 'default' in match.");
      synchronize(c);
      break;
    }
  }

  if (sawEnumPattern && matchEnum && !hasDefault && !hasCatchAll) {
    bool missing = false;
    for (int i = 0; i < variantUsedCount; i++) {
      if (!variantUsed[i]) {
        missing = true;
        break;
      }
    }
    if (missing) {
      errorAt(c, keyword, "Non-exhaustive match. Add missing enum cases or 'default'.");
    }
  }

  if (previousJump != -1) {
    patchJump(c, previousJump, keyword);
    emitByte(c, OP_POP, noToken());
  }

  consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after match cases.", openBrace);

  int matchEnd = c->chunk->count;
  patchJumpList(c, &endJumps, matchEnd, keyword);
  freeJumpList(&endJumps);
  free(variantUsed);
  constValueListFree(literalUsed, literalUsedCount, literalUsedCapacity);

  emitGetVarConstant(c, resultName);
  if (typecheckEnabled(c)) {
    if (!resultType) resultType = typeAny();
    typePush(c, resultType);
  } else {
    typePush(c, typeAny());
  }
}

static void variable(Compiler* c, bool canAssign) {
  Token name = previous(c);
  int nameIdx = emitStringConstant(c, name);
  if (check(c, TOKEN_LEFT_BRACE) && findStructInfo(c, name)) {
    c->pendingOptionalCall = false;
    c->lastExprWasVar = false;
    emitByte(c, OP_GET_VAR, name);
    emitShort(c, (uint16_t)nameIdx, name);
    typePush(c, typeLookup(c, name));
    consume(c, TOKEN_LEFT_BRACE, "Expect '{' after struct name.");
    map(c, false);
    if (typecheckEnabled(c)) {
      typePop(c);
      typePop(c);
      typePush(c, typeNamed(c->typecheck, stringFromToken(c->vm, name)));
    }
    emitByte(c, OP_CALL, name);
    emitByte(c, 1, name);
    return;
  }
  if (canAssign && match(c, TOKEN_EQUAL)) {
    c->lastExprWasVar = false;
    expression(c);
    Type* valueType = typePop(c);
    typeAssign(c, name, valueType);
    typePush(c, valueType);
    emitByte(c, OP_SET_VAR, name);
    emitShort(c, (uint16_t)nameIdx, name);
  } else {
    emitByte(c, OP_GET_VAR, name);
    emitShort(c, (uint16_t)nameIdx, name);
    typePush(c, typeLookup(c, name));
    c->lastExprWasVar = true;
    c->lastExprVar = name;
  }
}

static void thisExpr(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token token = previous(c);
  int name = emitStringConstant(c, token);
  emitByte(c, OP_GET_THIS, token);
  emitShort(c, (uint16_t)name, token);
  typePush(c, typeAny());
}

static void grouping(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token open = previous(c);
  expression(c);
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after expression.", open);
}

static void unary(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token op = previous(c);
  parsePrecedence(c, PREC_UNARY);
  Type* right = typePop(c);
  typePush(c, typeUnaryResult(c, op, right));
  switch (op.type) {
    case TOKEN_MINUS: emitByte(c, OP_NEGATE, op); break;
    case TOKEN_BANG: emitByte(c, OP_NOT, op); break;
    default: break;
  }
}

static void binary(Compiler* c, bool canAssign) {
  (void)canAssign;
  c->pendingOptionalCall = false;
  c->lastExprWasVar = false;
  Token op = previous(c);
  ParseRule* rule = getRule(op.type);
  parsePrecedence(c, (Precedence)(rule->precedence + 1));
  Type* right = typePop(c);
  Type* left = typePop(c);
  typePush(c, typeBinaryResult(c, op, left, right));
  switch (op.type) {
    case TOKEN_DOT_DOT: {
      int rightTemp = emitTempNameConstant(c, "range_r");
      emitDefineVarConstant(c, rightTemp);
      int leftTemp = emitTempNameConstant(c, "range_l");
      emitDefineVarConstant(c, leftTemp);
      int rangeFn = emitStringConstantFromChars(c, "range", 5);
      emitGetVarConstant(c, rangeFn);
      emitGetVarConstant(c, leftTemp);
      emitGetVarConstant(c, rightTemp);
      emitByte(c, OP_CALL, op);
      emitByte(c, 2, op);
      break;
    }
    case TOKEN_PLUS: emitByte(c, OP_ADD, op); break;
    case TOKEN_MINUS: emitByte(c, OP_SUBTRACT, op); break;
    case TOKEN_STAR: emitByte(c, OP_MULTIPLY, op); break;
    case TOKEN_SLASH: emitByte(c, OP_DIVIDE, op); break;
    case TOKEN_PERCENT: emitByte(c, OP_MODULO, op); break;
    case TOKEN_GREATER: emitByte(c, OP_GREATER, op); break;
    case TOKEN_GREATER_EQUAL: emitByte(c, OP_GREATER_EQUAL, op); break;
    case TOKEN_LESS: emitByte(c, OP_LESS, op); break;
    case TOKEN_LESS_EQUAL: emitByte(c, OP_LESS_EQUAL, op); break;
    case TOKEN_BANG_EQUAL: emitBytes(c, OP_EQUAL, OP_NOT, op); break;
    case TOKEN_EQUAL_EQUAL: emitByte(c, OP_EQUAL, op); break;
    default: break;
  }
}

static void andExpr(Compiler* c, bool canAssign) {
  (void)canAssign;
  c->pendingOptionalCall = false;
  Token op = previous(c);
  int jumpIfFalse = emitJump(c, OP_JUMP_IF_FALSE, op);
  emitByte(c, OP_POP, noToken());
  parsePrecedence(c, PREC_AND);
  Type* right = typePop(c);
  Type* left = typePop(c);
  typePush(c, typeLogicalResult(left, right));
  patchJump(c, jumpIfFalse, op);
}

static void orExpr(Compiler* c, bool canAssign) {
  (void)canAssign;
  c->pendingOptionalCall = false;
  Token op = previous(c);
  int jumpIfFalse = emitJump(c, OP_JUMP_IF_FALSE, op);
  int jumpToEnd = emitJump(c, OP_JUMP, op);
  patchJump(c, jumpIfFalse, op);
  emitByte(c, OP_POP, noToken());
  parsePrecedence(c, PREC_OR);
  Type* right = typePop(c);
  Type* left = typePop(c);
  typePush(c, typeLogicalResult(left, right));
  patchJump(c, jumpToEnd, op);
}

static void call(Compiler* c, bool canAssign) {
  (void)canAssign;
  c->lastExprWasVar = false;
  Token paren = previous(c);
  bool optionalCall = c->pendingOptionalCall;
  c->pendingOptionalCall = false;
  int argc = 0;
  if (!check(c, TOKEN_RIGHT_PAREN)) {
    do {
      if (argc >= ERK_MAX_ARGS) {
        errorAtCurrent(c, "Too many arguments.");
      }
      expression(c);
      argc++;
    } while (match(c, TOKEN_COMMA));
  }
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after arguments.", paren);
    if (typecheckEnabled(c)) {
      Type* argTypes[ERK_MAX_ARGS];
      for (int i = argc - 1; i >= 0; i--) {
        argTypes[i] = typePop(c);
      }
      Type* callee = typePop(c);
      Type* result = typeAny();
      if (callee && callee->kind == TYPE_FUNCTION) {
        int bindingCount = callee->typeParamCount;
        TypeBinding* bindings = NULL;
        if (bindingCount > 0 && callee->typeParams) {
          bindings = (TypeBinding*)malloc(sizeof(TypeBinding) * (size_t)bindingCount);
          if (!bindings) {
            fprintf(stderr, "Out of memory.\n");
            exit(1);
          }
          for (int i = 0; i < bindingCount; i++) {
            bindings[i].name = callee->typeParams[i].name;
            bindings[i].constraint = callee->typeParams[i].constraint;
            bindings[i].bound = NULL;
          }
        }
        if (callee->paramCount >= 0 && callee->paramCount != argc) {
          typeErrorAt(c, paren, "Function expects %d arguments but got %d.",
                      callee->paramCount, argc);
        } else if (callee->params) {
          int checkCount = callee->paramCount >= 0 ? callee->paramCount : argc;
          for (int i = 0; i < checkCount && i < argc; i++) {
            bool ok = true;
            if (bindings) {
              ok = typeUnify(c, callee->params[i], argTypes[i], bindings, bindingCount, paren);
            } else {
              ok = typeAssignable(callee->params[i], argTypes[i]);
            }
            if (!ok) {
              if (bindings && callee->params[i]->kind == TYPE_GENERIC) {
                continue;
              }
              char expected[64];
              char got[64];
              typeToString(callee->params[i], expected, sizeof(expected));
              typeToString(argTypes[i], got, sizeof(got));
              typeErrorAt(c, paren, "Argument %d expects %s but got %s.",
                          i + 1, expected, got);
            }
          }
        }
        result = callee->returnType ? callee->returnType : typeAny();
        if (bindings) {
          result = typeSubstitute(c->typecheck, result, bindings, bindingCount);
          free(bindings);
        }
      }
      if (!optionalCall) {
        typeEnsureNonNull(c, paren, callee, "Cannot call nullable value. Use '?.'.");
      } else if (typeIsNullable(callee)) {
      result = typeMakeNullable(c->typecheck, result);
    }
    typePush(c, result);
  }
  emitByte(c, optionalCall ? OP_CALL_OPTIONAL : OP_CALL, paren);
  emitByte(c, (uint8_t)argc, paren);
}

static void dot(Compiler* c, bool canAssign) {
  c->pendingOptionalCall = false;
  c->lastExprWasVar = false;
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect property name after '.'.");
  int nameIdx = emitStringConstant(c, name);
  Type* objectType = typePop(c);
  typeEnsureNonNull(c, name, objectType,
                    "Cannot access property on nullable value. Use '?.'.");
  if (canAssign && match(c, TOKEN_EQUAL)) {
    expression(c);
    Type* valueType = typePop(c);
    typePush(c, valueType);
    emitByte(c, OP_SET_PROPERTY, name);
    emitShort(c, (uint16_t)nameIdx, name);
  } else if (!c->forbidCall && check(c, TOKEN_LEFT_PAREN)) {
    Token paren = advance(c);
    int argc = 0;
    if (!check(c, TOKEN_RIGHT_PAREN)) {
      do {
        if (argc >= ERK_MAX_ARGS) {
          errorAtCurrent(c, "Too many arguments.");
        }
        expression(c);
        argc++;
      } while (match(c, TOKEN_COMMA));
    }
    consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after arguments.", paren);
    if (typecheckEnabled(c)) {
      Type* argTypes[ERK_MAX_ARGS];
      for (int i = argc - 1; i >= 0; i--) {
        argTypes[i] = typePop(c);
      }
      Type* memberType = typeLookupStdlibMember(c, objectType, name);
      Type* resultType = typeAny();
      if (memberType && memberType->kind == TYPE_FUNCTION) {
        if (memberType->paramCount >= 0 && memberType->paramCount != argc) {
          typeErrorAt(c, paren, "Function expects %d arguments but got %d.",
                      memberType->paramCount, argc);
        } else if (memberType->params) {
          int checkCount = memberType->paramCount >= 0 ? memberType->paramCount : argc;
          for (int i = 0; i < checkCount && i < argc; i++) {
            if (!typeAssignable(memberType->params[i], argTypes[i])) {
              char expected[64];
              char got[64];
              typeToString(memberType->params[i], expected, sizeof(expected));
              typeToString(argTypes[i], got, sizeof(got));
              typeErrorAt(c, paren, "Argument %d expects %s but got %s.",
                          i + 1, expected, got);
            }
          }
        }
        resultType = memberType->returnType ? memberType->returnType : typeAny();
      } else if (memberType && !typeIsAny(memberType)) {
        typeErrorAt(c, paren, "Property is not callable.");
      }
      typePush(c, resultType);
    }
    emitByte(c, OP_INVOKE, paren);
    emitShort(c, (uint16_t)nameIdx, name);
    emitByte(c, (uint8_t)argc, paren);
  } else {
    Type* memberType = typeAny();
    if (typecheckEnabled(c)) {
      memberType = typeLookupStdlibMember(c, objectType, name);
    }
    typePush(c, memberType);
    emitByte(c, OP_GET_PROPERTY, name);
    emitShort(c, (uint16_t)nameIdx, name);
  }
  (void)objectType;
}

static void optionalDot(Compiler* c, bool canAssign) {
  (void)canAssign;
  c->lastExprWasVar = false;
  if (check(c, TOKEN_LEFT_PAREN)) {
    if (c->forbidCall) {
      errorAtCurrent(c, "Optional call is not allowed here.");
      return;
    }
    c->pendingOptionalCall = true;
    return;
  }
  if (match(c, TOKEN_LEFT_BRACKET)) {
    Token bracket = previous(c);
    expression(c);
    Type* indexType = typePop(c);
    Type* objectType = typePop(c);
    consumeClosing(c, TOKEN_RIGHT_BRACKET, "Expect ']' after index.", bracket);
    Type* result = typeIndexResult(c, bracket, objectType, indexType);
    typePush(c, typeMakeNullable(c->typecheck, result));
    emitByte(c, OP_GET_INDEX_OPTIONAL, bracket);
    if (!c->forbidCall) {
      c->pendingOptionalCall = true;
    }
    return;
  }
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect property name after '?.'.");
  int nameIdx = emitStringConstant(c, name);
  Type* objectType = typePop(c);
  Type* memberType = typeAny();
  if (typecheckEnabled(c)) {
    memberType = typeLookupStdlibMember(c, objectType, name);
  }
  typePush(c, typeMakeNullable(c->typecheck, memberType));
  emitByte(c, OP_GET_PROPERTY_OPTIONAL, name);
  emitShort(c, (uint16_t)nameIdx, name);
  if (!c->forbidCall) {
    c->pendingOptionalCall = true;
  }
}

static void tryUnwrap(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token op = previous(c);
  c->pendingOptionalCall = false;
  c->lastExprWasVar = false;
  typePop(c);
  typePush(c, typeAny());
  emitByte(c, OP_TRY_UNWRAP, op);
}

static void index_(Compiler* c, bool canAssign) {
  c->pendingOptionalCall = false;
  c->lastExprWasVar = false;
  Token bracket = previous(c);
  expression(c);
  Type* indexType = typePop(c);
  Type* objectType = typePop(c);
  typeEnsureNonNull(c, bracket, objectType,
                    "Cannot index nullable value. Use '?.['.");
  consumeClosing(c, TOKEN_RIGHT_BRACKET, "Expect ']' after index.", bracket);
  if (canAssign && match(c, TOKEN_EQUAL)) {
    expression(c);
    Type* valueType = typePop(c);
    typeCheckIndexAssign(c, bracket, objectType, indexType, valueType);
    typePush(c, valueType);
    emitByte(c, OP_SET_INDEX, bracket);
  } else {
    typePush(c, typeIndexResult(c, bracket, objectType, indexType));
    emitByte(c, OP_GET_INDEX, bracket);
  }
}

static void array(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token open = previous(c);
  int count = 0;
  Type* elementType = NULL;
  emitByte(c, OP_ARRAY, noToken());
  emitShort(c, 0, noToken());
  int sizeOffset = c->chunk->count - 2;
  if (!check(c, TOKEN_RIGHT_BRACKET)) {
    do {
      expression(c);
      Type* itemType = typePop(c);
      elementType = typeMerge(c->typecheck, elementType, itemType);
      emitByte(c, OP_ARRAY_APPEND, noToken());
      count++;
    } while (match(c, TOKEN_COMMA));
  }
  consumeClosing(c, TOKEN_RIGHT_BRACKET, "Expect ']' after array literal.", open);
  c->chunk->code[sizeOffset] = (uint8_t)((count >> 8) & 0xff);
  c->chunk->code[sizeOffset + 1] = (uint8_t)(count & 0xff);
  if (typecheckEnabled(c)) {
    if (!elementType) elementType = typeAny();
    typePush(c, typeArray(c->typecheck, elementType));
  }
}

static void map(Compiler* c, bool canAssign) {
  (void)canAssign;
  Token open = previous(c);
  int count = 0;
  Type* valueType = NULL;
  emitByte(c, OP_MAP, noToken());
  emitShort(c, 0, noToken());
  int sizeOffset = c->chunk->count - 2;
  if (!check(c, TOKEN_RIGHT_BRACE)) {
    do {
      if (match(c, TOKEN_IDENTIFIER) || match(c, TOKEN_TYPE_KW)) {
        Token key = previous(c);
        char* keyName = copyTokenLexeme(key);
        ObjString* keyStr = takeStringWithLength(c->vm, keyName, key.length);
        emitConstant(c, OBJ_VAL(keyStr), key);
      } else if (match(c, TOKEN_STRING)) {
        Token key = previous(c);
        char* keyName = parseStringLiteral(key);
        ObjString* keyStr = takeStringWithLength(c->vm, keyName, (int)strlen(keyName));
        emitConstant(c, OBJ_VAL(keyStr), key);
      } else {
        errorAtCurrent(c, "Map keys must be identifiers or strings.");
        break;
      }
      consume(c, TOKEN_COLON, "Expect ':' after map key.");
      expression(c);
      Type* entryType = typePop(c);
      valueType = typeMerge(c->typecheck, valueType, entryType);
      emitByte(c, OP_MAP_SET, noToken());
      count++;
    } while (match(c, TOKEN_COMMA));
  }
  consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after map literal.", open);
  c->chunk->code[sizeOffset] = (uint8_t)((count >> 8) & 0xff);
  c->chunk->code[sizeOffset + 1] = (uint8_t)(count & 0xff);
  if (typecheckEnabled(c)) {
    if (!valueType) valueType = typeAny();
    typePush(c, typeMap(c->typecheck, typeString(), valueType));
  }
}

static ParseRule rules[TOKEN_EOF + 1];
static bool rulesInitialized = false;

typedef struct {
  const CompilerPlugin** entries;
  int count;
  int capacity;
} PluginRegistry;

static PluginRegistry pluginRegistry = {0};

void compiler_register_plugin(const CompilerPlugin* plugin) {
  if (!plugin) return;
  for (int i = 0; i < pluginRegistry.count; i++) {
    if (pluginRegistry.entries[i] == plugin) return;
  }
  if (pluginRegistry.capacity < pluginRegistry.count + 1) {
    int oldCap = pluginRegistry.capacity;
    pluginRegistry.capacity = GROW_CAPACITY(oldCap);
    pluginRegistry.entries = GROW_ARRAY(const CompilerPlugin*,
                                        pluginRegistry.entries,
                                        oldCap,
                                        pluginRegistry.capacity);
    if (!pluginRegistry.entries) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
  }
  pluginRegistry.entries[pluginRegistry.count++] = plugin;

  if (rulesInitialized && plugin->register_rules) {
    ParserRules parserRules = { rules, TOKEN_EOF + 1 };
    plugin->register_rules(&parserRules);
  }
}

void compilerApplyPluginRules(void) {
  if (pluginRegistry.count == 0) return;
  ParserRules parserRules = { rules, TOKEN_EOF + 1 };
  for (int i = 0; i < pluginRegistry.count; i++) {
    const CompilerPlugin* plugin = pluginRegistry.entries[i];
    if (plugin && plugin->register_rules) {
      plugin->register_rules(&parserRules);
    }
  }
}

bool compilerPluginParseStatement(Compiler* c) {
  for (int i = 0; i < pluginRegistry.count; i++) {
    const CompilerPlugin* plugin = pluginRegistry.entries[i];
    if (plugin && plugin->parse_statement && plugin->parse_statement(c)) {
      return true;
    }
  }
  return false;
}

bool compilerPluginParseExpression(Compiler* c, bool canAssign) {
  for (int i = 0; i < pluginRegistry.count; i++) {
    const CompilerPlugin* plugin = pluginRegistry.entries[i];
    if (plugin && plugin->parse_expression &&
        plugin->parse_expression(c, canAssign)) {
      return true;
    }
  }
  return false;
}

void compilerPluginTypeHooks(Compiler* c) {
  if (!typecheckEnabled(c)) return;
  for (int i = 0; i < pluginRegistry.count; i++) {
    const CompilerPlugin* plugin = pluginRegistry.entries[i];
    if (plugin && plugin->type_hook) {
      plugin->type_hook(c->typecheck);
    }
  }
}

static void initRules(void) {
  if (rulesInitialized) return;
  rulesInitialized = true;
  for (int i = 0; i <= TOKEN_EOF; i++) {
    rules[i] = (ParseRule){NULL, NULL, PREC_NONE};
  }
  rules[TOKEN_LEFT_PAREN] = (ParseRule){grouping, call, PREC_CALL};
  rules[TOKEN_LEFT_BRACKET] = (ParseRule){array, index_, PREC_CALL};
  rules[TOKEN_LEFT_BRACE] = (ParseRule){map, NULL, PREC_NONE};
  rules[TOKEN_DOT] = (ParseRule){NULL, dot, PREC_CALL};
  rules[TOKEN_DOT_DOT] = (ParseRule){NULL, binary, PREC_RANGE};
  rules[TOKEN_QUESTION] = (ParseRule){NULL, tryUnwrap, PREC_CALL};
  rules[TOKEN_QUESTION_DOT] = (ParseRule){NULL, optionalDot, PREC_CALL};
  rules[TOKEN_MINUS] = (ParseRule){unary, binary, PREC_TERM};
  rules[TOKEN_PLUS] = (ParseRule){NULL, binary, PREC_TERM};
  rules[TOKEN_SLASH] = (ParseRule){NULL, binary, PREC_FACTOR};
  rules[TOKEN_STAR] = (ParseRule){NULL, binary, PREC_FACTOR};
  rules[TOKEN_PERCENT] = (ParseRule){NULL, binary, PREC_FACTOR};
  rules[TOKEN_BANG] = (ParseRule){unary, NULL, PREC_NONE};
  rules[TOKEN_BANG_EQUAL] = (ParseRule){NULL, binary, PREC_EQUALITY};
  rules[TOKEN_EQUAL_EQUAL] = (ParseRule){NULL, binary, PREC_EQUALITY};
  rules[TOKEN_GREATER] = (ParseRule){NULL, binary, PREC_COMPARISON};
  rules[TOKEN_GREATER_EQUAL] = (ParseRule){NULL, binary, PREC_COMPARISON};
  rules[TOKEN_LESS] = (ParseRule){NULL, binary, PREC_COMPARISON};
  rules[TOKEN_LESS_EQUAL] = (ParseRule){NULL, binary, PREC_COMPARISON};
  rules[TOKEN_IDENTIFIER] = (ParseRule){variable, NULL, PREC_NONE};
  rules[TOKEN_TYPE_KW] = (ParseRule){variable, NULL, PREC_NONE};
  rules[TOKEN_STRING] = (ParseRule){string, NULL, PREC_NONE};
  rules[TOKEN_STRING_SEGMENT] = (ParseRule){stringSegment, NULL, PREC_NONE};
  rules[TOKEN_NUMBER] = (ParseRule){number, NULL, PREC_NONE};
  rules[TOKEN_AND] = (ParseRule){NULL, andExpr, PREC_AND};
  rules[TOKEN_OR] = (ParseRule){NULL, orExpr, PREC_OR};
  rules[TOKEN_MATCH] = (ParseRule){matchExpression, NULL, PREC_NONE};
  rules[TOKEN_FALSE] = (ParseRule){literal, NULL, PREC_NONE};
  rules[TOKEN_TRUE] = (ParseRule){literal, NULL, PREC_NONE};
  rules[TOKEN_NULL] = (ParseRule){literal, NULL, PREC_NONE};
  rules[TOKEN_THIS] = (ParseRule){thisExpr, NULL, PREC_NONE};
  compilerApplyPluginRules();
}

static ParseRule* getRule(ErkaoTokenType type) {
  return &rules[type];
}

static void parsePrecedence(Compiler* c, Precedence prec) {
  bool canAssign = prec <= PREC_ASSIGNMENT;
  advance(c);
  ParseFn prefixRule = getRule(previous(c).type)->prefix;
  if (prefixRule == NULL) {
    char message[128];
    snprintf(message, sizeof(message), "Expect expression. Found %s.",
             tokenDescription(previous(c).type));
    if (compilerPluginParseExpression(c, canAssign)) {
      goto parse_infix;
    }
    errorAt(c, previous(c), message);
    synchronizeExpression(c);
    c->panicMode = false;
    return;
  }
  prefixRule(c, canAssign);

parse_infix:
  while (prec <= getRule(peek(c).type)->precedence) {
    if (c->forbidCall && peek(c).type == TOKEN_LEFT_PAREN) {
      break;
    }
    advance(c);
    ParseFn infixRule = getRule(previous(c).type)->infix;
    if (infixRule != NULL) {
      infixRule(c, canAssign);
    }
  }

  if (canAssign && match(c, TOKEN_EQUAL)) {
    errorAt(c, previous(c), "Invalid assignment target.");
  }
}

static void expression(Compiler* c) {
  c->pendingOptionalCall = false;
  c->lastExprWasVar = false;
  memset(&c->lastExprVar, 0, sizeof(Token));
  parsePrecedence(c, PREC_ASSIGNMENT);
}

static void expressionStatement(Compiler* c) {
  expression(c);
  typePop(c);
  consume(c, TOKEN_SEMICOLON, "Expect ';' after expression.");
  emitByte(c, OP_POP, noToken());
  emitGc(c);
}

static bool isPatternAssignmentStart(Compiler* c) {
  if (!check(c, TOKEN_LEFT_BRACKET) && !check(c, TOKEN_LEFT_BRACE)) return false;
  ErkaoTokenType open = peek(c).type;
  ErkaoTokenType close = open == TOKEN_LEFT_BRACKET ? TOKEN_RIGHT_BRACKET : TOKEN_RIGHT_BRACE;
  int depth = 0;
  for (int i = c->current; i < c->tokens->count; i++) {
    ErkaoTokenType type = c->tokens->tokens[i].type;
    if (type == TOKEN_LEFT_BRACKET || type == TOKEN_LEFT_BRACE || type == TOKEN_LEFT_PAREN) {
      depth++;
      continue;
    }
    if (type == TOKEN_RIGHT_BRACKET || type == TOKEN_RIGHT_BRACE || type == TOKEN_RIGHT_PAREN) {
      depth--;
      if (depth < 0) return false;
      if (depth == 0 && type == close) {
        if (i + 1 < c->tokens->count &&
            c->tokens->tokens[i + 1].type == TOKEN_EQUAL) {
          return true;
        }
        return false;
      }
    }
    if (type == TOKEN_EOF) break;
  }
  return false;
}

static void patternAssignmentStatement(Compiler* c) {
  Pattern* pattern = parsePattern(c);
  consume(c, TOKEN_EQUAL, "Expect '=' after pattern.");
  expression(c);
  Type* valueType = typePop(c);
  consume(c, TOKEN_SEMICOLON, "Expect ';' after pattern assignment.");

  int matchValue = emitTempNameConstant(c, "match");
  emitDefineVarConstant(c, matchValue);

  PatternBindingList bindings;
  patternBindingListInit(&bindings);
  emitPatternMatchOrThrow(c, matchValue, pattern, &bindings);
  emitPatternBindings(c, matchValue, &bindings, OP_SET_VAR, valueType);
  patternBindingListFree(&bindings);
  freePattern(pattern);
  emitGc(c);
}

static void varDeclaration(Compiler* c, bool isConst, bool isExport, bool isPrivate) {
  Pattern* pattern = parsePattern(c);
  if (pattern->kind == PATTERN_BINDING) {
    Token name = pattern->token;
    freePattern(pattern);
    pattern = NULL;

    Type* declaredType = NULL;
    bool hasType = false;
    if (match(c, TOKEN_COLON)) {
      declaredType = parseType(c);
      hasType = true;
    }
    bool hasInitializer = match(c, TOKEN_EQUAL);
    Type* valueType = typeUnknown();
    if (hasInitializer) {
      expression(c);
      valueType = typePop(c);
    } else {
      if (isConst) {
        errorAt(c, name, "Const declarations require an initializer.");
      }
      emitByte(c, OP_NULL, noToken());
      valueType = typeNull();
    }
    consume(c, TOKEN_SEMICOLON, "Expect ';' after variable declaration.");
    int nameIdx = emitStringConstant(c, name);
    emitByte(c, isConst ? OP_DEFINE_CONST : OP_DEFINE_VAR, name);
    emitShort(c, (uint16_t)nameIdx, name);
    if (typecheckEnabled(c)) {
      if (hasType) {
        if (hasInitializer && !typeAssignable(declaredType, valueType)) {
          char expected[64];
          char got[64];
          typeToString(declaredType, expected, sizeof(expected));
          typeToString(valueType, got, sizeof(got));
          typeErrorAt(c, name, "Type mismatch. Expected %s but got %s.", expected, got);
        }
        typeDefine(c, name, declaredType, true);
      } else {
        Type* inferred = hasInitializer ? valueType : typeUnknown();
        typeDefine(c, name, inferred, false);
      }
    }
    if (isPrivate) {
      emitPrivateName(c, nameIdx, name);
    }
    if (isExport) {
      emitByte(c, OP_EXPORT, name);
      emitShort(c, (uint16_t)nameIdx, name);
    }
    emitGc(c);
    return;
  }

  if (match(c, TOKEN_COLON)) {
    errorAt(c, previous(c), "Type annotations require a single identifier.");
    parseType(c);
  }

  Type* valueType = typeUnknown();
  if (!match(c, TOKEN_EQUAL)) {
    errorAt(c, pattern->token, "Pattern declarations require an initializer.");
    emitByte(c, OP_NULL, noToken());
    valueType = typeNull();
  } else {
    expression(c);
    valueType = typePop(c);
  }
  consume(c, TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

  int matchValue = emitTempNameConstant(c, "match");
  emitDefineVarConstant(c, matchValue);

  PatternBindingList bindings;
  patternBindingListInit(&bindings);
  emitPatternMatchOrThrow(c, matchValue, pattern, &bindings);
  emitPatternBindings(c, matchValue, &bindings,
                      isConst ? OP_DEFINE_CONST : OP_DEFINE_VAR, valueType);

  if (isPrivate || isExport) {
    for (int i = 0; i < bindings.count; i++) {
      Token bind = bindings.entries[i].name;
      int nameIdx = emitStringConstant(c, bind);
      if (isPrivate) {
        emitPrivateName(c, nameIdx, bind);
      }
      if (isExport) {
        emitByte(c, OP_EXPORT, bind);
        emitShort(c, (uint16_t)nameIdx, bind);
      }
    }
  }

  patternBindingListFree(&bindings);
  freePattern(pattern);
  emitGc(c);
}

static bool isReservedTypeName(Token name) {
  return tokenMatches(name, "number") || tokenMatches(name, "string") ||
         tokenMatches(name, "bool") || tokenMatches(name, "boolean") ||
         tokenMatches(name, "null") || tokenMatches(name, "void") ||
         tokenMatches(name, "any") || tokenMatches(name, "array") ||
         tokenMatches(name, "map");
}

static void typeDeclaration(Compiler* c) {
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect type alias name.");
  if (isReservedTypeName(name)) {
    errorAt(c, name, "Cannot alias a built-in type name.");
  }
  consume(c, TOKEN_EQUAL, "Expect '=' after type alias name.");
  Type* aliasType = parseType(c);
  consume(c, TOKEN_SEMICOLON, "Expect ';' after type alias.");
  if (typecheckEnabled(c)) {
    typeAliasDefine(c, name, aliasType);
  }
}

static void block(Compiler* c, Token open) {
  while (!check(c, TOKEN_RIGHT_BRACE) && !isAtEnd(c)) {
    declaration(c);
  }
  consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after block.", open);
}

static void blockStatement(Compiler* c) {
  Token open = previous(c);
  emitByte(c, OP_BEGIN_SCOPE, noToken());
  c->scopeDepth++;
  typeCheckerEnterScope(c);
  block(c, open);
  emitByte(c, OP_END_SCOPE, noToken());
  c->scopeDepth--;
  typeCheckerExitScope(c);
  emitGc(c);
}

static void ifStatement(Compiler* c) {
  Token keyword = previous(c);
  if (match(c, TOKEN_LET)) {
    Pattern* pattern = parsePattern(c);
    consume(c, TOKEN_EQUAL, "Expect '=' after let pattern.");
    expression(c);
    Type* matchType = typePop(c);
    bool hasMatchVar = c->lastExprWasVar;
    Token matchVar = c->lastExprVar;
    int matchValue = emitTempNameConstant(c, "match");
    emitDefineVarConstant(c, matchValue);

    bool hasGuard = match(c, TOKEN_IF);
    PatternBindingList bindings;
    patternBindingListInit(&bindings);
    emitPatternMatchValue(c, matchValue, pattern, &bindings);
    int thenJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
    emitByte(c, OP_POP, noToken());

    emitByte(c, OP_BEGIN_SCOPE, noToken());
    c->scopeDepth++;
    typeCheckerEnterScope(c);
    emitPatternBindings(c, matchValue, &bindings, OP_DEFINE_VAR, matchType);
    if (typecheckEnabled(c) && hasMatchVar &&
        !patternBindingFind(&bindings, matchVar)) {
      Type* narrowed = typeNarrowByPattern(c, matchType, pattern);
      typeDefine(c, matchVar, narrowed ? narrowed : typeAny(), true);
    }

    int guardJump = -1;
    if (hasGuard) {
      expression(c);
      Type* guardType = typePop(c);
      if (typecheckEnabled(c) && guardType &&
          guardType->kind != TYPE_BOOL &&
          guardType->kind != TYPE_ANY &&
          guardType->kind != TYPE_UNKNOWN) {
        typeErrorAt(c, previous(c), "Guard expects bool.");
      }
      guardJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
      emitByte(c, OP_POP, noToken());
    }

    statement(c);

    emitByte(c, OP_END_SCOPE, noToken());
    c->scopeDepth--;
    typeCheckerExitScope(c);
    emitGc(c);

    bool hasElse = match(c, TOKEN_ELSE);
    int elseJump = -1;
    if (hasElse) {
      elseJump = emitJump(c, OP_JUMP, keyword);
    }

    int guardToElse = -1;
    if (guardJump != -1) {
      patchJump(c, guardJump, keyword);
      emitByte(c, OP_POP, noToken());
      emitByte(c, OP_END_SCOPE, noToken());
      emitGc(c);
      guardToElse = emitJump(c, OP_JUMP, keyword);
    }

    patchJump(c, thenJump, keyword);
    emitByte(c, OP_POP, noToken());
    if (guardToElse != -1) {
      patchJump(c, guardToElse, keyword);
    }

    patternBindingListFree(&bindings);
    freePattern(pattern);

    if (hasElse) {
      statement(c);
      patchJump(c, elseJump, keyword);
    }
    emitGc(c);
    return;
  }
  Token openParen = consume(c, TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
  if (match(c, TOKEN_MATCH)) {
    Pattern* pattern = parsePattern(c);
    consume(c, TOKEN_EQUAL, "Expect '=' after match pattern.");
    expression(c);
    Type* matchType = typePop(c);
    bool hasMatchVar = c->lastExprWasVar;
    Token matchVar = c->lastExprVar;
    int matchValue = emitTempNameConstant(c, "match");
    emitDefineVarConstant(c, matchValue);

    bool hasGuard = match(c, TOKEN_IF);
    PatternBindingList bindings;
    patternBindingListInit(&bindings);
    emitPatternMatchValue(c, matchValue, pattern, &bindings);
    int thenJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
    emitByte(c, OP_POP, noToken());

    emitByte(c, OP_BEGIN_SCOPE, noToken());
    c->scopeDepth++;
    typeCheckerEnterScope(c);
    emitPatternBindings(c, matchValue, &bindings, OP_DEFINE_VAR, matchType);
    if (typecheckEnabled(c) && hasMatchVar &&
        !patternBindingFind(&bindings, matchVar)) {
      Type* narrowed = typeNarrowByPattern(c, matchType, pattern);
      typeDefine(c, matchVar, narrowed ? narrowed : typeAny(), true);
    }

    int guardJump = -1;
    if (hasGuard) {
      expression(c);
      Type* guardType = typePop(c);
      if (typecheckEnabled(c) && guardType &&
          guardType->kind != TYPE_BOOL &&
          guardType->kind != TYPE_ANY &&
          guardType->kind != TYPE_UNKNOWN) {
        typeErrorAt(c, previous(c), "Guard expects bool.");
      }
      guardJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
      emitByte(c, OP_POP, noToken());
    }

    consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after if condition.", openParen);
    statement(c);

    emitByte(c, OP_END_SCOPE, noToken());
    c->scopeDepth--;
    typeCheckerExitScope(c);
    emitGc(c);

    bool hasElse = match(c, TOKEN_ELSE);
    int elseJump = -1;
    if (hasElse) {
      elseJump = emitJump(c, OP_JUMP, keyword);
    }

    int guardToElse = -1;
    if (guardJump != -1) {
      patchJump(c, guardJump, keyword);
      emitByte(c, OP_POP, noToken());
      emitByte(c, OP_END_SCOPE, noToken());
      emitGc(c);
      guardToElse = emitJump(c, OP_JUMP, keyword);
    }

    patchJump(c, thenJump, keyword);
    emitByte(c, OP_POP, noToken());
    if (guardToElse != -1) {
      patchJump(c, guardToElse, keyword);
    }

    patternBindingListFree(&bindings);
    freePattern(pattern);

    if (hasElse) {
      statement(c);
      patchJump(c, elseJump, keyword);
    }
    emitGc(c);
    return;
  }

  expression(c);
  typePop(c);
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after if condition.", openParen);
  int thenJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
  emitByte(c, OP_POP, noToken());
  statement(c);
  if (match(c, TOKEN_ELSE)) {
    int elseJump = emitJump(c, OP_JUMP, keyword);
    patchJump(c, thenJump, keyword);
    emitByte(c, OP_POP, noToken());
    statement(c);
    patchJump(c, elseJump, keyword);
  } else {
    int endJump = emitJump(c, OP_JUMP, keyword);
    patchJump(c, thenJump, keyword);
    emitByte(c, OP_POP, noToken());
    patchJump(c, endJump, keyword);
  }
  emitGc(c);
}

static void whileStatement(Compiler* c) {
  Token keyword = previous(c);
  int loopStart = c->chunk->count;
  if (match(c, TOKEN_LET)) {
    Pattern* pattern = parsePattern(c);
    consume(c, TOKEN_EQUAL, "Expect '=' after let pattern.");
    expression(c);
    Type* matchType = typePop(c);
    bool hasMatchVar = c->lastExprWasVar;
    Token matchVar = c->lastExprVar;
    int matchValue = emitTempNameConstant(c, "match");
    emitDefineVarConstant(c, matchValue);

    bool hasGuard = match(c, TOKEN_IF);
    PatternBindingList bindings;
    patternBindingListInit(&bindings);
    emitPatternMatchValue(c, matchValue, pattern, &bindings);
    int exitJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
    emitByte(c, OP_POP, noToken());

    int loopScopeDepth = c->scopeDepth;
    emitByte(c, OP_BEGIN_SCOPE, noToken());
    c->scopeDepth++;
    typeCheckerEnterScope(c);
    emitPatternBindings(c, matchValue, &bindings, OP_DEFINE_VAR, matchType);
    if (typecheckEnabled(c) && hasMatchVar &&
        !patternBindingFind(&bindings, matchVar)) {
      Type* narrowed = typeNarrowByPattern(c, matchType, pattern);
      typeDefine(c, matchVar, narrowed ? narrowed : typeAny(), true);
    }

    int guardJump = -1;
    if (hasGuard) {
      expression(c);
      Type* guardType = typePop(c);
      if (typecheckEnabled(c) && guardType &&
          guardType->kind != TYPE_BOOL &&
          guardType->kind != TYPE_ANY &&
          guardType->kind != TYPE_UNKNOWN) {
        typeErrorAt(c, previous(c), "Guard expects bool.");
      }
      guardJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
      emitByte(c, OP_POP, noToken());
    }

    BreakContext loop;
    loop.type = BREAK_LOOP;
    loop.enclosing = c->breakContext;
    loop.scopeDepth = loopScopeDepth;
    initJumpList(&loop.breaks);
    initJumpList(&loop.continues);
    c->breakContext = &loop;

    statement(c);
    emitByte(c, OP_END_SCOPE, noToken());
    c->scopeDepth--;
    typeCheckerExitScope(c);
    int continueTarget = c->chunk->count;
    emitGc(c);
    emitLoop(c, loopStart, keyword);
    c->breakContext = loop.enclosing;

    int guardToExit = -1;
    if (guardJump != -1) {
      patchJump(c, guardJump, keyword);
      emitByte(c, OP_POP, noToken());
      emitByte(c, OP_END_SCOPE, noToken());
      emitGc(c);
      guardToExit = emitJump(c, OP_JUMP, keyword);
    }

    patchJump(c, exitJump, keyword);
    emitByte(c, OP_POP, noToken());
    emitGc(c);
    int loopEnd = c->chunk->count;
    if (guardToExit != -1) {
      patchJump(c, guardToExit, keyword);
    }
    patchJumpList(c, &loop.breaks, loopEnd, keyword);
    patchJumpList(c, &loop.continues, continueTarget, keyword);
    freeJumpList(&loop.breaks);
    freeJumpList(&loop.continues);

    patternBindingListFree(&bindings);
    freePattern(pattern);
    return;
  }
  Token openParen = consume(c, TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
  if (match(c, TOKEN_MATCH)) {
    Pattern* pattern = parsePattern(c);
    consume(c, TOKEN_EQUAL, "Expect '=' after match pattern.");
    expression(c);
    Type* matchType = typePop(c);
    bool hasMatchVar = c->lastExprWasVar;
    Token matchVar = c->lastExprVar;
    int matchValue = emitTempNameConstant(c, "match");
    emitDefineVarConstant(c, matchValue);

    bool hasGuard = match(c, TOKEN_IF);
    PatternBindingList bindings;
    patternBindingListInit(&bindings);
    emitPatternMatchValue(c, matchValue, pattern, &bindings);
    int exitJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
    emitByte(c, OP_POP, noToken());

    int loopScopeDepth = c->scopeDepth;
    emitByte(c, OP_BEGIN_SCOPE, noToken());
    c->scopeDepth++;
    typeCheckerEnterScope(c);
    emitPatternBindings(c, matchValue, &bindings, OP_DEFINE_VAR, matchType);
    if (typecheckEnabled(c) && hasMatchVar &&
        !patternBindingFind(&bindings, matchVar)) {
      Type* narrowed = typeNarrowByPattern(c, matchType, pattern);
      typeDefine(c, matchVar, narrowed ? narrowed : typeAny(), true);
    }

    int guardJump = -1;
    if (hasGuard) {
      expression(c);
      Type* guardType = typePop(c);
      if (typecheckEnabled(c) && guardType &&
          guardType->kind != TYPE_BOOL &&
          guardType->kind != TYPE_ANY &&
          guardType->kind != TYPE_UNKNOWN) {
        typeErrorAt(c, previous(c), "Guard expects bool.");
      }
      guardJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
      emitByte(c, OP_POP, noToken());
    }

    consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after condition.", openParen);

    BreakContext loop;
    loop.type = BREAK_LOOP;
    loop.enclosing = c->breakContext;
    loop.scopeDepth = loopScopeDepth;
    initJumpList(&loop.breaks);
    initJumpList(&loop.continues);
    c->breakContext = &loop;

    statement(c);
    emitByte(c, OP_END_SCOPE, noToken());
    c->scopeDepth--;
    typeCheckerExitScope(c);
    int continueTarget = c->chunk->count;
    emitGc(c);
    emitLoop(c, loopStart, keyword);
    c->breakContext = loop.enclosing;

    int guardToExit = -1;
    if (guardJump != -1) {
      patchJump(c, guardJump, keyword);
      emitByte(c, OP_POP, noToken());
      emitByte(c, OP_END_SCOPE, noToken());
      emitGc(c);
      guardToExit = emitJump(c, OP_JUMP, keyword);
    }

    patchJump(c, exitJump, keyword);
    emitByte(c, OP_POP, noToken());
    emitGc(c);
    int loopEnd = c->chunk->count;
    if (guardToExit != -1) {
      patchJump(c, guardToExit, keyword);
    }
    patchJumpList(c, &loop.breaks, loopEnd, keyword);
    patchJumpList(c, &loop.continues, continueTarget, keyword);
    freeJumpList(&loop.breaks);
    freeJumpList(&loop.continues);

    patternBindingListFree(&bindings);
    freePattern(pattern);
    return;
  }

  expression(c);
  typePop(c);
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after condition.", openParen);
  int exitJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
  emitByte(c, OP_POP, noToken());

  BreakContext loop;
  loop.type = BREAK_LOOP;
  loop.enclosing = c->breakContext;
  loop.scopeDepth = c->scopeDepth;
  initJumpList(&loop.breaks);
  initJumpList(&loop.continues);
  c->breakContext = &loop;

  statement(c);
  int continueTarget = c->chunk->count;
  emitGc(c);
  emitLoop(c, loopStart, keyword);
  c->breakContext = loop.enclosing;

  patchJump(c, exitJump, keyword);
  emitByte(c, OP_POP, noToken());
  emitGc(c);
  int loopEnd = c->chunk->count;
  patchJumpList(c, &loop.breaks, loopEnd, keyword);
  patchJumpList(c, &loop.continues, continueTarget, keyword);
  freeJumpList(&loop.breaks);
  freeJumpList(&loop.continues);
}

static void forStatement(Compiler* c) {
  Token keyword = previous(c);
  emitByte(c, OP_BEGIN_SCOPE, noToken());
  c->scopeDepth++;
  typeCheckerEnterScope(c);
  Token openParen = consume(c, TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");

  if (match(c, TOKEN_SEMICOLON)) {
  } else if (match(c, TOKEN_LET)) {
    varDeclaration(c, false, false, false);
  } else if (match(c, TOKEN_CONST)) {
    varDeclaration(c, true, false, false);
  } else {
    expression(c);
    typePop(c);
    consume(c, TOKEN_SEMICOLON, "Expect ';' after loop initializer.");
    emitByte(c, OP_POP, noToken());
  }

  int loopStart = c->chunk->count;
  int exitJump = -1;
  if (!check(c, TOKEN_SEMICOLON)) {
    expression(c);
    typePop(c);
    exitJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
    emitByte(c, OP_POP, noToken());
  }
  consume(c, TOKEN_SEMICOLON, "Expect ';' after loop condition.");

  int incrementOffset = -1;
  bool hasIncrement = !check(c, TOKEN_RIGHT_PAREN);
  if (hasIncrement) {
    int bodyJump = emitJump(c, OP_JUMP, keyword);
    incrementOffset = c->chunk->count;
    expression(c);
    typePop(c);
    emitByte(c, OP_POP, noToken());
    emitLoop(c, loopStart, keyword);
    loopStart = incrementOffset;
    patchJump(c, bodyJump, keyword);
  }
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.", openParen);

  BreakContext loop;
  loop.type = BREAK_LOOP;
  loop.enclosing = c->breakContext;
  loop.scopeDepth = c->scopeDepth;
  initJumpList(&loop.breaks);
  initJumpList(&loop.continues);
  c->breakContext = &loop;

  statement(c);
  int continueTarget = hasIncrement ? incrementOffset : c->chunk->count;
  emitGc(c);
  emitLoop(c, loopStart, keyword);
  c->breakContext = loop.enclosing;

  if (exitJump != -1) {
    patchJump(c, exitJump, keyword);
    emitByte(c, OP_POP, noToken());
  }
  emitGc(c);
  int loopEnd = c->chunk->count;
  patchJumpList(c, &loop.breaks, loopEnd, keyword);
  patchJumpList(c, &loop.continues, continueTarget, keyword);
  freeJumpList(&loop.breaks);
  freeJumpList(&loop.continues);

  emitByte(c, OP_END_SCOPE, noToken());
  c->scopeDepth--;
  typeCheckerExitScope(c);
  emitGc(c);
}

static void foreachStatement(Compiler* c) {
  Token keyword = previous(c);
  emitByte(c, OP_BEGIN_SCOPE, noToken());
  c->scopeDepth++;
  typeCheckerEnterScope(c);
  Token openParen = consume(c, TOKEN_LEFT_PAREN, "Expect '(' after 'foreach'.");

  Token first = consume(c, TOKEN_IDENTIFIER, "Expect loop variable.");
  Token keyToken; Token valueToken;
  memset(&keyToken, 0, sizeof(Token));
  valueToken = first;
  bool hasKey = false;
  if (match(c, TOKEN_COMMA)) {
    keyToken = first;
    valueToken = consume(c, TOKEN_IDENTIFIER, "Expect value name after ','.");
    hasKey = true;
  }
  consume(c, TOKEN_IN, "Expect 'in' after foreach variable.");
  expression(c);
  Type* iterType = typePop(c);
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after foreach iterable.", openParen);

  int iterableName = emitTempNameConstant(c, "iterable");
  emitDefineVarConstant(c, iterableName);

  int iterFn = emitStringConstantFromChars(c, "iter", 4);
  emitGetVarConstant(c, iterFn);
  emitGetVarConstant(c, iterableName);
  emitByte(c, OP_CALL, noToken());
  emitByte(c, 1, noToken());
  int iterName = emitTempNameConstant(c, "iter");
  emitDefineVarConstant(c, iterName);

  int loopStart = c->chunk->count;
  int nextFn = emitStringConstantFromChars(c, "next", 4);
  emitGetVarConstant(c, nextFn);
  emitGetVarConstant(c, iterName);
  emitByte(c, OP_CALL, noToken());
  emitByte(c, 1, noToken());
  int stepName = emitTempNameConstant(c, "step");
  emitDefineVarConstant(c, stepName);

  Token doneToken = syntheticToken("done");
  emitGetVarConstant(c, stepName);
  emitPatternKeyConstant(c, doneToken, false, doneToken);
  emitByte(c, OP_GET_INDEX, keyword);
  int bodyJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
  emitByte(c, OP_POP, noToken());
  int exitJump = emitJump(c, OP_JUMP, keyword);
  patchJump(c, bodyJump, keyword);
  emitByte(c, OP_POP, noToken());

  BreakContext loop;
  loop.type = BREAK_LOOP;
  loop.enclosing = c->breakContext;
  loop.scopeDepth = c->scopeDepth;
  initJumpList(&loop.breaks);
  initJumpList(&loop.continues);
  c->breakContext = &loop;

  Token valueField = syntheticToken("value");
  Token keyField = syntheticToken("key");
  if (hasKey) {
    int keyName = emitStringConstant(c, keyToken);
    int valueName = emitStringConstant(c, valueToken);
    emitGetVarConstant(c, stepName);
    emitPatternKeyConstant(c, keyField, false, keyField);
    emitByte(c, OP_GET_INDEX, keyToken);
    emitByte(c, OP_DEFINE_VAR, keyToken);
    emitShort(c, (uint16_t)keyName, keyToken);

    emitGetVarConstant(c, stepName);
    emitPatternKeyConstant(c, valueField, false, valueField);
    emitByte(c, OP_GET_INDEX, valueToken);
    emitByte(c, OP_DEFINE_VAR, valueToken);
    emitShort(c, (uint16_t)valueName, valueToken);
  } else {
    int valueName = emitStringConstant(c, valueToken);
    emitGetVarConstant(c, stepName);
    emitPatternKeyConstant(c, valueField, false, valueField);
    emitByte(c, OP_GET_INDEX, valueToken);
    emitByte(c, OP_DEFINE_VAR, valueToken);
    emitShort(c, (uint16_t)valueName, valueToken);
  }

  if (typecheckEnabled(c)) {
    Type* keyType = typeAny();
    Type* valueType = typeAny();
    if (iterType && iterType->kind == TYPE_ARRAY) {
      keyType = typeNumber();
      valueType = iterType->elem ? iterType->elem : typeAny();
    } else if (iterType && iterType->kind == TYPE_MAP) {
      keyType = iterType->key ? iterType->key : typeString();
      valueType = iterType->value ? iterType->value : typeAny();
    } else if (typeNamedIs(iterType, "range")) {
      keyType = typeNumber();
      valueType = typeNumber();
    }
    if (hasKey) {
      typeDefine(c, keyToken, keyType, true);
    }
    typeDefine(c, valueToken, valueType, true);
  }

  statement(c);
  int continueTarget = loopStart;
  emitGc(c);
  emitLoop(c, loopStart, keyword);
  c->breakContext = loop.enclosing;

  patchJump(c, exitJump, keyword);
  emitGc(c);
  int loopEnd = c->chunk->count;
  patchJumpList(c, &loop.breaks, loopEnd, keyword);
  patchJumpList(c, &loop.continues, continueTarget, keyword);
  freeJumpList(&loop.breaks);
  freeJumpList(&loop.continues);

  emitByte(c, OP_END_SCOPE, noToken());
  c->scopeDepth--;
  typeCheckerExitScope(c);
  emitGc(c);
}

static void switchStatement(Compiler* c) {
  Token keyword = previous(c);
  const char* keywordName = keyword.type == TOKEN_MATCH ? "match" : "switch";
  char message[64];
  emitByte(c, OP_BEGIN_SCOPE, noToken());
  c->scopeDepth++;
  typeCheckerEnterScope(c);
  snprintf(message, sizeof(message), "Expect '(' after '%s'.", keywordName);
  Token openParen = consume(c, TOKEN_LEFT_PAREN, message);
  expression(c);
  Type* switchType = typePop(c);
  snprintf(message, sizeof(message), "Expect ')' after %s value.", keywordName);
  consumeClosing(c, TOKEN_RIGHT_PAREN, message, openParen);
  snprintf(message, sizeof(message), "Expect '{' after %s value.", keywordName);
  Token openBrace = consume(c, TOKEN_LEFT_BRACE, message);

  int switchValue = emitTempNameConstant(c, "switch");
  emitDefineVarConstant(c, switchValue);

  BreakContext ctx;
  ctx.type = BREAK_SWITCH;
  ctx.enclosing = c->breakContext;
  ctx.scopeDepth = c->scopeDepth;
  initJumpList(&ctx.breaks);
  initJumpList(&ctx.continues);
  c->breakContext = &ctx;

  JumpList endJumps;
  initJumpList(&endJumps);
  int previousJump = -1;
  bool isMatch = keyword.type == TOKEN_MATCH;
  EnumInfo* matchEnum = NULL;
  bool* variantUsed = NULL;
  int variantUsedCount = 0;
  bool sawEnumPattern = false;
  bool hasDefault = false;
  bool hasCatchAll = false;
  ConstValue* literalUsed = NULL;
  int literalUsedCount = 0;
  int literalUsedCapacity = 0;

  while (!check(c, TOKEN_RIGHT_BRACE) && !isAtEnd(c)) {
    if (match(c, TOKEN_CASE)) {
      if (previousJump != -1) {
        patchJump(c, previousJump, keyword);
        emitByte(c, OP_POP, noToken());
      }
      int guardJump = -1;
      bool guardScope = false;
      if (isMatch) {
        Pattern* pattern = parsePattern(c);
        bool hasGuard = false;
        PatternBindingList bindings;
        patternBindingListInit(&bindings);
        if (match(c, TOKEN_IF)) {
          hasGuard = true;
        }

        if (hasCatchAll || hasDefault) {
          errorAt(c, pattern->token, "Unreachable case.");
        }
        if (!hasGuard) {
          if (patternIsCatchAll(pattern)) {
            hasCatchAll = true;
          }
          ConstValue literalValue;
          if (patternConstValue(pattern, &literalValue)) {
            if (constValueListContains(literalUsed, literalUsedCount, &literalValue)) {
              errorAt(c, pattern->token, "Unreachable case.");
              constValueFree(&literalValue);
            } else {
              constValueListAdd(&literalUsed, &literalUsedCount,
                                &literalUsedCapacity, &literalValue);
            }
          }
        }

        if (pattern && pattern->kind == PATTERN_ENUM) {
          EnumInfo* info = findEnumInfo(c, pattern->as.enumPattern.enumToken);
          if (info && info->isAdt) {
            if (!matchEnum) {
              matchEnum = info;
              variantUsedCount = info->variantCount;
              if (variantUsedCount > 0) {
                variantUsed = (bool*)calloc((size_t)variantUsedCount, sizeof(bool));
                if (!variantUsed) {
                  fprintf(stderr, "Out of memory.\n");
                  exit(1);
                }
              }
            } else if (matchEnum != info) {
              errorAt(c, pattern->as.enumPattern.enumToken,
                      "Match patterns must use a single enum.");
            }

            EnumVariantInfo* variantInfo =
                findEnumVariant(info, pattern->as.enumPattern.variantToken);
            if (variantInfo && variantInfo->arity == pattern->as.enumPattern.argCount) {
              int variantIndex = enumVariantIndex(matchEnum,
                                                  pattern->as.enumPattern.variantToken);
              if (variantIndex >= 0 && variantIndex < variantUsedCount) {
                if (variantUsed && variantUsed[variantIndex]) {
                  errorAt(c, pattern->as.enumPattern.variantToken, "Unreachable case.");
                }
                if (!hasGuard && variantUsed) {
                  variantUsed[variantIndex] = true;
                }
              }
            }
            sawEnumPattern = true;
          }
        }

        emitPatternMatchValue(c, switchValue, pattern, &bindings);
        previousJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
        emitByte(c, OP_POP, noToken());
        if (hasGuard) {
          emitByte(c, OP_BEGIN_SCOPE, noToken());
          c->scopeDepth++;
          typeCheckerEnterScope(c);
          guardScope = true;
        }
        emitPatternBindings(c, switchValue, &bindings, OP_DEFINE_VAR, switchType);
        if (hasGuard) {
          expression(c);
          Type* guardType = typePop(c);
          if (typecheckEnabled(c) && guardType &&
              guardType->kind != TYPE_BOOL &&
              guardType->kind != TYPE_ANY &&
              guardType->kind != TYPE_UNKNOWN) {
            typeErrorAt(c, previous(c), "Guard expects bool.");
          }
          guardJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
          emitByte(c, OP_POP, noToken());
        }
        consume(c, TOKEN_COLON, "Expect ':' after case pattern.");
        patternBindingListFree(&bindings);
        freePattern(pattern);
      } else {
        emitGetVarConstant(c, switchValue);
        expression(c);
        Type* caseType = typePop(c);
        if (switchType && !typeIsAny(switchType) && !typeAssignable(switchType, caseType)) {
          char expected[64];
          char got[64];
          typeToString(switchType, expected, sizeof(expected));
          typeToString(caseType, got, sizeof(got));
          typeErrorAt(c, previous(c), "Case type %s does not match %s.", got, expected);
        }
        consume(c, TOKEN_COLON, "Expect ':' after case value.");
        emitByte(c, OP_EQUAL, keyword);
        previousJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
        emitByte(c, OP_POP, noToken());
      }

      while (!check(c, TOKEN_CASE) && !check(c, TOKEN_DEFAULT) &&
             !check(c, TOKEN_RIGHT_BRACE) && !isAtEnd(c)) {
        declaration(c);
      }
      if (guardScope) {
        emitByte(c, OP_END_SCOPE, noToken());
        c->scopeDepth--;
        typeCheckerExitScope(c);
        emitGc(c);
      }
      int endJump = emitJump(c, OP_JUMP, keyword);
      writeJumpList(&endJumps, endJump);
      if (guardJump != -1) {
        patchJump(c, guardJump, keyword);
        emitByte(c, OP_POP, noToken());
        if (guardScope) {
          emitByte(c, OP_END_SCOPE, noToken());
          emitGc(c);
        }
      }
    } else if (match(c, TOKEN_DEFAULT)) {
      if (hasCatchAll || hasDefault) {
        errorAt(c, previous(c), "Unreachable default.");
      }
      hasDefault = true;
      if (previousJump != -1) {
        patchJump(c, previousJump, keyword);
        emitByte(c, OP_POP, noToken());
        previousJump = -1;
      }
      consume(c, TOKEN_COLON, "Expect ':' after default.");
      while (!check(c, TOKEN_CASE) && !check(c, TOKEN_DEFAULT) &&
             !check(c, TOKEN_RIGHT_BRACE) && !isAtEnd(c)) {
        declaration(c);
      }
    } else {
      errorAtCurrent(c, "Expect 'case' or 'default' in switch.");
      synchronize(c);
      break;
    }
  }

  if (isMatch && sawEnumPattern && matchEnum && !hasDefault && !hasCatchAll) {
    bool missing = false;
    for (int i = 0; i < variantUsedCount; i++) {
      if (!variantUsed[i]) {
        missing = true;
        break;
      }
    }
    if (missing) {
      errorAt(c, keyword, "Non-exhaustive match. Add missing enum cases or 'default'.");
    }
  }

  if (previousJump != -1) {
    patchJump(c, previousJump, keyword);
    emitByte(c, OP_POP, noToken());
  }

  consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after switch cases.", openBrace);
  c->breakContext = ctx.enclosing;
  int switchEnd = c->chunk->count;
  patchJumpList(c, &endJumps, switchEnd, keyword);
  patchJumpList(c, &ctx.breaks, switchEnd, keyword);
  freeJumpList(&endJumps);
  freeJumpList(&ctx.breaks);
  freeJumpList(&ctx.continues);
  free(variantUsed);
  constValueListFree(literalUsed, literalUsedCount, literalUsedCapacity);

  emitByte(c, OP_END_SCOPE, noToken());
  c->scopeDepth--;
  typeCheckerExitScope(c);
  emitGc(c);
}

static void tryStatement(Compiler* c) {
  Token keyword = previous(c);
  Token openBrace = consume(c, TOKEN_LEFT_BRACE, "Expect '{' after 'try'.");

  int handlerJump = emitJump(c, OP_TRY, keyword);

  emitByte(c, OP_BEGIN_SCOPE, noToken());
  c->scopeDepth++;
  typeCheckerEnterScope(c);
  block(c, openBrace);
  emitByte(c, OP_END_SCOPE, noToken());
  c->scopeDepth--;
  typeCheckerExitScope(c);
  emitGc(c);

  emitByte(c, OP_END_TRY, keyword);
  int endJump = emitJump(c, OP_JUMP, keyword);
  patchJump(c, handlerJump, keyword);

  consume(c, TOKEN_CATCH, "Expect 'catch' after try block.");
  Token openParen = consume(c, TOKEN_LEFT_PAREN, "Expect '(' after 'catch'.");
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect catch binding name.");
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after catch binding.", openParen);
  Token catchBrace = consume(c, TOKEN_LEFT_BRACE, "Expect '{' after catch clause.");

  emitByte(c, OP_BEGIN_SCOPE, noToken());
  c->scopeDepth++;
  typeCheckerEnterScope(c);

  int nameIdx = emitStringConstant(c, name);
  emitByte(c, OP_DEFINE_VAR, name);
  emitShort(c, (uint16_t)nameIdx, name);
  if (typecheckEnabled(c)) {
    typeDefine(c, name, typeAny(), true);
  }

  block(c, catchBrace);

  emitByte(c, OP_END_SCOPE, noToken());
  c->scopeDepth--;
  typeCheckerExitScope(c);
  emitGc(c);

  patchJump(c, endJump, keyword);
}

static void throwStatement(Compiler* c) {
  Token keyword = previous(c);
  expression(c);
  typePop(c);
  consume(c, TOKEN_SEMICOLON, "Expect ';' after throw value.");
  emitByte(c, OP_THROW, keyword);
}

static void deferStatement(Compiler* c) {
  Token keyword = previous(c);
  bool savedForbid = c->forbidCall;
  c->pendingOptionalCall = false;
  c->lastExprWasVar = false;
  memset(&c->lastExprVar, 0, sizeof(Token));
  c->forbidCall = true;
  parsePrecedence(c, PREC_CALL);
  c->forbidCall = savedForbid;
  c->pendingOptionalCall = false;

  Token openParen = consume(c, TOKEN_LEFT_PAREN, "Expect '(' after defer callee.");
  int argc = 0;
  if (!check(c, TOKEN_RIGHT_PAREN)) {
    do {
      if (argc >= ERK_MAX_ARGS) {
        errorAtCurrent(c, "Too many arguments.");
      }
      expression(c);
      argc++;
    } while (match(c, TOKEN_COMMA));
  }
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after defer arguments.", openParen);
  consume(c, TOKEN_SEMICOLON, "Expect ';' after defer call.");

  if (typecheckEnabled(c)) {
    Type* argTypes[ERK_MAX_ARGS];
    for (int i = argc - 1; i >= 0; i--) {
      argTypes[i] = typePop(c);
    }
    Type* callee = typePop(c);
    if (callee && callee->kind == TYPE_FUNCTION) {
      if (callee->paramCount >= 0 && callee->paramCount != argc) {
        typeErrorAt(c, openParen, "Function expects %d arguments but got %d.",
                    callee->paramCount, argc);
      } else if (callee->params) {
        int checkCount = callee->paramCount >= 0 ? callee->paramCount : argc;
        for (int i = 0; i < checkCount && i < argc; i++) {
          if (!typeAssignable(callee->params[i], argTypes[i])) {
            char expected[64];
            char got[64];
            typeToString(callee->params[i], expected, sizeof(expected));
            typeToString(argTypes[i], got, sizeof(got));
            typeErrorAt(c, openParen, "Argument %d expects %s but got %s.",
                        i + 1, expected, got);
          }
        }
      }
    }
  }

  emitByte(c, OP_DEFER, keyword);
  emitByte(c, (uint8_t)argc, keyword);
  emitGc(c);
}

static void yieldStatement(Compiler* c) {
  Token keyword = previous(c);
  if (!c->enclosing) {
    errorAt(c, keyword, "Cannot use 'yield' outside of a function.");
    return;
  }
  if (c->yieldName < 0 || c->yieldFlagName < 0) {
    errorAt(c, keyword, "Yield is not available here.");
    return;
  }
  emitByte(c, OP_TRUE, keyword);
  emitSetVarConstant(c, c->yieldFlagName);
  emitByte(c, OP_POP, noToken());
  emitGetVarConstant(c, c->yieldName);
  expression(c);
  typePop(c);
  consume(c, TOKEN_SEMICOLON, "Expect ';' after yield value.");
  emitByte(c, OP_ARRAY_APPEND, keyword);
  emitByte(c, OP_POP, noToken());
  emitGc(c);
  c->hasYield = true;
}

static void breakStatement(Compiler* c) {
  Token keyword = previous(c);
  consume(c, TOKEN_SEMICOLON, "Expect ';' after 'break'.");
  if (!c->breakContext) {
    errorAt(c, keyword, "Cannot use 'break' outside of a loop or switch.");
    return;
  }
  emitScopeExits(c, c->breakContext->scopeDepth);
  int jump = emitJump(c, OP_JUMP, keyword);
  writeJumpList(&c->breakContext->breaks, jump);
}

static void continueStatement(Compiler* c) {
  Token keyword = previous(c);
  consume(c, TOKEN_SEMICOLON, "Expect ';' after 'continue'.");
  BreakContext* loop = findLoopContext(c);
  if (!loop) {
    errorAt(c, keyword, "Cannot use 'continue' outside of a loop.");
    return;
  }
  emitScopeExits(c, loop->scopeDepth);
  int jump = emitJump(c, OP_JUMP, keyword);
  writeJumpList(&loop->continues, jump);
}

static void returnStatement(Compiler* c) {
  Token keyword = previous(c);
  bool checkReturn = typecheckEnabled(c) && c->typecheck->currentReturn && !c->hasYield;
  if (!check(c, TOKEN_SEMICOLON)) {
    expression(c);
    Type* valueType = typePop(c);
    if (checkReturn) {
      if (!typeAssignable(c->typecheck->currentReturn, valueType)) {
        char expected[64];
        char got[64];
        typeToString(c->typecheck->currentReturn, expected, sizeof(expected));
        typeToString(valueType, got, sizeof(got));
        typeErrorAt(c, keyword, "Return type mismatch. Expected %s but got %s.", expected, got);
      }
    }
  } else {
    if (checkReturn && c->typecheck->currentReturn &&
        c->typecheck->currentReturn->kind != TYPE_NULL &&
        c->typecheck->currentReturn->kind != TYPE_ANY &&
        c->typecheck->currentReturn->kind != TYPE_UNKNOWN) {
      char expected[64];
      typeToString(c->typecheck->currentReturn, expected, sizeof(expected));
      typeErrorAt(c, keyword, "Return type mismatch. Expected %s but got null.", expected);
    }
    emitByte(c, OP_NULL, noToken());
  }
  consume(c, TOKEN_SEMICOLON, "Expect ';' after return value.");
  if (c->yieldName >= 0 && c->yieldFlagName >= 0) {
    emitGetVarConstant(c, c->yieldFlagName);
    int normalJump = emitJump(c, OP_JUMP_IF_FALSE, keyword);
    emitByte(c, OP_POP, noToken());
    emitByte(c, OP_POP, noToken());
    emitGetVarConstant(c, c->yieldName);
    emitByte(c, OP_RETURN, keyword);
    patchJump(c, normalJump, keyword);
    emitByte(c, OP_POP, noToken());
    emitByte(c, OP_RETURN, keyword);
  } else {
    emitByte(c, OP_RETURN, keyword);
  }
}

static void importStatement(Compiler* c) {
  Token keyword = previous(c);
  if (match(c, TOKEN_STAR)) {
    consume(c, TOKEN_AS, "Expect 'as' after '*'.");
    Token alias = consume(c, TOKEN_IDENTIFIER, "Expect name after 'as'.");
    consume(c, TOKEN_FROM, "Expect 'from' after import alias.");
    expression(c);
    typePop(c);
    consume(c, TOKEN_SEMICOLON, "Expect ';' after import.");
    emitByte(c, OP_IMPORT, keyword);
    emitByte(c, 1, keyword);
    uint16_t aliasIdx = (uint16_t)emitStringConstant(c, alias);
    emitShort(c, aliasIdx, keyword);
    emitGc(c);
    return;
  }

  if (check(c, TOKEN_IDENTIFIER) && checkNext(c, TOKEN_FROM)) {
    Token alias = consume(c, TOKEN_IDENTIFIER, "Expect name after 'import'.");
    consume(c, TOKEN_FROM, "Expect 'from' after import name.");
    expression(c);
    typePop(c);
    consume(c, TOKEN_SEMICOLON, "Expect ';' after import.");
    emitByte(c, OP_IMPORT_MODULE, keyword);
    int defaultIdx = emitStringConstantFromChars(c, "default", 7);
    emitByte(c, OP_GET_PROPERTY, keyword);
    emitShort(c, (uint16_t)defaultIdx, keyword);
    int nameIdx = emitStringConstant(c, alias);
    emitByte(c, OP_DEFINE_VAR, alias);
    emitShort(c, (uint16_t)nameIdx, alias);
    emitGc(c);
    return;
  }

  expression(c);
  typePop(c);
  Token alias; memset(&alias, 0, sizeof(Token));
  bool hasAlias = false;
  if (match(c, TOKEN_AS)) {
    alias = consume(c, TOKEN_IDENTIFIER, "Expect name after 'as'.");
    hasAlias = true;
  }
  consume(c, TOKEN_SEMICOLON, "Expect ';' after import.");
  emitByte(c, OP_IMPORT, keyword);
  emitByte(c, hasAlias ? 1 : 0, keyword);
  uint16_t aliasIdx = 0;
  if (hasAlias) {
    aliasIdx = (uint16_t)emitStringConstant(c, alias);
  }
  emitShort(c, aliasIdx, keyword);
  emitGc(c);
}

static void fromImportStatement(Compiler* c) {
  Token keyword = previous(c);
  expression(c);
  typePop(c);
  consume(c, TOKEN_IMPORT, "Expect 'import' after module path.");
  Token alias = consume(c, TOKEN_IDENTIFIER, "Expect name after 'import'.");
  consume(c, TOKEN_SEMICOLON, "Expect ';' after import.");
  emitByte(c, OP_IMPORT, keyword);
  emitByte(c, 1, keyword);
  uint16_t aliasIdx = (uint16_t)emitStringConstant(c, alias);
  emitShort(c, aliasIdx, keyword);
  emitGc(c);
}

static ObjFunction* compileFunction(Compiler* c, Token name, bool isInitializer,
                                    Type** outType, bool defineType);

static void functionDeclaration(Compiler* c, bool isExport, bool isPrivate) {
    Token name = consume(c, TOKEN_IDENTIFIER, "Expect function name.");
    Type* functionType = NULL;
    ObjFunction* function = compileFunction(c, name, false, &functionType, true);
    (void)functionType;
  if (!function) return;
  int constant = makeConstant(c, OBJ_VAL(function), name);
  emitByte(c, OP_CLOSURE, name);
  emitShort(c, (uint16_t)constant, name);
  int nameIdx = emitStringConstant(c, name);
  emitByte(c, OP_DEFINE_VAR, name);
  emitShort(c, (uint16_t)nameIdx, name);
  if (isPrivate) {
    emitPrivateName(c, nameIdx, name);
  }
  if (isExport) {
    emitByte(c, OP_EXPORT, name);
    emitShort(c, (uint16_t)nameIdx, name);
  }
  emitGc(c);
}

  static void interfaceDeclaration(Compiler* c) {
    Token nameToken = consume(c, TOKEN_IDENTIFIER, "Expect interface name.");
    ObjString* nameStr = stringFromToken(c->vm, nameToken);

    int typeParamCount = 0;
    TypeParam* typeParams = parseTypeParams(c, &typeParamCount);

    Token openBrace = consume(c, TOKEN_LEFT_BRACE, "Expect '{' before interface body.");

    int savedParamCount = 0;
    if (typecheckEnabled(c)) {
      savedParamCount = c->typecheck->typeParamCount;
      typeParamsPushList(c->typecheck, typeParams, typeParamCount);
    }

    InterfaceDef def;
    memset(&def, 0, sizeof(def));
    def.name = nameStr;
    def.typeParams = typeParams;
    def.typeParamCount = typeParamCount;

    while (!check(c, TOKEN_RIGHT_BRACE) && !isAtEnd(c)) {
      if (!match(c, TOKEN_FUN)) {
        errorAtCurrent(c, "Expect 'fun' in interface body.");
        synchronize(c);
        break;
      }
      Token methodName = consume(c, TOKEN_IDENTIFIER, "Expect method name.");
      Token openParen = consume(c, TOKEN_LEFT_PAREN, "Expect '(' after method name.");

      Type** paramTypes = NULL;
      int paramCount = 0;
      int paramCap = 0;
      if (!check(c, TOKEN_RIGHT_PAREN)) {
        do {
          Token paramName = consume(c, TOKEN_IDENTIFIER, "Expect parameter name.");
          Type* paramType = typeAny();
          if (match(c, TOKEN_COLON)) {
            paramType = parseType(c);
          }
          if (paramCount >= paramCap) {
            int oldCap = paramCap;
            paramCap = GROW_CAPACITY(oldCap);
            paramTypes = GROW_ARRAY(Type*, paramTypes, oldCap, paramCap);
            if (!paramTypes) {
              fprintf(stderr, "Out of memory.\n");
              exit(1);
            }
          }
          paramTypes[paramCount++] = paramType;
          (void)paramName;
        } while (match(c, TOKEN_COMMA));
      }
      consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.", openParen);

      Type* returnType = typeAny();
      if (match(c, TOKEN_COLON)) {
        returnType = parseType(c);
      }
      consume(c, TOKEN_SEMICOLON, "Expect ';' after interface method.");

      if (typecheckEnabled(c)) {
        if (def.methodCount >= def.methodCapacity) {
          int oldCap = def.methodCapacity;
          def.methodCapacity = GROW_CAPACITY(oldCap);
          def.methods = GROW_ARRAY(InterfaceMethod, def.methods, oldCap, def.methodCapacity);
          if (!def.methods) {
            fprintf(stderr, "Out of memory.\n");
            exit(1);
          }
        }
        Type* methodType = typeFunction(c->typecheck, paramTypes, paramCount, returnType);
        def.methods[def.methodCount].name = stringFromToken(c->vm, methodName);
        def.methods[def.methodCount].type = methodType;
        def.methodCount++;
      }
      free(paramTypes);
    }
    consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after interface body.", openBrace);

    if (typecheckEnabled(c)) {
      InterfaceDef* existing = typeRegistryFindInterface(gTypeRegistry, nameStr);
      if (existing) {
        typeErrorAt(c, nameToken, "Interface '%s' already defined.", nameStr->chars);
        if (def.methods) free(def.methods);
        if (def.typeParams) free(def.typeParams);
      } else {
        typeRegistryAddInterface(gTypeRegistry, &def);
      }
      typeParamsTruncate(c->typecheck, savedParamCount);
    } else {
      if (def.methods) free(def.methods);
      if (def.typeParams) free(def.typeParams);
    }
  }

  static ClassMethod* findClassMethod(ClassMethod* methods, int count, ObjString* name) {
    if (!methods || !name) return NULL;
    for (int i = 0; i < count; i++) {
      if (typeNamesEqual(methods[i].name, name)) return &methods[i];
    }
    return NULL;
  }

  static void checkClassImplements(Compiler* c, Token classNameToken, ObjString* className,
                                   ClassMethod* methods, int methodCount,
                                   Type** interfaces, Token* interfaceTokens, int interfaceCount) {
    if (!typecheckEnabled(c) || !gTypeRegistry) return;
    for (int i = 0; i < interfaceCount; i++) {
      Type* ifaceType = interfaces[i];
      Token ifaceToken = interfaceTokens[i];
      if (!ifaceType || ifaceType->kind != TYPE_NAMED || !ifaceType->name) continue;
      InterfaceDef* iface = typeRegistryFindInterface(gTypeRegistry, ifaceType->name);
      if (!iface) {
        typeErrorAt(c, ifaceToken, "Unknown interface '%s'.", ifaceType->name->chars);
        continue;
      }

      if (iface->typeParamCount > 0 &&
          ifaceType->typeArgCount > 0 &&
          ifaceType->typeArgCount != iface->typeParamCount) {
        typeErrorAt(c, ifaceToken, "Interface '%s' expects %d type arguments but got %d.",
                    ifaceType->name->chars, iface->typeParamCount, ifaceType->typeArgCount);
      }

      int bindingCount = iface->typeParamCount;
      TypeBinding* bindings = NULL;
      if (bindingCount > 0) {
        bindings = (TypeBinding*)malloc(sizeof(TypeBinding) * (size_t)bindingCount);
        if (!bindings) {
          fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
        for (int b = 0; b < bindingCount; b++) {
          bindings[b].name = iface->typeParams[b].name;
          bindings[b].constraint = iface->typeParams[b].constraint;
          bindings[b].bound = (ifaceType->typeArgCount > b) ? ifaceType->typeArgs[b] : NULL;
          if (bindings[b].bound &&
              !typeSatisfiesConstraint(bindings[b].bound, bindings[b].constraint)) {
            typeErrorAt(c, ifaceToken, "Type argument for '%s' must implement %s.",
                        bindings[b].name ? bindings[b].name->chars : "T",
                        bindings[b].constraint ? bindings[b].constraint->chars : "interface");
          }
        }
      }

      for (int m = 0; m < iface->methodCount; m++) {
        InterfaceMethod* method = &iface->methods[m];
        ClassMethod* impl = findClassMethod(methods, methodCount, method->name);
        if (!impl) {
          typeErrorAt(c, classNameToken,
                      "Class '%s' is missing method '%s' from interface '%s'.",
                      className ? className->chars : "class",
                      method->name ? method->name->chars : "method",
                      iface->name ? iface->name->chars : "interface");
          continue;
        }

        Type* expected = method->type;
        if (bindingCount > 0) {
          expected = typeSubstitute(c->typecheck, expected, bindings, bindingCount);
        }
        if (!typeAssignable(expected, impl->type)) {
          typeErrorAt(c, classNameToken,
                      "Method '%s' does not match interface '%s'.",
                      method->name ? method->name->chars : "method",
                      iface->name ? iface->name->chars : "interface");
        }
      }

      if (bindings) free(bindings);
    }
  }

static void structDeclarationWithName(Compiler* c, Token name,
                                      bool isExport, bool exportDefault, bool isPrivate) {
  if (findStructInfo(c, name)) {
    errorAt(c, name, "Struct already declared.");
  } else {
    compilerAddStruct(c, name);
  }

  ObjString* structName = stringFromToken(c->vm, name);
  Token openBrace = consume(c, TOKEN_LEFT_BRACE, "Expect '{' before struct body.");

  int nameConst = emitStringConstant(c, name);
  emitByte(c, OP_NULL, noToken());
  emitByte(c, OP_DEFINE_VAR, name);
  emitShort(c, (uint16_t)nameConst, name);

  int fieldsTemp = emitTempNameConstant(c, "struct_fields");
  emitByte(c, OP_MAP, noToken());
  emitShort(c, 0, noToken());
  emitDefineVarConstant(c, fieldsTemp);

  int defaultsTemp = emitTempNameConstant(c, "struct_defaults");
  emitByte(c, OP_MAP, noToken());
  emitShort(c, 0, noToken());
  emitDefineVarConstant(c, defaultsTemp);

  int readonlyTemp = emitTempNameConstant(c, "struct_readonly");
  emitByte(c, OP_MAP, noToken());
  emitShort(c, 0, noToken());
  emitDefineVarConstant(c, readonlyTemp);

  Token* fieldNames = NULL;
  int fieldCount = 0;
  int fieldCapacity = 0;

  if (!check(c, TOKEN_RIGHT_BRACE)) {
    for (;;) {
      bool isReadonly = match(c, TOKEN_READONLY);
      Token fieldName = consume(c, TOKEN_IDENTIFIER, "Expect field name.");
      for (int i = 0; i < fieldCount; i++) {
        if (fieldNames[i].length == fieldName.length &&
            memcmp(fieldNames[i].start, fieldName.start, (size_t)fieldName.length) == 0) {
          errorAt(c, fieldName, "Duplicate struct field.");
          break;
        }
      }
      if (fieldCount >= fieldCapacity) {
        int oldCap = fieldCapacity;
        fieldCapacity = GROW_CAPACITY(oldCap);
        fieldNames = GROW_ARRAY(Token, fieldNames, oldCap, fieldCapacity);
        if (!fieldNames) {
          fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
      }
      fieldNames[fieldCount++] = fieldName;

      Type* fieldType = NULL;
      bool hasType = false;
      if (match(c, TOKEN_COLON)) {
        fieldType = parseType(c);
        hasType = true;
      }

      ObjString* fieldKey = stringFromToken(c->vm, fieldName);

      emitGetVarConstant(c, fieldsTemp);
      emitConstant(c, OBJ_VAL(fieldKey), fieldName);
      emitByte(c, OP_TRUE, fieldName);
      emitByte(c, OP_MAP_SET, fieldName);
      emitByte(c, OP_POP, fieldName);

      if (isReadonly) {
        emitGetVarConstant(c, readonlyTemp);
        emitConstant(c, OBJ_VAL(fieldKey), fieldName);
        emitByte(c, OP_TRUE, fieldName);
        emitByte(c, OP_MAP_SET, fieldName);
        emitByte(c, OP_POP, fieldName);
      }

      if (match(c, TOKEN_EQUAL)) {
        emitGetVarConstant(c, defaultsTemp);
        emitConstant(c, OBJ_VAL(fieldKey), fieldName);
        expression(c);
        Type* defaultType = typePop(c);
        if (typecheckEnabled(c) && hasType && fieldType &&
            !typeAssignable(fieldType, defaultType)) {
          char expected[64];
          char got[64];
          typeToString(fieldType, expected, sizeof(expected));
          typeToString(defaultType, got, sizeof(got));
          typeErrorAt(c, fieldName, "Default value expects %s but got %s.", expected, got);
        }
        emitByte(c, OP_MAP_SET, fieldName);
        emitByte(c, OP_POP, fieldName);
      }
      if (match(c, TOKEN_COMMA) || match(c, TOKEN_SEMICOLON)) {
        if (check(c, TOKEN_RIGHT_BRACE)) break;
        continue;
      }
      break;
    }
  }

  consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after struct body.", openBrace);

  free(fieldNames);

  emitGetVarConstant(c, fieldsTemp);
  emitGetVarConstant(c, defaultsTemp);
  emitGetVarConstant(c, readonlyTemp);
  emitByte(c, OP_STRUCT, name);
  emitShort(c, (uint16_t)nameConst, name);

  if (isPrivate) {
    emitPrivateName(c, nameConst, name);
  }
  if (isExport) {
    emitByte(c, OP_EXPORT, name);
    emitShort(c, (uint16_t)nameConst, name);
  }
  if (exportDefault) {
    emitGetVarConstant(c, nameConst);
    int defaultIdx = emitStringConstantFromChars(c, "default", 7);
    emitExportValue(c, (uint16_t)defaultIdx, name);
  }
  emitGc(c);

  if (typecheckEnabled(c) && gTypeRegistry) {
    typeRegistryAddClass(gTypeRegistry, structName, NULL, 0);
  }
}

static void structDeclaration(Compiler* c, bool isExport, bool isPrivate) {
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect struct name.");
  structDeclarationWithName(c, name, isExport, false, isPrivate);
}

static void classDeclarationWithName(Compiler* c, Token name,
                                     bool isExport, bool exportDefault, bool isPrivate) {
    ObjString* className = stringFromToken(c->vm, name);
    int classTypeParamCount = 0;
    TypeParam* classTypeParams = parseTypeParams(c, &classTypeParamCount);
    int savedTypeParamCount = 0;
    if (typecheckEnabled(c)) {
      savedTypeParamCount = c->typecheck->typeParamCount;
      typeParamsPushList(c->typecheck, classTypeParams, classTypeParamCount);
    }

    Type** interfaces = NULL;
    Token* interfaceTokens = NULL;
    int interfaceCount = 0;
    int interfaceCapacity = 0;
    if (match(c, TOKEN_IMPLEMENTS)) {
      do {
        Token ifaceName = consume(c, TOKEN_IDENTIFIER, "Expect interface name.");
        Type* ifaceType = typeNamed(c->typecheck, stringFromToken(c->vm, ifaceName));
        if (check(c, TOKEN_LESS)) {
          ifaceType = parseTypeArguments(c, ifaceType, ifaceName);
        }
        if (interfaceCount >= interfaceCapacity) {
          int oldCap = interfaceCapacity;
          interfaceCapacity = GROW_CAPACITY(oldCap);
          interfaces = GROW_ARRAY(Type*, interfaces, oldCap, interfaceCapacity);
          interfaceTokens = GROW_ARRAY(Token, interfaceTokens, oldCap, interfaceCapacity);
          if (!interfaces || !interfaceTokens) {
            fprintf(stderr, "Out of memory.\n");
            exit(1);
          }
        }
        interfaces[interfaceCount] = ifaceType;
        interfaceTokens[interfaceCount] = ifaceName;
        interfaceCount++;
      } while (match(c, TOKEN_COMMA));
    }

    Token openBrace = consume(c, TOKEN_LEFT_BRACE, "Expect '{' before class body.");

    int nameConst = emitStringConstant(c, name);
    emitByte(c, OP_NULL, noToken());
    emitByte(c, OP_DEFINE_VAR, name);
    emitShort(c, (uint16_t)nameConst, name);

    int methodCount = 0;
    ClassMethod* methods = NULL;
    int methodCap = 0;
    bool classOk = true;
    while (!check(c, TOKEN_RIGHT_BRACE) && !isAtEnd(c)) {
      if (!match(c, TOKEN_FUN)) {
        errorAtCurrent(c, "Expect 'fun' before method declaration.");
        synchronize(c);
        break;
      }
      Token methodName = consume(c, TOKEN_IDENTIFIER, "Expect method name.");
      bool isInit = methodName.length == 4 && memcmp(methodName.start, "init", 4) == 0;
      Type* methodType = NULL;
      ObjFunction* method = compileFunction(c, methodName, isInit,
                                            typecheckEnabled(c) ? &methodType : NULL, false);
      if (!method) {
        classOk = false;
        break;
      }
      int constant = makeConstant(c, OBJ_VAL(method), methodName);
      emitByte(c, OP_CLOSURE, methodName);
      emitShort(c, (uint16_t)constant, methodName);
      methodCount++;
      if (typecheckEnabled(c) && methodType) {
        if (methodCount > methodCap) {
          int oldCap = methodCap;
          methodCap = GROW_CAPACITY(oldCap);
          methods = GROW_ARRAY(ClassMethod, methods, oldCap, methodCap);
          if (!methods) {
            fprintf(stderr, "Out of memory.\n");
            exit(1);
          }
        }
        methods[methodCount - 1].name = stringFromToken(c->vm, methodName);
        methods[methodCount - 1].type = methodType;
      }
    }
    if (!classOk) {
      goto class_cleanup;
    }
    consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after class body.", openBrace);

    emitByte(c, OP_CLASS, name);
    emitShort(c, (uint16_t)nameConst, name);
    emitShort(c, (uint16_t)methodCount, name);
    if (isPrivate) {
      emitPrivateName(c, nameConst, name);
    }
    if (isExport) {
      emitByte(c, OP_EXPORT, name);
      emitShort(c, (uint16_t)nameConst, name);
    }
    if (exportDefault) {
      emitGetVarConstant(c, nameConst);
      int defaultIdx = emitStringConstantFromChars(c, "default", 7);
      emitExportValue(c, (uint16_t)defaultIdx, name);
    }
    emitGc(c);

    if (typecheckEnabled(c)) {
      checkClassImplements(c, name, className, methods, methodCount,
                           interfaces, interfaceTokens, interfaceCount);
      if (gTypeRegistry) {
        ObjString** implemented = NULL;
        if (interfaceCount > 0) {
          implemented = (ObjString**)malloc(sizeof(ObjString*) * (size_t)interfaceCount);
          if (!implemented) {
            fprintf(stderr, "Out of memory.\n");
            exit(1);
          }
          for (int i = 0; i < interfaceCount; i++) {
            implemented[i] = interfaces[i]->name;
          }
        }
        typeRegistryAddClass(gTypeRegistry, className, implemented, interfaceCount);
      }
    }

class_cleanup:
    if (typecheckEnabled(c)) {
      typeParamsTruncate(c->typecheck, savedTypeParamCount);
    }
    free(methods);
    free(interfaces);
    free(interfaceTokens);
    free(classTypeParams);
    if (!classOk) return;
  }

static void classDeclaration(Compiler* c, bool isExport, bool isPrivate) {
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect class name.");
  classDeclarationWithName(c, name, isExport, false, isPrivate);
}

static void enumDeclaration(Compiler* c, bool isExport, bool isPrivate) {
  Token name = consume(c, TOKEN_IDENTIFIER, "Expect enum name.");
  Token openBrace = consume(c, TOKEN_LEFT_BRACE, "Expect '{' before enum body.");

  EnumInfo* enumInfo = compilerAddEnum(c, name);

  typedef struct {
    Token name;
    int arity;
    bool hasPayload;
    bool hasValue;
    double value;
  } EnumVariantTemp;

  EnumVariantTemp* variants = NULL;
  int variantCount = 0;
  int variantCapacity = 0;
  bool anyPayload = false;
  bool anyValue = false;

  if (!check(c, TOKEN_RIGHT_BRACE)) {
    do {
      Token member = consume(c, TOKEN_IDENTIFIER, "Expect enum member name.");
      int arity = 0;
      bool hasPayload = false;
      if (match(c, TOKEN_LEFT_PAREN)) {
        Token openParen = previous(c);
        hasPayload = true;
        if (!check(c, TOKEN_RIGHT_PAREN)) {
          do {
            consume(c, TOKEN_IDENTIFIER, "Expect payload name.");
            arity++;
          } while (match(c, TOKEN_COMMA));
        }
        consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after enum payload.", openParen);
      }

      bool hasValue = false;
      double value = 0.0;
      if (match(c, TOKEN_EQUAL)) {
        if (hasPayload) {
          errorAt(c, member, "Enum variants with payloads cannot have explicit values.");
        }
        hasValue = true;
        bool negative = false;
        if (match(c, TOKEN_MINUS)) {
          negative = true;
        }
        Token numToken = consume(c, TOKEN_NUMBER, "Expect number after '='.");
        value = parseNumberToken(numToken);
        if (negative) value = -value;
      }

      if (variantCount >= variantCapacity) {
        int oldCap = variantCapacity;
        variantCapacity = GROW_CAPACITY(oldCap);
        variants = (EnumVariantTemp*)realloc(variants,
                                             sizeof(EnumVariantTemp) * (size_t)variantCapacity);
        if (!variants) {
          fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
        memset(variants + oldCap, 0,
               sizeof(EnumVariantTemp) * (size_t)(variantCapacity - oldCap));
      }
      variants[variantCount++] = (EnumVariantTemp){
        member,
        arity,
        hasPayload,
        hasValue,
        value
      };
      if (hasPayload) anyPayload = true;
      if (hasValue) anyValue = true;
    } while (match(c, TOKEN_COMMA));
  }

  consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after enum body.", openBrace);

  if (anyPayload && anyValue) {
    errorAt(c, name, "Enums with payloads cannot use explicit numeric values.");
  }

  emitByte(c, OP_MAP, noToken());
  emitShort(c, (uint16_t)variantCount, noToken());
  int sizeOffset = c->chunk->count - 2;

  if (!anyPayload) {
    double nextValue = 0.0;
    for (int i = 0; i < variantCount; i++) {
      Token member = variants[i].name;
      enumInfoAddVariant(enumInfo, member, 0);
      char* memberName = copyTokenLexeme(member);
      ObjString* keyStr = takeStringWithLength(c->vm, memberName, member.length);
      emitConstant(c, OBJ_VAL(keyStr), member);

      double value = variants[i].hasValue ? variants[i].value : nextValue;
      nextValue = value + 1.0;

      emitConstant(c, NUMBER_VAL(value), member);
      emitByte(c, OP_MAP_SET, member);
    }
  } else {
    enumInfo->isAdt = true;
    char* enumNameChars = copyTokenLexeme(name);
    ObjString* enumNameStr = takeStringWithLength(c->vm, enumNameChars, name.length);
    for (int i = 0; i < variantCount; i++) {
      Token member = variants[i].name;
      int arity = variants[i].arity;
      enumInfoAddVariant(enumInfo, member, arity);
      char* memberName = copyTokenLexeme(member);
      ObjString* keyStr = takeStringWithLength(c->vm, memberName, member.length);
      emitConstant(c, OBJ_VAL(keyStr), member);
      if (arity == 0) {
        ObjMap* value = newEnumVariant(c->vm, enumNameStr, keyStr, 0, NULL);
        emitConstant(c, OBJ_VAL(value), member);
      } else {
        ObjEnumCtor* ctor = newEnumCtor(c->vm, enumNameStr, keyStr, arity);
        emitConstant(c, OBJ_VAL(ctor), member);
      }
      emitByte(c, OP_MAP_SET, member);
    }
  }

  c->chunk->code[sizeOffset] = (uint8_t)((variantCount >> 8) & 0xff);
  c->chunk->code[sizeOffset + 1] = (uint8_t)(variantCount & 0xff);

  int nameIdx = emitStringConstant(c, name);
  emitByte(c, OP_DEFINE_VAR, name);
  emitShort(c, (uint16_t)nameIdx, name);
  if (isPrivate) {
    emitPrivateName(c, nameIdx, name);
  }
  if (isExport) {
    emitByte(c, OP_EXPORT, name);
    emitShort(c, (uint16_t)nameIdx, name);
  }
  emitGc(c);
  free(variants);
}

typedef struct {
  uint16_t from;
  uint16_t to;
} ExportName;

static ExportName* parseExportList(Compiler* c, int* outCount, Token open) {
  int count = 0;
  int capacity = 4;
  ExportName* names = (ExportName*)malloc(sizeof(ExportName) * (size_t)capacity);
  if (!names) {
    fprintf(stderr, "Out of memory.\n");
    exit(1);
  }

  if (!check(c, TOKEN_RIGHT_BRACE)) {
    do {
      Token from;
      if (match(c, TOKEN_IDENTIFIER) || match(c, TOKEN_DEFAULT)) {
        from = previous(c);
      } else {
        errorAtCurrent(c, "Expect export name.");
        break;
      }
      Token to = from;
      if (match(c, TOKEN_AS)) {
        to = consume(c, TOKEN_IDENTIFIER, "Expect export name after 'as'.");
      }
      if (count >= capacity) {
        int oldCap = capacity;
        capacity = GROW_CAPACITY(oldCap);
        names = GROW_ARRAY(ExportName, names, oldCap, capacity);
        if (!names) {
          fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
      }
      names[count].from = (uint16_t)emitStringConstant(c, from);
      names[count].to = (uint16_t)emitStringConstant(c, to);
      count++;
    } while (match(c, TOKEN_COMMA));
  }
  consumeClosing(c, TOKEN_RIGHT_BRACE, "Expect '}' after export list.", open);

  *outCount = count;
  return names;
}

static void exportDeclaration(Compiler* c) {
  Token keyword = previous(c);
  bool allowExport = c->enclosing == NULL && c->scopeDepth == 0;
  if (!allowExport) {
    errorAt(c, keyword, "Export declarations must be at top level.");
  }

    if (match(c, TOKEN_DEFAULT)) {
        if (match(c, TOKEN_FUN)) {
        Token name = consume(c, TOKEN_IDENTIFIER, "Expect function name.");
        Type* functionType = NULL;
        ObjFunction* function = compileFunction(c, name, false, &functionType, true);
        (void)functionType;
        if (!function) return;
      int constant = makeConstant(c, OBJ_VAL(function), name);
      emitByte(c, OP_CLOSURE, name);
      emitShort(c, (uint16_t)constant, name);
      int nameIdx = emitStringConstant(c, name);
      emitByte(c, OP_DEFINE_VAR, name);
      emitShort(c, (uint16_t)nameIdx, name);
      if (allowExport) {
        emitGetVarConstant(c, nameIdx);
        int defaultIdx = emitStringConstantFromChars(c, "default", 7);
        emitExportValue(c, (uint16_t)defaultIdx, name);
      }
      emitGc(c);
      return;
    }
      if (match(c, TOKEN_CLASS)) {
        Token name = consume(c, TOKEN_IDENTIFIER, "Expect class name.");
        classDeclarationWithName(c, name, false, allowExport, false);
        return;
      }
      if (match(c, TOKEN_STRUCT)) {
        Token name = consume(c, TOKEN_IDENTIFIER, "Expect struct name.");
        structDeclarationWithName(c, name, false, allowExport, false);
        return;
      }
      expression(c);
    typePop(c);
    consume(c, TOKEN_SEMICOLON, "Expect ';' after export.");
    if (allowExport) {
      int defaultIdx = emitStringConstantFromChars(c, "default", 7);
      emitExportValue(c, (uint16_t)defaultIdx, keyword);
    } else {
      emitByte(c, OP_POP, keyword);
    }
    return;
  }

  if (match(c, TOKEN_STAR)) {
    consume(c, TOKEN_FROM, "Expect 'from' after '*'.");
    expression(c);
    typePop(c);
    consume(c, TOKEN_SEMICOLON, "Expect ';' after export.");
    if (allowExport) {
      emitByte(c, OP_IMPORT_MODULE, keyword);
      emitByte(c, OP_EXPORT_FROM, keyword);
      emitShort(c, 0, keyword);
    } else {
      emitByte(c, OP_POP, keyword);
    }
    emitGc(c);
    return;
  }

  if (match(c, TOKEN_LEFT_BRACE)) {
    Token openBrace = previous(c);
    int nameCount = 0;
    ExportName* names = parseExportList(c, &nameCount, openBrace);
    bool hasFrom = match(c, TOKEN_FROM);
    if (hasFrom) {
      expression(c);
      typePop(c);
    }
    consume(c, TOKEN_SEMICOLON, "Expect ';' after export.");

    if (allowExport) {
      if (hasFrom) {
        emitByte(c, OP_IMPORT_MODULE, keyword);
        emitByte(c, OP_EXPORT_FROM, keyword);
        emitShort(c, (uint16_t)nameCount, keyword);
        for (int i = 0; i < nameCount; i++) {
          emitShort(c, names[i].from, keyword);
          emitShort(c, names[i].to, keyword);
        }
      } else {
        for (int i = 0; i < nameCount; i++) {
          emitGetVarConstant(c, names[i].from);
          emitExportValue(c, names[i].to, keyword);
        }
      }
    } else if (hasFrom) {
      emitByte(c, OP_POP, keyword);
    }

    free(names);
    emitGc(c);
    return;
  }

  if (match(c, TOKEN_LET)) {
    varDeclaration(c, false, allowExport, false);
    return;
  }
  if (match(c, TOKEN_CONST)) {
    varDeclaration(c, true, allowExport, false);
    return;
  }
  if (match(c, TOKEN_FUN)) {
    functionDeclaration(c, allowExport, false);
    return;
  }
    if (match(c, TOKEN_CLASS)) {
      classDeclaration(c, allowExport, false);
      return;
    }
    if (match(c, TOKEN_STRUCT)) {
      structDeclaration(c, allowExport, false);
      return;
    }
    if (match(c, TOKEN_ENUM)) {
      enumDeclaration(c, allowExport, false);
      return;
    }
    if (match(c, TOKEN_INTERFACE)) {
      interfaceDeclaration(c);
      return;
    }

  Token name = consume(c, TOKEN_IDENTIFIER, "Expect declaration or identifier after 'export'.");
  consume(c, TOKEN_SEMICOLON, "Expect ';' after export.");
  if (allowExport) {
    emitExportName(c, name);
  }
}

static void privateDeclaration(Compiler* c) {
  Token keyword = previous(c);
  bool allowPrivate = c->enclosing == NULL && c->scopeDepth == 0;
  if (!allowPrivate) {
    errorAt(c, keyword, "Private declarations must be at top level.");
  }

  if (match(c, TOKEN_EXPORT)) {
    errorAt(c, keyword, "Private declarations cannot be exported.");
    exportDeclaration(c);
    return;
  }
  if (match(c, TOKEN_LET)) {
    varDeclaration(c, false, false, allowPrivate);
    return;
  }
  if (match(c, TOKEN_CONST)) {
    varDeclaration(c, true, false, allowPrivate);
    return;
  }
  if (match(c, TOKEN_FUN)) {
    functionDeclaration(c, false, allowPrivate);
    return;
  }
    if (match(c, TOKEN_CLASS)) {
      classDeclaration(c, false, allowPrivate);
      return;
    }
    if (match(c, TOKEN_STRUCT)) {
      structDeclaration(c, false, allowPrivate);
      return;
    }
  if (match(c, TOKEN_ENUM)) {
    enumDeclaration(c, false, allowPrivate);
    return;
  }
  if (match(c, TOKEN_INTERFACE)) {
    interfaceDeclaration(c);
    return;
  }

  errorAtCurrent(c, "Expect declaration after 'private'.");
}

static void declaration(Compiler* c) {
  if (match(c, TOKEN_PRIVATE)) {
    privateDeclaration(c);
  } else if (match(c, TOKEN_EXPORT)) {
    exportDeclaration(c);
  } else if (match(c, TOKEN_CLASS)) {
    classDeclaration(c, false, false);
  } else if (match(c, TOKEN_STRUCT)) {
    structDeclaration(c, false, false);
  } else if (match(c, TOKEN_FUN)) {
    functionDeclaration(c, false, false);
  } else if (match(c, TOKEN_INTERFACE)) {
    interfaceDeclaration(c);
  } else if (isTypeDeclarationStart(c)) {
    advance(c);
    typeDeclaration(c);
  } else if (match(c, TOKEN_CONST)) {
    varDeclaration(c, true, false, false);
  } else if (match(c, TOKEN_LET)) {
    varDeclaration(c, false, false, false);
  } else if (match(c, TOKEN_ENUM)) {
    enumDeclaration(c, false, false);
  } else if (match(c, TOKEN_IMPORT)) {
    importStatement(c);
  } else if (match(c, TOKEN_FROM)) {
    fromImportStatement(c);
  } else {
    statement(c);
  }
  if (c->panicMode) synchronize(c);
}

static void statement(Compiler* c) {
  if (compilerPluginParseStatement(c)) {
    return;
  }
  if (match(c, TOKEN_IF)) {
    ifStatement(c);
  } else if (match(c, TOKEN_WHILE)) {
    whileStatement(c);
  } else if (match(c, TOKEN_FOR)) {
    forStatement(c);
  } else if (match(c, TOKEN_FOREACH)) {
    foreachStatement(c);
  } else if (match(c, TOKEN_SWITCH) || match(c, TOKEN_MATCH)) {
    switchStatement(c);
    } else if (match(c, TOKEN_TRY)) {
      tryStatement(c);
    } else if (match(c, TOKEN_THROW)) {
      throwStatement(c);
    } else if (match(c, TOKEN_DEFER)) {
      deferStatement(c);
    } else if (match(c, TOKEN_YIELD)) {
      yieldStatement(c);
  } else if (match(c, TOKEN_RETURN)) {
    returnStatement(c);
  } else if (match(c, TOKEN_BREAK)) {
    breakStatement(c);
  } else if (match(c, TOKEN_CONTINUE)) {
    continueStatement(c);
  } else if ((check(c, TOKEN_LEFT_BRACKET) || check(c, TOKEN_LEFT_BRACE)) &&
             isPatternAssignmentStart(c)) {
    patternAssignmentStatement(c);
  } else if (match(c, TOKEN_LEFT_BRACE)) {
    blockStatement(c);
  } else {
    expressionStatement(c);
  }
}

static ObjFunction* compileFunction(Compiler* c, Token name, bool isInitializer,
                                    Type** outType, bool defineType) {
  int typeParamCount = 0;
  TypeParam* typeParams = parseTypeParams(c, &typeParamCount);
  int savedTypeParamCount = 0;
  if (typecheckEnabled(c)) {
    savedTypeParamCount = c->typecheck->typeParamCount;
    typeParamsPushList(c->typecheck, typeParams, typeParamCount);
  }

  Token openParen = consume(c, TOKEN_LEFT_PAREN, "Expect '(' after function name.");

  int arity = 0;
  bool sawDefault = false;
  int savedStart = c->current;

  if (!check(c, TOKEN_RIGHT_PAREN)) {
    do {
      Pattern* paramPattern = parsePattern(c);
      bool allowType = paramPattern->kind == PATTERN_BINDING;
      arity++;
      if (match(c, TOKEN_COLON)) {
        if (!allowType) {
          errorAt(c, previous(c), "Type annotations require a single identifier.");
        }
        parseType(c);
      }
      if (match(c, TOKEN_EQUAL)) {
        sawDefault = true;
        int depth = 0;
        while (!isAtEnd(c)) {
          if (check(c, TOKEN_COMMA) && depth == 0) break;
          if (check(c, TOKEN_RIGHT_PAREN) && depth == 0) break;
          if (check(c, TOKEN_LEFT_PAREN) || check(c, TOKEN_LEFT_BRACKET) ||
              check(c, TOKEN_LEFT_BRACE)) depth++;
          if (check(c, TOKEN_RIGHT_PAREN) || check(c, TOKEN_RIGHT_BRACKET) ||
              check(c, TOKEN_RIGHT_BRACE)) depth--;
          advance(c);
        }
      }
      freePattern(paramPattern);
    } while (match(c, TOKEN_COMMA));
  }
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.", openParen);

  c->current = savedStart;

  int minArity = arity;
  ObjString** params = NULL;
  Token* paramTokens = NULL;
  Type** paramTypes = NULL;
  bool* paramHasType = NULL;
  Pattern** paramPatterns = NULL;
  char** paramNameStorage = NULL;
  int* defaultStarts = NULL;
  int* defaultEnds = NULL;
  if (arity > 0) {
    params = (ObjString**)malloc(sizeof(ObjString*) * (size_t)arity);
    paramTokens = (Token*)malloc(sizeof(Token) * (size_t)arity);
    paramTypes = (Type**)malloc(sizeof(Type*) * (size_t)arity);
    paramHasType = (bool*)malloc(sizeof(bool) * (size_t)arity);
    paramPatterns = (Pattern**)malloc(sizeof(Pattern*) * (size_t)arity);
    paramNameStorage = (char**)malloc(sizeof(char*) * (size_t)arity);
    defaultStarts = (int*)malloc(sizeof(int) * (size_t)arity);
    defaultEnds = (int*)malloc(sizeof(int) * (size_t)arity);
    if (!params || !paramTokens || !paramTypes || !paramHasType ||
        !paramPatterns || !paramNameStorage || !defaultStarts || !defaultEnds) {
      fprintf(stderr, "Out of memory.\n");
      exit(1);
    }
    for (int i = 0; i < arity; i++) {
      defaultStarts[i] = -1;
      defaultEnds[i] = -1;
      paramTypes[i] = typeUnknown();
      paramHasType[i] = false;
      paramPatterns[i] = NULL;
      paramNameStorage[i] = NULL;
    }
  }

  sawDefault = false;
  int paramIdx = 0;
  if (!check(c, TOKEN_RIGHT_PAREN)) {
    do {
      if (paramIdx >= arity) break;
      Pattern* paramPattern = parsePattern(c);
      bool allowType = paramPattern->kind == PATTERN_BINDING;
      Token paramName = paramPattern->token;
      if (paramPattern->kind == PATTERN_BINDING) {
        freePattern(paramPattern);
        paramPattern = NULL;
      } else {
        char buffer[64];
        int length = snprintf(buffer, sizeof(buffer), "__arg%d", paramIdx);
        if (length < 0) length = 0;
        if (length >= (int)sizeof(buffer)) length = (int)sizeof(buffer) - 1;
        char* nameCopy = (char*)malloc((size_t)length + 1);
        if (!nameCopy) {
          fprintf(stderr, "Out of memory.\n");
          exit(1);
        }
        memcpy(nameCopy, buffer, (size_t)length);
        nameCopy[length] = '\0';
        paramNameStorage[paramIdx] = nameCopy;
        paramName.start = nameCopy;
        paramName.length = length;
        paramPatterns[paramIdx] = paramPattern;
      }
      params[paramIdx] = stringFromToken(c->vm, paramName);
      paramTokens[paramIdx] = paramName;
      if (match(c, TOKEN_COLON)) {
        if (!allowType) {
          errorAt(c, previous(c), "Type annotations require a single identifier.");
          parseType(c);
        } else {
          paramTypes[paramIdx] = parseType(c);
          paramHasType[paramIdx] = true;
        }
      }
      if (match(c, TOKEN_EQUAL)) {
        if (!sawDefault) minArity = paramIdx;
        sawDefault = true;
        defaultStarts[paramIdx] = c->current;
        int depth = 0;
        while (!isAtEnd(c)) {
          if (check(c, TOKEN_COMMA) && depth == 0) break;
          if (check(c, TOKEN_RIGHT_PAREN) && depth == 0) break;
          if (check(c, TOKEN_LEFT_PAREN) || check(c, TOKEN_LEFT_BRACKET) ||
              check(c, TOKEN_LEFT_BRACE)) depth++;
          if (check(c, TOKEN_RIGHT_PAREN) || check(c, TOKEN_RIGHT_BRACKET) ||
              check(c, TOKEN_RIGHT_BRACE)) depth--;
          advance(c);
        }
        defaultEnds[paramIdx] = c->current;
      } else if (sawDefault) {
        errorAt(c, paramName, "Parameters with defaults must be last.");
      }
      paramIdx++;
    } while (match(c, TOKEN_COMMA));
  }
  consumeClosing(c, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.", openParen);

  Type* returnType = typeAny();
  if (match(c, TOKEN_COLON)) {
    returnType = parseType(c);
  }
  if (typecheckEnabled(c)) {
    typeParamsTruncate(c->typecheck, savedTypeParamCount);
  }
  Token openBrace = consume(c, TOKEN_LEFT_BRACE, "Expect '{' before function body.");
  int bodyStart = c->current;

  Type* functionType = NULL;
  if (outType) {
    functionType = typeFunction(c->typecheck, paramTypes, arity, returnType);
    if (functionType && typeParamCount > 0 && typeParams) {
      functionType->typeParamCount = typeParamCount;
      functionType->typeParams = (TypeParam*)malloc(sizeof(TypeParam) * (size_t)typeParamCount);
      if (!functionType->typeParams) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
      }
      for (int i = 0; i < typeParamCount; i++) {
        functionType->typeParams[i] = typeParams[i];
      }
    }
    *outType = functionType;
    if (defineType && typecheckEnabled(c)) {
      typeDefine(c, name, functionType, true);
    }
  }

  Chunk* chunk = (Chunk*)malloc(sizeof(Chunk));
  if (!chunk) { fprintf(stderr, "Out of memory.\n"); exit(1); }
  initChunk(chunk);

  ObjString* fnName = stringFromToken(c->vm, name);
  ObjFunction* function = newFunction(c->vm, fnName, arity, minArity, isInitializer, params, chunk, NULL, NULL);

  Compiler fnCompiler;
  fnCompiler.vm = c->vm;
  fnCompiler.tokens = c->tokens;
  fnCompiler.source = c->source;
  fnCompiler.path = c->path;
  fnCompiler.current = bodyStart;
  fnCompiler.panicMode = false;
  fnCompiler.hadError = false;
  fnCompiler.chunk = chunk;
  fnCompiler.scopeDepth = 0;
  fnCompiler.tempIndex = 0;
  fnCompiler.pendingOptionalCall = false;
  fnCompiler.forbidCall = false;
  fnCompiler.lastExprWasVar = false;
  memset(&fnCompiler.lastExprVar, 0, sizeof(Token));
  fnCompiler.hasYield = false;
  fnCompiler.yieldName = -1;
  fnCompiler.yieldFlagName = -1;
  fnCompiler.breakContext = NULL;
  fnCompiler.enclosing = c;
  fnCompiler.enums = NULL;
  fnCompiler.enumCount = 0;
  fnCompiler.enumCapacity = 0;
  fnCompiler.structs = NULL;
  fnCompiler.structCount = 0;
  fnCompiler.structCapacity = 0;

  TypeChecker fnTypeChecker;
  typeCheckerInit(&fnTypeChecker, c->typecheck,
                  c->typecheck ? c->typecheck->enabled : false);
  fnTypeChecker.currentReturn = returnType;
    if (c->typecheck && c->typecheck->enabled) {
      typeParamsPushList(&fnTypeChecker, typeParams, typeParamCount);
    }
  fnCompiler.typecheck = &fnTypeChecker;

  if (typecheckEnabled(&fnCompiler)) {
    for (int i = 0; i < arity; i++) {
      Type* paramType = paramHasType && paramHasType[i] ? paramTypes[i] : typeUnknown();
      bool isExplicit = paramHasType && paramHasType[i];
      typeDefine(&fnCompiler, paramTokens[i], paramType, isExplicit);
    }
  }

  fnCompiler.yieldName = emitStringConstantFromChars(&fnCompiler, "__yield", 7);
  emitByte(&fnCompiler, OP_ARRAY, noToken());
  emitShort(&fnCompiler, 0, noToken());
  emitDefineVarConstant(&fnCompiler, fnCompiler.yieldName);
  fnCompiler.yieldFlagName = emitStringConstantFromChars(&fnCompiler, "__yield_used", 12);
  emitByte(&fnCompiler, OP_FALSE, noToken());
  emitDefineVarConstant(&fnCompiler, fnCompiler.yieldFlagName);
  if (typecheckEnabled(&fnCompiler)) {
    typeDefine(&fnCompiler, syntheticToken("__yield"),
               typeArray(fnCompiler.typecheck, typeAny()), true);
    typeDefine(&fnCompiler, syntheticToken("__yield_used"), typeBool(), true);
  }

  for (int i = 0; i < arity; i++) {
    if (!defaultStarts || defaultStarts[i] < 0) continue;

    Token ptoken = paramTokens[i];
    emitByte(&fnCompiler, OP_ARG_COUNT, ptoken);
    emitConstant(&fnCompiler, NUMBER_VAL((double)(i + 1)), ptoken);
    emitByte(&fnCompiler, OP_LESS, ptoken);
    int skipJump = emitJump(&fnCompiler, OP_JUMP_IF_FALSE, ptoken);
    emitByte(&fnCompiler, OP_POP, noToken());

    int savedCurrent = fnCompiler.current;
    fnCompiler.current = defaultStarts[i];
    expression(&fnCompiler);
    Type* defaultType = typePop(&fnCompiler);
    if (typecheckEnabled(&fnCompiler) && paramHasType && paramHasType[i]) {
      if (!typeAssignable(paramTypes[i], defaultType)) {
        char expected[64];
        char got[64];
        typeToString(paramTypes[i], expected, sizeof(expected));
        typeToString(defaultType, got, sizeof(got));
        typeErrorAt(&fnCompiler, ptoken, "Default value expects %s but got %s.", expected, got);
      }
    }
    fnCompiler.current = savedCurrent;

    int nameIndex = emitStringConstant(&fnCompiler, ptoken);
    emitByte(&fnCompiler, OP_SET_VAR, ptoken);
    emitShort(&fnCompiler, (uint16_t)nameIndex, ptoken);
    emitByte(&fnCompiler, OP_POP, noToken());

    int endJump = emitJump(&fnCompiler, OP_JUMP, ptoken);
    patchJump(&fnCompiler, skipJump, ptoken);
    emitByte(&fnCompiler, OP_POP, noToken());
    patchJump(&fnCompiler, endJump, ptoken);
    emitGc(&fnCompiler);
  }

  if (paramPatterns) {
    for (int i = 0; i < arity; i++) {
      Pattern* pattern = paramPatterns[i];
      if (!pattern) continue;
      PatternBindingList bindings;
      patternBindingListInit(&bindings);
      int paramNameIdx = emitStringConstant(&fnCompiler, paramTokens[i]);
      emitPatternMatchOrThrow(&fnCompiler, paramNameIdx, pattern, &bindings);
      Type* paramType = typeAny();
      if (paramHasType && paramHasType[i]) {
        paramType = paramTypes[i];
      }
      emitPatternBindings(&fnCompiler, paramNameIdx, &bindings, OP_DEFINE_VAR, paramType);
      patternBindingListFree(&bindings);
      freePattern(pattern);
      paramPatterns[i] = NULL;
    }
  }

  fnCompiler.current = bodyStart;
  while (!check(&fnCompiler, TOKEN_RIGHT_BRACE) && !isAtEnd(&fnCompiler)) {
    declaration(&fnCompiler);
  }
  consumeClosing(&fnCompiler, TOKEN_RIGHT_BRACE, "Expect '}' after function body.", openBrace);

  emitByte(&fnCompiler, OP_NULL, noToken());
  if (fnCompiler.yieldName >= 0 && fnCompiler.yieldFlagName >= 0) {
    emitGetVarConstant(&fnCompiler, fnCompiler.yieldFlagName);
    int normalJump = emitJump(&fnCompiler, OP_JUMP_IF_FALSE, noToken());
    emitByte(&fnCompiler, OP_POP, noToken());
    emitByte(&fnCompiler, OP_POP, noToken());
    emitGetVarConstant(&fnCompiler, fnCompiler.yieldName);
    emitByte(&fnCompiler, OP_RETURN, noToken());
    patchJump(&fnCompiler, normalJump, noToken());
    emitByte(&fnCompiler, OP_POP, noToken());
    emitByte(&fnCompiler, OP_RETURN, noToken());
  } else {
    emitByte(&fnCompiler, OP_RETURN, noToken());
  }

  c->current = fnCompiler.current;

  if (paramPatterns) {
    for (int i = 0; i < arity; i++) {
      if (paramPatterns[i]) {
        freePattern(paramPatterns[i]);
      }
    }
    free(paramPatterns);
  }
  if (paramNameStorage) {
    for (int i = 0; i < arity; i++) {
      free(paramNameStorage[i]);
    }
    free(paramNameStorage);
  }
  free(paramTokens);
  free(paramTypes);
  free(paramHasType);
  free(defaultStarts);
  free(defaultEnds);
  free(typeParams);

  typeCheckerFree(&fnTypeChecker);
  compilerEnumsFree(&fnCompiler);
  compilerStructsFree(&fnCompiler);

  if (fnCompiler.hadError) {
    c->hadError = true;
    return NULL;
  }

  optimizeChunk(c->vm, chunk);
  return function;
}

  ObjFunction* compile(VM* vm, const TokenArray* tokens, const char* source,
                       const char* path, bool* hadError) {
    initRules();

    Chunk* chunk = (Chunk*)malloc(sizeof(Chunk));
  if (!chunk) { fprintf(stderr, "Out of memory.\n"); exit(1); }
  initChunk(chunk);

  ObjFunction* function = newFunction(vm, NULL, 0, 0, false, NULL, chunk, NULL, NULL);

    Compiler c;
    TypeChecker typecheck;
    TypeRegistry registry;
    typeRegistryInit(&registry);
    typeCheckerInit(&typecheck, NULL, vm->typecheck);
    c.vm = vm;
    c.tokens = tokens;
    c.source = source;
    c.path = path;
  c.current = 0;
  c.panicMode = false;
  c.hadError = false;
  c.chunk = chunk;
  c.scopeDepth = 0;
  c.tempIndex = 0;
  c.pendingOptionalCall = false;
  c.forbidCall = false;
  c.lastExprWasVar = false;
  memset(&c.lastExprVar, 0, sizeof(Token));
  c.hasYield = false;
  c.yieldName = -1;
  c.yieldFlagName = -1;
  c.breakContext = NULL;
    c.enclosing = NULL;
    c.typecheck = &typecheck;
    c.enums = NULL;
  c.enumCount = 0;
  c.enumCapacity = 0;
  c.structs = NULL;
  c.structCount = 0;
  c.structCapacity = 0;
    vm->compiler = &c;
    gTypeRegistry = &registry;
    typeDefineStdlib(&c);

  while (!isAtEnd(&c)) {
    declaration(&c);
  }

  emitByte(&c, OP_NULL, noToken());
  emitByte(&c, OP_RETURN, noToken());

    vm->compiler = NULL;
    gTypeRegistry = NULL;

  *hadError = c.hadError;
  if (c.hadError) {
    compilerEnumsFree(&c);
    compilerStructsFree(&c);
    typeCheckerFree(&typecheck);
    typeRegistryFree(&registry);
    return NULL;
  }
  optimizeChunk(vm, chunk);
  compilerEnumsFree(&c);
  compilerStructsFree(&c);
  typeCheckerFree(&typecheck);
  typeRegistryFree(&registry);
  return function;
}

