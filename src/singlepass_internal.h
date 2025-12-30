#ifndef ERKAO_SINGLEPASS_INTERNAL_H
#define ERKAO_SINGLEPASS_INTERNAL_H

#include "singlepass.h"
#include "common.h"
#include "diagnostics.h"

typedef enum {
  CONST_NULL,
  CONST_BOOL,
  CONST_NUMBER,
  CONST_STRING
} ConstType;

typedef struct {
  ConstType type;
  bool ownsString;
  union {
    bool boolean;
    double number;
    struct { const char* chars; int length; } string;
  } as;
} ConstValue;

typedef enum {
  PATTERN_LITERAL,
  PATTERN_BINDING,
  PATTERN_PIN,
  PATTERN_WILDCARD,
  PATTERN_ARRAY,
  PATTERN_MAP,
  PATTERN_ENUM
} PatternKind;

typedef struct Pattern Pattern;

typedef struct {
  Pattern** items;
  int count;
  int capacity;
  bool hasRest;
  Token restName;
} PatternList;

typedef struct {
  Token key;
  bool keyIsString;
  Pattern* value;
} PatternMapEntry;

typedef struct {
  PatternMapEntry* entries;
  int count;
  int capacity;
  bool hasRest;
  Token restName;
} PatternMap;

typedef struct {
  Token enumToken;
  Token variantToken;
  Pattern** args;
  int argCount;
  int argCapacity;
} PatternEnum;

struct Pattern {
  PatternKind kind;
  Token token;
  union {
    PatternList array;
    PatternMap map;
    PatternEnum enumPattern;
  } as;
};

typedef enum {
  PATH_INDEX,
  PATH_KEY
} PatternPathKind;

typedef struct {
  PatternPathKind kind;
  int index;
  Token key;
  bool keyIsString;
} PatternPathStep;

typedef struct {
  PatternPathStep* steps;
  int count;
  int capacity;
} PatternPath;

typedef enum {
  PATTERN_BIND_PATH,
  PATTERN_BIND_ARRAY_REST,
  PATTERN_BIND_MAP_REST
} PatternBindingKind;

typedef struct {
  Token key;
  bool keyIsString;
} PatternRestKey;

typedef struct {
  Token name;
  PatternPathStep* steps;
  int stepCount;
  PatternBindingKind kind;
  int restIndex;
  PatternRestKey* restKeys;
  int restKeyCount;
} PatternBinding;

typedef struct {
  PatternBinding* entries;
  int count;
  int capacity;
} PatternBindingList;

typedef struct {
  int jump;
  PatternPathStep* steps;
  int stepCount;
  Token token;
} PatternFailure;

typedef struct {
  PatternFailure* entries;
  int count;
  int capacity;
} PatternFailureList;

typedef enum {
  TYPE_ANY,
  TYPE_UNKNOWN,
  TYPE_NUMBER,
  TYPE_STRING,
  TYPE_BOOL,
  TYPE_NULL,
  TYPE_ARRAY,
  TYPE_MAP,
  TYPE_NAMED,
  TYPE_GENERIC,
  TYPE_UNION,
  TYPE_FUNCTION
} TypeKind;

typedef struct {
  ObjString* name;
  ObjString* constraint;
} TypeParam;

typedef struct Type {
  TypeKind kind;
  ObjString* name;
  struct Type* elem;
  struct Type* key;
  struct Type* value;
  struct Type** params;
  int paramCount;
  struct Type* returnType;
  TypeParam* typeParams;
  int typeParamCount;
  struct Type** typeArgs;
  int typeArgCount;
  struct Type** unionTypes;
  int unionCount;
  bool nullable;
} Type;

typedef struct {
  ObjString* name;
  Type* type;
  bool explicitType;
  int depth;
} TypeEntry;

typedef struct {
  ObjString* name;
  Type* type;
  int depth;
} TypeAlias;

typedef struct {
  ObjString* name;
  Type* type;
} InterfaceMethod;

typedef struct {
  ObjString* name;
  TypeParam* typeParams;
  int typeParamCount;
  InterfaceMethod* methods;
  int methodCount;
  int methodCapacity;
} InterfaceDef;

typedef struct {
  ObjString* name;
  ObjString** interfaces;
  int interfaceCount;
  int interfaceCapacity;
} ClassDef;

typedef struct {
  InterfaceDef* interfaces;
  int interfaceCount;
  int interfaceCapacity;
  ClassDef* classes;
  int classCount;
  int classCapacity;
} TypeRegistry;

typedef struct {
  ObjString* name;
  ObjString* constraint;
  Type* bound;
} TypeBinding;

typedef struct {
  ObjString* name;
  Type* type;
} ClassMethod;

struct TypeChecker {
  bool enabled;
  int errorCount;
  int scopeDepth;
  struct TypeChecker* enclosing;
  TypeEntry* entries;
  int count;
  int capacity;
  TypeAlias* aliases;
  int aliasCount;
  int aliasCapacity;
  Type** stack;
  int stackCount;
  int stackCapacity;
  Type** allocated;
  int allocatedCount;
  int allocatedCapacity;
  Type* currentReturn;
  TypeParam* typeParams;
  int typeParamCount;
  int typeParamCapacity;
};

typedef struct EnumVariantInfo {
  char* name;
  int nameLength;
  int arity;
} EnumVariantInfo;

struct EnumInfo {
  char* name;
  int nameLength;
  EnumVariantInfo* variants;
  int variantCount;
  int variantCapacity;
  bool isAdt;
};

extern TypeRegistry* gTypeRegistry;

bool isAtEnd(Compiler* c);
Token peek(Compiler* c);
Token previous(Compiler* c);
Token advance(Compiler* c);
bool check(Compiler* c, ErkaoTokenType type);
bool checkNext(Compiler* c, ErkaoTokenType type);
bool checkNextNext(Compiler* c, ErkaoTokenType type);
bool match(Compiler* c, ErkaoTokenType type);
Token noToken(void);
const char* keywordLexeme(ErkaoTokenType type);
const char* tokenDescription(ErkaoTokenType type);
void noteAt(Compiler* c, Token token, const char* message);
void synchronizeExpression(Compiler* c);
void errorAt(Compiler* c, Token token, const char* message);
void errorAtCurrent(Compiler* c, const char* message);
Token consume(Compiler* c, ErkaoTokenType type, const char* message);
Token consumeClosing(Compiler* c, ErkaoTokenType type, const char* message, Token open);
void synchronize(Compiler* c);

void emitByte(Compiler* c, uint8_t byte, Token token);
void emitBytes(Compiler* c, uint8_t a, uint8_t b, Token token);
void emitShort(Compiler* c, uint16_t value, Token token);
int makeConstant(Compiler* c, Value value, Token token);
void emitConstant(Compiler* c, Value value, Token token);
int emitJump(Compiler* c, uint8_t instruction, Token token);
void patchJump(Compiler* c, int offset, Token token);
void emitLoop(Compiler* c, int loopStart, Token token);
int emitStringConstant(Compiler* c, Token token);
int emitStringConstantFromChars(Compiler* c, const char* chars, int len);
int emitTempNameConstant(Compiler* c, const char* prefix);
void emitGetVarConstant(Compiler* c, int idx);
void emitSetVarConstant(Compiler* c, int idx);
void emitDefineVarConstant(Compiler* c, int idx);
void emitExportName(Compiler* c, Token name);
void emitExportValue(Compiler* c, uint16_t nameIdx, Token token);
void emitPrivateName(Compiler* c, int nameIdx, Token token);
void emitGc(Compiler* c);
void initJumpList(JumpList* list);
void writeJumpList(JumpList* list, int offset);
void freeJumpList(JumpList* list);
void patchJumpTo(Compiler* c, int offset, int target, Token token);
void patchJumpList(Compiler* c, JumpList* list, int target, Token token);
void emitScopeExits(Compiler* c, int depth);
BreakContext* findLoopContext(Compiler* c);
char* copyTokenLexeme(Token token);
Token syntheticToken(const char* text);

bool constValueIsTruthy(const ConstValue* v);
bool constValueFromValue(Value value, ConstValue* out);
bool constValueEquals(const ConstValue* a, const ConstValue* b);
bool constValueConcat(const ConstValue* a, const ConstValue* b, ConstValue* out);
bool constValueStringify(const ConstValue* input, ConstValue* out);
void constValueFree(ConstValue* v);
bool constValueListContains(ConstValue* values, int count, ConstValue* value);
void constValueListAdd(ConstValue** values, int* count, int* capacity,
                       ConstValue* value);
void constValueListFree(ConstValue* values, int count, int capacity);

bool typecheckEnabled(Compiler* c);
Type* typeAny(void);
Type* typeUnknown(void);
Type* typeNumber(void);
Type* typeString(void);
Type* typeBool(void);
Type* typeNull(void);
Type* typeAlloc(TypeChecker* tc, TypeKind kind);
void typeCheckerInit(TypeChecker* tc, TypeChecker* enclosing, bool enabled);
void typeCheckerFree(TypeChecker* tc);
void typeRegistryInit(TypeRegistry* registry);
void typeRegistryFree(TypeRegistry* registry);
InterfaceDef* typeRegistryFindInterface(TypeRegistry* registry, ObjString* name);
ClassDef* typeRegistryFindClass(TypeRegistry* registry, ObjString* name);
void typeRegistryAddInterface(TypeRegistry* registry, const InterfaceDef* def);
void typeRegistryAddClass(TypeRegistry* registry, ObjString* name,
                          ObjString** interfaces, int interfaceCount);
bool typeRegistryClassImplements(TypeRegistry* registry, ObjString* className,
                                 ObjString* ifaceName);
void typeParamsPushList(TypeChecker* tc, const TypeParam* params, int count);
void typeParamsTruncate(TypeChecker* tc, int count);
TypeParam* typeParamFindToken(TypeChecker* tc, Token token);
void typePush(Compiler* c, Type* type);
Type* typePop(Compiler* c);
bool typeIsAny(Type* type);
bool typeIsNullable(Type* type);
Type* typeClone(TypeChecker* tc, Type* src);
Type* typeMakeNullable(TypeChecker* tc, Type* type);
bool typeEquals(Type* a, Type* b);
bool typeNamesEqual(ObjString* a, ObjString* b);
bool typeAssignable(Type* dst, Type* src);
bool typeSatisfiesConstraint(Type* actual, ObjString* constraint);
bool typeUnify(Compiler* c, Type* pattern, Type* actual,
               TypeBinding* bindings, int bindingCount, Token token);
Type* typeSubstitute(TypeChecker* tc, Type* type, TypeBinding* bindings,
                     int bindingCount);
void typeToString(Type* type, char* buffer, size_t size);
void typeErrorAt(Compiler* c, Token token, const char* format, ...);
void typeCheckerEnterScope(Compiler* c);
void typeCheckerExitScope(Compiler* c);
void typeDefine(Compiler* c, Token name, Type* type, bool explicitType);
TypeAlias* typeAliasLookup(TypeChecker* tc, ObjString* name);
void typeAliasDefine(Compiler* c, Token name, Type* type);
TypeEntry* typeLookupEntry(TypeChecker* tc, ObjString* name);
Type* typeLookup(Compiler* c, Token name);
void typeAssign(Compiler* c, Token name, Type* valueType);
void typeDefineSynthetic(Compiler* c, const char* name, Type* type);
bool typeNamedIs(Type* type, const char* name);
Type* typeFunctionN(TypeChecker* tc, int paramCount, Type* returnType, ...);
Type* typeLookupStdlibMember(Compiler* c, Type* objectType, Token name);
void typeDefineStdlib(Compiler* c);
Type* typeNamed(TypeChecker* tc, ObjString* name);
Type* typeGeneric(TypeChecker* tc, ObjString* name);
Type* typeArray(TypeChecker* tc, Type* elem);
Type* typeMap(TypeChecker* tc, Type* key, Type* value);
Type* typeUnion(TypeChecker* tc, Type* a, Type* b);
Type* typeFunction(TypeChecker* tc, Type** params, int paramCount, Type* returnType);
TypeParam* parseTypeParams(Compiler* c, int* outCount);
Type* parseTypeArguments(Compiler* c, Type* base, Token typeToken);
Type* parseType(Compiler* c);
Type* typeMerge(TypeChecker* tc, Type* current, Type* next);
bool typeEnsureNonNull(Compiler* c, Token token, Type* type, const char* message);
Type* typeUnaryResult(Compiler* c, Token op, Type* right);
Type* typeBinaryResult(Compiler* c, Token op, Type* left, Type* right);
Type* typeLogicalResult(Type* left, Type* right);
Type* typeIndexResult(Compiler* c, Token op, Type* objectType, Type* indexType);
void typeCheckIndexAssign(Compiler* c, Token op, Type* objectType, Type* indexType,
                          Type* valueType);

void compilerEnumsFree(Compiler* c);
EnumInfo* compilerAddEnum(Compiler* c, Token name);
EnumInfo* findEnumInfo(Compiler* c, Token name);
EnumVariantInfo* enumInfoAddVariant(EnumInfo* info, Token name, int arity);
EnumVariantInfo* findEnumVariant(EnumInfo* info, Token name);
int enumVariantIndex(EnumInfo* info, Token name);
void enumInfoSetAdt(EnumInfo* info, bool isAdt);

char* parseStringChars(const char* start, int length);
bool isTripleQuoted(Token token);
char* parseStringLiteral(Token token);
char* parseStringSegment(Token token);
double parseNumberToken(Token token);
bool tokenMatches(Token token, const char* text);

Pattern* parsePattern(Compiler* c);
void freePattern(Pattern* pattern);
void patternBindingListInit(PatternBindingList* list);
void patternBindingListFree(PatternBindingList* list);
PatternBinding* patternBindingFind(PatternBindingList* list, Token name);
bool patternIsCatchAll(Pattern* pattern);
bool patternConstValue(Pattern* pattern, ConstValue* out);
void emitPatternMatchValue(Compiler* c, int switchValue, Pattern* pattern,
                           PatternBindingList* bindings);
void emitPatternBindings(Compiler* c, int switchValue, PatternBindingList* bindings,
                         uint8_t defineOp, Type* matchType);
Type* typeNarrowByPattern(Compiler* c, Type* valueType, Pattern* pattern);
void emitPatternMatchOrThrow(Compiler* c, int switchValue, Pattern* pattern,
                             PatternBindingList* bindings);
void emitPatternRestKeyArray(Compiler* c, PatternBinding* binding);
void emitPatternKeyConstant(Compiler* c, Token key, bool keyIsString, Token token);

void compilerApplyPluginRules(void);
bool compilerPluginParseStatement(Compiler* c);
bool compilerPluginParseExpression(Compiler* c, bool canAssign);
void compilerPluginTypeHooks(Compiler* c);

void optimizeChunk(VM* vm, Chunk* chunk);

#endif
